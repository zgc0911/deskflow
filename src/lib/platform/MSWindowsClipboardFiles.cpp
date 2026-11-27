/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboardFiles.h"

#include "base/Log.h"
#include "deskflow/Clipboard.h"
#include "deskflow/ClipboardFileList.h"
#include "platform/MSWindowsClipboard.h"
#include "platform/MSWindowsClipboardBitmapConverter.h"
#include "platform/MSWindowsClipboardHTMLConverter.h"
#include "platform/MSWindowsClipboardUTF16Converter.h"
#include "platform/MSWindowsRemoteFileSource.h"
#include "platform/MSWindowsVirtualFileDataObject.h"

#include <QString>

#include <ShellAPI.h>
#include <ole2.h>

namespace {

const wchar_t *const kWindowName = L"DeskflowClipboardFiles";

//! Copy what a converter made into the data object, it is offered from memory
void addConverted(
    MSWindowsVirtualFileDataObject &object, const IMSWindowsClipboardConverter &converter, const std::string &data
)
{
  HANDLE converted = converter.fromIClipboard(data);
  if (converted == nullptr) {
    return;
  }
  const SIZE_T size = GlobalSize(converted);
  const void *bytes = GlobalLock(converted);
  if (bytes != nullptr) {
    object.addGlobalFormat(converter.getWin32Format(), bytes, size);
    GlobalUnlock(converted);
  }
  GlobalFree(converted);
}

} // namespace

MSWindowsClipboardFiles::MSWindowsClipboardFiles(std::shared_ptr<deskflow::RemoteFiles> files)
    : m_files(std::move(files)),
      m_wake(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
  m_thread = std::thread([this] { run(); });
  std::unique_lock lock{m_mutex};
  m_changed.wait(lock, [this] { return m_ready; });
}

MSWindowsClipboardFiles::~MSWindowsClipboardFiles()
{
  {
    std::scoped_lock lock{m_mutex};
    m_stop = true;
  }
  SetEvent(m_wake);
  if (m_thread.joinable()) {
    m_thread.join();
  }
  CloseHandle(m_wake);
}

UINT MSWindowsClipboardFiles::ownershipFormat()
{
  return MSWindowsClipboard::getOwnershipFormat();
}

uint64_t MSWindowsClipboardFiles::filesPut() const
{
  return m_filesPut;
}

void MSWindowsClipboardFiles::post(Job job)
{
  {
    std::scoped_lock lock{m_mutex};
    m_jobs.push_back(std::move(job));
    ++m_posted;
  }
  SetEvent(m_wake);
}

bool MSWindowsClipboardFiles::waitUntilDone(int timeoutMs) const
{
  std::unique_lock lock{m_mutex};
  return m_changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return m_done == m_posted; });
}

void MSWindowsClipboardFiles::set(const IClipboard *clipboard)
{
  if (clipboard == nullptr) {
    m_lastData.clear();
    post([this] { empty(); });
    return;
  }

  // the clipboard of the caller is gone by the time the thread gets to it
  auto copy = std::make_shared<Clipboard>();
  Clipboard::copy(copy.get(), clipboard);
  std::string data = copy->marshall();

  // the same clipboard arrives once for each clipboard ID, write it only once unless it was replaced since
  const DWORD written = m_sequenceAfterWrite;
  if (data == m_lastData && written != 0 && written == GetClipboardSequenceNumber()) {
    return;
  }
  m_lastData = std::move(data);
  m_sequenceAfterWrite = 0;

  bool hasFiles = false;
  if (copy->open(copy->getTime())) {
    hasFiles = copy->has(IClipboard::Format::FileList);
    copy->close();
  }
  post([this, copy, hasFiles] {
    if (hasFiles) {
      putFiles(*copy);
    } else {
      putFormats(*copy);
    }
  });
}

