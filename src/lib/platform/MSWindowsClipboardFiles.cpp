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

// another application may keep the clipboard open for a moment. it may even wait for this
// thread meanwhile, to render what it reads from the clipboard Deskflow put there.
const ULONGLONG kRetryForMs = 2000;
const DWORD kRetryDelayMs = 10;

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

//! Handle the messages of this thread for \p milliseconds, calls of other applications among them
void handleMessagesFor(DWORD milliseconds)
{
  const ULONGLONG until = GetTickCount64() + milliseconds;
  for (;;) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= until) {
      return;
    }
    MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(until - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
}

//! True if the clipboard can be opened now, it is closed again
bool clipboardFree(HWND window)
{
  if (!OpenClipboard(window)) {
    return false;
  }
  CloseClipboard();
  return true;
}

} // namespace

MSWindowsClipboardFiles::MSWindowsClipboardFiles(
    std::shared_ptr<deskflow::RemoteFiles> files, std::function<void()> written
)
    : m_files(std::move(files)),
      m_written(std::move(written)),
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

MSWindowsClipboardFiles::Ownership MSWindowsClipboardFiles::ownership() const
{
  {
    // jobs are posted by the thread that asks, so none comes in between
    std::scoped_lock lock{m_mutex};
    if (m_done != m_posted) {
      return Ownership::Unknown;
    }
  }
  return MSWindowsClipboard::isOwnedByDeskflow() ? Ownership::Deskflow : Ownership::Other;
}

uint64_t MSWindowsClipboardFiles::post(Job job)
{
  uint64_t number = 0;
  {
    std::scoped_lock lock{m_mutex};
    m_jobs.push_back(std::move(job));
    number = ++m_posted;
  }
  SetEvent(m_wake);
  return number;
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
    m_lastDataJob = 0;
    post([this] { return empty(); });
    return;
  }

  // the clipboard of the caller is gone by the time the thread gets to it
  auto copy = std::make_shared<Clipboard>();
  Clipboard::copy(copy.get(), clipboard);
  std::string data = copy->marshall();

  // the same clipboard arrives once for each clipboard ID, a moment apart. the first is written,
  // or is about to be: the second is left out, unless the first failed or was replaced since.
  if (data == m_lastData && m_lastDataJob != 0) {
    std::scoped_lock lock{m_mutex};
    const bool waiting = m_done < m_lastDataJob;
    const bool stillThere = m_writtenJob == m_lastDataJob && m_writtenSequence == GetClipboardSequenceNumber();
    if (waiting || stillThere) {
      return;
    }
  }

  bool hasFiles = false;
  if (copy->open(copy->getTime())) {
    hasFiles = copy->has(IClipboard::Format::FileList);
    copy->close();
  }
  m_lastData = std::move(data);
  m_lastDataJob = post([this, copy, hasFiles] { return hasFiles ? putFiles(*copy) : putFormats(*copy); });
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
    Job job;
    bool stop = false;
    {
      std::scoped_lock lock{m_mutex};
      if (!m_jobs.empty()) {
        job = std::move(m_jobs.front());
        m_jobs.pop_front();
      }
      stop = m_stop;
    }

    if (job) {
      const bool written = write(job);
      bool idle = false;
      {
        std::scoped_lock lock{m_mutex};
        ++m_done;
        if (written) {
          m_writtenJob = m_done;
          m_writtenSequence = GetClipboardSequenceNumber();
        }
        idle = m_done == m_posted;
      }
      m_changed.notify_all();
      if (idle && m_written) {
        m_written();
      }
      continue;
    }
    if (stop) {
      break;
    }

    // calls from other applications arrive as messages, they are handled while waiting
    const DWORD woken = MsgWaitForMultipleObjectsEx(1, &m_wake, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (woken == WAIT_OBJECT_0 + 1) {
      handleMessagesFor(0);
    } else if (woken == WAIT_FAILED) {
      LOG_ERR("the clipboard thread can't wait: %d", static_cast<int>(GetLastError()));
      break;
    }
  }

  // the files can't be fetched once this process is gone: they are taken off the clipboard.
  // never flushed, that would fetch every one of them.
  if (m_current != nullptr && OleIsCurrentClipboard(m_current) == S_OK && !write([this] { return takeOff(); })) {
    LOG_WARN("can't take the files of the other computer off the clipboard");
  }
  forgetCurrent();
  if (m_window != nullptr) {
    DestroyWindow(m_window);
  }
  if (SUCCEEDED(initialized)) {
    OleUninitialize();
  }
}

bool MSWindowsClipboardFiles::write(const Job &job)
{
  const ULONGLONG giveUpAt = GetTickCount64() + kRetryForMs;
  for (;;) {
    const bool written = clipboardFree(m_window) && job();
    // what was put there before is gone once the clipboard was emptied, whether or not the rest worked
    if (m_current != nullptr && OleIsCurrentClipboard(m_current) != S_OK) {
      forgetCurrent();
    }
    if (written) {
      return true;
    }

    {
      std::scoped_lock lock{m_mutex};
      if (!m_jobs.empty()) {
        LOG_DEBUG("can't change the clipboard now, a later change replaces this one");
        return false;
      }
    }
    if (GetTickCount64() >= giveUpAt) {
      LOG_WARN("can't change the clipboard, another application keeps it open");
      return false;
    }
    handleMessagesFor(kRetryDelayMs);
  }
}

void MSWindowsClipboardFiles::forgetCurrent()
{
  if (m_current != nullptr) {
    m_current->Release();
    m_current = nullptr;
  }
}

bool MSWindowsClipboardFiles::putFiles(const IClipboard &clipboard)
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
    return putFormats(clipboard);
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
    object->Release();
    if (result == CLIPBRD_E_CANT_OPEN) {
      // somebody else has the clipboard open, tried again in a moment
      return false;
    }
    LOG_WARN("can't put the copied files on the clipboard: 0x%08x", static_cast<unsigned>(result));
    return putFormats(clipboard);
  }
  // the clipboard holds a reference of its own
  forgetCurrent();
  m_current = object;
  ++m_filesPut;
  LOG_DEBUG("put %d files copied on the other computer on the clipboard", static_cast<int>(list.entries.size()));
  return true;
}

bool MSWindowsClipboardFiles::putFormats(const IClipboard &clipboard) const
{
  MSWindowsClipboard target(m_window);
  return Clipboard::copy(&target, &clipboard);
}

bool MSWindowsClipboardFiles::empty() const
{
  MSWindowsClipboard target(m_window);
  if (!target.open(0)) {
    return false;
  }
  const bool emptied = target.empty();
  target.close();
  return emptied;
}

bool MSWindowsClipboardFiles::takeOff()
{
  if (m_current == nullptr || OleIsCurrentClipboard(m_current) != S_OK) {
    return true;
  }
  return SUCCEEDED(OleSetClipboard(nullptr));
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
