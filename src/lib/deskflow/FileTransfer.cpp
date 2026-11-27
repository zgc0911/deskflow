/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/FileTransfer.h"

#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace deskflow {

namespace {

//! Requests of one computer that are answered at the same time
const size_t kMaxServedPerChannel = 16;

//! File lists of other computers that are remembered
const size_t kMaxRemoteOffers = 8;

//! File data written to a connection before waiting for it to drain
const uint64_t kMaxUnflushedBytes = 2 * FileTransfer::Protocol::ChunkSize;

//! File data read ahead of what was sent
const uint64_t kMaxReadAheadBytes = 2 * FileTransfer::Protocol::ChunkSize;

//! Directory entries put in one message
const int kEntriesPerMessage = 256;
const size_t kListingBytesPerMessage = 48 * 1024;

//! A listing larger than this is refused by the computer that asked for it
const uint64_t kMaxListingBytes = 64ULL * 1024 * 1024;

//! Work for the thread that runs the event queue
class FileTransferTask : public EventData
{
public:
  explicit FileTransferTask(std::function<void()> task) : m_task(std::move(task))
  {
  }

  void run() const
  {
    m_task();
  }

private:
  std::function<void()> m_task;
};

bool isLink(const QFileInfo &info)
{
  return info.isSymbolicLink() || info.isJunction();
}

FileTransferStatus statusFromError(uint32_t code)
{
  using enum FileTransfer::Error;
  switch (static_cast<FileTransfer::Error>(code)) {
  case Disabled:
    return FileTransferStatus::Refused;
  case UnknownOffer:
  case BadPath:
  case NotFound:
    return FileTransferStatus::NotFound;
  default:
    return FileTransferStatus::Failed;
  }
}

} // namespace

//! One request made to another computer
/*!
Shared between the thread that runs the event queue, which fills it, and the
thread that waits for the answer.
*/
struct FileTransferRequest
{
  std::mutex mutex;
  std::condition_variable changed;
  uint32_t id = 0;
  uint8_t kind = 0;
  //! Most bytes that may be buffered here or on their way
  uint32_t window = 0;
  //! Received content that was not used yet
  std::deque<std::string> chunks;
  //! Bytes of the first chunk that were used already
  size_t chunkOffset = 0;
  uint64_t buffered = 0;
  //! Bytes used since room was last granted to the other computer
  uint32_t unacknowledged = 0;
  //! Counts messages, to tell a slow answer from no answer
  uint64_t activity = 0;
  //! No more messages will arrive
  bool finished = false;
  bool cancelled = false;
  FileTransferStatus status = FileTransferStatus::Ok;
};

struct FileTransferShared
{
  std::mutex mutex;
  //! Cleared when the FileTransfer is destroyed
  FileTransfer *owner = nullptr;
  IEventQueue *events = nullptr;
  uint32_t nextRequestId = 1;
  std::map<uint32_t, std::shared_ptr<FileTransferRequest>> requests;
  int replyTimeoutMs = 30000;

  //! Run \p task on the thread of the event queue, returns false if the owner is gone
  bool post(std::function<void(FileTransfer &)> task)
  {
    std::scoped_lock lock{mutex};
    if (owner == nullptr) {
      return false;
    }
    FileTransfer *target = owner;
    events->addEvent(Event(EventTypes::FileTransferTask, target, new FileTransferTask([target, task = std::move(task)] {
                             task(*target);
                           })));
    return true;
  }

  std::shared_ptr<FileTransferRequest> newRequest(uint8_t kind, uint32_t window)
  {
    auto request = std::make_shared<FileTransferRequest>();
    request->kind = kind;
    request->window = window;

    std::scoped_lock lock{mutex};
    if (owner == nullptr) {
      return nullptr;
    }
    request->id = nextRequestId++;
    if (nextRequestId == 0) {
      nextRequestId = 1;
    }
    requests[request->id] = request;
    return request;
  }

  int replyTimeout()
  {
    std::scoped_lock lock{mutex};
    return replyTimeoutMs;
  }

