/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXNfsServer.h"

#include "base/Log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace deskflow::nfs {

namespace {

// ONC RPC, RFC 5531
const uint32_t kRpcCall = 0;
const uint32_t kRpcReply = 1;
const uint32_t kRpcVersion = 2;
const uint32_t kMessageAccepted = 0;
const uint32_t kAcceptSuccess = 0;
const uint32_t kAcceptProgramUnavailable = 1;
const uint32_t kAcceptProgramMismatch = 2;
const uint32_t kAcceptProcedureUnavailable = 3;
const uint32_t kAcceptGarbageArguments = 4;
const uint32_t kLastFragment = 0x80000000U;

// the two programs, RFC 1813
const uint32_t kProgramMount = 100005;
const uint32_t kProgramNfs = 100003;
const uint32_t kProgramVersion = 3;
const uint32_t kAuthSys = 1;
const uint32_t kMountAccessDenied = 13;

// requests are small, anything bigger is not from the system's client
const uint32_t kMaxRecordSize = 256 * 1024;

//! Calls answered at the same time, more wait for their turn
const int kMaxRunningCalls = 48;

const uint64_t kFileSystemId = 0x44464c57; // "DFLW"
const uint32_t kDirectoryListingOverhead = 256;
//! No entry of a directory listing takes less room than this in a reply
const size_t kSmallestDirectoryEntry = 28;

// what may be done with a file or directory, NFS access bits
const uint32_t kAccessRead = 0x01;
const uint32_t kAccessLookup = 0x02;
const uint32_t kAccessExecute = 0x20;

bool sendAll(int socket, const std::string &data)
{
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(socket, data.data() + sent, data.size() - sent, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool receiveAll(int socket, void *buffer, size_t size)
{
  auto *target = static_cast<uint8_t *>(buffer);
  size_t received = 0;
  while (received < size) {
    const ssize_t n = ::recv(socket, target + received, size - received, 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    received += static_cast<size_t>(n);
  }
  return true;
}

} // namespace

//! Encodes values the way RPC wants them: big-endian, padded to four bytes
class XdrWriter
{
public:
  void u32(uint32_t value)
  {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value)
    };
    m_data.append(reinterpret_cast<const char *>(bytes), 4);
  }

  void u64(uint64_t value)
  {
    u32(static_cast<uint32_t>(value >> 32));
    u32(static_cast<uint32_t>(value & 0xffffffffULL));
  }

  void boolean(bool value)
  {
    u32(value ? 1 : 0);
  }

  void status(Status value)
  {
    u32(static_cast<uint32_t>(value));
  }

  //! Bytes with a length in front
  void opaque(const void *bytes, size_t size)
  {
    u32(static_cast<uint32_t>(size));
    fixed(bytes, size);
  }

  void opaque(const std::string &bytes)
  {
    opaque(bytes.data(), bytes.size());
  }

  //! Bytes without a length
  void fixed(const void *bytes, size_t size)
  {
    m_data.append(static_cast<const char *>(bytes), size);
    m_data.append((4 - size % 4) % 4, '\0');
  }

  void handle(const FileHandle &value)
  {
    opaque(value.data(), value.size());
  }

  void append(const XdrWriter &other)
  {
    m_data.append(other.m_data);
  }

  size_t size() const
  {
    return m_data.size();
  }

  const std::string &data() const
  {
    return m_data;
  }

private:
  std::string m_data;
};

//! Decodes values, remembers if the data ran out or made no sense
class XdrReader
{
public:
  explicit XdrReader(const std::string &data) : m_data(data)
  {
  }

  bool ok() const
  {
    return m_ok;
  }

  uint32_t u32()
  {
    if (m_data.size() - m_position < 4) {
      m_ok = false;
      m_position = m_data.size();
      return 0;
    }
    const auto *bytes = reinterpret_cast<const uint8_t *>(m_data.data() + m_position);
    m_position += 4;
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
  }

  uint64_t u64()
  {
    const uint64_t high = u32();
    const uint64_t low = u32();
    return (high << 32) | low;
  }

  //! Bytes with a length in front, refused if longer than \p limit
  std::string opaque(size_t limit)
  {
    const size_t size = u32();
    if (!m_ok || size > limit) {
      m_ok = false;
      return {};
    }
    return fixed(size);
  }

  std::string fixed(size_t size)
  {
    const size_t padded = size + (4 - size % 4) % 4;
    if (!m_ok || m_data.size() - m_position < padded) {
      m_ok = false;
      m_position = m_data.size();
      return {};
    }
    std::string value = m_data.substr(m_position, size);
    m_position += padded;
    return value;
  }

