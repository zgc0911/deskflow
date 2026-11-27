/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <ObjIdl.h>
#include <ShlDisp.h>

//! One file or directory offered as a virtual file
struct MSWindowsVirtualFileEntry
{
  //! Path relative to the paste target, components separated by backslashes
  std::wstring relativePath;
  bool isDirectory = false;
  uint64_t size = 0;
  //! Seconds since the Unix epoch, zero when unknown
  int64_t modifiedTime = 0;
  //! Tells the source which file to open, the same however often the files are listed
  size_t sourceId = 0;
};

//! Sequential reader for the contents of one virtual file
class IMSWindowsVirtualFileReader
{
public:
  virtual ~IMSWindowsVirtualFileReader() = default;

  //! Read file contents
  /*!
  Reads up to \p size bytes into \p buffer, blocking until at least one byte
  is available or the end of the file is reached. \p bytesRead is zero at the
  end of the file. Returns false if the contents can not be read.
  */
  virtual bool read(void *buffer, uint32_t size, uint32_t &bytesRead) = 0;
};

//! Source of the virtual files placed on the clipboard
/*!
The files do not exist on this computer. They are listed and read on demand,
when another application pastes them.
*/
class IMSWindowsVirtualFileSource
{
public:
  virtual ~IMSWindowsVirtualFileSource() = default;

  //! List all files and directories
  /*!
  Directories must come before their contents. May block. Returns false if
  the list can not be produced. Called again for every paste, the files may
  have changed in between.
  */
  virtual bool list(std::vector<MSWindowsVirtualFileEntry> &entries) = 0;

  //! Open the contents of a file
  /*!
  \p sourceId is that of an entry returned by list(), of this listing or an
  earlier one. Reading starts at \p offset. Returns nullptr if the file can
  not be opened.
  */
  virtual std::unique_ptr<IMSWindowsVirtualFileReader> open(size_t sourceId, uint64_t offset) = 0;
};

//! Clipboard data object that offers files which are transferred when pasted
/*!
Exposes the shell formats CFSTR_FILEDESCRIPTORW and CFSTR_FILECONTENTS. The
application that pastes (usually Explorer) reads each file through an IStream
and writes it to the destination itself, showing its own progress.
*/
class MSWindowsVirtualFileDataObject : public IDataObject, public IDataObjectAsyncCapability
{
public:
  explicit MSWindowsVirtualFileDataObject(std::shared_ptr<IMSWindowsVirtualFileSource> source);
  MSWindowsVirtualFileDataObject(MSWindowsVirtualFileDataObject const &) = delete;
  MSWindowsVirtualFileDataObject &operator=(MSWindowsVirtualFileDataObject const &) = delete;

  //! Offer an additional format whose data is held in memory
  void addGlobalFormat(UINT format, const void *data, size_t size);

  //! True while an application is copying the files on a background thread
  bool inAsyncOperation() const;

  //! Number of asynchronous copy operations that have finished
  uint32_t finishedAsyncOperations() const;

  static UINT fileDescriptorFormat();
  static UINT fileContentsFormat();
  static UINT preferredDropEffectFormat();

  // IUnknown
  IFACEMETHODIMP QueryInterface(REFIID riid, void **object) override;
  IFACEMETHODIMP_(ULONG) AddRef() override;
  IFACEMETHODIMP_(ULONG) Release() override;

  // IDataObject
  IFACEMETHODIMP GetData(FORMATETC *format, STGMEDIUM *medium) override;
  IFACEMETHODIMP GetDataHere(FORMATETC *format, STGMEDIUM *medium) override;
  IFACEMETHODIMP QueryGetData(FORMATETC *format) override;
  IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC *formatIn, FORMATETC *formatOut) override;
  IFACEMETHODIMP SetData(FORMATETC *format, STGMEDIUM *medium, BOOL release) override;
  IFACEMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC **enumerator) override;
  IFACEMETHODIMP DAdvise(FORMATETC *format, DWORD flags, IAdviseSink *sink, DWORD *connection) override;
  IFACEMETHODIMP DUnadvise(DWORD connection) override;
  IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA **enumerator) override;

  // IDataObjectAsyncCapability
  IFACEMETHODIMP SetAsyncMode(BOOL doOpAsync) override;
  IFACEMETHODIMP GetAsyncMode(BOOL *isOpAsync) override;
  IFACEMETHODIMP StartOperation(IBindCtx *reserved) override;
  IFACEMETHODIMP InOperation(BOOL *inAsyncOp) override;
  IFACEMETHODIMP EndOperation(HRESULT result, IBindCtx *reserved, DWORD effects) override;

private:
  // destroyed through Release()
  virtual ~MSWindowsVirtualFileDataObject() = default;

  //! List the files, again if \p fresh and the last listing is from an earlier paste
  bool ensureListed(bool fresh);
  HRESULT getFileDescriptor(STGMEDIUM *medium);
  HRESULT getFileContents(LONG index, STGMEDIUM *medium);
  HRESULT getGlobalFormat(UINT format, STGMEDIUM *medium) const;
  bool hasGlobalFormat(UINT format) const;

  LONG m_refCount = 1;
  std::shared_ptr<IMSWindowsVirtualFileSource> m_source;

  mutable std::mutex m_mutex;
  bool m_listed = false;
  bool m_listFailed = false;
  std::chrono::steady_clock::time_point m_listedAt{};
  std::vector<MSWindowsVirtualFileEntry> m_entries;
  std::map<UINT, std::vector<uint8_t>> m_globalFormats;
  bool m_asyncMode = true;
  bool m_inOperation = false;
  uint32_t m_finishedOperations = 0;
};
