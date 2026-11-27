/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsVirtualFileTests.h"

#include "platform/MSWindowsVirtualFileDataObject.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <ShObjIdl.h>
#include <ShlObj.h>

#include <atomic>

namespace {

QByteArray makeData(qsizetype size, int seed)
{
  QByteArray data(size, Qt::Uninitialized);
  for (qsizetype i = 0; i < size; ++i) {
    data[i] = static_cast<char>((i * 31 + seed + (i >> 8)) & 0xff);
  }
  return data;
}

} // namespace

//! Reader that hands out the data in small pieces, like a network source would
class MemoryVirtualFileReader : public IMSWindowsVirtualFileReader
{
public:
  MemoryVirtualFileReader(const QByteArray &data, uint64_t offset, std::atomic<int> &reads)
      : m_data(data),
        m_offset(static_cast<qsizetype>(std::min<uint64_t>(offset, static_cast<uint64_t>(data.size())))),
        m_reads(reads)
  {
  }

  bool read(void *buffer, uint32_t size, uint32_t &bytesRead) override
  {
    ++m_reads;
    const qsizetype remaining = m_data.size() - m_offset;
    const qsizetype take = std::min<qsizetype>({remaining, static_cast<qsizetype>(size), s_pieceSize});
    if (take > 0) {
      std::memcpy(buffer, m_data.constData() + m_offset, static_cast<size_t>(take));
      m_offset += take;
    }
    bytesRead = static_cast<uint32_t>(take);
    return true;
  }

private:
  inline static const qsizetype s_pieceSize = 64 * 1024;
  QByteArray m_data;
  qsizetype m_offset;
  std::atomic<int> &m_reads;
};

class MemoryVirtualFileSource : public IMSWindowsVirtualFileSource
{
public:
  void addDirectory(const QString &path)
  {
    m_entries.push_back({path.toStdWString(), true, 0, s_modifiedTime});
    m_contents.push_back({});
  }

  void addFile(const QString &path, const QByteArray &data)
  {
    m_entries.push_back({path.toStdWString(), false, static_cast<uint64_t>(data.size()), s_modifiedTime});
    m_contents.push_back(data);
  }

  bool list(std::vector<MSWindowsVirtualFileEntry> &entries) override
  {
    ++listCalls;
    entries = m_entries;
    return true;
  }

  std::unique_ptr<IMSWindowsVirtualFileReader> open(size_t index, uint64_t offset) override
  {
    ++openCalls;
    if (index >= m_contents.size() || m_entries[index].isDirectory) {
      return nullptr;
    }
    return std::make_unique<MemoryVirtualFileReader>(m_contents[index], offset, readCalls);
  }

  const std::vector<MSWindowsVirtualFileEntry> &entries() const
  {
    return m_entries;
  }

  const QByteArray &contents(size_t index) const
  {
    return m_contents[index];
  }

  void resetCounters()
  {
    listCalls = 0;
    openCalls = 0;
    readCalls = 0;
  }

  std::atomic<int> listCalls = 0;
  std::atomic<int> openCalls = 0;
  std::atomic<int> readCalls = 0;

  // 2026-01-02 03:04:06 UTC, an even number of seconds so FAT style rounding can't change it
  inline static const int64_t s_modifiedTime = 1767323046;

private:
  std::vector<MSWindowsVirtualFileEntry> m_entries;
  std::vector<QByteArray> m_contents;
};

namespace {

FORMATETC descriptorFormat()
{
  return {
      static_cast<CLIPFORMAT>(MSWindowsVirtualFileDataObject::fileDescriptorFormat()), nullptr, DVASPECT_CONTENT, -1,
      TYMED_HGLOBAL
  };
}

FORMATETC contentsFormat(LONG index)
{
  return {
      static_cast<CLIPFORMAT>(MSWindowsVirtualFileDataObject::fileContentsFormat()), nullptr, DVASPECT_CONTENT, index,
      TYMED_ISTREAM
  };
}

//! Read a stream to its end in pieces of \p pieceSize bytes
bool readAll(IStream *stream, ULONG pieceSize, QByteArray &data)
{
  data.clear();
  QByteArray piece(static_cast<qsizetype>(pieceSize), Qt::Uninitialized);
  for (;;) {
    ULONG got = 0;
    const HRESULT result = stream->Read(piece.data(), pieceSize, &got);
    if (FAILED(result)) {
      return false;
    }
    data.append(piece.constData(), static_cast<qsizetype>(got));
    if (result == S_FALSE || got == 0) {
      return true;
    }
  }
}

} // namespace

void MSWindowsVirtualFileTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Debug);

  const HRESULT result = OleInitialize(nullptr);
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("OleInitialize failed: 0x%1").arg(result, 8, 16)));
  m_oleInitialized = true;

  m_source = std::make_shared<MemoryVirtualFileSource>();
  m_source->addDirectory(QStringLiteral("Folder"));
  m_source->addFile(QStringLiteral("Folder\\nested.bin"), makeData(300 * 1024, 7));
  m_source->addFile(QStringLiteral("Folder\\空文件.txt"), QByteArray());
  m_source->addDirectory(QStringLiteral("Folder\\Sub"));
  m_source->addFile(QStringLiteral("Folder\\Sub\\deep.txt"), QByteArrayLiteral("deep"));
  m_source->addFile(QStringLiteral("top 文件.dat"), makeData(5 * 1024 * 1024 + 123, 99));
}

void MSWindowsVirtualFileTests::cleanupTestCase()
{
  if (m_oleInitialized) {
    OleSetClipboard(nullptr);
    OleUninitialize();
  }
}

void MSWindowsVirtualFileTests::fileDescriptorListsEntries()
{
  m_source->resetCounters();
  auto *object = new MSWindowsVirtualFileDataObject(m_source);

  FORMATETC format = descriptorFormat();
  QCOMPARE(object->QueryGetData(&format), S_OK);
  QCOMPARE(m_source->listCalls.load(), 0);

  STGMEDIUM medium = {};
  QCOMPARE(object->GetData(&format, &medium), S_OK);
  QCOMPARE(medium.tymed, static_cast<DWORD>(TYMED_HGLOBAL));
  QCOMPARE(m_source->listCalls.load(), 1);

  const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(medium.hGlobal));
  QVERIFY(group != nullptr);
  const auto &entries = m_source->entries();
  QCOMPARE(group->cItems, static_cast<UINT>(entries.size()));
  for (UINT index = 0; index < group->cItems; ++index) {
    const FILEDESCRIPTORW &descriptor = group->fgd[index];
    const auto &entry = entries[index];
    QCOMPARE(QString::fromWCharArray(descriptor.cFileName), QString::fromStdWString(entry.relativePath));
    QVERIFY((descriptor.dwFlags & FD_ATTRIBUTES) != 0);
    QVERIFY((descriptor.dwFlags & FD_WRITESTIME) != 0);
    QCOMPARE((descriptor.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, entry.isDirectory);
    if (!entry.isDirectory) {
      QVERIFY((descriptor.dwFlags & FD_FILESIZE) != 0);
      const uint64_t size = (static_cast<uint64_t>(descriptor.nFileSizeHigh) << 32) | descriptor.nFileSizeLow;
      QCOMPARE(size, entry.size);
    }
  }
  GlobalUnlock(medium.hGlobal);
  ReleaseStgMedium(&medium);

  // listing happens once, later requests reuse it
  QCOMPARE(object->GetData(&format, &medium), S_OK);
  ReleaseStgMedium(&medium);
  QCOMPARE(m_source->listCalls.load(), 1);
  QCOMPARE(m_source->openCalls.load(), 0);

  QCOMPARE(object->Release(), 0UL);
}

