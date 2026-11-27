/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>
#include <vector>

//! Files on a pasteboard, as paths
/*!
Finder and other programs that copy files put one file URL on the
pasteboard for each file. Reading them does not look at the files. Writing
does: the system looks at each file to decide what else to offer for it, so
the files must answer at once.
*/
class OSXPasteboardFiles
{
public:
  //! The general pasteboard, or one of its own that is thrown away with this object
  explicit OSXPasteboardFiles(bool ownPasteboard = false);

  //! A pasteboard with a name, which another object can use too. For tests.
  /*!
  \p releaseWhenDone throws it away with this object.
  */
  OSXPasteboardFiles(const std::string &name, bool releaseWhenDone);
  ~OSXPasteboardFiles();
  OSXPasteboardFiles(OSXPasteboardFiles const &) = delete;
  OSXPasteboardFiles &operator=(OSXPasteboardFiles const &) = delete;

  //! The paths of the files on the pasteboard, in UTF-8. False if there are none.
  bool read(std::vector<std::string> &paths) const;

  //! Replace what is on the pasteboard with files
  /*!
  \p isDirectory tells for each path whether it is a directory. The
  pasteboard is marked as being for this computer only: it is not handed to
  other devices of the user, which would read all the files right away.
  */
  long write(const std::vector<std::string> &paths, const std::vector<bool> &isDirectory);

  //! Counts the changes of the pasteboard, write() returns the count it made, or -1 if it failed
  long changeCount() const;

  //! Take everything off the pasteboard
  void clear();

  //! Replace what is on the pasteboard with text, as another program would. For tests.
  void writeText(const std::string &text);

  //! Put file URLs on the pasteboard the way Finder does, with references to the files. For tests.
  void writeFileReferences(const std::vector<std::string> &paths);

private:
  //! The NSPasteboard, retained
  void *m_pasteboard = nullptr;
  bool m_own = false;
};
