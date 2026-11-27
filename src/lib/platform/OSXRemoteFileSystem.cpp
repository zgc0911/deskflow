/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXRemoteFileSystem.h"

#include "base/Log.h"
#include "deskflow/FileTransfer.h"

#include <QString>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace deskflow {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

const uint64_t kRootId = 1;

//! Longest name the system accepts, in bytes
const size_t kMaxNameLength = 255;

//! Bytes taken from a download at a time
const uint32_t kPumpRead = 256 * 1024;

//! A listing that failed is not asked for again before this much time has passed
const auto kListingRetry = milliseconds(2000);

//! Stands for the listing of a directory where a position in a file is expected
const uint64_t kListingKey = UINT64_MAX;

nfs::Status statusOf(FileTransferStatus status)
{
  switch (status) {
  case FileTransferStatus::Ok:
    return nfs::Status::Ok;
  case FileTransferStatus::NotFound:
    // no longer offered, or deleted since
    return nfs::Status::Stale;
  default:
    return nfs::Status::Io;
  }
}

//! What names are compared by: the system treats the two ways to write an accented letter as the same name
std::string keyOf(const std::string &name)
{
  return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()))
      .normalized(QString::NormalizationForm_C)
      .toUtf8()
      .toStdString();
}

bool isUsableName(const std::string &name)
{
  if (!ClipboardFileList::isValidName(name) || name.size() > kMaxNameLength) {
    return false;
  }
  // the system refuses names that are not UTF-8
  return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())).toUtf8().toStdString() == name;
}

std::string hexOf(uint64_t value)
{
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
  return buffer;
}

struct Node
{
  enum class Kind
  {
    Root,
    Offer, //!< The directory of one list of copied files
    Entry  //!< A file or directory of another computer
  };

  enum class Listing
  {
    None,
    Done,
    Failed
  };

  uint64_t id = 0;
  uint64_t parent = 0;
  //! The directory of the list this belongs to
  uint64_t offer = 0;
  Kind kind = Kind::Entry;
  nfs::FileHandle handle{};
  std::string name;
  bool isDirectory = false;
  uint64_t size = 0;
  int64_t modifiedTime = 0;

  // where it is on the other computer
  uint64_t offerId = 0;
  uint32_t item = 0;
  std::string relativePath;

  // what a directory holds
  Listing listing = Listing::None;
  //! The other computer is being asked, for the first time or again
  bool loading = false;
  Clock::time_point listedAt{};
  nfs::Status failure = nfs::Status::Io;
  Clock::time_point failedAt{};
  std::vector<uint64_t> children;
  std::map<std::string, uint64_t> byName;
  //! Changes whenever \c children change, positions in a listing are those of one version
  uint64_t version = 1;

  // for the directory of a list: a session is a run of calls with no long pause in it
  Clock::time_point lastUsed{};
  Clock::time_point sessionStart{};
  //! When a call last came for anything of the list
  Clock::time_point calledAt{};
};

//! A download of one file, from some position towards its end
struct Stream
{
  struct Chunk
  {
    uint64_t offset = 0;
    std::string data;
  };

  uint64_t node = 0;
  uint64_t offer = 0;
  std::unique_ptr<RemoteFileReader> reader;
  std::deque<Chunk> chunks;
  //! Position of the first byte that is held, and of the byte after the last one received
  uint64_t begin = 0;
  uint64_t end = 0;
  //! Everything before this was handed out, or given up on
  uint64_t servedTo = 0;
  //! Pieces behind servedTo that were handed out already, start and end
  std::map<uint64_t, uint64_t> served;
  //! The furthest that anybody asked for
  uint64_t wanted = 0;
  //! Where another download takes over, this one goes no further
  uint64_t stopAt = UINT64_MAX;
  //! Reached stopAt, no more comes
  bool complete = false;
  bool atEnd = false;
  bool failed = false;
  nfs::Status failure = nfs::Status::Io;
  Clock::time_point failedAt{};
  //! Given up: replaced, not used for too long, or the file is gone or changed
  bool stopped = false;
  //! Reads that wait for this download
  int waiting = 0;
  Clock::time_point lastUsed{};
};

} // namespace

struct OSXRemoteFileSystem::State : std::enable_shared_from_this<OSXRemoteFileSystem::State>
{
  using Lock = std::unique_lock<std::mutex>;