void MSWindowsClipboardFiles::run()
{
  const HRESULT initialized = OleInitialize(nullptr);
  if (FAILED(initialized)) {
    LOG_ERR(
        "can't initialize OLE for the clipboard, files can't be pasted: 0x%08x", static_cast<unsigned>(initialized)
    );
  }
  // a message only window, it owns the clipboard when this thread writes it
  m_window = CreateWindowExW(
      0, L"STATIC", kWindowName, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr
  );
  {
    std::scoped_lock lock{m_mutex};
    m_ready = true;
  }
  m_changed.notify_all();

  for (;;) {
    std::deque<Job> jobs;
    bool stop = false;
    {
      std::scoped_lock lock{m_mutex};
      jobs.swap(m_jobs);
      stop = m_stop;
    }
    for (auto &job : jobs) {
      job();
      {
        std::scoped_lock lock{m_mutex};
        ++m_done;
      }
      m_changed.notify_all();
    }
    if (stop) {
      break;
    }

    // calls from other applications arrive as messages, they are handled while waiting
    const DWORD woken = MsgWaitForMultipleObjectsEx(1, &m_wake, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (woken == WAIT_OBJECT_0 + 1) {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    } else if (woken == WAIT_FAILED) {
      LOG_ERR("the clipboard thread can't wait: %d", static_cast<int>(GetLastError()));
      break;
    }
  }

  // the files can't be fetched once this process is gone: they are taken off the clipboard.
  // never flushed, that would fetch every one of them.
  if (m_current != nullptr && OleIsCurrentClipboard(m_current) == S_OK) {
    OleSetClipboard(nullptr);
  }
  forgetCurrent();
  if (m_window != nullptr) {
    DestroyWindow(m_window);
  }
  if (SUCCEEDED(initialized)) {
    OleUninitialize();
  }
}

void MSWindowsClipboardFiles::forgetCurrent()
{
  if (m_current != nullptr) {
    m_current->Release();
    m_current = nullptr;
  }
}

void MSWindowsClipboardFiles::putFiles(const IClipboard &clipboard)
{
  deskflow::ClipboardFileList list;
  std::string fileList;
  std::string text;
  std::string html;
  std::string bitmap;
  if (clipboard.open(clipboard.getTime())) {
    using enum IClipboard::Format;
    fileList = clipboard.get(FileList);
    text = clipboard.has(Text) ? clipboard.get(Text) : "";
    html = clipboard.has(HTML) ? clipboard.get(HTML) : "";
    bitmap = clipboard.has(Bitmap) ? clipboard.get(Bitmap) : "";
    clipboard.close();
  }
  if (!deskflow::ClipboardFileList::unmarshall(fileList, list)) {
    LOG_WARN("the list of copied files is not valid, the files can't be pasted");
    putFormats(clipboard);
    return;
  }

  auto source = std::make_shared<MSWindowsRemoteFileSource>(m_files, list);
  auto *object = new MSWindowsVirtualFileDataObject(source);
  // what else was copied with the files, and the mark that it is Deskflow's
  const BYTE mark = 0;
  object->addGlobalFormat(ownershipFormat(), &mark, sizeof(mark));
  if (!text.empty()) {
    addConverted(*object, MSWindowsClipboardUTF16Converter(), text);
  }
  if (!html.empty()) {
    addConverted(*object, MSWindowsClipboardHTMLConverter(), html);
  }
  if (!bitmap.empty()) {
    addConverted(*object, MSWindowsClipboardBitmapConverter(), bitmap);
  }

  const HRESULT result = OleSetClipboard(object);
  if (FAILED(result)) {
    LOG_WARN("can't put the copied files on the clipboard: 0x%08x", static_cast<unsigned>(result));
    object->Release();
    putFormats(clipboard);
    return;
  }
  // the clipboard holds a reference of its own
  forgetCurrent();
  m_current = object;
  m_sequenceAfterWrite = GetClipboardSequenceNumber();
  ++m_filesPut;
  LOG_DEBUG("put %d files copied on the other computer on the clipboard", static_cast<int>(list.entries.size()));
}

void MSWindowsClipboardFiles::putFormats(const IClipboard &clipboard)
{
  MSWindowsClipboard target(m_window);
  if (!Clipboard::copy(&target, &clipboard)) {
    LOG_WARN("failed to set the clipboard");
  }
  // emptying the clipboard dropped what was there before
  forgetCurrent();
  m_sequenceAfterWrite = GetClipboardSequenceNumber();
}

void MSWindowsClipboardFiles::empty()
{
  MSWindowsClipboard target(m_window);
  if (target.open(0)) {
    target.empty();
    target.close();
  }
  forgetCurrent();
  m_sequenceAfterWrite = GetClipboardSequenceNumber();
}

bool MSWindowsClipboardFiles::readFiles(HWND window, std::vector<std::string> &paths)
{
  paths.clear();
  // files of other computers are virtual: they have no paths and are never on the clipboard this way
  if (!IsClipboardFormatAvailable(CF_HDROP)) {
    return false;
  }

  bool opened = false;
  for (int attempt = 0; attempt < 5 && !opened; ++attempt) {
    opened = OpenClipboard(window) != FALSE;
    if (!opened) {
      Sleep(5);
    }
  }
  if (!opened) {
    LOG_WARN("can't open the clipboard to read the copied files: %d", static_cast<int>(GetLastError()));
    return false;
  }

  auto *drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
  if (drop != nullptr) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT index = 0; index < count; ++index) {
      const UINT length = DragQueryFileW(drop, index, nullptr, 0);
      std::wstring path(length + 1, L'\0');
      if (DragQueryFileW(drop, index, path.data(), length + 1) == length && length > 0) {
        path.resize(length);
        paths.push_back(QString::fromStdWString(path).toStdString());
      }
    }
  }
  CloseClipboard();
  return !paths.empty();
}
