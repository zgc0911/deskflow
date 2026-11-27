/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

class IClipboard;
struct IDataObject;

namespace deskflow {
class RemoteFiles;
}

//! Writes the clipboard on a thread of its own, files of other computers included
/*!
Files copied on another computer are put on the clipboard as virtual files:
the application that pastes, usually Explorer, asks for the descriptions and
contents of the files through COM, and that may take as long as the other
computer takes to answer. The calls arrive on the thread that put the data
on the clipboard, so that thread is not the one that runs the event queue,
which has to keep handling the network meanwhile.

Every change of the clipboard goes through that thread, in the order they
were asked for: a later clipboard can never be overtaken by an earlier one.
What is put on the clipboard is marked as Deskflow's, so that the computer
does not take it for something the user copied.
*/
class MSWindowsClipboardFiles
{
public:
  explicit MSWindowsClipboardFiles(std::shared_ptr<deskflow::RemoteFiles> files);
  ~MSWindowsClipboardFiles();
  MSWindowsClipboardFiles(MSWindowsClipboardFiles const &) = delete;
  MSWindowsClipboardFiles &operator=(MSWindowsClipboardFiles const &) = delete;

  //! Replace what the clipboard holds, nullptr empties it
  /*!
  Returns at once, the clipboard changes a moment later. A clipboard with a
  list of files offers the files, next to the other formats it holds.
  */
  void set(const IClipboard *clipboard);

  //! Wait until every change asked for so far is done, for tests
  bool waitUntilDone(int timeoutMs) const;

  //! Number of changes that put files on the clipboard, for tests
  uint64_t filesPut() const;

  //! Read the paths of the files copied on this computer
  /*!
  Opens the clipboard for \p window. Files that another computer copied are
  not reported, they are not real files here.
  */
  static bool readFiles(HWND window, std::vector<std::string> &paths);

  //! The format that marks the clipboard as Deskflow's
  static UINT ownershipFormat();

private:
  using Job = std::function<void()>;

  void run();
  void post(Job job);
  void putFiles(const IClipboard &clipboard);
  void putFormats(const IClipboard &clipboard);
  void empty();
  //! Forget the data object that was put on the clipboard last
  void forgetCurrent();

  std::shared_ptr<deskflow::RemoteFiles> m_files;

  mutable std::mutex m_mutex;
  mutable std::condition_variable m_changed;
  std::deque<Job> m_jobs;
  bool m_stop = false;
  bool m_ready = false;
  uint64_t m_posted = 0;
  uint64_t m_done = 0;
  //! Wakes the thread when a job is posted
  HANDLE m_wake = nullptr;

  //! Skips writing the same clipboard twice in a row, once for each clipboard ID
  std::string m_lastData;
  //! The clipboard sequence number after the last write, zero while it is not known
  std::atomic<DWORD> m_sequenceAfterWrite{0};
  std::atomic<uint64_t> m_filesPut{0};

  // used by the thread only
  HWND m_window = nullptr;
  IDataObject *m_current = nullptr;

  std::thread m_thread;
};
