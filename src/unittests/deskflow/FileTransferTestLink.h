/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

// Two FileTransfer objects connected to each other inside one process, and
// helpers to make and compare files. Shared by the tests of the transfer
// itself and of the platform code that pastes with it.

#include "base/Event.h"
#include "base/EventQueue.h"
#include "deskflow/FileTransfer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace deskflow::test {

using Protocol = deskflow::FileTransfer::Protocol;

inline const qint64 kMiB = 1024 * 1024;

class TestTask : public EventData
{
public:
  explicit TestTask(std::function<void()> task) : m_task(std::move(task))
  {
  }
  std::function<void()> m_task;
};

//! A real event queue running on its own thread, like the one that drives the core
class EventLoop
{
public:
  EventLoop()
  {
    m_events.addHandler(EventTypes::FileTransferTask, this, [](const Event &event) {
      static_cast<const TestTask *>(event.getDataObject())->m_task();
    });
    m_thread = std::thread([this] { m_events.loop(); });
    m_events.waitForReady();
  }

  ~EventLoop()
  {
    m_events.addEvent(Event(EventTypes::Quit));
    m_thread.join();
    m_events.removeHandler(EventTypes::FileTransferTask, this);
  }

  EventQueue &events()
  {
    return m_events;
  }

  //! Run \p task on the event thread, later
  void post(std::function<void()> task)
  {
    m_events.addEvent(Event(EventTypes::FileTransferTask, this, new TestTask(std::move(task))));
  }

  //! Run \p task on the event thread and wait for its result
  template <typename Task> auto call(Task task) -> decltype(task())
  {
    using Result = decltype(task());
    std::promise<Result> promise;
    auto future = promise.get_future();
    post([&promise, &task] {
      if constexpr (std::is_void_v<Result>) {
        task();
        promise.set_value();
      } else {
        promise.set_value(task());
      }
    });
    return future.get();
  }

private:
  EventQueue m_events;
  std::thread m_thread;
};

//! Records what one computer asks of the other and what it gets back
struct Monitor
{
  void onRequest(uint32_t id, uint8_t kind, const std::string &arguments)
  {
    std::scoped_lock lock{mutex};
    ++requests;
    lastRequestId = id;
    if (kind == Protocol::RequestRead && arguments.size() >= 4) {
      std::string_view tail = std::string_view(arguments).substr(arguments.size() - 4);
      uint32_t credit = 0;
      deskflow::wire::getU32(tail, credit);
      allowed[id] = credit;
      sent[id] = 0;
    }
  }

  void onCredit(uint32_t id, uint32_t bytes)
  {
    std::scoped_lock lock{mutex};
    ++credits;
    allowed[id] += bytes;
  }

  void onData(uint32_t id, const std::string &content)
  {
    std::scoped_lock lock{mutex};
    dataBytes += content.size();
    if (!needle.empty() && content.find(needle) != std::string::npos) {
      needleSeen = true;
    }
    const auto found = sent.find(id);
    if (found == sent.end()) {
      return;
    }
    found->second += content.size();
    if (found->second > allowed[id]) {
      windowExceeded = true;
    }
    if (content.size() > Protocol::ChunkSize) {
      chunkTooBig = true;
    }
  }

  void onError(uint32_t id, const std::string &content)
  {
    std::scoped_lock lock{mutex};
    std::string_view data = content;
    uint32_t code = 0;
    deskflow::wire::getU32(data, code);
    errors[id] = code;
  }

  uint64_t totalData()
  {
    std::scoped_lock lock{mutex};
    return dataBytes;
  }

  uint32_t errorFor(uint32_t id)
  {
    std::scoped_lock lock{mutex};
    const auto found = errors.find(id);
    return (found == errors.end()) ? 0 : found->second;
  }

  std::mutex mutex;
  uint64_t requests = 0;
  uint64_t credits = 0;
  uint32_t lastRequestId = 0;
  uint64_t dataBytes = 0;
  std::map<uint32_t, uint64_t> allowed;
  std::map<uint32_t, uint64_t> sent;
  std::map<uint32_t, uint32_t> errors;
  bool windowExceeded = false;
  bool chunkTooBig = false;
  //! Bytes that must never be sent
  std::string needle;
  bool needleSeen = false;
};

//! One end of a connection between two FileTransfer objects on the same event queue
/*!
A message is delivered to the other end by a later event, and each send is
followed by the event a real connection posts when its output has drained.
*/
class LoopbackChannel : public IFileTransferChannel
{
public:
  explicit LoopbackChannel(EventLoop &loop) : m_loop(loop)
  {
  }

  void attach(FileTransfer *local, FileTransfer *peer, LoopbackChannel *peerChannel, Monitor *asked, Monitor *answered)
  {
    m_local = local;
    m_peer = peer;
    m_peerChannel = peerChannel;
    m_asked = asked;
    m_answered = answered;
  }

  //! Messages vanish from now on, in both directions
  void cut()
  {
    m_cut = true;
  }

  void sendFileRequest(uint32_t requestId, uint8_t kind, const std::string &arguments) override
  {
    m_asked->onRequest(requestId, kind, arguments);
    deliver([this, requestId, kind, arguments] { m_peer->handleRequest(m_peerChannel, requestId, kind, arguments); });
  }

  void sendFileResponse(uint32_t requestId, uint8_t mark, const std::string &content) override
  {
    if (mark == Protocol::MarkData) {
      m_answered->onData(requestId, content);
    } else if (mark == Protocol::MarkError) {
      m_answered->onError(requestId, content);
    }
    deliver([this, requestId, mark, content] {
      std::string received = content;
      m_peer->handleResponse(m_peerChannel, requestId, mark, received);
    });
  }

