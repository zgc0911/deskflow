/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsVirtualFileDataObject.h"

#include "base/Log.h"

#include <QString>

#include <ShlObj.h>

#include <algorithm>
#include <cstring>

namespace {

//! The shell stores file names in a fixed MAX_PATH buffer
const size_t kMaxDescriptorPath = MAX_PATH - 1;

//! Seconds between 1601-01-01 and 1970-01-01
const uint64_t kFileTimeEpochOffset = 11644473600ULL;

FILETIME toFileTime(int64_t unixSeconds)
{
  const uint64_t ticks = (static_cast<uint64_t>(unixSeconds) + kFileTimeEpochOffset) * 10000000ULL;
  FILETIME fileTime;
  fileTime.dwLowDateTime = static_cast<DWORD>(ticks & 0xffffffffULL);
  fileTime.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  return fileTime;
}

//! File names are logged as UTF-8, the log is not wide character aware
std::string toUtf8(const std::wstring &text)
{
  return QString::fromStdWString(text).toStdString();
}

HGLOBAL copyToGlobal(const void *data, size_t size)
{
  // GlobalAlloc fails for a zero size, the consumer only needs a valid handle
  HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, std::max<size_t>(size, 1));
  if (global == nullptr) {
    return nullptr;
  }
  if (size > 0) {
    void *target = GlobalLock(global);
    if (target == nullptr) {
      GlobalFree(global);
      return nullptr;
    }
    std::memcpy(target, data, size);
    GlobalUnlock(global);
  }
  return global;
}

//! Stream over the contents of one virtual file
/*!
The stream aggregates the free threaded marshaler so that an application
copying on a background thread reads without going through the message loop
of the thread that owns the clipboard.
*/
class VirtualFileStream : public IStream
{
public:
  VirtualFileStream(
      std::shared_ptr<IMSWindowsVirtualFileSource> source, size_t sourceIndex, const MSWindowsVirtualFileEntry &entry
  )
      : m_source(std::move(source)),
        m_sourceIndex(sourceIndex),
        m_entry(entry)
  {
    CoCreateFreeThreadedMarshaler(static_cast<IStream *>(this), &m_marshaler);
  }

  VirtualFileStream(VirtualFileStream const &) = delete;
  VirtualFileStream &operator=(VirtualFileStream const &) = delete;