  //! Stop waiting for a request and tell the other computer, safe to call more than once
  void abandon(const std::shared_ptr<FileTransferRequest> &request, FileTransferStatus status)
  {
    bool wasActive = false;
    {
      std::scoped_lock lock{request->mutex};
      wasActive = !request->finished && !request->cancelled;
      if (!request->finished) {
        request->finished = true;
        request->status = status;
      }
      request->cancelled = true;
      request->chunks.clear();
      request->chunkOffset = 0;
      request->buffered = 0;
    }
    request->changed.notify_all();

    {
      std::scoped_lock lock{mutex};
      requests.erase(request->id);
    }
    if (wasActive) {
      const uint32_t id = request->id;
      post([id](FileTransfer &owner) { owner.sendCancel(id); });
    }
  }
};

//
// RemoteFileReader
//

RemoteFileReader::RemoteFileReader(
    std::shared_ptr<FileTransferShared> shared, std::shared_ptr<FileTransferRequest> request
)
    : m_shared(std::move(shared)),
      m_request(std::move(request))
{
}

RemoteFileReader::~RemoteFileReader()
{
  cancel();
}

void RemoteFileReader::cancel()
{
  if (m_request != nullptr) {
    m_shared->abandon(m_request, FileTransferStatus::Cancelled);
  }
}

FileTransferStatus RemoteFileReader::read(void *buffer, uint32_t size, uint32_t &bytesRead)
{
  bytesRead = 0;
  if (m_request == nullptr) {
    return FileTransferStatus::Disconnected;
  }
  if (size == 0) {
    return FileTransferStatus::Ok;
  }

  const auto timeout = std::chrono::milliseconds(m_shared->replyTimeout());
  const uint32_t id = m_request->id;
  FileTransferStatus status = FileTransferStatus::Ok;
  uint32_t credit = 0;
  bool timedOut = false;

  {
    std::unique_lock lock{m_request->mutex};

    // about to wait for data: hand back every byte of room that was used
    if (m_request->buffered == 0 && !m_request->finished && m_request->unacknowledged > 0) {
      const uint32_t early = m_request->unacknowledged;
      m_request->unacknowledged = 0;
      lock.unlock();
      m_shared->post([id, early](FileTransfer &owner) { owner.sendCredit(id, early); });
      lock.lock();
    }

    while (m_request->buffered == 0 && !m_request->finished && !m_request->cancelled) {
      const uint64_t before = m_request->activity;
      const bool expired = m_request->changed.wait_for(lock, timeout) == std::cv_status::timeout;
      if (expired && m_request->buffered == 0 && !m_request->finished && !m_request->cancelled &&
          m_request->activity == before) {
        timedOut = true;
        break;
      }
    }

    if (!timedOut) {
      if (m_request->buffered > 0) {
        auto *target = static_cast<char *>(buffer);
        while (bytesRead < size && !m_request->chunks.empty()) {
          const std::string &chunk = m_request->chunks.front();
          const size_t available = chunk.size() - m_request->chunkOffset;
          const size_t take = std::min<size_t>(available, size - bytesRead);
          std::memcpy(target + bytesRead, chunk.data() + m_request->chunkOffset, take);
          bytesRead += static_cast<uint32_t>(take);
          m_request->chunkOffset += take;
          if (m_request->chunkOffset == chunk.size()) {
            m_request->chunks.pop_front();
            m_request->chunkOffset = 0;
          }
        }
        m_request->buffered -= bytesRead;
        m_request->unacknowledged += bytesRead;
        if (!m_request->finished && m_request->unacknowledged >= m_request->window / 2) {
          credit = m_request->unacknowledged;
          m_request->unacknowledged = 0;
        }
      } else {
        // finished and everything was handed out: the end of the file, or why the transfer stopped
        status = m_request->status;
      }
    }
  }

  if (timedOut) {
    LOG_WARN("the other computer stopped sending a file");
    m_shared->abandon(m_request, FileTransferStatus::TimedOut);
    return FileTransferStatus::TimedOut;
  }
  if (credit > 0) {
    m_shared->post([id, credit](FileTransfer &owner) { owner.sendCredit(id, credit); });
  }
  return status;
}

