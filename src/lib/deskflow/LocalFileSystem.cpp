/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/LocalFileSystem.h"

#include "base/Log.h"

#include <QDir>
#include <QFile>

#if defined(Q_OS_WIN)

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WtsApi32.h>

#include <cwchar>
#include <string>
#include <vector>

#else

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#endif

namespace deskflow {

#if defined(Q_OS_WIN)

namespace {

//! The form of a path that Windows takes as it is
/*!
Without the prefix Windows changes a path before it uses it: a long one fails,
and the name of a device, such as "nul.txt", leads to the device.
*/
std::wstring nativePath(const QString &path)
{
  const QString native = QDir::toNativeSeparators(path);
  if (native.startsWith(QLatin1String("\\\\?\\")) || native.startsWith(QLatin1String("\\\\.\\"))) {
    return native.toStdWString();
  }
  if (native.startsWith(QLatin1String("\\\\"))) {
    return (QStringLiteral("\\\\?\\UNC\\") + native.mid(2)).toStdWString();
  }
  return (QStringLiteral("\\\\?\\") + native).toStdWString();
}

int64_t toUnixSeconds(const FILETIME &time)
{
  // 100 nanosecond steps since 1601
  const uint64_t ticks = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  const uint64_t unixEpoch = 116444736000000000ULL;
  return (ticks < unixEpoch) ? 0 : static_cast<int64_t>((ticks - unixEpoch) / 10000000ULL);
}

LocalEntry describe(DWORD attributes, DWORD reparseTag, DWORD sizeHigh, DWORD sizeLow, const FILETIME &written)
{
  LocalEntry entry;
  // links lead elsewhere. other reparse points, such as files kept in the cloud, are what they seem
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(reparseTag)) {
    entry.type = LocalEntry::Type::Link;
  } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    entry.type = LocalEntry::Type::Directory;
  } else if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0) {
    entry.type = LocalEntry::Type::Other;
  } else {
    entry.type = LocalEntry::Type::File;
    entry.size = (static_cast<uint64_t>(sizeHigh) << 32) | sizeLow;
  }
  entry.modifiedTime = toUnixSeconds(written);
  return entry;
}

bool runsAsSystem()
{
  static const bool system = [] {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return false;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    bool isSystem = false;
    if (size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
      const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
      isSystem = IsWellKnownSid(user->User.Sid, WinLocalSystemSid) != FALSE;
    }
    CloseHandle(token);
    return isSystem;
  }();
  return system;
}

} // namespace

LocalEntry LocalFileSystem::inspect(const QString &path)
{
  const std::wstring native = nativePath(path);
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!GetFileAttributesExW(native.c_str(), GetFileExInfoStandard, &data)) {
    return {};
  }
  DWORD tag = 0;
  if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    // the kind of reparse point is only told by a search, which takes the path as a pattern.
    // a path that is looked at here has no wildcards: Windows does not allow them in names.
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileExW(native.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE) {
      return {};
    }
    FindClose(search);
    tag = found.dwReserved0;
  }
  return describe(data.dwFileAttributes, tag, data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime);
}

bool LocalFileSystem::actAsSessionUser()
{
  if (!runsAsSystem()) {
    return true;
  }

  // the user may log off and another log on, so who that is is asked again now and then
  thread_local ULONGLONG s_checkedAt = 0;
  thread_local bool s_acting = false;
  const ULONGLONG now = GetTickCount64();
  if (s_acting && now - s_checkedAt < 5000) {
    return true;
  }
  s_checkedAt = now;
  s_acting = false;

  DWORD session = 0;
  HANDLE user = nullptr;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || !WTSQueryUserToken(session, &user)) {
    LOG_WARN("files are not shared, no user is logged on: %d", static_cast<int>(GetLastError()));
    RevertToSelf();
    return false;
  }
  HANDLE impersonation = nullptr;
  const bool duplicated =
      DuplicateTokenEx(
          user, TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr, SecurityImpersonation, TokenImpersonation, &impersonation
      ) != FALSE;
  CloseHandle(user);
  if (duplicated) {
    s_acting = SetThreadToken(nullptr, impersonation) != FALSE;
    CloseHandle(impersonation);
  }
  if (!s_acting) {
    LOG_WARN("files are not shared, can't act as the logged on user: %d", static_cast<int>(GetLastError()));
    RevertToSelf();
  }
  return s_acting;
}