  std::mutex mutex;
  std::condition_variable changed;
  std::shared_ptr<RemoteFiles> files;
  Limits limits;
  bool stopping = false;
  //! Counts interruptions, a call that waits fails when it changes
  uint64_t interruptions = 0;

  uint64_t nextId = kRootId + 1;
  std::unordered_map<uint64_t, Node> nodes;
  std::map<nfs::FileHandle, uint64_t> handles;
  //! The directories of the lists, oldest first
  std::vector<uint64_t> offers;
  std::vector<std::shared_ptr<Stream>> streams;
  //! How often in a row a call was told to come back later, by file and position
  std::map<std::pair<uint64_t, uint64_t>, int> sentAway;
  //! Threads that download or list
  int threads = 0;
  int listings = 0;
  uint64_t fetched = 0;
  uint64_t listed = 0;

  // everything below is called with the mutex locked

  Node *find(uint64_t id)
  {
    const auto found = nodes.find(id);
    return (found == nodes.end()) ? nullptr : &found->second;
  }

  Node *find(const nfs::FileHandle &handle)
  {
    const auto found = handles.find(handle);
    return (found == handles.end()) ? nullptr : find(found->second);
  }

  Node &add(uint64_t parent, const std::string &name, bool isDirectory)
  {
    Node node;
    node.id = nextId++;
    node.parent = parent;
    node.name = name;
    node.isDirectory = isDirectory;
    // nobody must be able to guess the handle of a file
    arc4random_buf(node.handle.data(), node.handle.size());
    handles[node.handle] = node.id;
    const uint64_t id = node.id;
    // adding never moves the nodes that exist, pointers to them stay good
    return nodes.emplace(id, std::move(node)).first->second;
  }

  //! Forget a file or a directory with all that is in it
  void remove(uint64_t id)
  {
    const auto found = nodes.find(id);
    if (found == nodes.end()) {
      return;
    }
    const std::vector<uint64_t> children = found->second.children;
    handles.erase(found->second.handle);
    nodes.erase(found);
    stopStreamsOf(id);
    for (const uint64_t child : children) {
      remove(child);
    }
  }

  static nfs::Attributes attributesOf(const Node &node)
  {
    nfs::Attributes attributes;
    attributes.isDirectory = node.isDirectory;
    attributes.size = node.isDirectory ? 0 : node.size;
    attributes.modifiedTime = node.modifiedTime;
    attributes.fileId = node.id;
    return attributes;
  }

  //! Take note that a file or directory is still in use by a call that waits
  void keepInUse(const Node &node, Clock::time_point now)
  {
    if (Node *offer = find(node.offer); offer != nullptr) {
      offer->lastUsed = now;
      offer->calledAt = now;
    }
  }

  //! Whether a list is being pasted from: a read waits for it, or a call came a moment ago
  bool isInUse(const Node &offer, Clock::time_point now) const
  {
    return isBeingRead(offer.id) || now - offer.calledAt < milliseconds(limits.freshMs);
  }

  //! Whether a read waits for the data of a file of a list
  bool isBeingRead(uint64_t offer) const
  {
    return std::any_of(streams.begin(), streams.end(), [offer](const std::shared_ptr<Stream> &stream) {
      return stream->offer == offer && stream->waiting > 0;
    });
  }

  //! Take note that a call for a file or directory arrived. A call after a pause starts a new session.
  void touch(const Node &node, Clock::time_point now)
  {
    Node *offer = find(node.offer);
    if (offer == nullptr) {
      return;
    }
    // a read that waits for slow data is no pause
    if (now - offer->lastUsed > milliseconds(limits.freshMs) && !isBeingRead(offer->id)) {
      offer->sessionStart = now;
      // what was downloaded before the pause may be out of date by now
      for (const auto &stream : streams) {
        if (stream->offer == offer->id && stream->waiting == 0) {
          stop(*stream);
        }
      }
    }
    offer->lastUsed = now;
    offer->calledAt = now;
  }

  //! Whether a listing dates from before the current session
  bool isOutdated(const Node &node)
  {
    const Node *offer = find(node.offer);
    return offer != nullptr && node.listedAt < offer->sessionStart;
  }