//
// RemoteFiles
//

RemoteFiles::RemoteFiles(std::shared_ptr<FileTransferShared> shared) : m_shared(std::move(shared))
{
}

FileTransferStatus RemoteFiles::listDirectory(
    uint64_t offerId, uint32_t item, const std::string &relativePath, std::vector<ClipboardFileEntry> &entries
)
{
  entries.clear();
  if (relativePath.size() > FileTransfer::Protocol::MaxPathLength) {
    return FileTransferStatus::NotFound;
  }

  auto request = m_shared->newRequest(FileTransfer::Protocol::RequestList, 0);
  if (request == nullptr) {
    return FileTransferStatus::Disconnected;
  }

  const std::string arguments = FileTransfer::encodeRequest(offerId, item, relativePath, 0, 0);
  const bool posted = m_shared->post([request, offerId, arguments](FileTransfer &owner) {
    owner.startRequest(request, offerId, FileTransfer::Protocol::RequestList, arguments);
  });
  if (!posted) {
    m_shared->abandon(request, FileTransferStatus::Disconnected);
    return FileTransferStatus::Disconnected;
  }

  const auto timeout = std::chrono::milliseconds(m_shared->replyTimeout());
  FileTransferStatus status = FileTransferStatus::Ok;
  bool timedOut = false;
  std::string content;
  {
    std::unique_lock lock{request->mutex};
    while (!request->finished) {
      const uint64_t before = request->activity;
      const bool expired = request->changed.wait_for(lock, timeout) == std::cv_status::timeout;
      if (expired && !request->finished && request->activity == before) {
        timedOut = true;
        break;
      }
    }
    if (!timedOut) {
      status = request->status;
      for (const auto &chunk : request->chunks) {
        content.append(chunk);
      }
    }
  }

  if (timedOut) {
    LOG_WARN("the other computer did not answer a request to list files");
    m_shared->abandon(request, FileTransferStatus::TimedOut);
    return FileTransferStatus::TimedOut;
  }
  if (status != FileTransferStatus::Ok) {
    return status;
  }
  if (!ClipboardFileList::readEntries(content, entries)) {
    LOG_WARN("the other computer sent a malformed list of files");
    return FileTransferStatus::Failed;
  }
  return FileTransferStatus::Ok;
}

std::unique_ptr<RemoteFileReader> RemoteFiles::openFile(
    uint64_t offerId, uint32_t item, const std::string &relativePath, uint64_t offset, uint32_t window
)
{
  if (window == 0) {
    window = FileTransfer::Protocol::DefaultWindow;
  }
  window = std::clamp(window, FileTransfer::Protocol::ChunkSize, FileTransfer::Protocol::MaxWindow);

  std::shared_ptr<FileTransferRequest> request;
  if (relativePath.size() <= FileTransfer::Protocol::MaxPathLength) {
    request = m_shared->newRequest(FileTransfer::Protocol::RequestRead, window);
  }
  if (request != nullptr) {
    const std::string arguments = FileTransfer::encodeRequest(offerId, item, relativePath, offset, window);
    const bool posted = m_shared->post([request, offerId, arguments](FileTransfer &owner) {
      owner.startRequest(request, offerId, FileTransfer::Protocol::RequestRead, arguments);
    });
    if (!posted) {
      m_shared->abandon(request, FileTransferStatus::Disconnected);
    }
  }
  return std::unique_ptr<RemoteFileReader>(new RemoteFileReader(m_shared, request));
}

//
// reading local files
//

//! The open file or directory of a request that is being answered
/*!
Only touched by the worker thread. Opening and reading can block for a long
time, for example on a disk that has to spin up, on a network volume or while
the system asks the user for permission, and none of that may hold up the
thread that runs the event queue.
*/
struct FileTransfer::ServedIo
{
  QFile file;
  std::unique_ptr<QDirIterator> listing;
};

//! What the worker thread hands back for one step of a request
struct FileTransfer::IoResult
{
  bool ok = true;
  Error error = Error::ReadFailed;
  const char *message = "";
  //! File data or encoded directory entries
  std::string content;
  //! Nothing is left to read or list
  bool end = false;
  //! Bytes asked for but not delivered, they are available to the reader again
  uint64_t unused = 0;
};