  bool handle(FileHandle &value)
  {
    const std::string bytes = opaque(64);
    if (!m_ok || bytes.size() != value.size()) {
      return false;
    }
    std::memcpy(value.data(), bytes.data(), value.size());
    return true;
  }

private:
  const std::string &m_data;
  size_t m_position = 0;
  bool m_ok = true;
};

namespace {

void putAttributes(XdrWriter &out, const Attributes &attributes)
{
  out.u32(attributes.isDirectory ? 2 : 1);
  // the volume can't be written to, it is mounted read-only. the bits are those a copy gets:
  // what is pasted must not end up read-only.
  out.u32(attributes.isDirectory ? 0755 : 0644);
  out.u32(attributes.isDirectory ? 2 : 1);
  out.u32(static_cast<uint32_t>(::getuid()));
  out.u32(static_cast<uint32_t>(::getgid()));
  const uint64_t size = attributes.isDirectory ? 512 : attributes.size;
  out.u64(size);
  out.u64(size);
  out.u32(0);
  out.u32(0);
  out.u64(kFileSystemId);
  out.u64(attributes.fileId);
  // access, modification and change time
  const auto seconds = static_cast<uint32_t>(std::max<int64_t>(attributes.modifiedTime, 0));
  for (int i = 0; i < 3; ++i) {
    out.u32(seconds);
    out.u32(0);
  }
}

//! Attributes that may be left out
void putOptionalAttributes(XdrWriter &out, IFileSystem &fileSystem, const FileHandle &handle)
{
  Attributes attributes;
  if (fileSystem.getAttributes(handle, attributes) == Status::Ok) {
    out.boolean(true);
    putAttributes(out, attributes);
  } else {
    out.boolean(false);
  }
}

//! Attributes before and after a change, there never is a change
void putNoChange(XdrWriter &out)
{
  out.boolean(false);
  out.boolean(false);
}

} // namespace

//! One connection of the NFS client
/*!
Held by the thread that receives on it and by every call that is being
answered on it. The socket is closed when the last of them lets go, so a
late answer can never go to a socket that was reused for something else.
*/
struct Server::Connection
{
  explicit Connection(int fd) : socket(fd)
  {
  }

  ~Connection()
  {
    ::close(socket);
  }

  Connection(Connection const &) = delete;
  Connection &operator=(Connection const &) = delete;

  const int socket;
  std::mutex writeMutex;
};

Server::Server(IFileSystem &fileSystem) : m_fileSystem(fileSystem)
{
}

Server::~Server()
{
  stop();
}

bool Server::start(const std::string &exportPath)
{
  if (m_listenSocket >= 0) {
    return true;
  }

  m_exportPath = exportPath;
  m_stopping = false;
  m_mountAllowed = true;

  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    LOG_ERR("can't create a socket for the file volume: %s", strerror(errno));
    return false;
  }

  // loopback only, nothing of this is reachable from the network
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  socklen_t length = sizeof(address);
  if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || ::listen(listener, 8) != 0 ||
      ::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
    LOG_ERR("can't listen for the file volume: %s", strerror(errno));
    ::close(listener);
    return false;
  }

  if (::pipe(m_wakePipe) != 0) {
    LOG_ERR("can't create a pipe for the file volume: %s", strerror(errno));
    ::close(listener);
    return false;
  }
  for (const int end : m_wakePipe) {
    ::fcntl(end, F_SETFD, FD_CLOEXEC);
  }
  ::fcntl(listener, F_SETFD, FD_CLOEXEC);

  m_listenSocket = listener;
  m_port = ntohs(address.sin_port);
  m_acceptThread = std::thread([this] { acceptLoop(); });
  LOG_DEBUG("file volume server listens on port %d", m_port);
  return true;
}

void Server::stop()
{
  if (m_listenSocket < 0) {
    return;
  }
  {
    std::scoped_lock lock{m_mutex};
    m_stopping = true;
    m_changed.notify_all();
  }

  // wakes the accept loop
  const char wake = 1;
  if (::write(m_wakePipe[1], &wake, 1) < 0) {
    LOG_DEBUG("can't wake the file volume server: %s", strerror(errno));
  }
  if (m_acceptThread.joinable()) {
    m_acceptThread.join();
  }
  ::close(m_listenSocket);
  ::close(m_wakePipe[0]);
  ::close(m_wakePipe[1]);
  m_listenSocket = -1;
  m_wakePipe[0] = m_wakePipe[1] = -1;
  m_port = 0;

  // wake the receive loops, then wait for them and for the calls that are being answered
  std::unique_lock lock{m_mutex};
  for (const auto &connection : m_connections) {
    ::shutdown(connection->socket, SHUT_RDWR);
  }
  m_changed.wait(lock, [this] { return m_threads == 0; });
  m_connections.clear();
}

