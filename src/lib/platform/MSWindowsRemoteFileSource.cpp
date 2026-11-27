/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsRemoteFileSource.h"

#include "base/Log.h"
#include "deskflow/FileTransfer.h"

#include <QString>

#include <deque>
#include <string_view>

using deskflow::ClipboardFileEntry;
using deskflow::FileTransferStatus;

namespace {

//! The shell keeps a path in a buffer of MAX_PATH characters
const size_t kMaxPath = MAX_PATH - 1;

const char *describe(FileTransferStatus status)
{
  switch (status) {
  case FileTransferStatus::Ok:
    return "ok";
  case FileTransferStatus::Cancelled:
    return "cancelled";
  case FileTransferStatus::Disconnected:
    return "the other computer is not connected";
  case FileTransferStatus::TimedOut:
    return "the other computer did not answer";
  case FileTransferStatus::Refused:
    return "file sharing is turned off";
  case FileTransferStatus::NotFound:
    return "no longer there";
  case FileTransferStatus::Failed:
    return "failed";
  }
  return "?";
}

std::string toUtf8(const std::wstring &text)
{
  return QString::fromStdWString(text).toStdString();
}

//! How Windows compares names: without regard to case
std::wstring folded(const std::wstring &name)
{
  return QString::fromStdWString(name).toCaseFolded().toStdWString();
}

bool isReservedName(const std::wstring &name)
{
  // the part before the first dot decides, whatever the case and spaces before the dot:
  // "con.txt" and "con .txt" are the console
  std::wstring base = folded(name.substr(0, name.find(L'.')));
  while (!base.empty() && base.back() == L' ') {
    base.pop_back();
  }
  static const wchar_t *const reserved[] = {L"con", L"prn", L"aux", L"nul", L"conin$", L"conout$"};
  for (const wchar_t *word : reserved) {
    if (base == word) {
      return true;
    }
  }
  // the ports take a digit, superscript ones too
  if (base.size() == 4 && (base.compare(0, 3, L"com") == 0 || base.compare(0, 3, L"lpt") == 0)) {
    const wchar_t number = base[3];
    return (number >= L'0' && number <= L'9') || number == L'\u00b9' || number == L'\u00b2' || number == L'\u00b3';
  }
  return false;
}

//! Reads one file of the other computer
class RemoteReader : public IMSWindowsVirtualFileReader
{
public:
  RemoteReader(std::unique_ptr<deskflow::RemoteFileReader> reader, std::string path)
      : m_reader(std::move(reader)),
        m_path(std::move(path))
  {
  }

  bool read(void *buffer, uint32_t size, uint32_t &bytesRead) override
  {
    const FileTransferStatus status = m_reader->read(buffer, size, bytesRead);
    if (status != FileTransferStatus::Ok) {
      LOG_WARN("can't read a file of the other computer, %s: %s", describe(status), m_path.c_str());
      return false;
    }
    return true;
  }

private:
  std::unique_ptr<deskflow::RemoteFileReader> m_reader;
  std::string m_path;
};

} // namespace

MSWindowsRemoteFileSource::MSWindowsRemoteFileSource(
    std::shared_ptr<deskflow::RemoteFiles> files, deskflow::ClipboardFileList list
)
    : MSWindowsRemoteFileSource(std::move(files), std::move(list), Limits())
{
}

MSWindowsRemoteFileSource::MSWindowsRemoteFileSource(
    std::shared_ptr<deskflow::RemoteFiles> files, deskflow::ClipboardFileList list, const Limits &limits
)
    : m_files(std::move(files)),
      m_list(std::move(list)),
      m_limits(limits)
{
}

uint64_t MSWindowsRemoteFileSource::offerId() const
{
  return m_list.offerId;
}

std::wstring MSWindowsRemoteFileSource::windowsName(const std::string &name)
{
  std::wstring changed = QString::fromStdString(name).toStdWString();
  for (wchar_t &character : changed) {
    if (character < 0x20 || std::wstring_view(L"<>:\"/\\|?*").find(character) != std::wstring_view::npos) {
      character = L'_';
    }
  }
  // names can't end with a dot or a space
  for (auto character = changed.rbegin(); character != changed.rend() && (*character == L'.' || *character == L' ');
       ++character) {
    *character = L'_';
  }
  if (changed.empty()) {
    return L"_";
  }
  if (isReservedName(changed)) {
    changed.insert(changed.begin(), L'_');
  }
  return changed;
}

std::wstring MSWindowsRemoteFileSource::uniqueName(const std::wstring &name, bool isDirectory, UsedNames &used)
{
  if (used.insert(folded(name)).second) {
    return name;
  }
  // "a.txt" and "A.txt" are the same name here: the second becomes "A (2).txt"
  const size_t dot = isDirectory ? std::wstring::npos : name.rfind(L'.');
  const bool hasExtension = dot != std::wstring::npos && dot > 0;
  const std::wstring stem = hasExtension ? name.substr(0, dot) : name;
  const std::wstring extension = hasExtension ? name.substr(dot) : std::wstring();
  for (int number = 2;; ++number) {
    const std::wstring candidate = stem + L" (" + std::to_wstring(number) + L")" + extension;
    if (used.insert(folded(candidate)).second) {
      return candidate;
    }
  }
}