//! Runs one job at a time on a thread of its own
class FileTransfer::Worker
{
public:
  Worker() : m_thread([this] { run(); })
  {
  }

  ~Worker()
  {
    {
      std::scoped_lock lock{m_mutex};
      m_stop = true;
    }
    m_changed.notify_all();
    m_thread.join();
  }

  void add(std::function<void()> job)
  {
    {
      std::scoped_lock lock{m_mutex};
      m_jobs.push_back(std::move(job));
    }
    m_changed.notify_all();
  }

private:
  void run()
  {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock{m_mutex};
        m_changed.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
        if (m_stop) {
          return;
        }
        job = std::move(m_jobs.front());
        m_jobs.pop_front();
      }
      job();
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::deque<std::function<void()>> m_jobs;
  bool m_stop = false;
  // last, so that everything it uses exists before it starts
  std::thread m_thread;
};

//
// FileTransfer
//

FileTransfer::FileTransfer(IEventQueue *events) : m_events(events), m_shared(std::make_shared<FileTransferShared>())
{
  assert(m_events != nullptr);
  m_shared->owner = this;
  m_shared->events = m_events;
  m_remoteFiles = std::shared_ptr<RemoteFiles>(new RemoteFiles(m_shared));
  m_events->addHandler(EventTypes::FileTransferTask, this, [this](const Event &event) { handleTask(event); });
}

FileTransfer::~FileTransfer()
{
  // first of all, its jobs use this object
  m_worker.reset();
  m_events->removeHandler(EventTypes::FileTransferTask, this);

  // wake every thread that waits for an answer, none will come
  std::map<uint32_t, std::shared_ptr<FileTransferRequest>> requests;
  {
    std::scoped_lock lock{m_shared->mutex};
    m_shared->owner = nullptr;
    m_shared->events = nullptr;
    requests.swap(m_shared->requests);
  }
  for (const auto &entry : requests) {
    const auto &request = entry.second;
    {
      std::scoped_lock lock{request->mutex};
      if (!request->finished) {
        request->finished = true;
        request->status = FileTransferStatus::Disconnected;
      }
    }
    request->changed.notify_all();
  }
}

void FileTransfer::handleTask(const Event &event)
{
  const auto *task = static_cast<const FileTransferTask *>(event.getDataObject());
  if (task != nullptr) {
    task->run();
  }
}

void FileTransfer::setEnabled(bool enabled)
{
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  LOG_DEBUG("sharing files through the clipboard is %s", enabled ? "on" : "off");
  if (enabled) {
    return;
  }

  // stop everything that is in progress, in both directions
  {
    std::scoped_lock lock{m_registryMutex};
    m_registry.withdraw();
  }
  for (const auto &entry : m_served) {
    sendError(entry.first.first, entry.first.second, Error::Disabled, "file sharing was turned off");
  }
  m_served.clear();
  m_unflushed.clear();
  updateServedCount();
  m_remoteOffers.clear();

  std::vector<uint32_t> ids;
  for (const auto &entry : m_requestChannels) {
    ids.push_back(entry.first);
  }
  for (const uint32_t id : ids) {
    sendCancel(id);
    finishRequest(id, FileTransferStatus::Refused);
  }
}

bool FileTransfer::isEnabled() const
{
  return m_enabled;
}

std::shared_ptr<RemoteFiles> FileTransfer::remoteFiles() const
{
  return m_remoteFiles;
}

size_t FileTransfer::servedRequestCount() const
{
  return m_servedCount.load();
}

void FileTransfer::updateServedCount()
{
  m_servedCount.store(m_served.size());
}

void FileTransfer::setReplyTimeout(int milliseconds)
{
  std::scoped_lock lock{m_shared->mutex};
  m_shared->replyTimeoutMs = milliseconds;
}

//
// files copied on this computer
//