void Server::threadEnded()
{
  // notified with the mutex held: once it is released this object may be gone
  std::scoped_lock lock{m_mutex};
  --m_threads;
  m_changed.notify_all();
}

uint16_t Server::port() const
{
  return m_port;
}

void Server::setMountAllowed(bool allowed)
{
  m_mountAllowed = allowed;
}

uint64_t Server::callCount() const
{
  return m_calls;
}

void Server::acceptLoop()
{
  while (!m_stopping) {
    pollfd waitFor[2] = {{m_listenSocket, POLLIN, 0}, {m_wakePipe[0], POLLIN, 0}};
    if (::poll(waitFor, 2, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (m_stopping || (waitFor[1].revents != 0)) {
      break;
    }
    if ((waitFor[0].revents & POLLIN) == 0) {
      continue;
    }

    const int socket = ::accept(m_listenSocket, nullptr, nullptr);
    if (socket < 0) {
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      break;
    }

    int enable = 1;
    ::fcntl(socket, F_SETFD, FD_CLOEXEC);
    ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));

    auto connection = std::make_shared<Connection>(socket);
    std::scoped_lock lock{m_mutex};
    if (m_stopping) {
      break;
    }
    m_connections.push_back(connection);
    ++m_threads;
    std::thread([this, connection] {
      receiveLoop(connection);
      {
        std::scoped_lock forget{m_mutex};
        std::erase(m_connections, connection);
      }
      threadEnded();
    }).detach();
  }
}

void Server::receiveLoop(const std::shared_ptr<Connection> &connection)
{
  while (!m_stopping) {
    // a record comes in one or more fragments, each with a length in front
    std::string record;
    bool last = false;
    while (!last) {
      uint8_t header[4];
      if (!receiveAll(connection->socket, header, sizeof(header))) {
        return;
      }
      const uint32_t mark = (static_cast<uint32_t>(header[0]) << 24) | (static_cast<uint32_t>(header[1]) << 16) |
                            (static_cast<uint32_t>(header[2]) << 8) | static_cast<uint32_t>(header[3]);
      last = (mark & kLastFragment) != 0;
      const uint32_t size = mark & ~kLastFragment;
      if (size > kMaxRecordSize || record.size() + size > kMaxRecordSize) {
        LOG_WARN("dropping a connection to the file volume, the request is too big");
        ::shutdown(connection->socket, SHUT_RDWR);
        return;
      }
      const size_t before = record.size();
      record.resize(before + size);
      if (size > 0 && !receiveAll(connection->socket, record.data() + before, size)) {
        return;
      }
    }

    // a call may wait for data, so each gets a thread, up to a limit
    std::unique_lock lock{m_mutex};
    m_changed.wait(lock, [this] { return m_stopping || m_runningCalls < kMaxRunningCalls; });
    if (m_stopping) {
      return;
    }
    ++m_runningCalls;
    ++m_threads;
    lock.unlock();
    std::thread([this, connection, record = std::move(record)]() mutable {
      handleRecord(connection, std::move(record));
      {
        std::scoped_lock done{m_mutex};
        --m_runningCalls;
      }
      threadEnded();
    }).detach();
  }
}

void Server::handleRecord(const std::shared_ptr<Connection> &connection, std::string record)
{
  const std::string reply = handleCall(record);
  if (reply.empty()) {
    return;
  }

  XdrWriter header;
  header.u32(kLastFragment | static_cast<uint32_t>(reply.size()));
  std::scoped_lock lock{connection->writeMutex};
  if (!sendAll(connection->socket, header.data()) || !sendAll(connection->socket, reply)) {
    ::shutdown(connection->socket, SHUT_RDWR);
  }
}