  // IUnknown
  IFACEMETHODIMP QueryInterface(REFIID riid, void **object) override
  {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ISequentialStream) || IsEqualIID(riid, IID_IStream)) {
      *object = static_cast<IStream *>(this);
      AddRef();
      return S_OK;
    }
    if (IsEqualIID(riid, IID_IMarshal) && m_marshaler != nullptr) {
      return m_marshaler->QueryInterface(riid, object);
    }
    return E_NOINTERFACE;
  }

  IFACEMETHODIMP_(ULONG) AddRef() override
  {
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
  }

  IFACEMETHODIMP_(ULONG) Release() override
  {
    const LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) {
      delete this;
    }
    return static_cast<ULONG>(count);
  }

  // ISequentialStream
  IFACEMETHODIMP Read(void *buffer, ULONG size, ULONG *bytesRead) override
  {
    if (bytesRead != nullptr) {
      *bytesRead = 0;
    }
    if (buffer == nullptr) {
      return STG_E_INVALIDPOINTER;
    }

    std::scoped_lock lock{m_mutex};
    if (m_failed) {
      return STG_E_READFAULT;
    }

    ULONG total = 0;
    auto *target = static_cast<uint8_t *>(buffer);
    while (total < size && m_position < m_entry.size) {
      if (m_reader == nullptr) {
        m_reader = m_source->open(m_sourceIndex, m_position);
        if (m_reader == nullptr) {
          LOG_WARN("can't open virtual file for reading: %s", toUtf8(m_entry.relativePath).c_str());
          m_failed = true;
          return STG_E_READFAULT;
        }
      }

      uint32_t got = 0;
      if (!m_reader->read(target + total, size - total, got)) {
        LOG_WARN("failed to read virtual file: %s", toUtf8(m_entry.relativePath).c_str());
        m_failed = true;
        m_reader.reset();
        return STG_E_READFAULT;
      }
      if (got == 0) {
        // the file is shorter than announced
        break;
      }
      total += got;
      m_position += got;
    }

    if (bytesRead != nullptr) {
      *bytesRead = total;
    }
    return (total == size) ? S_OK : S_FALSE;
  }

  IFACEMETHODIMP Write(const void *, ULONG, ULONG *) override
  {
    return STG_E_ACCESSDENIED;
  }

  // IStream
  IFACEMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *newPosition) override
  {
    std::scoped_lock lock{m_mutex};

    int64_t base = 0;
    switch (origin) {
    case STREAM_SEEK_SET:
      base = 0;
      break;
    case STREAM_SEEK_CUR:
      base = static_cast<int64_t>(m_position);
      break;
    case STREAM_SEEK_END:
      base = static_cast<int64_t>(m_entry.size);
      break;
    default:
      return STG_E_INVALIDFUNCTION;
    }

    const int64_t target = base + move.QuadPart;
    if (target < 0) {
      return STG_E_INVALIDFUNCTION;
    }

    if (static_cast<uint64_t>(target) != m_position) {
      // the reader is sequential, reopen at the new position on the next read
      m_reader.reset();
      m_position = static_cast<uint64_t>(target);
    }
    if (newPosition != nullptr) {
      newPosition->QuadPart = m_position;
    }
    return S_OK;
  }

  IFACEMETHODIMP SetSize(ULARGE_INTEGER) override
  {
    return STG_E_ACCESSDENIED;
  }

  IFACEMETHODIMP
  CopyTo(IStream *target, ULARGE_INTEGER size, ULARGE_INTEGER *bytesRead, ULARGE_INTEGER *bytesWritten) override
  {
    if (target == nullptr) {
      return STG_E_INVALIDPOINTER;
    }

    std::vector<uint8_t> buffer(256 * 1024);
    uint64_t totalRead = 0;
    uint64_t totalWritten = 0;
    HRESULT result = S_OK;
    while (totalRead < size.QuadPart) {
      const auto wanted = static_cast<ULONG>(std::min<uint64_t>(buffer.size(), size.QuadPart - totalRead));
      ULONG got = 0;
      result = Read(buffer.data(), wanted, &got);
      if (FAILED(result)) {
        break;
      }
      result = S_OK;
      if (got == 0) {
        break;
      }
      totalRead += got;

      ULONG written = 0;
      result = target->Write(buffer.data(), got, &written);
      totalWritten += written;
      if (FAILED(result) || written != got) {
        if (SUCCEEDED(result)) {
          result = STG_E_WRITEFAULT;
        }
        break;
      }
    }

    if (bytesRead != nullptr) {
      bytesRead->QuadPart = totalRead;
    }
    if (bytesWritten != nullptr) {
      bytesWritten->QuadPart = totalWritten;
    }
    return result;
  }

  IFACEMETHODIMP Commit(DWORD) override
  {
    return S_OK;
  }

  IFACEMETHODIMP Revert() override
  {
    return S_OK;
  }

  IFACEMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
  {
    return STG_E_INVALIDFUNCTION;
  }

  IFACEMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
  {
    return STG_E_INVALIDFUNCTION;
  }

  IFACEMETHODIMP Stat(STATSTG *stat, DWORD flags) override
  {
    if (stat == nullptr) {
      return STG_E_INVALIDPOINTER;
    }

    std::memset(stat, 0, sizeof(*stat));
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = m_entry.size;
    stat->grfMode = STGM_READ;
    if (m_entry.modifiedTime != 0) {
      stat->mtime = toFileTime(m_entry.modifiedTime);
    }

    if ((flags & STATFLAG_NONAME) == 0) {
      const auto separator = m_entry.relativePath.find_last_of(L'\\');
      const std::wstring name =
          (separator == std::wstring::npos) ? m_entry.relativePath : m_entry.relativePath.substr(separator + 1);
      const size_t bytes = (name.size() + 1) * sizeof(wchar_t);
      stat->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
      if (stat->pwcsName == nullptr) {
        return STG_E_INSUFFICIENTMEMORY;
      }
      std::memcpy(stat->pwcsName, name.c_str(), bytes);
    }
    return S_OK;
  }

  IFACEMETHODIMP Clone(IStream **) override
  {
    return E_NOTIMPL;
  }