std::string FileTransfer::offerLocalFiles(const std::vector<std::string> &paths)
{
  std::scoped_lock lock{m_registryMutex};
  if (!m_enabled) {
    m_registry.withdraw();
    return {};
  }

  const ClipboardFileList list = m_registry.offer(paths);
  if (list.entries.empty()) {
    return {};
  }
  LOG_DEBUG("offering %d copied files to other computers", static_cast<int>(list.entries.size()));
  return list.marshall();
}

void FileTransfer::withdrawLocalFiles()
{
  std::scoped_lock lock{m_registryMutex};
  m_registry.withdraw();
}

bool FileTransfer::isLocalOffer(std::string_view fileList)
{
  ClipboardFileList list;
  if (!ClipboardFileList::unmarshall(fileList, list)) {
    return false;
  }
  std::scoped_lock lock{m_registryMutex};
  return m_registry.isOffered(list.offerId);
}

void FileTransfer::sendError(IFileTransferChannel *channel, uint32_t requestId, Error error, const char *message)
{
  LOG_DEBUG("refusing file request %u: %s", requestId, message);
  std::string content;
  wire::putU32(content, static_cast<uint32_t>(error));
  content.append(message);
  channel->sendFileResponse(requestId, Protocol::MarkError, content);
}

std::string FileTransfer::encodeRequest(
    uint64_t offerId, uint32_t item, std::string_view relativePath, uint64_t offset, uint32_t credit
)
{
  std::string arguments;
  wire::putU64(arguments, offerId);
  wire::putU32(arguments, item);
  wire::putU16(arguments, static_cast<uint16_t>(relativePath.size()));
  arguments.append(relativePath);
  wire::putU64(arguments, offset);
  wire::putU32(arguments, credit);
  return arguments;
}

void FileTransfer::handleRequest(
    IFileTransferChannel *channel, uint32_t requestId, uint8_t kind, const std::string &arguments
)
{
  if (!m_enabled) {
    sendError(channel, requestId, Error::Disabled, "file sharing is turned off");
    return;
  }

  std::string_view data = arguments;
  uint64_t offerId = 0;
  uint32_t item = 0;
  uint16_t pathLength = 0;
  std::string_view pathView;
  uint64_t offset = 0;
  uint32_t credit = 0;
  if (!wire::getU64(data, offerId) || !wire::getU32(data, item) || !wire::getU16(data, pathLength) ||
      !wire::getBytes(data, pathLength, pathView) || !wire::getU64(data, offset) || !wire::getU32(data, credit) ||
      !data.empty()) {
    sendError(channel, requestId, Error::BadPath, "malformed request");
    return;
  }
  if (kind != Protocol::RequestList && kind != Protocol::RequestRead) {
    sendError(channel, requestId, Error::BadPath, "unknown kind of request");
    return;
  }

  const ServedKey key{channel, requestId};
  // a reused identifier replaces the request it was used for
  m_served.erase(key);
  const auto servedForChannel = std::count_if(m_served.begin(), m_served.end(), [channel](const auto &entry) {
    return entry.first.first == channel;
  });
  if (static_cast<size_t>(servedForChannel) >= kMaxServedPerChannel) {
    updateServedCount();
    sendError(channel, requestId, Error::Busy, "too many requests at once");
    return;
  }

  Served served;
  served.kind = kind;
  served.serial = ++m_nextSerial;
  served.io = std::make_shared<ServedIo>();
  served.credit = (kind == Protocol::RequestRead) ? std::min<uint32_t>(credit, Protocol::MaxWindow) : 0;
  served.busy = true;
  const auto io = served.io;
  const uint64_t serial = served.serial;
  m_served.insert_or_assign(key, std::move(served));
  updateServedCount();

  // finding and opening the file is the first thing that may block
  const std::string relativePath(pathView);
  runJob(key, serial, [this, io, kind, offerId, item, relativePath, offset] {
    IoResult result;
    QString localPath;
    FileOfferRegistry::Resolved resolved;
    {
      std::scoped_lock lock{m_registryMutex};
      resolved = m_registry.resolve(offerId, item, relativePath, localPath);
    }
    switch (resolved) {
      using enum FileOfferRegistry::Resolved;
    case Ok:
      break;
    case UnknownOffer:
      return IoResult{false, Error::UnknownOffer, "these files are no longer offered"};
    case BadPath:
      return IoResult{false, Error::BadPath, "not one of the offered files"};
    case NotFound:
      return IoResult{false, Error::NotFound, "no such file or directory"};
    }

    const QFileInfo info(localPath);
    if (kind == Protocol::RequestList) {
      if (!info.isDir()) {
        return IoResult{false, Error::WrongType, "not a directory"};
      }
      io->listing = std::make_unique<QDirIterator>(
          localPath, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot
      );
      return result;
    }

    if (!info.isFile()) {
      return IoResult{false, Error::WrongType, "not a file"};
    }
    io->file.setFileName(localPath);
    if (!io->file.open(QIODevice::ReadOnly | QIODevice::Unbuffered) ||
        (offset > 0 && !io->file.seek(static_cast<qint64>(offset)))) {
      return IoResult{false, Error::ReadFailed, "can not open the file"};
    }
    result.end = io->file.atEnd();
    return result;
  });
}

