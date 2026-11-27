/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXRemoteFileVolume.h"

#include "base/Log.h"
#include "platform/OSXNfsServer.h"

#include <QDir>
#include <QStandardPaths>
#include <QString>

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

#include <crt_externs.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace deskflow {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

//! Name of the mount point, which is what the system calls the volume
const char *const kVolumeName = "Deskflow";

// How the volume is mounted:
// - vers=3,tcp,port,mountport: the server in this process, found without asking a port mapper
// - rdonly,nolocks: nothing can be written or locked
// - noresvport: a user who is not root can not use a reserved port
// - soft,intr,timeo=20,retrans=3: if this process is gone a call fails after a few seconds and
//   does not hang. A soft read-only volume that does not answer for a minute is unmounted by
//   the system.
// - rsize: file data is asked for in pieces of 1 MiB
// - nobrowse: not shown in Finder and not indexed
// - actimeo=1,nonegnamecache: sizes and dates are asked for again after a second, so that a
//   file that was changed after it was copied is noticed
// - mutejukebox: a call that is told to come back later again and again does not make the
//   system take the volume for dead
const char *const kMountOptions = "vers=3,tcp,port=%1,mountport=%1,rdonly,nolocks,noresvport,soft,intr,timeo=20,"
                                  "retrans=3,rsize=1048576,nobrowse,actimeo=1,nonegnamecache,mutejukebox";

std::string randomHex(size_t bytes)
{
  std::vector<uint8_t> random(bytes);
  arc4random_buf(random.data(), random.size());
  static const char digits[] = "0123456789abcdef";
  std::string text;
  for (const uint8_t byte : random) {
    text.push_back(digits[byte >> 4]);
    text.push_back(digits[byte & 0x0f]);
  }
  return text;
}