void MSWindowsVirtualFileTests::fileDescriptorSkipsTooLongPaths()
{
  auto source = std::make_shared<MemoryVirtualFileSource>();
  source->addFile(QStringLiteral("ok.txt"), QByteArrayLiteral("ok"));
  source->addFile(QString(MAX_PATH, QLatin1Char('x')), QByteArrayLiteral("too long"));
  source->addFile(QStringLiteral("after.txt"), QByteArrayLiteral("after"));
  auto *object = new MSWindowsVirtualFileDataObject(source);

  FORMATETC format = descriptorFormat();
  STGMEDIUM medium = {};
  QCOMPARE(object->GetData(&format, &medium), S_OK);
  const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(medium.hGlobal));
  QVERIFY(group != nullptr);
  QCOMPARE(group->cItems, 2U);
  QCOMPARE(QString::fromWCharArray(group->fgd[0].cFileName), QStringLiteral("ok.txt"));
  QCOMPARE(QString::fromWCharArray(group->fgd[1].cFileName), QStringLiteral("after.txt"));
  GlobalUnlock(medium.hGlobal);
  ReleaseStgMedium(&medium);

  // the second described file is the third file of the source
  FORMATETC contents = contentsFormat(1);
  QCOMPARE(object->GetData(&contents, &medium), S_OK);
  QByteArray data;
  QVERIFY(readAll(medium.pstm, 16, data));
  QCOMPARE(data, QByteArrayLiteral("after"));
  ReleaseStgMedium(&medium);

  contents = contentsFormat(2);
  QCOMPARE(object->GetData(&contents, &medium), DV_E_LINDEX);

  QCOMPARE(object->Release(), 0UL);
}

void MSWindowsVirtualFileTests::fileContentsStreamsData()
{
  m_source->resetCounters();
  auto *object = new MSWindowsVirtualFileDataObject(m_source);

  const auto &entries = m_source->entries();
  for (size_t index = 0; index < entries.size(); ++index) {
    FORMATETC format = contentsFormat(static_cast<LONG>(index));
    STGMEDIUM medium = {};
    if (entries[index].isDirectory) {
      QCOMPARE(object->GetData(&format, &medium), DV_E_LINDEX);
      continue;
    }

    const int opensBefore = m_source->openCalls.load();
    QCOMPARE(object->GetData(&format, &medium), S_OK);
    QCOMPARE(medium.tymed, static_cast<DWORD>(TYMED_ISTREAM));
    QVERIFY(medium.pstm != nullptr);
    // nothing is read until the stream is read
    QCOMPARE(m_source->openCalls.load(), opensBefore);

    STATSTG stat = {};
    QCOMPARE(medium.pstm->Stat(&stat, STATFLAG_NONAME), S_OK);
    QCOMPARE(static_cast<uint64_t>(stat.cbSize.QuadPart), entries[index].size);

    QByteArray data;
    QVERIFY(readAll(medium.pstm, 100 * 1000, data));
    QCOMPARE(data.size(), m_source->contents(index).size());
    QVERIFY(data == m_source->contents(index));
    ReleaseStgMedium(&medium);
  }

  FORMATETC wrongMedium = contentsFormat(1);
  wrongMedium.tymed = TYMED_HGLOBAL;
  STGMEDIUM medium = {};
  QCOMPARE(object->GetData(&wrongMedium, &medium), DV_E_TYMED);

  QCOMPARE(object->Release(), 0UL);
}

void MSWindowsVirtualFileTests::streamSeekRestartsReading()
{
  m_source->resetCounters();
  auto *object = new MSWindowsVirtualFileDataObject(m_source);

  // Folder\nested.bin
  const size_t index = 1;
  const QByteArray &expected = m_source->contents(index);
  FORMATETC format = contentsFormat(static_cast<LONG>(index));
  STGMEDIUM medium = {};
  QCOMPARE(object->GetData(&format, &medium), S_OK);
  IStream *stream = medium.pstm;

  QByteArray head(1000, Qt::Uninitialized);
  ULONG got = 0;
  QCOMPARE(stream->Read(head.data(), 1000, &got), S_OK);
  QCOMPARE(got, 1000UL);
  QVERIFY(head == expected.left(1000));

  // asking for the current position must not restart the transfer
  LARGE_INTEGER move = {};
  ULARGE_INTEGER position = {};
  QCOMPARE(stream->Seek(move, STREAM_SEEK_CUR, &position), S_OK);
  QCOMPARE(static_cast<uint64_t>(position.QuadPart), uint64_t{1000});
  QCOMPARE(m_source->openCalls.load(), 1);

  move.QuadPart = 200 * 1024;
  QCOMPARE(stream->Seek(move, STREAM_SEEK_SET, &position), S_OK);
  QByteArray tail;
  QVERIFY(readAll(stream, 4096, tail));
  QVERIFY(tail == expected.mid(200 * 1024));
  QCOMPARE(m_source->openCalls.load(), 2);

  // reading at the end returns nothing
  QCOMPARE(stream->Read(head.data(), 1000, &got), S_FALSE);
  QCOMPARE(got, 0UL);

  move.QuadPart = -4;
  QCOMPARE(stream->Seek(move, STREAM_SEEK_END, &position), S_OK);
  QCOMPARE(static_cast<uint64_t>(position.QuadPart), static_cast<uint64_t>(expected.size() - 4));

  ReleaseStgMedium(&medium);
  QCOMPARE(object->Release(), 0UL);
}