void FileTransfer::runJob(const ServedKey &key, uint64_t serial, std::function<IoResult()> job)
{
  if (m_worker == nullptr) {
    m_worker = std::make_unique<Worker>();
  }
  m_worker->add([shared = m_shared, key, serial, job = std::move(job)] {
    auto result = std::make_shared<IoResult>(job());
    shared->post([key, serial, result](FileTransfer &owner) { owner.jobFinished(key, serial, *result); });
  });
}

void FileTransfer::jobFinished(const ServedKey &key, uint64_t serial, IoResult &result)
{
  const auto found = m_served.find(key);
  if (found == m_served.end() || found->second.serial != serial) {
    // cancelled, or the connection is gone
    return;
  }

  Served &served = found->second;
  served.busy = false;
  if (!result.ok) {
    sendError(key.first, key.second, result.error, result.message);
    m_served.erase(found);
    updateServedCount();
    return;
  }

  served.opened = true;
  served.credit += result.unused;
  if (!result.content.empty()) {
    served.readyBytes += result.content.size();
    served.ready.push_back(std::move(result.content));
  }
  if (result.end) {
    served.finished = true;
  }
  pump(key.first);
}

void FileTransfer::handleCredit(IFileTransferChannel *channel, uint32_t requestId, uint32_t bytes)
{
  const auto found = m_served.find(ServedKey{channel, requestId});
  if (found == m_served.end() || found->second.kind != Protocol::RequestRead) {
    return;
  }
  // never let the other computer build up more room than a reader may ask for
  found->second.credit = std::min<uint64_t>(found->second.credit + bytes, Protocol::MaxWindow);
  pump(channel);
}

void FileTransfer::handleCancel(IFileTransferChannel *channel, uint32_t requestId)
{
  if (m_served.erase(ServedKey{channel, requestId}) > 0) {
    LOG_DEBUG("file request %u was cancelled by the other computer", requestId);
    updateServedCount();
  }
}

void FileTransfer::channelFlushed(IFileTransferChannel *channel)
{
  if (m_unflushed.erase(channel) > 0 || !m_served.empty()) {
    pump(channel);
  }
}

void FileTransfer::pump(IFileTransferChannel *channel)
{
  // collect first, answering a request may finish it
  std::vector<ServedKey> keys;
  for (const auto &entry : m_served) {
    if (entry.first.first == channel) {
      keys.push_back(entry.first);
    }
  }

  for (const auto &key : keys) {
    const auto found = m_served.find(key);
    if (found != m_served.end() && !pumpServed(key, found->second)) {
      m_served.erase(key);
    }
  }
  updateServedCount();
}

