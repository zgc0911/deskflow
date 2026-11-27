/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardFileList.h"

class IClipboard;

#include <memory>
#include <string>
#include <vector>

class OSXPasteboardFiles;

namespace deskflow {
class OSXRemoteFileVolume;
}

//! The copied files part of the macOS clipboard
/*!
Files copied on this computer are read from the pasteboard as paths. Files
copied on another computer are shown on a volume and put on the pasteboard
with their paths there. Every method is called on the thread that runs the
event queue and returns without waiting for the volume.
*/
class OSXClipboardFiles
{
public:
  OSXClipboardFiles(
      std::unique_ptr<OSXPasteboardFiles> pasteboard, std::unique_ptr<deskflow::OSXRemoteFileVolume> volume
  );
  //! Files this put on the pasteboard are taken off, they can't be pasted once the volume is gone
  ~OSXClipboardFiles();
  OSXClipboardFiles(OSXClipboardFiles const &) = delete;
  OSXClipboardFiles &operator=(OSXClipboardFiles const &) = delete;

  //! Put the files of another computer on the pasteboard
  /*!
  Returns false if none of them can be shown, the pasteboard is not changed
  then. The files replace what else the clipboard holds: an item on the
  pasteboard is a file or something else. The system looks at the files
  while they are put there, which the volume answers from \p list.
  */
  bool put(const deskflow::ClipboardFileList &list);

  //! Put the files a clipboard of another computer holds on the pasteboard, false if it holds none
  bool put(const IClipboard &clipboard);

  //! The files copied on this computer, those of other computers left out
  bool get(std::vector<std::string> &paths) const;

  //! True if the pasteboard holds what put() wrote, not something the user copied since
  bool holdsOwnWrite() const;

  //! Counts the changes of the pasteboard
  long changeCount() const;

  deskflow::OSXRemoteFileVolume &volume();

private:
  std::unique_ptr<OSXPasteboardFiles> m_pasteboard;
  std::unique_ptr<deskflow::OSXRemoteFileVolume> m_volume;
  //! What put() wrote last, and the change count of the pasteboard it made
  std::vector<std::string> m_written;
  long m_writtenAt = -1;
};