struct LocalDirectory::Handle
{
  HANDLE search = INVALID_HANDLE_VALUE;
  WIN32_FIND_DATAW found = {};
  //! \c found holds an entry that was not handed out yet
  bool pending = false;
};

LocalDirectory::LocalDirectory(const QString &path) : m_handle(std::make_unique<Handle>())
{
  const std::wstring pattern = nativePath(path) + L"\\*";
  m_handle->search = FindFirstFileExW(
      pattern.c_str(), FindExInfoBasic, &m_handle->found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH
  );
  if (m_handle->search != INVALID_HANDLE_VALUE) {
    m_open = true;
    m_handle->pending = true;
    return;
  }
  // only the root of a drive has neither "." nor "..", it is empty then
  m_open = GetLastError() == ERROR_FILE_NOT_FOUND;
  m_failed = !m_open;
  m_atEnd = true;
}

LocalDirectory::~LocalDirectory()
{
  if (m_handle->search != INVALID_HANDLE_VALUE) {
    FindClose(m_handle->search);
  }
}

bool LocalDirectory::next(QString &name, LocalEntry &entry)
{
  for (;;) {
    if (!m_handle->pending) {
      if (m_atEnd) {
        return false;
      }
      if (!FindNextFileW(m_handle->search, &m_handle->found)) {
        m_failed = GetLastError() != ERROR_NO_MORE_FILES;
        m_atEnd = true;
        return false;
      }
    }
    m_handle->pending = false;

    const WIN32_FIND_DATAW &found = m_handle->found;
    if (std::wcscmp(found.cFileName, L".") == 0 || std::wcscmp(found.cFileName, L"..") == 0) {
      continue;
    }
    name = QString::fromWCharArray(found.cFileName);
    entry = describe(
        found.dwFileAttributes, found.dwReserved0, found.nFileSizeHigh, found.nFileSizeLow, found.ftLastWriteTime
    );
    return true;
  }
}

#else

namespace {

LocalEntry describe(const struct stat &status)
{
  LocalEntry entry;
  if (S_ISLNK(status.st_mode)) {
    entry.type = LocalEntry::Type::Link;
  } else if (S_ISDIR(status.st_mode)) {
    entry.type = LocalEntry::Type::Directory;
  } else if (S_ISREG(status.st_mode)) {
    entry.type = LocalEntry::Type::File;
    entry.size = static_cast<uint64_t>(status.st_size);
  } else {
    entry.type = LocalEntry::Type::Other;
  }
  entry.modifiedTime = static_cast<int64_t>(status.st_mtime);
  return entry;
}

} // namespace

LocalEntry LocalFileSystem::inspect(const QString &path)
{
  struct stat status = {};
  if (lstat(QFile::encodeName(path).constData(), &status) != 0) {
    return {};
  }
  return describe(status);
}

bool LocalFileSystem::actAsSessionUser()
{
  return true;
}

struct LocalDirectory::Handle
{
  DIR *directory = nullptr;
};

LocalDirectory::LocalDirectory(const QString &path) : m_handle(std::make_unique<Handle>())
{
  m_handle->directory = opendir(QFile::encodeName(path).constData());
  m_open = m_handle->directory != nullptr;
  if (!m_open) {
    m_failed = true;
    m_atEnd = true;
  }
}

LocalDirectory::~LocalDirectory()
{
  if (m_handle->directory != nullptr) {
    closedir(m_handle->directory);
  }
}

bool LocalDirectory::next(QString &name, LocalEntry &entry)
{
  while (!m_atEnd) {
    errno = 0;
    const dirent *found = readdir(m_handle->directory);
    if (found == nullptr) {
      m_failed = errno != 0;
      m_atEnd = true;
      return false;
    }
    if (std::strcmp(found->d_name, ".") == 0 || std::strcmp(found->d_name, "..") == 0) {
      continue;
    }

    struct stat status = {};
    if (fstatat(dirfd(m_handle->directory), found->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
        // gone since the directory was read
        continue;
      }
      m_failed = true;
      m_atEnd = true;
      return false;
    }
    name = QFile::decodeName(found->d_name);
    entry = describe(status);
    return true;
  }
  return false;
}

#endif

bool LocalDirectory::isOpen() const
{
  return m_open;
}

bool LocalDirectory::failed() const
{
  return m_failed;
}

bool LocalDirectory::atEnd() const
{
  return m_atEnd;
}

} // namespace deskflow