bool FileTransfer::pumpServed(const ServedKey &key, Served &served)
{
  if (!served.opened) {
    return true;
  }
  IFileTransferChannel *channel = key.first;

  // send what the worker has read, as long as the connection keeps up
  uint64_t &unflushed = m_unflushed[channel];
  while (!served.ready.empty() && unflushed < kMaxUnflushedBytes) {
    const std::string &content = served.ready.front();
    channel->sendFileResponse(key.second, Protocol::MarkData, content);
    unflushed += content.size();
    served.sent += content.size();
    served.readyBytes -= content.size();
    served.ready.pop_front();
  }

  if (served.ready.empty() && served.finished) {
    std::string content;
    if (served.kind == Protocol::RequestRead) {
      wire::putU64(content, served.sent);
    }
    channel->sendFileResponse(key.second, Protocol::MarkEnd, content);
    return false;
  }
  if (served.busy || served.finished || served.readyBytes >= kMaxReadAheadBytes) {
    return true;
  }

  const auto io = served.io;
  if (served.kind == Protocol::RequestList) {
    served.busy = true;
    runJob(key, served.serial, [io] {
      IoResult result;
      int count = 0;
      while (io->listing->hasNext() && count < kEntriesPerMessage && result.content.size() < kListingBytesPerMessage) {
        const QFileInfo info = io->listing->nextFileInfo();
        if (isLink(info) || (!info.isFile() && !info.isDir())) {
          continue;
        }

        ClipboardFileEntry entry;
        entry.name = info.fileName().toStdString();
        if (!ClipboardFileList::isValidName(entry.name)) {
          continue;
        }
#if defined(Q_OS_MACOS)
        // how Finder shows a folder, of no use anywhere else
        if (entry.name == ".DS_Store") {
          continue;
        }
#endif
        entry.isDirectory = info.isDir();
        entry.size = entry.isDirectory ? 0 : static_cast<uint64_t>(info.size());
        entry.modifiedTime = info.lastModified().toSecsSinceEpoch();
        ClipboardFileList::appendEntry(result.content, entry);
        ++count;
      }
      result.end = !io->listing->hasNext();
      return result;
    });
    return true;
  }

  // read no more than the other computer has room for, the room is taken now and given back if it is not used
  if (served.credit == 0) {
    return true;
  }
  const uint64_t wanted = std::min<uint64_t>(Protocol::ChunkSize, served.credit);
  served.credit -= wanted;
  served.busy = true;
  runJob(key, served.serial, [io, wanted] {
    IoResult result;
    result.content.resize(static_cast<size_t>(wanted));
    const qint64 got = io->file.read(result.content.data(), static_cast<qint64>(wanted));
    if (got < 0) {
      return IoResult{false, Error::ReadFailed, "failed to read the file"};
    }
    result.content.resize(static_cast<size_t>(got));
    result.unused = wanted - static_cast<uint64_t>(got);
    result.end = (got == 0) || io->file.atEnd();
    return result;
  });
  return true;
}

//
// files copied on another computer
//

bool FileTransfer::acceptRemoteFiles(std::string_view fileList, IFileTransferChannel *channel)
{
  ClipboardFileList list;
  if (!ClipboardFileList::unmarshall(fileList, list)) {
    LOG_WARN("ignoring a malformed list of copied files");
    return false;
  }
  if (!m_enabled) {
    return false;
  }

  std::erase_if(m_remoteOffers, [&list](const auto &offer) { return offer.first == list.offerId; });
  m_remoteOffers.emplace_back(list.offerId, channel);
  if (m_remoteOffers.size() > kMaxRemoteOffers) {
    m_remoteOffers.erase(m_remoteOffers.begin());
  }
  LOG_DEBUG("another computer offers %d copied files", static_cast<int>(list.entries.size()));
  return true;
}

void FileTransfer::startRequest(
    const std::shared_ptr<FileTransferRequest> &request, uint64_t offerId, uint8_t kind, const std::string &arguments
)
{
  {
    std::scoped_lock lock{request->mutex};
    if (request->finished || request->cancelled) {
      return;
    }
  }

  if (!m_enabled) {
    finishRequest(request->id, FileTransferStatus::Refused);
    return;
  }

  const auto found = std::find_if(m_remoteOffers.begin(), m_remoteOffers.end(), [offerId](const auto &offer) {
    return offer.first == offerId;
  });
  if (found == m_remoteOffers.end()) {
    finishRequest(request->id, FileTransferStatus::Disconnected);
    return;
  }

  m_requestChannels[request->id] = found->second;
  found->second->sendFileRequest(request->id, kind, arguments);
}