  //! Tell a call to come back later, or to give up if that was said too often
  nfs::Status comeBackLater(uint64_t node, uint64_t position)
  {
    if (sentAway.size() > 4096) {
      sentAway.clear();
    }
    const std::pair<uint64_t, uint64_t> key{node, position};
    int &times = sentAway[key];
    if (times >= limits.retries) {
      sentAway.erase(key);
      return nfs::Status::Io;
    }
    ++times;
    return nfs::Status::TryLater;
  }

  //
  // directories
  //

  //! Fill in a new entry of a directory
  Node &addEntry(Node &directory, const ClipboardFileEntry &entry, uint32_t position)
  {
    const bool topLevel = (directory.kind == Node::Kind::Offer);
    Node &child = add(directory.id, entry.name, entry.isDirectory);
    child.offer = directory.offer;
    child.offerId = directory.offerId;
    child.item = topLevel ? position : directory.item;
    if (!topLevel) {
      child.relativePath = directory.relativePath.empty() ? entry.name : directory.relativePath + "/" + entry.name;
    }
    return child;
  }

  //! Replace what a directory holds, keeping the entries that are still the same
  void setEntries(Node &directory, const std::vector<ClipboardFileEntry> &entries, Clock::time_point now)
  {
    const bool topLevel = (directory.kind == Node::Kind::Offer);
    std::map<std::string, uint64_t> before = std::move(directory.byName);
    const std::vector<uint64_t> previous = std::move(directory.children);
    directory.byName.clear();
    directory.children.clear();

    int unusable = 0;
    for (size_t index = 0; index < entries.size(); ++index) {
      const ClipboardFileEntry &entry = entries[index];
      if (entry.isMissing) {
        continue;
      }
      if (!isUsableName(entry.name)) {
        ++unusable;
        continue;
      }
      const std::string key = keyOf(entry.name);
      if (directory.byName.contains(key)) {
        ++unusable;
        continue;
      }

      Node *child = nullptr;
      const auto known = before.find(key);
      if (known != before.end()) {
        child = find(known->second);
        const bool same = child != nullptr && child->name == entry.name && child->isDirectory == entry.isDirectory &&
                          (!topLevel || child->item == index);
        if (same) {
          before.erase(known);
        } else {
          // something else took its place
          child = nullptr;
        }
      }
      if (child == nullptr) {
        child = &addEntry(directory, entry, static_cast<uint32_t>(index));
      } else if (!entry.isDirectory && (child->size != entry.size || child->modifiedTime != entry.modifiedTime)) {
        // the file was changed, what was downloaded of it is of no use
        stopStreamsOf(child->id);
      }
      child->size = entry.isDirectory ? 0 : entry.size;
      child->modifiedTime = entry.modifiedTime;
      directory.children.push_back(child->id);
      directory.byName.emplace(key, child->id);
    }

    for (const auto &gone : before) {
      remove(gone.second);
    }
    if (unusable > 0) {
      LOG_DEBUG("%d copied files can't be shown here, their names are too long or not valid", unusable);
    }
    if (directory.children != previous) {
      // the positions a client got before mean something else now
      ++directory.version;
    }
    directory.listing = Node::Listing::Done;
    directory.listedAt = now;
  }

  //! Ask the other computer what a directory holds, on another thread
  void startListing(Node &directory)
  {
    directory.loading = true;
    ++listings;
    ++threads;
    ++listed;
    std::thread([state = shared_from_this(), id = directory.id, topLevel = (directory.kind == Node::Kind::Offer),
                 offerId = directory.offerId, item = directory.item, path = directory.relativePath] {
      std::vector<ClipboardFileEntry> entries;
      const FileTransferStatus status = topLevel ? state->files->listOffer(offerId, entries)
                                                 : state->files->listDirectory(offerId, item, path, entries);
      {
        std::scoped_lock lock{state->mutex};
        state->listingArrived(id, status, entries);
        --state->listings;
        --state->threads;
      }
      state->changed.notify_all();
    }).detach();
  }