void MSWindowsVirtualFileTests::extraFormatsAreServed()
{
  auto *object = new MSWindowsVirtualFileDataObject(m_source);
  const wchar_t text[] = L"names only";
  object->addGlobalFormat(CF_UNICODETEXT, text, sizeof(text));

  FORMATETC format = {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  QCOMPARE(object->QueryGetData(&format), S_OK);
  STGMEDIUM medium = {};
  QCOMPARE(object->GetData(&format, &medium), S_OK);
  const auto *copied = static_cast<const wchar_t *>(GlobalLock(medium.hGlobal));
  QVERIFY(copied != nullptr);
  QCOMPARE(QString::fromWCharArray(copied), QStringLiteral("names only"));
  GlobalUnlock(medium.hGlobal);
  ReleaseStgMedium(&medium);

  // files are offered for copying, not moving
  FORMATETC effectFormat = {
      static_cast<CLIPFORMAT>(MSWindowsVirtualFileDataObject::preferredDropEffectFormat()), nullptr, DVASPECT_CONTENT,
      -1, TYMED_HGLOBAL
  };
  QCOMPARE(object->GetData(&effectFormat, &medium), S_OK);
  const auto *effect = static_cast<const DWORD *>(GlobalLock(medium.hGlobal));
  QVERIFY(effect != nullptr);
  QCOMPARE(*effect, static_cast<DWORD>(DROPEFFECT_COPY));
  GlobalUnlock(medium.hGlobal);
  ReleaseStgMedium(&medium);

  FORMATETC unknown = {CF_BITMAP, nullptr, DVASPECT_CONTENT, -1, TYMED_GDI};
  QCOMPARE(object->QueryGetData(&unknown), DV_E_FORMATETC);

  // every offered format is enumerated
  IEnumFORMATETC *enumerator = nullptr;
  QCOMPARE(object->EnumFormatEtc(DATADIR_GET, &enumerator), S_OK);
  QList<CLIPFORMAT> formats;
  FORMATETC next = {};
  while (enumerator->Next(1, &next, nullptr) == S_OK) {
    formats.append(next.cfFormat);
  }
  enumerator->Release();
  QVERIFY(formats.contains(static_cast<CLIPFORMAT>(MSWindowsVirtualFileDataObject::fileDescriptorFormat())));
  QVERIFY(formats.contains(static_cast<CLIPFORMAT>(MSWindowsVirtualFileDataObject::fileContentsFormat())));
  QVERIFY(formats.contains(static_cast<CLIPFORMAT>(MSWindowsVirtualFileDataObject::preferredDropEffectFormat())));
  QVERIFY(formats.contains(static_cast<CLIPFORMAT>(CF_UNICODETEXT)));

  QCOMPARE(object->Release(), 0UL);
}

void MSWindowsVirtualFileTests::clipboardDoesNotReadUntilAsked()
{
  m_source->resetCounters();
  auto *object = new MSWindowsVirtualFileDataObject(m_source);

  HRESULT result = OleSetClipboard(object);
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("OleSetClipboard failed: 0x%1").arg(result, 8, 16)));
  QCOMPARE(OleIsCurrentClipboard(object), S_OK);

  // give other programs a chance to react to the clipboard change
  QTest::qWait(1500);
  QCOMPARE(m_source->listCalls.load(), 0);
  QCOMPARE(m_source->openCalls.load(), 0);

  // the formats are visible to other programs without transferring anything
  QVERIFY(IsClipboardFormatAvailable(MSWindowsVirtualFileDataObject::fileDescriptorFormat()));
  QVERIFY(IsClipboardFormatAvailable(MSWindowsVirtualFileDataObject::fileContentsFormat()));
  QCOMPARE(m_source->listCalls.load(), 0);

  IDataObject *fromClipboard = nullptr;
  result = OleGetClipboard(&fromClipboard);
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("OleGetClipboard failed: 0x%1").arg(result, 8, 16)));

  FORMATETC format = descriptorFormat();
  STGMEDIUM medium = {};
  QCOMPARE(fromClipboard->GetData(&format, &medium), S_OK);
  const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(medium.hGlobal));
  QVERIFY(group != nullptr);
  QCOMPARE(group->cItems, static_cast<UINT>(m_source->entries().size()));
  GlobalUnlock(medium.hGlobal);
  ReleaseStgMedium(&medium);
  QCOMPARE(m_source->listCalls.load(), 1);
  QCOMPARE(m_source->openCalls.load(), 0);

  // Folder\Sub\deep.txt
  FORMATETC contents = contentsFormat(4);
  QCOMPARE(fromClipboard->GetData(&contents, &medium), S_OK);
  QByteArray data;
  QVERIFY(readAll(medium.pstm, 64, data));
  QCOMPARE(data, QByteArrayLiteral("deep"));
  ReleaseStgMedium(&medium);

  fromClipboard->Release();
  QCOMPARE(OleSetClipboard(nullptr), S_OK);
  object->Release();
}