void FileTransfer::sendCredit(uint32_t requestId, uint32_t bytes)
{
  const auto found = m_requestChannels.find(requestId);
  if (found != m_requestChannels.end()) {
    found->second->sendFileCredit(requestId, bytes);
  }
}

void FileTransfer::sendCancel(uint32_t requestId)
{
  const auto found = m_requestChannels.find(requestId);
  if (found != m_requestChannels.end()) {
    found->second->sendFileCancel(requestId);
    m_requestChannels.erase(found);
  }
}

void FileTransfer::finishRequest(uint32_t requestId, FileTransferStatus status)
{
  m_requestChannels.erase(requestId);

  std::shared_ptr<FileTransferRequest> request;
  {
    std::scoped_lock lock{m_shared->mutex};
    const auto found = m_shared->requests.find(requestId);
    if (found == m_shared->requests.end()) {
      return;
    }
    request = found->second;
    m_shared->requests.erase(found);
  }

  {
    std::scoped_lock lock{request->mutex};
    if (!request->finished) {
      request->finished = true;
      request->status = status;
    }
    ++request->activity;
  }
  request->changed.notify_all();
}

void FileTransfer::handleResponse(IFileTransferChannel *channel, uint32_t requestId, uint8_t mark, std::string &content)
{
  // only the computer that was asked may answer
  const auto found = m_requestChannels.find(requestId);
  if (found == m_requestChannels.end() || found->second != channel) {
    return;
  }

  std::shared_ptr<FileTransferRequest> request;
  {
    std::scoped_lock lock{m_shared->mutex};
    const auto pending = m_shared->requests.find(requestId);
    if (pending != m_shared->requests.end()) {
      request = pending->second;
    }
  }
  if (request == nullptr) {
    // given up by its reader. the channel stays known until the cancel that is on its way here was sent
    return;
  }

  if (mark == Protocol::MarkData) {
    bool tooMuch = false;
    {
      std::scoped_lock lock{request->mutex};
      if (request->cancelled || request->finished) {
        return;
      }
      const uint64_t limit = (request->kind == Protocol::RequestRead) ? request->window : kMaxListingBytes;
      if (request->buffered + content.size() > limit) {
        tooMuch = true;
      } else {
        request->buffered += content.size();
        request->chunks.push_back(std::move(content));
        ++request->activity;
      }
    }
    if (tooMuch) {
      LOG_WARN("the other computer sent more file data than it was given room for");
      sendCancel(requestId);
      finishRequest(requestId, FileTransferStatus::Failed);
      return;
    }
    request->changed.notify_all();
  } else if (mark == Protocol::MarkEnd) {
    finishRequest(requestId, FileTransferStatus::Ok);
  } else if (mark == Protocol::MarkError) {
    std::string_view data = content;
    uint32_t code = 0;
    if (!wire::getU32(data, code)) {
      code = static_cast<uint32_t>(Error::ReadFailed);
    }
    LOG_DEBUG("the other computer refused file request %u, error %u: %s", requestId, code, std::string(data).c_str());
    finishRequest(requestId, statusFromError(code));
  } else {
    LOG_WARN("the other computer sent an unknown kind of file answer");
    sendCancel(requestId);
    finishRequest(requestId, FileTransferStatus::Failed);
  }
}

void FileTransfer::channelClosed(IFileTransferChannel *channel)
{
  std::erase_if(m_served, [channel](const auto &entry) { return entry.first.first == channel; });
  m_unflushed.erase(channel);
  updateServedCount();
  std::erase_if(m_remoteOffers, [channel](const auto &offer) { return offer.second == channel; });

  std::vector<uint32_t> ids;
  for (const auto &[id, requestChannel] : m_requestChannels) {
    if (requestChannel == channel) {
      ids.push_back(id);
    }
  }
  for (const uint32_t id : ids) {
    finishRequest(id, FileTransferStatus::Disconnected);
  }
}

} // namespace deskflow