  void sendFileCredit(uint32_t requestId, uint32_t bytes) override
  {
    m_asked->onCredit(requestId, bytes);
    deliver([this, requestId, bytes] { m_peer->handleCredit(m_peerChannel, requestId, bytes); });
  }

  void sendFileCancel(uint32_t requestId) override
  {
    ++cancels;
    deliver([this, requestId] { m_peer->handleCancel(m_peerChannel, requestId); });
  }

  std::atomic<int> cancels = 0;

private:
  void deliver(std::function<void()> message)
  {
    if (m_cut || m_peerChannel->m_cut) {
      return;
    }
    m_loop.post([this, message = std::move(message)] {
      if (!m_cut && !m_peerChannel->m_cut) {
        message();
      }
    });
    m_loop.post([this] {
      if (!m_cut) {
        m_local->channelFlushed(this);
      }
    });
  }

  EventLoop &m_loop;
  FileTransfer *m_local = nullptr;
  FileTransfer *m_peer = nullptr;
  LoopbackChannel *m_peerChannel = nullptr;
  Monitor *m_asked = nullptr;
  Monitor *m_answered = nullptr;
  std::atomic<bool> m_cut = false;
};

//! Two computers, A and B, connected to each other
struct Link
{
  Link() : aToB(loop), bToA(loop)
  {
    loop.call([this] {
      a = std::make_unique<FileTransfer>(&loop.events());
      b = std::make_unique<FileTransfer>(&loop.events());
      aToB.attach(a.get(), b.get(), &bToA, &askedByA, &askedByB);
      bToA.attach(b.get(), a.get(), &aToB, &askedByB, &askedByA);
    });
    remoteOfA = a->remoteFiles();
    remoteOfB = b->remoteFiles();
  }

  ~Link()
  {
    loop.call([this] {
      aToB.cut();
      bToA.cut();
      if (a != nullptr) {
        a->channelClosed(&aToB);
      }
      if (b != nullptr) {
        b->channelClosed(&bToA);
      }
      a.reset();
      b.reset();
    });
  }

  //! A copies \p paths and B learns about it, like when the clipboard is sent over
  ClipboardFileList copyOnA(const std::vector<std::string> &paths)
  {
    ClipboardFileList list;
    const std::string fileList = loop.call([&] { return a->offerLocalFiles(paths); });
    if (!ClipboardFileList::unmarshall(fileList, list)) {
      return list;
    }
    const bool accepted = loop.call([&] { return b->acceptRemoteFiles(fileList, &bToA); });
    return accepted ? list : ClipboardFileList{};
  }

  ClipboardFileList copyOnB(const std::vector<std::string> &paths)
  {
    ClipboardFileList list;
    const std::string fileList = loop.call([&] { return b->offerLocalFiles(paths); });
    if (!ClipboardFileList::unmarshall(fileList, list)) {
      return list;
    }
    const bool accepted = loop.call([&] { return a->acceptRemoteFiles(fileList, &aToB); });
    return accepted ? list : ClipboardFileList{};
  }

  //! Wait until A answers no more requests, false if that takes too long
  bool waitUntilAIsIdle(int timeoutMs = 3000)
  {
    QElapsedTimer timer;
    timer.start();
    while (a->servedRequestCount() != 0) {
      if (timer.elapsed() > timeoutMs) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  }

  EventLoop loop;
  std::unique_ptr<FileTransfer> a;
  std::unique_ptr<FileTransfer> b;
  LoopbackChannel aToB;
  LoopbackChannel bToA;
  //! What A asks of B, and what B asks of A
  Monitor askedByA;
  Monitor askedByB;
  //! How A reads the files of B, and how B reads the files of A
  std::shared_ptr<RemoteFiles> remoteOfA;
  std::shared_ptr<RemoteFiles> remoteOfB;
};

inline bool writeFile(const QString &path, const QByteArray &data)
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

//! Content that does not compress or repeat, so that any mix up of blocks shows
inline QByteArray patternBlock(quint64 seed, qint64 size)
{
  std::mt19937_64 generator(seed);
  QByteArray block((size + 7) / 8 * 8, Qt::Uninitialized);
  auto *words = reinterpret_cast<quint64 *>(block.data());
  for (qint64 i = 0; i < block.size() / 8; ++i) {
    words[i] = generator();
  }
  block.truncate(size);
  return block;
}

inline bool writePattern(const QString &path, qint64 size, quint64 seed)
{
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  for (qint64 offset = 0; offset < size; offset += kMiB) {
    const QByteArray block =
        patternBlock(seed + static_cast<quint64>(offset / kMiB), std::min<qint64>(kMiB, size - offset));
    if (file.write(block) != block.size()) {
      return false;
    }
  }
  return true;
}

inline QByteArray hashOfFile(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return "unreadable";
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(&file);
  return hash.result().toHex();
}

//! Describe a file or a whole directory as "relative path -> kind and SHA-256", links left out
inline void describe(const QString &path, const QString &name, QMap<QString, QByteArray> &description)
{
  const QFileInfo info(path);
  if (info.isSymbolicLink()) {
    return;
  }
  if (!info.isDir()) {
    description.insert(name, "file " + QByteArray::number(info.size()) + " " + hashOfFile(path));
    return;
  }
  description.insert(name, "directory");
  QDirIterator children(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  while (children.hasNext()) {
    const QFileInfo child = children.nextFileInfo();
    describe(child.filePath(), name + "/" + child.fileName(), description);
  }
}

} // namespace deskflow::test