  void listingArrived(uint64_t id, FileTransferStatus status, const std::vector<ClipboardFileEntry> &entries)
  {
    Node *directory = find(id);
    if (directory == nullptr) {
      return;
    }
    directory->loading = false;
    const auto now = Clock::now();

    if (status == FileTransferStatus::Ok) {
      setEntries(*directory, entries, now);
      return;
    }
    if (directory->listing == Node::Listing::Done && status != FileTransferStatus::NotFound) {
      // could not ask again: go on with what is known and leave it at that for this session
      directory->listedAt = now;
      return;
    }

    // never listed, or it is gone
    const std::vector<uint64_t> children = directory->children;
    directory->children.clear();
    directory->byName.clear();
    if (!children.empty()) {
      ++directory->version;
    }
    directory->listing = Node::Listing::Failed;
    directory->failure = statusOf(status);
    directory->failedAt = now;
    for (const uint64_t child : children) {
      remove(child);
    }
  }

  //! Make sure that it is known what a directory holds, and that this is not out of date
  /*!
  May wait, the mutex is released meanwhile and every pointer to a node is
  to be looked up again.
  */
  nfs::Status list(Lock &lock, uint64_t id, Clock::time_point deadline)
  {
    Clock::time_point refreshDeadline{};
    bool waitsForRefresh = false;
    const uint64_t interruptionsBefore = interruptions;
    for (;;) {
      if (stopping || interruptions != interruptionsBefore) {
        return nfs::Status::Io;
      }
      Node *directory = find(id);
      if (directory == nullptr) {
        return nfs::Status::Stale;
      }
      if (!directory->isDirectory) {
        return nfs::Status::NotDirectory;
      }
      if (directory->kind == Node::Kind::Root) {
        return nfs::Status::Ok;
      }

      const auto now = Clock::now();
      if (directory->listing == Node::Listing::Failed) {
        if (now - directory->failedAt < kListingRetry) {
          return directory->failure;
        }
        directory->listing = Node::Listing::None;
      }

      const bool known = (directory->listing == Node::Listing::Done);
      if (known && !isOutdated(*directory)) {
        sentAway.erase({id, kListingKey});
        return nfs::Status::Ok;
      }
      if (!directory->loading && listings < limits.listings) {
        startListing(*directory);
      }

      Clock::time_point until = deadline;
      if (known) {
        // asking again must not take long, what is known will do otherwise
        if (!waitsForRefresh) {
          waitsForRefresh = true;
          refreshDeadline = now + milliseconds(limits.refreshWaitMs);
        }
        if (now >= refreshDeadline) {
          return nfs::Status::Ok;
        }
        until = std::min(deadline, refreshDeadline);
      } else if (now >= deadline) {
        return comeBackLater(id, kListingKey);
      }
      changed.wait_until(lock, until);
    }
  }

  //
  // file contents
  //

  //! Give a download up and free what it holds. It stays in the list until it is collected.
  void stop(Stream &stream)
  {
    if (stream.stopped) {
      return;
    }
    stream.stopped = true;
    stream.reader->cancel();
    stream.chunks.clear();
    stream.served.clear();
    stream.begin = stream.end;
  }

  void stopStreamsOf(uint64_t node)
  {
    for (const auto &stream : streams) {
      if (stream->node == node) {
        stop(*stream);
      }
    }
  }

  //! Drop the downloads that were given up and that nobody waits for any more
  void collectStreams()
  {
    std::erase_if(streams, [](const std::shared_ptr<Stream> &stream) {
      return stream->stopped && stream->waiting == 0;
    });
  }

  //! The download that has, or soon will have, what is at a position of a file
  std::shared_ptr<Stream> pickStream(uint64_t node, uint64_t offset)
  {
    std::shared_ptr<Stream> best;
    for (const auto &stream : streams) {
      if (stream->node != node || stream->stopped || offset < stream->begin || offset >= stream->stopAt) {
        continue;
      }
      // one that is still running gets there, unless it is far away
      if (!stream->atEnd && !stream->failed && !stream->complete && offset > stream->end + limits.span) {
        continue;
      }
      if (best == nullptr || stream->begin > best->begin) {
        best = stream;
      }
    }
    return best;
  }