void MSWindowsVirtualFileTests::shellCopiesVirtualFiles()
{
  m_source->resetCounters();
  QTemporaryDir destination;
  QVERIFY(destination.isValid());

  auto *object = new MSWindowsVirtualFileDataObject(m_source);

  // the drop target of a folder is what Explorer uses when files are pasted into it
  const std::wstring destinationPath = QDir::toNativeSeparators(destination.path()).toStdWString();
  IShellItem *folder = nullptr;
  HRESULT result = SHCreateItemFromParsingName(destinationPath.c_str(), nullptr, IID_PPV_ARGS(&folder));
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("no shell item: 0x%1").arg(result, 8, 16)));

  IDropTarget *target = nullptr;
  result = folder->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target));
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("no drop target: 0x%1").arg(result, 8, 16)));

  const POINTL point = {0, 0};
  DWORD effect = DROPEFFECT_COPY;
  result = target->DragEnter(object, MK_LBUTTON, point, &effect);
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("DragEnter failed: 0x%1").arg(result, 8, 16)));
  QVERIFY2((effect & DROPEFFECT_COPY) != 0, "the folder does not accept the virtual files");

  effect = DROPEFFECT_COPY;
  result = target->Drop(object, MK_LBUTTON, point, &effect);
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("Drop failed: 0x%1").arg(result, 8, 16)));

  // the shell may copy on another thread, which calls back into this one
  const auto &entries = m_source->entries();
  const auto allCopied = [&] {
    if (object->inAsyncOperation()) {
      return false;
    }
    for (size_t index = 0; index < entries.size(); ++index) {
      const QFileInfo info(destination.filePath(QString::fromStdWString(entries[index].relativePath)));
      if (!info.exists()) {
        return false;
      }
      if (!entries[index].isDirectory && static_cast<uint64_t>(info.size()) != entries[index].size) {
        return false;
      }
    }
    return true;
  };
  QElapsedTimer timer;
  timer.start();
  while (!allCopied() && timer.elapsed() < 60000) {
    QTest::qWait(50);
  }
  qInfo(
      "shell copy took %lld ms, async operations finished: %u, list calls: %d, open calls: %d, read calls: %d",
      static_cast<long long>(timer.elapsed()), object->finishedAsyncOperations(), m_source->listCalls.load(),
      m_source->openCalls.load(), m_source->readCalls.load()
  );
  QVERIFY2(allCopied(), "the shell did not copy every file in time");

  for (size_t index = 0; index < entries.size(); ++index) {
    const QString path = destination.filePath(QString::fromStdWString(entries[index].relativePath));
    if (entries[index].isDirectory) {
      QVERIFY(QFileInfo(path).isDir());
      continue;
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.readAll() == m_source->contents(index));
  }

  // each file is listed once and read once, from start to end
  QCOMPARE(m_source->listCalls.load(), 1);
  QCOMPARE(m_source->openCalls.load(), 3);

  target->Release();
  folder->Release();
  object->Release();
}

QTEST_MAIN(MSWindowsVirtualFileTests)
