/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QString>

#include <cstdint>
#include <memory>

namespace deskflow {

//! What is at a local path
struct LocalEntry
{
  enum class Type
  {
    Missing, //!< Nothing is there, or it can not be looked at
    File,
    Directory,
    Link, //!< A symbolic link, a junction or a mount point: it may lead anywhere
    Other //!< A device, a pipe or a socket
  };

  Type type = Type::Missing;
  //! Size of a file in bytes
  uint64_t size = 0;
  //! Seconds since the Unix epoch
  int64_t modifiedTime = 0;
};

//! Looks at local files the way sharing them needs it
/*!
Nothing here follows a link at the end of a path or opens a file. So macOS does
not ask the user for permission to look, and a Windows shortcut is a file like
any other: Qt describes what a shortcut leads to instead, and resolves neither
junctions nor mount points in a canonical path.
*/
namespace LocalFileSystem {

//! What is at \p path, an absolute path with "/" between directories
LocalEntry inspect(const QString &path);

//! Make the calling thread act as the user logged on to the session of this process
/*!
On Windows the core may run as SYSTEM, which may read what the user may not.
Files are read for the user, so the thread then acts as that user. Returns
false if that is needed and not possible, nothing may be read then. Does
nothing elsewhere, or when the process runs as a user.
*/
bool actAsSessionUser();

} // namespace LocalFileSystem

//! The entries of a local directory, one at a time
class LocalDirectory
{
public:
  explicit LocalDirectory(const QString &path);
  ~LocalDirectory();
  LocalDirectory(LocalDirectory const &) = delete;
  LocalDirectory &operator=(LocalDirectory const &) = delete;

  //! False if the directory can not be read
  bool isOpen() const;

  //! Get the next entry, without "." and ".."
  /*!
  Returns false when there is none, at the end or because listing failed half
  way: failed() tells the two apart. A link is reported as a link.
  */
  bool next(QString &name, LocalEntry &entry);

  //! True if the directory could not be read to its end
  bool failed() const;

  //! True once every entry was handed out, or listing failed
  bool atEnd() const;

private:
  struct Handle;
  std::unique_ptr<Handle> m_handle;
  bool m_open = false;
  bool m_failed = false;
  bool m_atEnd = false;
};

} // namespace deskflow