  //! Start a download, returns nullptr if there are too many that are in use
  std::shared_ptr<Stream> openStream(const Node &node, uint64_t offset, Clock::time_point now)
  {
    if (streams.size() >= limits.streams) {
      // make room: the one that was not used for the longest time, unless a read waits for it
      auto oldest = streams.end();
      for (auto it = streams.begin(); it != streams.end(); ++it) {
        if ((*it)->waiting == 0 && (oldest == streams.end() || (*it)->lastUsed < (*oldest)->lastUsed)) {
          oldest = it;
        }
      }
      if (oldest == streams.end()) {
        return nullptr;
      }
      stop(**oldest);
      streams.erase(oldest);
    }

    auto stream = std::make_shared<Stream>();
    stream->node = node.id;
    stream->offer = node.offer;
    stream->begin = stream->end = stream->servedTo = stream->wanted = offset;
    stream->lastUsed = now;
    // reads that belong together do not always arrive in order. if one that was further on came
    // first and started a download, this one only fills the gap in front of it.
    for (const auto &other : streams) {
      if (other->node == node.id && !other->stopped && other->begin > offset) {
        stream->stopAt = std::min(stream->stopAt, other->begin);
      }
    }
    stream->reader = files->openFile(node.offerId, node.item, node.relativePath, offset, limits.window);
    streams.push_back(stream);

    ++threads;
    std::thread([state = shared_from_this(), stream] { state->pump(stream); }).detach();
    return stream;
  }

  //! Free what was handed out and is not kept for a reader that asks again
  void trim(Stream &stream)
  {
    const uint64_t keepFrom = (stream.servedTo > limits.keepBehind) ? stream.servedTo - limits.keepBehind : 0;
    while (!stream.chunks.empty() && stream.chunks.front().offset + stream.chunks.front().data.size() <= keepFrom) {
      stream.chunks.pop_front();
    }
    stream.begin = stream.chunks.empty() ? stream.end : stream.chunks.front().offset;
  }

  //! Note that everything before \p position was handed out or is not waited for any more
  void advanceServed(Stream &stream, uint64_t position)
  {
    stream.servedTo = std::max(stream.servedTo, position);
    while (!stream.served.empty() && stream.served.begin()->first <= stream.servedTo) {
      stream.servedTo = std::max(stream.servedTo, stream.served.begin()->second);
      stream.served.erase(stream.served.begin());
    }
    trim(stream);
  }

  void markServed(Stream &stream, uint64_t from, uint64_t to)
  {
    if (from <= stream.servedTo) {
      advanceServed(stream, to);
    } else {
      uint64_t &end = stream.served[from];
      end = std::max(end, to);
    }
  }

  static void copyOut(const Stream &stream, uint64_t offset, uint64_t count, std::string &data)
  {
    data.clear();
    data.reserve(static_cast<size_t>(count));
    for (const auto &chunk : stream.chunks) {
      const uint64_t chunkEnd = chunk.offset + chunk.data.size();
      if (chunkEnd <= offset) {
        continue;
      }
      const uint64_t from = std::max(offset, chunk.offset);
      const uint64_t to = std::min(offset + count, chunkEnd);
      if (from >= to) {
        break;
      }
      data.append(chunk.data, static_cast<size_t>(from - chunk.offset), static_cast<size_t>(to - from));
    }
  }

  //! Runs on a thread of its own for each download
  void pump(const std::shared_ptr<Stream> &stream)
  {
    std::string buffer;
    Lock lock{mutex};
    while (!stopping && !stream->stopped && !stream->atEnd && !stream->failed) {
      if (stream->end >= stream->stopAt) {
        stream->complete = true;
        stream->reader->cancel();
        changed.notify_all();
        break;
      }
      // no further than what was asked for, plus a bit
      if (stream->end >= std::max(stream->wanted, stream->servedTo) + limits.readAhead) {
        if (isIdle(*stream)) {
          stop(*stream);
          break;
        }
        changed.wait_for(lock, milliseconds(500));
        continue;
      }

      const auto size = static_cast<uint32_t>(std::min<uint64_t>(kPumpRead, stream->stopAt - stream->end));
      lock.unlock();
      buffer.resize(size);
      uint32_t got = 0;
      const FileTransferStatus status = stream->reader->read(buffer.data(), size, got);
      lock.lock();
      if (stopping || stream->stopped) {
        break;
      }

      if (status != FileTransferStatus::Ok) {
        stream->failed = true;
        stream->failure = statusOf(status);
        stream->failedAt = Clock::now();
      } else if (got == 0) {
        stream->atEnd = true;
        // shorter than the listing said: the system must know, or it fills the rest with zeros
        Node *node = find(stream->node);
        if (node != nullptr && stream->end < node->size) {
          node->size = stream->end;
        }
      } else {
        buffer.resize(got);
        stream->chunks.push_back({stream->end, std::move(buffer)});
        buffer = {};
        stream->end += got;
        fetched += got;
        trim(*stream);
      }
      changed.notify_all();
    }

    // done with the other computer. what was received stays for the readers that are behind, until
    // nobody asks any more. a failure is remembered for a while so that all of them see it.
    while (!stopping && !stream->stopped) {
      const bool forgotten =
          stream->failed && Clock::now() - stream->failedAt > milliseconds(limits.failureMs) && stream->waiting == 0;
      if (forgotten || isIdle(*stream)) {
        stop(*stream);
        break;
      }
      changed.wait_for(lock, milliseconds(500));
    }

    collectStreams();
    --threads;
    lock.unlock();
    changed.notify_all();
  }