std::string Server::handleCall(const std::string &record)
{
  XdrReader in(record);
  const uint32_t xid = in.u32();
  const uint32_t type = in.u32();
  const uint32_t rpcVersion = in.u32();
  const uint32_t program = in.u32();
  const uint32_t version = in.u32();
  const uint32_t procedure = in.u32();
  // who calls: not checked, nothing but this computer's own client can connect and find a handle
  in.u32();
  in.opaque(400);
  in.u32();
  in.opaque(400);
  if (!in.ok() || type != kRpcCall || rpcVersion != kRpcVersion) {
    return {};
  }
  ++m_calls;

  XdrWriter reply;
  reply.u32(xid);
  reply.u32(kRpcReply);
  reply.u32(kMessageAccepted);
  // no verifier
  reply.u32(0);
  reply.u32(0);

  if (program != kProgramMount && program != kProgramNfs) {
    reply.u32(kAcceptProgramUnavailable);
    return reply.data();
  }
  if (version != kProgramVersion) {
    reply.u32(kAcceptProgramMismatch);
    reply.u32(kProgramVersion);
    reply.u32(kProgramVersion);
    return reply.data();
  }

  XdrWriter result;
  const bool known = (program == kProgramMount) ? mountCall(procedure, in, result) : nfsCall(procedure, in, result);
  if (!known) {
    reply.u32(kAcceptProcedureUnavailable);
  } else if (!in.ok()) {
    reply.u32(kAcceptGarbageArguments);
  } else {
    reply.u32(kAcceptSuccess);
    reply.append(result);
  }
  return reply.data();
}

bool Server::mountCall(uint32_t procedure, XdrReader &arguments, XdrWriter &result)
{
  switch (procedure) {
  case 0: // nothing
    return true;

  case 1: { // mount
    const std::string path = arguments.opaque(1024);
    if (!arguments.ok()) {
      return true;
    }
    if (!m_mountAllowed || path != m_exportPath) {
      LOG_WARN("refusing a request to mount the file volume");
      result.u32(kMountAccessDenied);
      return true;
    }
    result.u32(0);
    result.handle(m_fileSystem.rootHandle());
    // how callers may identify themselves
    result.u32(1);
    result.u32(kAuthSys);
    return true;
  }

  case 2: // list of mounts, none are told
  case 5: // list of exports, none are told
    result.boolean(false);
    return true;

  case 3: // unmount
    arguments.opaque(1024);
    return true;

  case 4: // unmount all
    return true;

  default:
    return false;
  }
}