private:
  // destroyed through Release()
  virtual ~VirtualFileStream()
  {
    if (m_marshaler != nullptr) {
      m_marshaler->Release();
    }
  }

  LONG m_refCount = 1;
  IUnknown *m_marshaler = nullptr;
  std::shared_ptr<IMSWindowsVirtualFileSource> m_source;
  size_t m_sourceIndex;
  MSWindowsVirtualFileEntry m_entry;

  std::mutex m_mutex;
  std::unique_ptr<IMSWindowsVirtualFileReader> m_reader;
  uint64_t m_position = 0;
  bool m_failed = false;
};

} // namespace

//
// MSWindowsVirtualFileDataObject
//

MSWindowsVirtualFileDataObject::MSWindowsVirtualFileDataObject(std::shared_ptr<IMSWindowsVirtualFileSource> source)
    : m_source(std::move(source))
{
  // tell the application that pastes to copy rather than move
  const DWORD effect = DROPEFFECT_COPY;
  addGlobalFormat(preferredDropEffectFormat(), &effect, sizeof(effect));
}

void MSWindowsVirtualFileDataObject::addGlobalFormat(UINT format, const void *data, size_t size)
{
  std::scoped_lock lock{m_mutex};
  const auto *bytes = static_cast<const uint8_t *>(data);
  m_globalFormats[format] = std::vector<uint8_t>(bytes, bytes + size);
}

bool MSWindowsVirtualFileDataObject::inAsyncOperation() const
{
  std::scoped_lock lock{m_mutex};
  return m_inOperation;
}

uint32_t MSWindowsVirtualFileDataObject::finishedAsyncOperations() const
{
  std::scoped_lock lock{m_mutex};
  return m_finishedOperations;
}

UINT MSWindowsVirtualFileDataObject::fileDescriptorFormat()
{
  static const UINT s_format = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
  return s_format;
}

UINT MSWindowsVirtualFileDataObject::fileContentsFormat()
{
  static const UINT s_format = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
  return s_format;
}