  bool isIdle(const Stream &stream) const
  {
    return stream.waiting == 0 && Clock::now() - stream.lastUsed > milliseconds(limits.idleMs);
  }

  nfs::Status
  read(Lock &lock, const nfs::FileHandle &handle, uint64_t offset, uint32_t count, std::string &data, bool &endOfFile)
  {
    data.clear();
    endOfFile = false;

    Node *node = find(handle);
    if (node == nullptr) {
      return nfs::Status::Stale;
    }
    if (node->isDirectory) {
      return nfs::Status::IsDirectory;
    }
    const uint64_t id = node->id;
    if (count == 0) {
      endOfFile = (offset >= node->size);
      return nfs::Status::Ok;
    }

    const auto arrived = Clock::now();
    const auto deadline = arrived + milliseconds(limits.waitMs);
    const auto partialDeadline = arrived + milliseconds(limits.partialWaitMs);
    const uint64_t wantedEnd = offset + count;
    const uint64_t interruptionsBefore = interruptions;
    touch(*node, arrived);

    for (;;) {
      if (stopping || interruptions != interruptionsBefore) {
        return nfs::Status::Io;
      }
      node = find(id);
      if (node == nullptr) {
        return nfs::Status::Stale;
      }
      const auto now = Clock::now();
      keepInUse(*node, now);
      collectStreams();

      std::shared_ptr<Stream> stream = pickStream(id, offset);
      if (stream == nullptr) {
        stream = openStream(*node, offset, now);
      }
      if (stream != nullptr) {
        stream->lastUsed = now;
        if (wantedEnd > stream->wanted) {
          stream->wanted = wantedEnd;
          changed.notify_all();
        }
        // a read from far ahead: what lies before it is not waited for any longer
        if (wantedEnd > stream->servedTo + limits.span) {
          advanceServed(*stream, wantedEnd - limits.span);
        }

        const uint64_t available = (stream->end > offset) ? std::min<uint64_t>(count, stream->end - offset) : 0;
        const bool noMoreComes = stream->atEnd || stream->failed || stream->complete;
        if (available == count || (available > 0 && (noMoreComes || now >= partialDeadline))) {
          copyOut(*stream, offset, available, data);
          markServed(*stream, offset, offset + available);
          endOfFile = stream->atEnd && (offset + available >= stream->end);
          sentAway.erase({id, offset});
          return nfs::Status::Ok;
        }
        if (available == 0 && stream->atEnd) {
          endOfFile = true;
          sentAway.erase({id, offset});
          return nfs::Status::Ok;
        }
        if (available == 0 && stream->failed) {
          sentAway.erase({id, offset});
          return stream->failure;
        }
      }
      if (now >= deadline) {
        return comeBackLater(id, offset);
      }

      Clock::time_point until = deadline;
      if (stream != nullptr) {
        if (stream->end > offset) {
          until = std::min(until, partialDeadline);
        }
        ++stream->waiting;
      }
      changed.wait_until(lock, until);
      if (stream != nullptr) {
        --stream->waiting;
      }
    }
  }
};

OSXRemoteFileSystem::OSXRemoteFileSystem(std::shared_ptr<RemoteFiles> files) : OSXRemoteFileSystem(std::move(files), {})
{
}

OSXRemoteFileSystem::OSXRemoteFileSystem(std::shared_ptr<RemoteFiles> files, const Limits &limits)
    : m_state(std::make_shared<State>())
{
  m_state->files = std::move(files);
  m_state->limits = limits;

  Node root;
  root.id = kRootId;
  root.parent = kRootId;
  root.kind = Node::Kind::Root;
  root.isDirectory = true;
  root.modifiedTime = static_cast<int64_t>(std::time(nullptr));
  root.listing = Node::Listing::Done;
  arc4random_buf(root.handle.data(), root.handle.size());
  m_state->handles[root.handle] = root.id;
  m_state->nodes.emplace(root.id, std::move(root));
}