bool Server::nfsCall(uint32_t procedure, XdrReader &arguments, XdrWriter &result)
{
  switch (procedure) {
  case 0: // nothing
    return true;

  case 2:    // set attributes
  case 7:    // write
  case 8:    // create
  case 9:    // make directory
  case 10:   // make link
  case 11:   // make special file
  case 12:   // remove
  case 13:   // remove directory
  case 21: { // commit
    result.status(Status::ReadOnly);
    putNoChange(result);
    return true;
  }

  case 14: // rename, two directories that did not change
    result.status(Status::ReadOnly);
    putNoChange(result);
    putNoChange(result);
    return true;

  case 15: // hard link, a file and a directory that did not change
    result.status(Status::ReadOnly);
    result.boolean(false);
    putNoChange(result);
    return true;

  default:
    break;
  }

  // every other procedure starts with a handle
  FileHandle handle{};
  if (procedure > 21) {
    return false;
  }
  if (!arguments.handle(handle)) {
    if (arguments.ok()) {
      // well formed, but not one of ours
      result.status(Status::BadHandle);
      if (procedure != 1) {
        result.boolean(false);
      }
    }
    return true;
  }

  switch (procedure) {
  case 1: { // get attributes
    Attributes attributes;
    const Status status = m_fileSystem.getAttributes(handle, attributes);
    result.status(status);
    if (status == Status::Ok) {
      putAttributes(result, attributes);
    }
    return true;
  }

  case 3: { // look up a name
    const std::string name = arguments.opaque(1024);
    if (!arguments.ok()) {
      return true;
    }
    FileHandle found{};
    Attributes attributes;
    Status status = Status::Ok;
    if (name == ".") {
      found = handle;
      status = m_fileSystem.getAttributes(handle, attributes);
    } else {
      status = m_fileSystem.lookup(handle, name, found, attributes);
    }
    result.status(status);
    if (status == Status::Ok) {
      result.handle(found);
      result.boolean(true);
      putAttributes(result, attributes);
    }
    putOptionalAttributes(result, m_fileSystem, handle);
    return true;
  }

  case 4: { // what may be done
    const uint32_t wanted = arguments.u32();
    if (!arguments.ok()) {
      return true;
    }
    Attributes attributes;
    const Status status = m_fileSystem.getAttributes(handle, attributes);
    result.status(status);
    if (status == Status::Ok) {
      result.boolean(true);
      putAttributes(result, attributes);
      result.u32(wanted & (kAccessRead | kAccessLookup | kAccessExecute));
    } else {
      result.boolean(false);
    }
    return true;
  }

  case 5: // read a link, there are none
    result.status(Status::Invalid);
    putOptionalAttributes(result, m_fileSystem, handle);
    return true;

  case 6: { // read
    const uint64_t offset = arguments.u64();
    const uint32_t count = arguments.u32();
    if (!arguments.ok()) {
      return true;
    }
    std::string data;
    bool endOfFile = false;
    const Status status = m_fileSystem.read(handle, offset, std::min(count, s_maxRead), data, endOfFile);
    result.status(status);
    putOptionalAttributes(result, m_fileSystem, handle);
    if (status == Status::Ok) {
      result.u32(static_cast<uint32_t>(data.size()));
      result.boolean(endOfFile);
      result.opaque(data);
    }
    return true;
  }

  case 16:   // read directory
  case 17: { // read directory with attributes
    const bool plus = (procedure == 17);
    const uint64_t cookie = arguments.u64();
    arguments.fixed(8);
    if (plus) {
      arguments.u32();
    }
    const uint32_t maxSize = arguments.u32();
    if (!arguments.ok()) {
      return true;
    }

    // an entry is found again by its position: the directory itself is at 0, the one above
    // at 1, what the directory holds follows. the position after an entry is its cookie.
    const size_t budget = std::max<size_t>(maxSize, 2 * kDirectoryListingOverhead) - kDirectoryListingOverhead;
    Attributes own;
    std::vector<DirectoryEntry> entries;
    bool complete = true;
    Status status = m_fileSystem.getAttributes(handle, own);
    if (status == Status::Ok && !own.isDirectory) {
      status = Status::NotDirectory;
    }
    if (status == Status::Ok) {
      // more than fits, the rest is asked for with the cookie of the last entry that did
      const uint64_t first = (cookie > 2) ? cookie - 2 : 0;
      status = m_fileSystem.readDirectory(handle, first, budget / kSmallestDirectoryEntry + 1, entries, complete);
    }
    result.status(status);
    putOptionalAttributes(result, m_fileSystem, handle);
    if (status != Status::Ok) {
      return true;
    }

    uint64_t position = std::max<uint64_t>(cookie, 2);
    if (cookie < 2) {
      DirectoryEntry above{"..", handle, own};
      m_fileSystem.lookup(handle, "..", above.handle, above.attributes);
      entries.insert(entries.begin(), above);
      position = 1;
      if (cookie < 1) {
        entries.insert(entries.begin(), DirectoryEntry{".", handle, own});
        position = 0;
      }
    }

    // the listing is always the same, so there is nothing to verify
    const uint8_t verifier[8] = {};
    result.fixed(verifier, sizeof(verifier));

    XdrWriter list;
    for (const DirectoryEntry &entry : entries) {
      XdrWriter item;
      item.boolean(true);
      item.u64(entry.attributes.fileId);
      item.opaque(entry.name);
      item.u64(++position);
      if (plus) {
        item.boolean(true);
        putAttributes(item, entry.attributes);
        item.boolean(true);
        item.handle(entry.handle);
      }
      if (list.size() + item.size() > budget) {
        complete = false;
        break;
      }
      list.append(item);
    }
    result.append(list);
    result.boolean(false);
    result.boolean(complete);
    return true;
  }

  case 18: { // file system statistics
    result.status(Status::Ok);
    putOptionalAttributes(result, m_fileSystem, handle);
    // a nominal size, nothing is free
    result.u64(1ULL << 40);
    result.u64(0);
    result.u64(0);
    result.u64(1 << 20);
    result.u64(0);
    result.u64(0);
    result.u32(0);
    return true;
  }

  case 19: { // file system information
    result.status(Status::Ok);
    putOptionalAttributes(result, m_fileSystem, handle);
    // read sizes: most, preferred, multiple
    result.u32(s_maxRead);
    result.u32(s_maxRead);
    result.u32(4096);
    // write sizes, never used
    result.u32(s_maxRead);
    result.u32(s_maxRead);
    result.u32(4096);
    // preferred size of a directory listing
    result.u32(64 * 1024);
    result.u64(0x7fffffffffffffffULL);
    // times are in whole seconds
    result.u32(1);
    result.u32(0);
    // the same limits apply everywhere in the volume
    result.u32(0x8);
    return true;
  }

  case 20: { // limits of path names
    result.status(Status::Ok);
    putOptionalAttributes(result, m_fileSystem, handle);
    // links per file, longest name
    result.u32(1);
    result.u32(255);
    // too long names are refused, owners can't be changed, names keep their case and differ by it
    result.boolean(true);
    result.boolean(true);
    result.boolean(false);
    result.boolean(true);
    return true;
  }

  default:
    return false;
  }
}

} // namespace deskflow::nfs
