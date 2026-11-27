/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardFileList.h"
#include "platform/MSWindowsVirtualFileDataObject.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace deskflow {
class RemoteFiles;
}

//! The files another computer copied, offered as virtual files
/*!
When the application that pastes asks what there is, the whole tree is
listed from the other computer, as it is at that moment. A file is fetched
when it is copied.

Windows does not allow some names that other systems do. Such names are
changed, names that then differ only in case get a number, and entries
whose path is too long for the clipboard are left out with all they hold.
Requests to the other computer always use the original names.
*/
class MSWindowsRemoteFileSource : public IMSWindowsVirtualFileSource
{
public:
  struct Limits
  {
    //! More entries are left out
    size_t maxEntries = 65536;
    //! Room the other computer is given for file data that is on its way
    uint32_t window = 2 * 1024 * 1024;
  };

  MSWindowsRemoteFileSource(std::shared_ptr<deskflow::RemoteFiles> files, deskflow::ClipboardFileList list);
  MSWindowsRemoteFileSource(
      std::shared_ptr<deskflow::RemoteFiles> files, deskflow::ClipboardFileList list, const Limits &limits
  );

  uint64_t offerId() const;

  // IMSWindowsVirtualFileSource overrides
  bool list(std::vector<MSWindowsVirtualFileEntry> &entries) override;
  std::unique_ptr<IMSWindowsVirtualFileReader> open(size_t sourceId, uint64_t offset) override;

  //! A name changed so that Windows allows it
  static std::wstring windowsName(const std::string &name);

private:
  //! Where a file or directory is on the other computer
  struct Remote
  {
    uint32_t item = 0;
    std::string relativePath;
  };

  //! Names used in one directory so far, compared the way Windows does
  using UsedNames = std::set<std::wstring>;

  size_t remember(uint32_t item, const std::string &relativePath);
  static std::wstring uniqueName(const std::wstring &name, bool isDirectory, UsedNames &used);

  std::shared_ptr<deskflow::RemoteFiles> m_files;
  deskflow::ClipboardFileList m_list;
  Limits m_limits;

  std::mutex m_mutex;
  //! What each source identifier stands for, they are never used twice
  std::vector<Remote> m_remote;
};