OSXRemoteFileSystem::~OSXRemoteFileSystem()
{
  shutdown();
}

std::vector<std::string> OSXRemoteFileSystem::addOffer(const ClipboardFileList &list)
{
  std::vector<std::string> paths(list.entries.size());
  if (list.offerId == 0 || list.entries.empty()) {
    return paths;
  }

  State &state = *m_state;
  std::scoped_lock lock{state.mutex};
  if (state.stopping) {
    return paths;
  }

  const std::string directoryName = hexOf(list.offerId);
  Node *root = state.find(kRootId);
  Node *offer = nullptr;
  if (const auto known = root->byName.find(directoryName); known != root->byName.end()) {
    offer = state.find(known->second);
  }

  const auto now = Clock::now();
  if (offer == nullptr) {
    offer = &state.add(kRootId, directoryName, true);
    offer->kind = Node::Kind::Offer;
    offer->offer = offer->id;
    offer->offerId = list.offerId;
    offer->modifiedTime = static_cast<int64_t>(std::time(nullptr));
    root->children.push_back(offer->id);
    root->byName.emplace(directoryName, offer->id);
    root->modifiedTime = offer->modifiedTime;
    ++root->version;
  } else {
    // the one that was copied last is dropped last
    std::erase(state.offers, offer->id);
  }
  state.offers.push_back(offer->id);

  // the list tells how the files were when the clipboard was sent, which was a moment ago. what
  // looks at them right away, such as the pasteboard when their paths are put on it, is answered
  // from it, nothing waits for the other computer. a later look starts a new session and asks
  // how they are then.
  state.setEntries(*offer, list.entries, now);
  offer->sessionStart = now;
  offer->lastUsed = now;

  // too many lists: the oldest go first, but not one that is being pasted from, unless there are
  // twice as many. that one goes once it is left alone and another list arrives.
  for (auto next = state.offers.begin(); state.offers.size() > state.limits.offers && next != state.offers.end();) {
    const uint64_t id = *next;
    const Node *candidate = state.find(id);
    const bool keep = id == offer->id || (candidate != nullptr && state.offers.size() <= 2 * state.limits.offers &&
                                          state.isInUse(*candidate, now));
    if (keep) {
      ++next;
      continue;
    }
    next = state.offers.erase(next);
    if (candidate != nullptr) {
      root->byName.erase(candidate->name);
    }
    std::erase(root->children, id);
    ++root->version;
    state.remove(id);
  }

  for (const uint64_t id : offer->children) {
    const Node *child = state.find(id);
    if (child != nullptr && child->item < paths.size()) {
      paths[child->item] = directoryName + "/" + child->name;
    }
  }
  return paths;
}

bool OSXRemoteFileSystem::shutdown(int waitMs)
{
  State &state = *m_state;
  State::Lock lock{state.mutex};
  if (!state.stopping) {
    state.stopping = true;
    for (const auto &stream : state.streams) {
      state.stop(*stream);
    }
    state.streams.clear();
  }
  state.changed.notify_all();

  // the threads only hold on to the shared state, so it does no harm if one of them is
  // slow to notice. listings can not be interrupted, they end when the other computer answers.
  return state.changed.wait_for(lock, milliseconds(waitMs), [&state] { return state.threads == 0; });
}

void OSXRemoteFileSystem::interrupt()
{
  std::scoped_lock lock{m_state->mutex};
  ++m_state->interruptions;
  m_state->changed.notify_all();
}

int OSXRemoteFileSystem::threadCount() const
{
  std::scoped_lock lock{m_state->mutex};
  return m_state->threads;
}

uint64_t OSXRemoteFileSystem::fetchedBytes() const
{
  std::scoped_lock lock{m_state->mutex};
  return m_state->fetched;
}

uint64_t OSXRemoteFileSystem::listingCount() const
{
  std::scoped_lock lock{m_state->mutex};
  return m_state->listed;
}

size_t OSXRemoteFileSystem::streamCount() const
{
  std::scoped_lock lock{m_state->mutex};
  return m_state->streams.size();
}

