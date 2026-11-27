/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace deskflow::nfs {

//! Names a file or directory to the system, opaque to it
using FileHandle = std::array<uint8_t, 16>;

//! Results of file system operations, the values are those of NFS version 3
enum class Status : uint32_t
{
  Ok = 0,
  NoEntry = 2,
  Io = 5,
  AccessDenied = 13,
  NotDirectory = 20,
  IsDirectory = 21,
  Invalid = 22,
  ReadOnly = 30,
  Stale = 70,
  BadHandle = 10001,
  //! A position in a directory that is no good any more, the directory changed since
  BadCookie = 10003,
  //! Not ready yet, the system waits a moment and asks again
  TryLater = 10008
};

struct Attributes
{
  bool isDirectory = false;
  uint64_t size = 0;
  //! Seconds since the Unix epoch
  int64_t modifiedTime = 0;
  //! Stays the same for as long as the file exists
  uint64_t fileId = 0;
};

struct DirectoryEntry
{
  std::string name;
  FileHandle handle{};
  Attributes attributes;
};

//! The files a Server makes available
/*!
Called from the threads of the server, several at a time. A call may block
while it waits for data, but should answer with Status::TryLater rather than
keep the system waiting for long.
*/
class IFileSystem
{
public:
  virtual ~IFileSystem() = default;

  virtual FileHandle rootHandle() = 0;
  virtual Status getAttributes(const FileHandle &handle, Attributes &attributes) = 0;

  //! Find \p name in a directory, ".." names the directory above
  virtual Status
  lookup(const FileHandle &directory, const std::string &name, FileHandle &handle, Attributes &attributes) = 0;

  //! List a directory, without the entries for the directory itself and the one above
  /*!
  Returns at most \p limit entries, starting with the one at position
  \p first. \p complete tells that the last entry of the directory is among
  them. \p version changes whenever what the directory holds changes: a
  position is one in the directory of that version.
  */
  virtual Status readDirectory(
      const FileHandle &directory, uint64_t first, size_t limit, std::vector<DirectoryEntry> &entries, bool &complete,
      uint64_t &version
  ) = 0;

  //! Read from a file. \p endOfFile tells that nothing follows the returned data.
  virtual Status read(const FileHandle &file, uint64_t offset, uint32_t count, std::string &data, bool &endOfFile) = 0;
};

//! A read-only NFS version 3 server for the loopback interface
/*!
Lets the system mount an IFileSystem as a volume, without any kernel
extension: the system's own NFS client connects to this server over TCP on
127.0.0.1. Implements the MOUNT and NFS programs on one port, as far as a
read-only mount needs them.

Only the mount this process sets up is meant to succeed. The handle of the
root directory is given once, to the first client that knows the export
path. File handles can not be guessed, and those of one server mean
nothing to another: a volume that is mounted again gets new ones.
*/
class Server
{
public:
  //! Largest amount of file data in one read
  static constexpr uint32_t s_maxRead = 1024 * 1024;

  explicit Server(IFileSystem &fileSystem);
  ~Server();
  Server(Server const &) = delete;
  Server &operator=(Server const &) = delete;

  //! Start listening. \p exportPath is the path a client has to mount.
  bool start(const std::string &exportPath);

  //! Stop listening and drop every connection
  /*!
  Waits for the calls that are being answered. Calls into the file system
  that wait for something must be woken first.
  */
  void stop();

  //! The TCP port on 127.0.0.1, zero if not started
  uint16_t port() const;

  //! Whether a mount request is still answered with the root handle, only one ever is
  void setMountAllowed(bool allowed);

  //! Number of calls handled so far, for tests
  uint64_t callCount() const;

private:
  struct Connection;

  void acceptLoop();
  void receiveLoop(const std::shared_ptr<Connection> &connection);
  //! The last thing a thread of a connection or a call does with this object
  void threadEnded();
  void handleRecord(const std::shared_ptr<Connection> &connection, std::string record);
  std::string handleCall(const std::string &record);
  bool mountCall(uint32_t procedure, class XdrReader &arguments, class XdrWriter &result);
  bool nfsCall(uint32_t procedure, class XdrReader &arguments, class XdrWriter &result);
  //! A handle of the file system as this server tells it, and back
  FileHandle outward(const FileHandle &handle) const;
  FileHandle inward(const FileHandle &handle) const;

  IFileSystem &m_fileSystem;
  //! Makes the handles this server tells its own
  FileHandle m_mask{};
  std::string m_exportPath;
  std::atomic<bool> m_mountAllowed{true};
  std::atomic<bool> m_stopping{false};
  std::atomic<uint64_t> m_calls{0};
  int m_listenSocket = -1;
  //! Written to in order to wake the thread that accepts connections
  int m_wakePipe[2] = {-1, -1};
  uint16_t m_port = 0;
  std::thread m_acceptThread;

  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::vector<std::shared_ptr<Connection>> m_connections;
  //! Calls that run on a thread of their own
  int m_runningCalls = 0;
  //! Threads of connections and calls that still use this object
  int m_threads = 0;
};

} // namespace deskflow::nfs