UINT MSWindowsVirtualFileDataObject::preferredDropEffectFormat()
{
  static const UINT s_format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
  return s_format;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::QueryInterface(REFIID riid, void **object)
{
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject)) {
    *object = static_cast<IDataObject *>(this);
  } else if (IsEqualIID(riid, __uuidof(IDataObjectAsyncCapability))) {
    *object = static_cast<IDataObjectAsyncCapability *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

IFACEMETHODIMP_(ULONG) MSWindowsVirtualFileDataObject::AddRef()
{
  return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

IFACEMETHODIMP_(ULONG) MSWindowsVirtualFileDataObject::Release()
{
  const LONG count = InterlockedDecrement(&m_refCount);
  if (count == 0) {
    delete this;
  }
  return static_cast<ULONG>(count);
}

bool MSWindowsVirtualFileDataObject::ensureListed()
{
  {
    std::scoped_lock lock{m_mutex};
    if (m_listed) {
      return !m_listFailed;
    }
  }

  // not under the lock, listing may take a while
  std::vector<MSWindowsVirtualFileEntry> entries;
  const bool ok = m_source->list(entries);

  std::scoped_lock lock{m_mutex};
  if (m_listed) {
    return !m_listFailed;
  }
  m_listed = true;
  m_listFailed = !ok;
  if (!ok) {
    LOG_WARN("failed to list the files offered on the clipboard");
    return false;
  }

  m_entries.clear();
  for (size_t index = 0; index < entries.size(); ++index) {
    if (entries[index].relativePath.empty()) {
      continue;
    }
    if (entries[index].relativePath.size() > kMaxDescriptorPath) {
      LOG_WARN("skipping file, path is too long for the clipboard: %s", toUtf8(entries[index].relativePath).c_str());
      continue;
    }
    m_entries.push_back({entries[index], index});
  }
  return true;
}

HRESULT MSWindowsVirtualFileDataObject::getFileDescriptor(STGMEDIUM *medium)
{
  if (!ensureListed()) {
    return E_FAIL;
  }

  std::scoped_lock lock{m_mutex};
  if (m_entries.empty()) {
    return DV_E_FORMATETC;
  }

  const size_t count = m_entries.size();
  const size_t bytes = sizeof(FILEGROUPDESCRIPTORW) + (count - 1) * sizeof(FILEDESCRIPTORW);
  HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (global == nullptr) {
    return E_OUTOFMEMORY;
  }

  auto *group = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(global));
  if (group == nullptr) {
    GlobalFree(global);
    return E_OUTOFMEMORY;
  }

  group->cItems = static_cast<UINT>(count);
  for (size_t index = 0; index < count; ++index) {
    const auto &entry = m_entries[index].entry;
    FILEDESCRIPTORW &descriptor = group->fgd[index];
    descriptor.dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI;
    if (entry.isDirectory) {
      descriptor.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    } else {
      descriptor.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
      descriptor.dwFlags |= FD_FILESIZE;
      descriptor.nFileSizeLow = static_cast<DWORD>(entry.size & 0xffffffffULL);
      descriptor.nFileSizeHigh = static_cast<DWORD>(entry.size >> 32);
    }
    if (entry.modifiedTime != 0) {
      descriptor.dwFlags |= FD_WRITESTIME;
      descriptor.ftLastWriteTime = toFileTime(entry.modifiedTime);
    }
    // the buffer is zero initialised and the path is known to fit
    std::memcpy(descriptor.cFileName, entry.relativePath.c_str(), entry.relativePath.size() * sizeof(wchar_t));
  }
  GlobalUnlock(global);

  medium->tymed = TYMED_HGLOBAL;
  medium->hGlobal = global;
  medium->pUnkForRelease = nullptr;
  return S_OK;
}

HRESULT MSWindowsVirtualFileDataObject::getFileContents(LONG index, STGMEDIUM *medium)
{
  if (!ensureListed()) {
    return E_FAIL;
  }

  std::scoped_lock lock{m_mutex};
  if (index < 0 || static_cast<size_t>(index) >= m_entries.size()) {
    return DV_E_LINDEX;
  }

  const ListedEntry &listed = m_entries[static_cast<size_t>(index)];
  if (listed.entry.isDirectory) {
    return DV_E_LINDEX;
  }

  medium->tymed = TYMED_ISTREAM;
  medium->pstm = new VirtualFileStream(m_source, listed.sourceIndex, listed.entry);
  medium->pUnkForRelease = nullptr;
  return S_OK;
}

HRESULT MSWindowsVirtualFileDataObject::getGlobalFormat(UINT format, STGMEDIUM *medium) const
{
  std::scoped_lock lock{m_mutex};
  const auto found = m_globalFormats.find(format);
  if (found == m_globalFormats.end()) {
    return DV_E_FORMATETC;
  }

  HGLOBAL global = copyToGlobal(found->second.data(), found->second.size());
  if (global == nullptr) {
    return E_OUTOFMEMORY;
  }

  medium->tymed = TYMED_HGLOBAL;
  medium->hGlobal = global;
  medium->pUnkForRelease = nullptr;
  return S_OK;
}

bool MSWindowsVirtualFileDataObject::hasGlobalFormat(UINT format) const
{
  std::scoped_lock lock{m_mutex};
  return m_globalFormats.contains(format);
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::GetData(FORMATETC *format, STGMEDIUM *medium)
{
  if (format == nullptr || medium == nullptr) {
    return E_INVALIDARG;
  }
  std::memset(medium, 0, sizeof(*medium));

  if ((format->dwAspect & DVASPECT_CONTENT) == 0) {
    return DV_E_DVASPECT;
  }

  if (format->cfFormat == fileDescriptorFormat()) {
    if ((format->tymed & TYMED_HGLOBAL) == 0) {
      return DV_E_TYMED;
    }
    return getFileDescriptor(medium);
  }

  if (format->cfFormat == fileContentsFormat()) {
    if ((format->tymed & TYMED_ISTREAM) == 0) {
      return DV_E_TYMED;
    }
    return getFileContents(format->lindex, medium);
  }

  if ((format->tymed & TYMED_HGLOBAL) == 0) {
    return hasGlobalFormat(format->cfFormat) ? DV_E_TYMED : DV_E_FORMATETC;
  }
  return getGlobalFormat(format->cfFormat, medium);
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::GetDataHere(FORMATETC *, STGMEDIUM *)
{
  return E_NOTIMPL;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::QueryGetData(FORMATETC *format)
{
  if (format == nullptr) {
    return E_INVALIDARG;
  }
  if ((format->dwAspect & DVASPECT_CONTENT) == 0) {
    return DV_E_DVASPECT;
  }

  if (format->cfFormat == fileDescriptorFormat()) {
    return ((format->tymed & TYMED_HGLOBAL) != 0) ? S_OK : DV_E_TYMED;
  }
  if (format->cfFormat == fileContentsFormat()) {
    return ((format->tymed & TYMED_ISTREAM) != 0) ? S_OK : DV_E_TYMED;
  }
  if (hasGlobalFormat(format->cfFormat)) {
    return ((format->tymed & TYMED_HGLOBAL) != 0) ? S_OK : DV_E_TYMED;
  }
  return DV_E_FORMATETC;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::GetCanonicalFormatEtc(FORMATETC *, FORMATETC *formatOut)
{
  if (formatOut != nullptr) {
    formatOut->ptd = nullptr;
  }
  return E_NOTIMPL;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::SetData(FORMATETC *format, STGMEDIUM *medium, BOOL release)
{
  if (format == nullptr || medium == nullptr) {
    return E_INVALIDARG;
  }

  // the shell records the outcome of a paste here, only memory formats are kept
  if (format->cfFormat == fileDescriptorFormat() || format->cfFormat == fileContentsFormat()) {
    return E_ACCESSDENIED;
  }
  if (medium->tymed != TYMED_HGLOBAL || medium->hGlobal == nullptr) {
    return DV_E_TYMED;
  }

  const SIZE_T size = GlobalSize(medium->hGlobal);
  const void *data = GlobalLock(medium->hGlobal);
  if (data == nullptr) {
    return E_OUTOFMEMORY;
  }
  addGlobalFormat(format->cfFormat, data, size);
  GlobalUnlock(medium->hGlobal);

  if (release) {
    ReleaseStgMedium(medium);
  }
  return S_OK;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::EnumFormatEtc(DWORD direction, IEnumFORMATETC **enumerator)
{
  if (enumerator == nullptr) {
    return E_POINTER;
  }
  *enumerator = nullptr;
  if (direction != DATADIR_GET) {
    return E_NOTIMPL;
  }

  std::vector<FORMATETC> formats;
  formats.push_back({static_cast<CLIPFORMAT>(fileDescriptorFormat()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
  formats.push_back({static_cast<CLIPFORMAT>(fileContentsFormat()), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM});
  {
    std::scoped_lock lock{m_mutex};
    for (const auto &entry : m_globalFormats) {
      formats.push_back({static_cast<CLIPFORMAT>(entry.first), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
    }
  }
  return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()), formats.data(), enumerator);
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *)
{
  return OLE_E_ADVISENOTSUPPORTED;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::DUnadvise(DWORD)
{
  return OLE_E_ADVISENOTSUPPORTED;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::EnumDAdvise(IEnumSTATDATA **)
{
  return OLE_E_ADVISENOTSUPPORTED;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::SetAsyncMode(BOOL doOpAsync)
{
  std::scoped_lock lock{m_mutex};
  m_asyncMode = (doOpAsync != FALSE);
  return S_OK;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::GetAsyncMode(BOOL *isOpAsync)
{
  if (isOpAsync == nullptr) {
    return E_POINTER;
  }
  std::scoped_lock lock{m_mutex};
  *isOpAsync = m_asyncMode ? VARIANT_TRUE : VARIANT_FALSE;
  return S_OK;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::StartOperation(IBindCtx *)
{
  std::scoped_lock lock{m_mutex};
  m_inOperation = true;
  return S_OK;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::InOperation(BOOL *inAsyncOp)
{
  if (inAsyncOp == nullptr) {
    return E_POINTER;
  }
  std::scoped_lock lock{m_mutex};
  *inAsyncOp = m_inOperation ? VARIANT_TRUE : VARIANT_FALSE;
  return S_OK;
}

IFACEMETHODIMP MSWindowsVirtualFileDataObject::EndOperation(HRESULT result, IBindCtx *, DWORD effects)
{
  LOG_DEBUG(
      "paste of virtual files finished, result=0x%08x effects=%u", static_cast<unsigned>(result),
      static_cast<unsigned>(effects)
  );
  std::scoped_lock lock{m_mutex};
  m_inOperation = false;
  ++m_finishedOperations;
  return S_OK;
}