size_t MSWindowsRemoteFileSource::remember(uint32_t item, const std::string &relativePath)
{
  std::scoped_lock lock{m_mutex};
  // listed again, the same file keeps its identifier
  const auto known = m_ids.find({item, relativePath});
  if (known != m_ids.end()) {
    return known->second;
  }
  m_remote.push_back({item, relativePath});
  const size_t id = m_remote.size() - 1;
  m_ids.emplace(std::make_pair(item, relativePath), id);
  return id;
}

bool MSWindowsRemoteFileSource::list(std::vector<MSWindowsVirtualFileEntry> &entries)
{
  entries.clear();

  // the copied entries as they are now: a file may have changed or gone since it was copied
  std::vector<ClipboardFileEntry> copied;
  const FileTransferStatus status = m_files->listOffer(m_list.offerId, copied);
  if (status != FileTransferStatus::Ok) {
    LOG_WARN("can't list the files copied on the other computer: %s", describe(status));
    return false;
  }
  if (copied.size() != m_list.entries.size()) {
    LOG_WARN(
        "the other computer told of %d copied files instead of %d", static_cast<int>(copied.size()),
        static_cast<int>(m_list.entries.size())
    );
    return false;
  }

  struct Directory
  {
    uint32_t item = 0;
    std::string remotePath;
    std::wstring windowsPath;
  };
  std::deque<Directory> directories;
  size_t leftOut = 0;

  const auto add = [&](uint32_t item, const std::string &remotePath, const std::wstring &windowsPath,
                       const ClipboardFileEntry &entry) {
    if (windowsPath.size() > kMaxPath) {
      LOG_WARN("can't paste a file, its path is too long for Windows: %s", toUtf8(windowsPath).c_str());
      ++leftOut;
      return;
    }
    if (entries.size() >= m_limits.maxEntries) {
      ++leftOut;
      return;
    }
    MSWindowsVirtualFileEntry described;
    described.relativePath = windowsPath;
    described.isDirectory = entry.isDirectory;
    described.size = entry.isDirectory ? 0 : entry.size;
    described.modifiedTime = entry.modifiedTime;
    described.sourceId = remember(item, remotePath);
    entries.push_back(std::move(described));
    if (entry.isDirectory) {
      directories.push_back({item, remotePath, windowsPath});
    }
  };

  UsedNames topNames;
  for (uint32_t item = 0; item < copied.size(); ++item) {
    const ClipboardFileEntry &entry = copied[item];
    if (entry.isMissing) {
      LOG_INFO("a copied file is no longer there: %s", entry.name.c_str());
      continue;
    }
    add(item, {}, uniqueName(windowsName(entry.name), entry.isDirectory, topNames), entry);
  }

  // directories before what they hold, as the shell wants it
  while (!directories.empty()) {
    if (entries.size() >= m_limits.maxEntries) {
      // nothing more fits: what these hold is not even asked for
      LOG_WARN(
          "too many copied files to paste, what %d of the copied directories hold is left out",
          static_cast<int>(directories.size())
      );
      directories.clear();
      break;
    }
    const Directory directory = std::move(directories.front());
    directories.pop_front();

    std::vector<ClipboardFileEntry> children;
    const FileTransferStatus listed =
        m_files->listDirectory(m_list.offerId, directory.item, directory.remotePath, children);
    if (listed == FileTransferStatus::NotFound) {
      // gone since, it is pasted empty
      LOG_INFO("a copied directory is no longer there: %s", toUtf8(directory.windowsPath).c_str());
      continue;
    }
    if (listed != FileTransferStatus::Ok) {
      LOG_WARN(
          "can't list a directory copied on the other computer, %s: %s", describe(listed),
          toUtf8(directory.windowsPath).c_str()
      );
      return false;
    }

    UsedNames names;
    for (const ClipboardFileEntry &child : children) {
      const std::string remotePath =
          directory.remotePath.empty() ? child.name : directory.remotePath + "/" + child.name;
      const std::wstring windowsPath =
          directory.windowsPath + L"\\" + uniqueName(windowsName(child.name), child.isDirectory, names);
      add(directory.item, remotePath, windowsPath, child);
    }
  }

  if (leftOut > 0) {
    LOG_WARN(
        "%d copied files can't be pasted here, there are too many or their paths are too long",
        static_cast<int>(leftOut)
    );
  }
  return true;
}

std::unique_ptr<IMSWindowsVirtualFileReader> MSWindowsRemoteFileSource::open(size_t sourceId, uint64_t offset)
{
  Remote remote;
  {
    std::scoped_lock lock{m_mutex};
    if (sourceId >= m_remote.size()) {
      return nullptr;
    }
    remote = m_remote[sourceId];
  }
  auto reader = m_files->openFile(m_list.offerId, remote.item, remote.relativePath, offset, m_limits.window);
  const std::string path =
      m_list.entries.at(remote.item).name + (remote.relativePath.empty() ? std::string() : "/" + remote.relativePath);
  return std::make_unique<RemoteReader>(std::move(reader), path);
}
