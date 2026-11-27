/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardFileList.h"
#include "platform/OSXRemoteFileSystem.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace deskflow {

class RemoteFiles;

namespace nfs {
class Server;
}

//! Mounts the files that other computers have copied as a read-only volume
/*!
A program that pastes files wants paths. The volume gives it paths to files
that are still on another computer: the NFS client of the system mounts a
server that runs inside this process and answers from an
OSXRemoteFileSystem. This takes no kernel extension and no administrator
rights, and nothing outside of this computer can reach the server.

Mounting, checking and unmounting happen on a thread of their own, so that
the thread that runs the event queue never waits for the mount command or
for the volume. The volume is mounted as soon as the object is made, and
mounted again if it goes away. Finder does not show it. If this process
dies the system unmounts it a minute after something next tries to use it,
and the next process that makes a volume in the same directory cleans up.
*/
class OSXRemoteFileVolume
{
public:
  struct Options
  {
    //! Where mount points are made, it should be one that only the user can enter
    std::string directory;
    OSXRemoteFileSystem::Limits limits;
    //! The program that mounts, tests replace it
    std::string mountCommand = "/sbin/mount_nfs";
    int mountTimeoutMs = 10000;
    //! How often it is checked that the volume is still mounted
    int checkIntervalMs = 5000;
    //! How long to wait before mounting again after it failed
    int retryAfterMs = 60000;
    //! The process the mount point is named after, zero for this one. Tests pretend to be another.
    long owner = 0;
  };

  OSXRemoteFileVolume(std::shared_ptr<RemoteFiles> files, Options options);
  ~OSXRemoteFileVolume();
  OSXRemoteFileVolume(OSXRemoteFileVolume const &) = delete;
  OSXRemoteFileVolume &operator=(OSXRemoteFileVolume const &) = delete;

  //! Make the files of a list available and tell where they are
  /*!
  Returns a path for each entry of the list, empty for an entry that can not
  be shown. All of them are empty while the volume is not mounted. Never
  waits for the volume.
  */
  std::vector<std::string> addFiles(const ClipboardFileList &list);

  //! True if \p path is in the directory of the volumes, of this process or of others
  /*!
  Such a path is never to be looked at from the thread that runs the event
  queue: it may be on a volume that no longer answers.
  */
  bool contains(const std::string &path) const;

  bool isMounted() const;

  //! Where the volume is mounted, empty if it is not
  std::string mountPoint() const;

  //! Wait until the volume is mounted or mounting failed, returns whether it is mounted. For tests.
  bool waitUntilMounted(int timeoutMs) const;

  //! Unmount for good. Files that are being pasted fail, the object is of no use afterwards.
  void unmount();

  //! Stop answering without unmounting, as if this process had died. For tests.
  void abandon();

  //! The files behind the volume, for tests
  OSXRemoteFileSystem &fileSystem();

  //! Port of the server behind the volume, zero if it does not run. For tests.
  uint16_t serverPort() const;

  //! The export path the server answers to, for tests
  std::string exportPath() const;

  //! The directory for mount points that is used when there is no reason for another
  static std::string defaultDirectory();

  //! Unmount and remove what processes that no longer run have left behind in \p directory
  static void removeLeftovers(const std::string &directory);

private:
  enum class Mounting
  {
    Pending,
    Mounted,
    Failed
  };

  void run();
  //! Runs on the thread of the volume, without the mutex
  bool mountNow();
  void discardMount(bool unmountIt);

  Options m_options;
  //! The directory of the volumes, as given and with symbolic links resolved
  std::string m_base;
  std::string m_resolvedBase;
  std::unique_ptr<OSXRemoteFileSystem> m_fileSystem;

  mutable std::mutex m_mutex;
  mutable std::condition_variable m_changed;
  Mounting m_mounting = Mounting::Pending;
  bool m_stop = false;
  //! Makes the mount command give up, read without the mutex
  std::atomic<bool> m_cancel{false};
  bool m_abandon = false;
  std::chrono::steady_clock::time_point m_nextAttempt{};
  std::string m_mountPoint;
  std::string m_exportPath;
  uint16_t m_port = 0;

  // used by the thread of the volume only
  std::unique_ptr<nfs::Server> m_server;
  //! The directory made for one mount, and the mount point in it, as made and as the system names it
  std::string m_ownDirectory;
  std::string m_madeMountPoint;
  std::string m_mountedAt;

  std::thread m_thread;
};

} // namespace deskflow