nfs::FileHandle OSXRemoteFileSystem::rootHandle()
{
  std::scoped_lock lock{m_state->mutex};
  return m_state->find(kRootId)->handle;
}

nfs::Status OSXRemoteFileSystem::getAttributes(const nfs::FileHandle &handle, nfs::Attributes &attributes)
{
  State &state = *m_state;
  State::Lock lock{state.mutex};
  if (state.stopping) {
    return nfs::Status::Io;
  }
  Node *node = state.find(handle);
  if (node == nullptr) {
    return nfs::Status::Stale;
  }

  const uint64_t id = node->id;
  if (node->kind == Node::Kind::Entry) {
    // its size and date are part of the listing of the directory it is in
    const auto now = Clock::now();
    state.touch(*node, now);
    const nfs::Status status = state.list(lock, node->parent, now + milliseconds(state.limits.waitMs));
    node = state.find(id);
    if (node == nullptr) {
      return nfs::Status::Stale;
    }
    if (status == nfs::Status::TryLater) {
      return status;
    }
  }
  attributes = State::attributesOf(*node);
  return nfs::Status::Ok;
}

nfs::Status OSXRemoteFileSystem::lookup(
    const nfs::FileHandle &directory, const std::string &name, nfs::FileHandle &handle, nfs::Attributes &attributes
)
{
  State &state = *m_state;
  State::Lock lock{state.mutex};
  if (state.stopping) {
    return nfs::Status::Io;
  }
  Node *parent = state.find(directory);
  if (parent == nullptr) {
    return nfs::Status::Stale;
  }
  if (!parent->isDirectory) {
    return nfs::Status::NotDirectory;
  }

  if (name == "..") {
    const Node *above = state.find(parent->parent);
    if (above == nullptr) {
      return nfs::Status::Stale;
    }
    handle = above->handle;
    attributes = State::attributesOf(*above);
    return nfs::Status::Ok;
  }

  const uint64_t id = parent->id;
  const auto now = Clock::now();
  state.touch(*parent, now);
  const nfs::Status status = state.list(lock, id, now + milliseconds(state.limits.waitMs));
  if (status != nfs::Status::Ok) {
    return status;
  }
  parent = state.find(id);
  if (parent == nullptr) {
    return nfs::Status::Stale;
  }

  const auto found = parent->byName.find(keyOf(name));
  const Node *child = (found == parent->byName.end()) ? nullptr : state.find(found->second);
  if (child == nullptr) {
    return nfs::Status::NoEntry;
  }
  handle = child->handle;
  attributes = State::attributesOf(*child);
  return nfs::Status::Ok;
}

nfs::Status OSXRemoteFileSystem::readDirectory(
    const nfs::FileHandle &directory, uint64_t first, size_t limit, std::vector<nfs::DirectoryEntry> &entries,
    bool &complete, uint64_t &version
)
{
  entries.clear();
  complete = true;
  version = 0;

  State &state = *m_state;
  State::Lock lock{state.mutex};
  if (state.stopping) {
    return nfs::Status::Io;
  }
  Node *parent = state.find(directory);
  if (parent == nullptr) {
    return nfs::Status::Stale;
  }

  const uint64_t id = parent->id;
  const auto now = Clock::now();
  state.touch(*parent, now);
  const nfs::Status status = state.list(lock, id, now + milliseconds(state.limits.waitMs));
  if (status != nfs::Status::Ok) {
    return status;
  }
  parent = state.find(id);
  if (parent == nullptr) {
    return nfs::Status::Stale;
  }

  version = parent->version;
  uint64_t position = first;
  for (; position < parent->children.size() && entries.size() < limit; ++position) {
    const Node *child = state.find(parent->children[static_cast<size_t>(position)]);
    if (child != nullptr) {
      entries.push_back({child->name, child->handle, State::attributesOf(*child)});
    }
  }
  complete = (position >= parent->children.size());
  return nfs::Status::Ok;
}

nfs::Status OSXRemoteFileSystem::read(
    const nfs::FileHandle &file, uint64_t offset, uint32_t count, std::string &data, bool &endOfFile
)
{
  State::Lock lock{m_state->mutex};
  return m_state->read(lock, file, offset, count, data, endOfFile);
}

} // namespace deskflow