std::string withoutTrailingSlashes(std::string path)
{
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

std::string resolved(const std::string &path)
{
  char buffer[PATH_MAX];
  return (::realpath(path.c_str(), buffer) != nullptr) ? std::string(buffer) : path;
}

bool isBelow(const std::string &path, const std::string &directory)
{
  return !directory.empty() && path.size() > directory.size() + 1 && path[directory.size()] == '/' &&
         path.compare(0, directory.size(), directory) == 0;
}

//! The process that made a directory named "<process id>-<random>", zero for any other name
pid_t ownerOf(const std::string &name)
{
  const size_t dash = name.find('-');
  if (dash == std::string::npos || dash == 0 || dash > 9) {
    return 0;
  }
  pid_t owner = 0;
  for (size_t i = 0; i < dash; ++i) {
    if (name[i] < '0' || name[i] > '9') {
      return 0;
    }
    owner = owner * 10 + (name[i] - '0');
  }
  return owner;
}

bool isRunning(pid_t process)
{
  return ::kill(process, 0) == 0 || errno != ESRCH;
}

//! The NFS volumes mounted somewhere below \p directory, from the list of mounts
/*!
The list is taken without asking the volumes themselves: one that no longer
answers would keep this waiting.
*/
std::vector<std::string> nfsMountsBelow(const std::string &directory)
{
  std::vector<std::string> found;
  const int count = ::getfsstat(nullptr, 0, MNT_NOWAIT);
  if (count <= 0) {
    return found;
  }
  std::vector<struct statfs> mounts(static_cast<size_t>(count) + 8);
  const int filled = ::getfsstat(mounts.data(), static_cast<int>(mounts.size() * sizeof(struct statfs)), MNT_NOWAIT);
  for (int i = 0; i < filled; ++i) {
    const struct statfs &mount = mounts[static_cast<size_t>(i)];
    if (std::strcmp(mount.f_fstypename, "nfs") == 0 && isBelow(mount.f_mntonname, directory)) {
      found.emplace_back(mount.f_mntonname);
    }
  }
  return found;
}

bool isMountedAt(const std::string &point)
{
  const std::string parent = point.substr(0, point.rfind('/'));
  for (const std::string &mount : nfsMountsBelow(parent)) {
    if (mount == point) {
      return true;
    }
  }
  return false;
}

//! Run a program and wait for it, returns its exit status or -1 if it could not be run or took too long
/*!
The program is killed when it takes longer than \p timeoutMs or when
\p cancel is set.
*/
int runCommand(
    const std::vector<std::string> &arguments, int timeoutMs, const std::atomic<bool> &cancel, std::string &output
)
{
  output.clear();
  int pipeEnds[2];
  if (::pipe(pipeEnds) != 0) {
    output = std::strerror(errno);
    return -1;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDERR_FILENO);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  // nothing of this process leaks into the program, such as the sockets of the server
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);

  std::vector<char *> argv;
  for (const std::string &argument : arguments) {
    argv.push_back(const_cast<char *>(argument.c_str()));
  }
  argv.push_back(nullptr);

  pid_t child = 0;
  const int spawned = ::posix_spawn(&child, argv[0], &actions, &attributes, argv.data(), *_NSGetEnviron());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  ::close(pipeEnds[1]);
  if (spawned != 0) {
    ::close(pipeEnds[0]);
    output = std::strerror(spawned);
    return -1;
  }

  // read what it says until it is done, or until it took too long
  const auto deadline = Clock::now() + milliseconds(timeoutMs);
  bool timedOut = false;
  for (;;) {
    const auto left = std::chrono::duration_cast<milliseconds>(deadline - Clock::now()).count();
    if (left <= 0 || cancel) {
      timedOut = true;
      break;
    }
    pollfd readable = {pipeEnds[0], POLLIN, 0};
    const int ready = ::poll(&readable, 1, static_cast<int>(std::min<long long>(left, 100)));
    if ((ready < 0 && errno == EINTR) || ready == 0) {
      continue;
    }
    if (ready < 0) {
      break;
    }
    char buffer[512];
    const ssize_t got = ::read(pipeEnds[0], buffer, sizeof(buffer));
    if (got <= 0) {
      break;
    }
    if (output.size() < 4096) {
      output.append(buffer, static_cast<size_t>(got));
    }
  }
  ::close(pipeEnds[0]);

  if (timedOut) {
    ::kill(child, SIGKILL);
  }
  int status = 0;
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  if (timedOut) {
    output = cancel ? "stopped" : "did not end in time";
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace

OSXRemoteFileVolume::OSXRemoteFileVolume(std::shared_ptr<RemoteFiles> files, Options options)
    : m_options(std::move(options)),
      m_base(withoutTrailingSlashes(m_options.directory)),
      m_fileSystem(std::make_unique<OSXRemoteFileSystem>(std::move(files), m_options.limits))
{
  if (m_options.owner == 0) {
    m_options.owner = static_cast<long>(::getpid());
  }
  // a local directory, quick to make: from here on paths below it can be told apart
  if (!QDir().mkpath(QString::fromStdString(m_base))) {
    LOG_ERR("can't create the directory for copied files: %s", m_base.c_str());
  }
  ::chmod(m_base.c_str(), 0700);
  m_resolvedBase = resolved(m_base);
  m_thread = std::thread([this] { run(); });
}

OSXRemoteFileVolume::~OSXRemoteFileVolume()
{
  unmount();
}

std::string OSXRemoteFileVolume::defaultDirectory()
{
  const QString caches = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
  return (caches + QStringLiteral("/Deskflow/RemoteFiles")).toStdString();
}

OSXRemoteFileSystem &OSXRemoteFileVolume::fileSystem()
{
  return *m_fileSystem;
}

uint16_t OSXRemoteFileVolume::serverPort() const
{
  std::scoped_lock lock{m_mutex};
  return m_port;
}

std::string OSXRemoteFileVolume::exportPath() const
{
  std::scoped_lock lock{m_mutex};
  return m_exportPath;
}

std::string OSXRemoteFileVolume::mountPoint() const
{
  std::scoped_lock lock{m_mutex};
  return m_mountPoint;
}

bool OSXRemoteFileVolume::isMounted() const
{
  std::scoped_lock lock{m_mutex};
  return m_mounting == Mounting::Mounted;
}

bool OSXRemoteFileVolume::waitUntilMounted(int timeoutMs) const
{
  std::unique_lock lock{m_mutex};
  m_changed.wait_for(lock, milliseconds(timeoutMs), [this] { return m_stop || m_mounting != Mounting::Pending; });
  return m_mounting == Mounting::Mounted;
}

bool OSXRemoteFileVolume::contains(const std::string &path) const
{
  return isBelow(path, m_base) || isBelow(path, m_resolvedBase);
}

std::vector<std::string> OSXRemoteFileVolume::addFiles(const ClipboardFileList &list)
{
  std::vector<std::string> paths(list.entries.size());
  std::string point;
  {
    std::scoped_lock lock{m_mutex};
    if (m_stop || m_mounting != Mounting::Mounted) {
      LOG_DEBUG("the volume for copied files is not mounted, the files can't be pasted");
      return paths;
    }
    point = m_mountPoint;
  }

  const std::vector<std::string> relative = m_fileSystem->addOffer(list);
  for (size_t i = 0; i < relative.size() && i < paths.size(); ++i) {
    if (!relative[i].empty()) {
      paths[i] = point + "/" + relative[i];
    }
  }
  return paths;
}

void OSXRemoteFileVolume::unmount()
{
  {
    std::scoped_lock lock{m_mutex};
    m_stop = true;
    m_cancel = true;
  }
  m_changed.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void OSXRemoteFileVolume::abandon()
{
  {
    std::scoped_lock lock{m_mutex};
    m_abandon = true;
    m_stop = true;
    m_cancel = true;
  }
  m_changed.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void OSXRemoteFileVolume::run()
{
  std::unique_lock lock{m_mutex};
  while (!m_stop) {
    if (m_mounting != Mounting::Mounted && Clock::now() >= m_nextAttempt) {
      lock.unlock();
      const bool mounted = mountNow();
      lock.lock();
      if (!mounted) {
        m_mounting = Mounting::Failed;
        m_nextAttempt = Clock::now() + milliseconds(m_options.retryAfterMs);
      }
      m_changed.notify_all();
      continue;
    }

    if (m_mounting == Mounting::Mounted) {
      const std::string point = m_mountPoint;
      lock.unlock();
      const bool stillMounted = isMountedAt(point);
      lock.lock();
      if (!stillMounted && !m_stop) {
        // the system gives a volume up if it got no answer for too long, and anybody can unmount it
        LOG_WARN("the volume for copied files is no longer mounted, mounting it again");
        m_mounting = Mounting::Pending;
        m_mountPoint.clear();
        m_port = 0;
        lock.unlock();
        discardMount(false);
        lock.lock();
        m_nextAttempt = Clock::now();
        m_changed.notify_all();
        continue;
      }
    }

    const auto wake =
        (m_mounting == Mounting::Mounted) ? Clock::now() + milliseconds(m_options.checkIntervalMs) : m_nextAttempt;
    m_changed.wait_until(lock, wake, [this] { return m_stop; });
  }

  const bool abandon = m_abandon;
  m_mounting = Mounting::Pending;
  m_mountPoint.clear();
  m_port = 0;
  lock.unlock();

  if (abandon) {
    // the server goes, the volume stays, as when a process dies
    m_fileSystem->interrupt();
    if (m_server != nullptr) {
      m_server->stop();
      m_server.reset();
    }
  } else {
    // calls that wait for another computer fail now, the system is not left waiting for them
    m_fileSystem->shutdown();
    discardMount(true);
  }

  lock.lock();
  m_changed.notify_all();
}

bool OSXRemoteFileVolume::mountNow()
{
  // a mount point is used once: one that the system gave up on can stay unusable
  removeLeftovers(m_base);
  const std::string ownDirectory = m_base + "/" + std::to_string(m_options.owner) + "-" + randomHex(4);
  const std::string point = ownDirectory + "/" + kVolumeName;
  if (::mkdir(ownDirectory.c_str(), 0700) != 0 || ::mkdir(point.c_str(), 0700) != 0) {
    LOG_ERR("can't create the mount point for copied files: %s", std::strerror(errno));
    ::rmdir(ownDirectory.c_str());
    return false;
  }

  // nobody can guess the path to mount, and after the mount below nobody else is let in
  const std::string exportPath = "/" + randomHex(16);
  auto server = std::make_unique<nfs::Server>(*m_fileSystem);
  if (!server->start(exportPath)) {
    ::rmdir(point.c_str());
    ::rmdir(ownDirectory.c_str());
    return false;
  }

  const std::string options = QString::fromLatin1(kMountOptions).arg(server->port()).toStdString();
  std::string output;
  const int status = runCommand(
      {m_options.mountCommand, "-o", options, "127.0.0.1:" + exportPath, point}, m_options.mountTimeoutMs, m_cancel,
      output
  );
  // the path the system itself uses, without symbolic links. found through the local
  // directory, the volume is not to be asked.
  const std::string mountedAt = resolved(ownDirectory) + "/" + kVolumeName;
  if (status != 0 || !isMountedAt(mountedAt)) {
    while (!output.empty() && (output.back() == '\n' || output.back() == ' ')) {
      output.pop_back();
    }
    LOG_ERR("can't mount the volume for copied files: %s", output.empty() ? "no reason given" : output.c_str());
    // in case the command got further than it could tell
    ::unmount(point.c_str(), MNT_FORCE);
    server->stop();
    ::rmdir(point.c_str());
    ::rmdir(ownDirectory.c_str());
    return false;
  }
  server->setMountAllowed(false);

  m_ownDirectory = ownDirectory;
  m_madeMountPoint = point;
  m_mountedAt = mountedAt;
  {
    std::scoped_lock lock{m_mutex};
    m_mounting = Mounting::Mounted;
    // programs are given the path the system itself uses, without symbolic links in it
    m_mountPoint = mountedAt;
    m_exportPath = exportPath;
    m_port = server->port();
  }
  m_server = std::move(server);
  LOG_DEBUG("files copied on other computers appear in %s", mountedAt.c_str());
  return true;
}

void OSXRemoteFileVolume::discardMount(bool unmountIt)
{
  if (unmountIt && !m_mountedAt.empty() && isMountedAt(m_mountedAt) && ::unmount(m_mountedAt.c_str(), MNT_FORCE) != 0) {
    LOG_WARN("can't unmount the volume for copied files: %s", std::strerror(errno));
  }
  m_mountedAt.clear();
  // the server last: the system says goodbye to it when it unmounts
  m_fileSystem->interrupt();
  if (m_server != nullptr) {
    m_server->stop();
    m_server.reset();
  }
  if (!m_madeMountPoint.empty()) {
    ::rmdir(m_madeMountPoint.c_str());
    m_madeMountPoint.clear();
  }
  if (!m_ownDirectory.empty()) {
    ::rmdir(m_ownDirectory.c_str());
    m_ownDirectory.clear();
  }
}

void OSXRemoteFileVolume::removeLeftovers(const std::string &directory)
{
  const std::string base = resolved(withoutTrailingSlashes(directory));

  // volumes of processes that are gone
  for (const std::string &point : nfsMountsBelow(base)) {
    const std::string below = point.substr(base.size() + 1);
    const pid_t owner = ownerOf(below.substr(0, below.find('/')));
    if (owner <= 0 || isRunning(owner)) {
      continue;
    }
    LOG_DEBUG("unmounting a volume for copied files that process %d left behind", static_cast<int>(owner));
    if (::unmount(point.c_str(), MNT_FORCE) != 0) {
      LOG_DEBUG("can't unmount %s: %s", point.c_str(), std::strerror(errno));
    }
  }

  // and the directories they were mounted in, which fails if one is still in use
  std::vector<std::string> names;
  if (DIR *entries = ::opendir(base.c_str()); entries != nullptr) {
    while (const dirent *entry = ::readdir(entries)) {
      names.emplace_back(entry->d_name);
    }
    ::closedir(entries);
  }
  for (const std::string &name : names) {
    const pid_t owner = ownerOf(name);
    if (owner > 0 && !isRunning(owner)) {
      ::rmdir((base + "/" + name + "/" + kVolumeName).c_str());
      ::rmdir((base + "/" + name).c_str());
    }
  }
}

} // namespace deskflow
