/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXClipboardFiles.h"

#include "base/Log.h"
#include "platform/OSXPasteboardFiles.h"
#include "platform/OSXRemoteFileVolume.h"

OSXClipboardFiles::OSXClipboardFiles(
    std::unique_ptr<OSXPasteboardFiles> pasteboard, std::unique_ptr<deskflow::OSXRemoteFileVolume> volume
)
    : m_pasteboard(std::move(pasteboard)),
      m_volume(std::move(volume))
{
}

OSXClipboardFiles::~OSXClipboardFiles() = default;

deskflow::OSXRemoteFileVolume &OSXClipboardFiles::volume()
{
  return *m_volume;
}

bool OSXClipboardFiles::put(const deskflow::ClipboardFileList &list)
{
  // shown on the volume, the files are fetched when a program reads them, which is when they are pasted
  const std::vector<std::string> shown = m_volume->addFiles(list);
  std::vector<std::string> paths;
  std::vector<bool> isDirectory;
  for (size_t i = 0; i < shown.size() && i < list.entries.size(); ++i) {
    if (!shown[i].empty()) {
      paths.push_back(shown[i]);
      isDirectory.push_back(list.entries[i].isDirectory);
    }
  }
  if (paths.empty()) {
    LOG_WARN("the files copied on the other computer can't be pasted here");
    return false;
  }
  if (paths.size() < list.entries.size()) {
    LOG_WARN(
        "%d of the files copied on the other computer can't be pasted here, their names are too long or not valid",
        static_cast<int>(list.entries.size() - paths.size())
    );
  }

  // the same files arrive once for each clipboard, the pasteboard holds them already
  if (paths == m_written && holdsOwnWrite()) {
    return true;
  }
  m_writtenAt = m_pasteboard->write(paths, isDirectory);
  if (m_writtenAt < 0) {
    LOG_WARN("failed to put the copied files on the pasteboard");
    m_written.clear();
    return false;
  }
  m_written = paths;
  LOG_DEBUG("put %d files copied on the other computer on the pasteboard", static_cast<int>(paths.size()));
  return true;
}

bool OSXClipboardFiles::get(std::vector<std::string> &paths) const
{
  paths.clear();
  std::vector<std::string> found;
  if (!m_pasteboard->read(found)) {
    return false;
  }
  for (auto &path : found) {
    // files of other computers, shown here: never offered back, and not to be looked at on this thread
    if (!m_volume->contains(path)) {
      paths.push_back(std::move(path));
    }
  }
  return !paths.empty();
}

bool OSXClipboardFiles::holdsOwnWrite() const
{
  return m_writtenAt >= 0 && m_pasteboard->changeCount() == m_writtenAt;
}
