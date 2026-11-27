/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsClipboardFilesTests.h"

#include "../deskflow/FileTransferTestLink.h"

#include "deskflow/Clipboard.h"
#include "platform/MSWindowsClipboard.h"
#include "platform/MSWindowsClipboardFiles.h"
#include "platform/MSWindowsRemoteFileSource.h"
#include "platform/MSWindowsVirtualFileDataObject.h"

#include <QElapsedTimer>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <ShObjIdl.h>
#include <ShellAPI.h>
#include <ShlObj.h>
#include <ole2.h>

using deskflow::ClipboardFileList;
using namespace deskflow::test;

namespace {

//! Opens the clipboard in the tests
HWND g_window = nullptr;

ClipboardFileList copyOnA(Link &link, const std::vector<QString> &paths)
{
  std::vector<std::string> local;
  for (const auto &path : paths) {
    local.push_back(path.toStdString());
  }
  return link.copyOnA(local);
}

//! A clipboard as it arrives from another computer
std::unique_ptr<::Clipboard> received(const std::string &text, const ClipboardFileList *files)
{
  auto clipboard = std::make_unique<::Clipboard>();
  clipboard->open(1);
  if (!text.empty()) {
    clipboard->add(IClipboard::Format::Text, text);
  }
  if (files != nullptr) {
    clipboard->add(IClipboard::Format::FileList, files->marshall());
  }
  clipboard->close();
  return clipboard;
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs)
{
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > timeoutMs) {
      return false;
    }
    // this thread is an apartment of its own, calls into it come as messages
    QTest::qWait(20);
  }
  return true;
}

//! Wait for the clipboard thread while handling messages: it may send one to this thread, such as
//! WM_DESTROYCLIPBOARD when a window of this thread owned the clipboard before
bool finished(const MSWindowsClipboardFiles &clipboard)
{
  return waitFor([&clipboard] { return clipboard.waitUntilDone(0); }, 5000);
}

bool openClipboard()
{
  return waitFor([] { return OpenClipboard(g_window) != FALSE; }, 2000);
}

//! The text of the clipboard, which this thread has open
QString textOfOpenClipboard()
{
  QString text;
  HANDLE data = GetClipboardData(CF_UNICODETEXT);
  if (data != nullptr) {
    const auto *characters = static_cast<const wchar_t *>(GlobalLock(data));
    if (characters != nullptr) {
      text = QString::fromWCharArray(characters);
      GlobalUnlock(data);
    }
  }
  return text;
}

QString clipboardText()
{
  if (!openClipboard()) {
    return QStringLiteral("<can't open the clipboard>");
  }
  const QString text = textOfOpenClipboard();
  CloseClipboard();
  return text;
}

//! Keeps the clipboard open on a thread of its own, the way another application may
class ClipboardHolder
{
public:
  //! \p whileOpen runs once the clipboard is open, \p beforeClose once it is released
  explicit ClipboardHolder(std::function<void()> whileOpen = {}, std::function<void()> beforeClose = {})
  {
    m_thread = std::thread([this, whileOpen, beforeClose] {
      bool opened = false;
      for (int attempt = 0; attempt < 500 && !opened; ++attempt) {
        opened = OpenClipboard(nullptr) != FALSE;
        if (!opened) {
          Sleep(10);
        }
      }
      if (opened && whileOpen) {
        whileOpen();
      }
      m_open = opened;
      m_ready = true;
      {
        std::unique_lock lock{m_mutex};
        m_changed.wait(lock, [this] { return m_release; });
      }
      if (opened) {
        if (beforeClose) {
          beforeClose();
        }
        CloseClipboard();
      }
      m_closed = true;
    });
  }

  ~ClipboardHolder()
  {
    release();
    m_thread.join();
  }

  ClipboardHolder(ClipboardHolder const &) = delete;
  ClipboardHolder &operator=(ClipboardHolder const &) = delete;

  //! True once the clipboard is open, messages of this thread are handled meanwhile
  bool open()
  {
    return waitFor([this] { return m_ready.load(); }, 10000) && m_open;
  }

  void release()
  {
    {
      std::scoped_lock lock{m_mutex};
      m_release = true;
    }
    m_changed.notify_all();
  }

  //! True once the clipboard is closed again, messages of this thread are handled meanwhile
  bool closed()
  {
    return waitFor([this] { return m_closed.load(); }, 10000);
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_release = false;
  std::atomic<bool> m_ready = false;
  std::atomic<bool> m_open = false;
  std::atomic<bool> m_closed = false;
  std::thread m_thread;
};

uint64_t requestsOf(Monitor &monitor)
{
  std::scoped_lock lock{monitor.mutex};
  return monitor.requests;
}

bool hasFileDescriptors()
{
  return IsClipboardFormatAvailable(MSWindowsVirtualFileDataObject::fileDescriptorFormat()) != FALSE;
}

QByteArray readStream(IStream *stream)
{
  QByteArray data;
  char buffer[64 * 1024];
  for (;;) {
    ULONG got = 0;
    const HRESULT result = stream->Read(buffer, sizeof(buffer), &got);
    if (FAILED(result)) {
      return "read failed";
    }
    data.append(buffer, static_cast<qsizetype>(got));
    if (got < sizeof(buffer)) {
      return data;
    }
  }
}

QByteArray contentsOf(const QString &path)
{
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("unreadable");
}

QString pathOf(const MSWindowsVirtualFileEntry &entry)
{
  return QString::fromStdWString(entry.relativePath);
}

} // namespace

QString MSWindowsClipboardFilesTests::sourcePath(const QString &relative) const
{
  return QDir(m_source->path()).filePath(relative);
}

void MSWindowsClipboardFilesTests::initTestCase()
{
  const HRESULT result = OleInitialize(nullptr);
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("OleInitialize failed: 0x%1").arg(result, 8, 16)));
  m_oleInitialized = true;
  g_window = CreateWindowExW(
      0, L"STATIC", L"MSWindowsClipboardFilesTests", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
      nullptr
  );
  QVERIFY(g_window != nullptr);

  // the computer that copies has:
  //   big.bin              24 MiB that do not repeat
  //   tree/a.txt
  //   tree/empty.bin
  //   tree/中文 目录/b.bin   3 MiB
  //   tree/nested/deeper/c.txt
  //   one/Report.txt, two/report.TXT
  m_source = std::make_unique<QTemporaryDir>();
  QVERIFY(m_source->isValid());
  QVERIFY(QDir().mkpath(sourcePath(QStringLiteral("tree/中文 目录"))));
  QVERIFY(QDir().mkpath(sourcePath("tree/nested/deeper")));
  QVERIFY(QDir().mkpath(sourcePath("one")));
  QVERIFY(QDir().mkpath(sourcePath("two")));
  QVERIFY(writePattern(sourcePath("big.bin"), 24 * kMiB, 41));
  QVERIFY(writeFile(sourcePath("tree/a.txt"), "first file"));
  QVERIFY(writeFile(sourcePath("tree/empty.bin"), {}));
  QVERIFY(writePattern(sourcePath(QStringLiteral("tree/中文 目录/b.bin")), 3 * kMiB, 42));
  QVERIFY(writeFile(sourcePath("tree/nested/deeper/c.txt"), "deep down"));
  QVERIFY(writeFile(sourcePath("one/Report.txt"), "the first report"));
  QVERIFY(writeFile(sourcePath("two/report.TXT"), "the second report"));
}

void MSWindowsClipboardFilesTests::cleanupTestCase()
{
  m_source.reset();
  if (g_window != nullptr) {
    DestroyWindow(g_window);
    g_window = nullptr;
  }
  if (m_oleInitialized) {
    OleSetClipboard(nullptr);
    OleUninitialize();
  }
}

void MSWindowsClipboardFilesTests::windowsName_changesWhatWindowsDoesNotAllow()
{
  using Source = MSWindowsRemoteFileSource;
  QCOMPARE(Source::windowsName("plain.txt"), std::wstring(L"plain.txt"));
  QCOMPARE(Source::windowsName("\xe4\xb8\xad\xe6\x96\x87.txt"), std::wstring(L"中文.txt"));
  QCOMPARE(Source::windowsName("a:b?c*d\"e<f>g|h\\i.txt"), std::wstring(L"a_b_c_d_e_f_g_h_i.txt"));
  QCOMPARE(Source::windowsName("tab\there"), std::wstring(L"tab_here"));
  QCOMPARE(Source::windowsName("ends with dot."), std::wstring(L"ends with dot_"));
  QCOMPARE(Source::windowsName("ends with space "), std::wstring(L"ends with space_"));
  QCOMPARE(Source::windowsName("CON"), std::wstring(L"_CON"));
  QCOMPARE(Source::windowsName("con.txt"), std::wstring(L"_con.txt"));
  QCOMPARE(Source::windowsName("Com1.log"), std::wstring(L"_Com1.log"));
  QCOMPARE(Source::windowsName("lpt9"), std::wstring(L"_lpt9"));
  QCOMPARE(Source::windowsName("COM0"), std::wstring(L"_COM0"));
  QCOMPARE(Source::windowsName("LPT0.txt"), std::wstring(L"_LPT0.txt"));
  // superscript digits are port numbers as well
  QCOMPARE(Source::windowsName("COM\xc2\xb9.txt"), std::wstring(L"_COM\u00b9.txt"));
  QCOMPARE(Source::windowsName("lpt\xc2\xb3"), std::wstring(L"_lpt\u00b3"));
  // spaces before the dot don't count
  QCOMPARE(Source::windowsName("NUL .txt"), std::wstring(L"_NUL .txt"));
  QCOMPARE(Source::windowsName("conout$"), std::wstring(L"_conout$"));
  QCOMPARE(Source::windowsName("console"), std::wstring(L"console"));
  QCOMPARE(Source::windowsName("COM10"), std::wstring(L"COM10"));
  QCOMPARE(Source::windowsName("COM\xc2\xb4"), std::wstring(L"COM\u00b4"));
}

void MSWindowsClipboardFilesTests::remoteSource_listsTheWholeTree()
{
  Link link;
  const ClipboardFileList list = copyOnA(link, {sourcePath("tree"), sourcePath("big.bin")});
  QCOMPARE(list.entries.size(), size_t{2});
  MSWindowsRemoteFileSource source(link.remoteOfB, list);

  std::vector<MSWindowsVirtualFileEntry> entries;
  QVERIFY(source.list(entries));
  std::map<QString, MSWindowsVirtualFileEntry> byPath;
  std::set<size_t> ids;
  for (const auto &entry : entries) {
    byPath[pathOf(entry)] = entry;
    ids.insert(entry.sourceId);
  }
  const QStringList expected = {
      "big.bin",
      "tree",
      "tree\\a.txt",
      "tree\\empty.bin",
      QStringLiteral("tree\\中文 目录"),
      QStringLiteral("tree\\中文 目录\\b.bin"),
      "tree\\nested",
      "tree\\nested\\deeper",
      "tree\\nested\\deeper\\c.txt"
  };
  QCOMPARE(entries.size(), size_t(expected.size()));
  for (const QString &path : expected) {
    QVERIFY2(byPath.contains(path), qPrintable(path));
  }
  QCOMPARE(ids.size(), entries.size());
  QVERIFY(byPath["tree"].isDirectory);
  QCOMPARE(byPath["big.bin"].size, uint64_t(24 * kMiB));
  QCOMPARE(byPath[QStringLiteral("tree\\中文 目录\\b.bin")].size, uint64_t(3 * kMiB));

  // a directory comes before what it holds
  for (size_t index = 0; index < entries.size(); ++index) {
    const QString path = pathOf(entries[index]);
    const qsizetype slash = path.lastIndexOf('\\');
    if (slash > 0) {
      const QString parent = path.left(slash);
      const auto before =
          std::find_if(entries.begin(), entries.begin() + static_cast<qsizetype>(index), [&](const auto &entry) {
            return pathOf(entry) == parent;
          });
      QVERIFY2(before != entries.begin() + static_cast<qsizetype>(index), qPrintable(path));
    }
  }

  // listed again for the next paste: each keeps its identifier, nothing more is kept
  std::vector<MSWindowsVirtualFileEntry> again;
  QVERIFY(source.list(again));
  QCOMPARE(again.size(), entries.size());
  for (const auto &entry : again) {
    QCOMPARE(entry.sourceId, byPath[pathOf(entry)].sourceId);
  }
  QVERIFY(source.open(*ids.rbegin() + 1, 0) == nullptr);
}

void MSWindowsClipboardFilesTests::remoteSource_tooManyEntries_stopsListing()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QDir root(directory.path());
  for (int index = 0; index < 20; ++index) {
    const QString sub = QStringLiteral("many/sub%1").arg(index);
    QVERIFY(root.mkpath(sub));
    QVERIFY(writeFile(root.filePath(sub + "/file.txt"), "in a directory"));
  }
  Link link;
  const ClipboardFileList list = copyOnA(link, {root.filePath("many")});
  MSWindowsRemoteFileSource::Limits limits;
  limits.maxEntries = 5;
  MSWindowsRemoteFileSource source(link.remoteOfB, list, limits);

  const uint64_t before = requestsOf(link.askedByB);
  std::vector<MSWindowsVirtualFileEntry> entries;
  QVERIFY(source.list(entries));
  QCOMPARE(entries.size(), size_t{5});
  // the copied entries and the copied directory are asked for, the directories in it are not
  QCOMPARE(requestsOf(link.askedByB) - before, uint64_t{2});
}

void MSWindowsClipboardFilesTests::remoteSource_namesThatDifferInCase_getANumber()
{
  Link link;
  const ClipboardFileList list = copyOnA(link, {sourcePath("one/Report.txt"), sourcePath("two/report.TXT")});
  QCOMPARE(list.entries.size(), size_t{2});
  MSWindowsRemoteFileSource source(link.remoteOfB, list);

  std::vector<MSWindowsVirtualFileEntry> entries;
  QVERIFY(source.list(entries));
  QCOMPARE(entries.size(), size_t{2});
  QCOMPARE(pathOf(entries[0]), QStringLiteral("Report.txt"));
  QCOMPARE(pathOf(entries[1]), QStringLiteral("report (2).TXT"));

  // and each is read from where it came from
  for (size_t index = 0; index < entries.size(); ++index) {
    auto reader = source.open(entries[index].sourceId, 0);
    QVERIFY(reader != nullptr);
    char buffer[256];
    uint32_t got = 0;
    QVERIFY(reader->read(buffer, sizeof(buffer), got));
    QCOMPARE(
        QByteArray(buffer, static_cast<qsizetype>(got)),
        index == 0 ? QByteArray("the first report") : QByteArray("the second report")
    );
  }
}

void MSWindowsClipboardFilesTests::remoteSource_readsFilesAsTheyAreNow()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = QDir(directory.path()).filePath("changes.bin");
  QVERIFY(writeFile(path, "short"));

  Link link;
  const ClipboardFileList list = copyOnA(link, {path});
  MSWindowsRemoteFileSource source(link.remoteOfB, list);

  // changed between copying and pasting
  const QByteArray now = patternBlock(43, 300 * 1024);
  QVERIFY(writeFile(path, now));
  std::vector<MSWindowsVirtualFileEntry> entries;
  QVERIFY(source.list(entries));
  QCOMPARE(entries.size(), size_t{1});
  QCOMPARE(entries[0].size, uint64_t(now.size()));

  const auto readFrom = [&](uint64_t offset) {
    auto reader = source.open(entries[0].sourceId, offset);
    QByteArray data;
    char buffer[64 * 1024];
    for (;;) {
      uint32_t got = 0;
      if (!reader->read(buffer, sizeof(buffer), got)) {
        return QByteArray("read failed");
      }
      if (got == 0) {
        return data;
      }
      data.append(buffer, static_cast<qsizetype>(got));
    }
  };
  QCOMPARE(readFrom(0), now);
  QCOMPARE(readFrom(1000), now.mid(1000));
  QVERIFY(source.open(9999, 0) == nullptr);
}

void MSWindowsClipboardFilesTests::remoteSource_goneFiles_areLeftOut()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString gone = QDir(directory.path()).filePath("gone.txt");
  const QString stays = QDir(directory.path()).filePath("stays.txt");
  QVERIFY(writeFile(gone, "soon gone"));
  QVERIFY(writeFile(stays, "stays"));

  Link link;
  const ClipboardFileList list = copyOnA(link, {gone, stays});
  MSWindowsRemoteFileSource source(link.remoteOfB, list);
  QVERIFY(QFile::remove(gone));

  std::vector<MSWindowsVirtualFileEntry> entries;
  QVERIFY(source.list(entries));
  QCOMPARE(entries.size(), size_t{1});
  QCOMPARE(pathOf(entries[0]), QStringLiteral("stays.txt"));
}

void MSWindowsClipboardFilesTests::remoteSource_otherComputerGone_listFails()
{
  Link link;
  const ClipboardFileList list = copyOnA(link, {sourcePath("tree")});
  MSWindowsRemoteFileSource source(link.remoteOfB, list);
  link.loop.call([&] {
    link.aToB.cut();
    link.bToA.cut();
    link.b->channelClosed(&link.bToA);
  });

  std::vector<MSWindowsVirtualFileEntry> entries;
  QElapsedTimer timer;
  timer.start();
  QVERIFY(!source.list(entries));
  QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
}

void MSWindowsClipboardFilesTests::shell_pastesTheFilesOfTheOtherComputer()
{
  Link link;
  const ClipboardFileList list = copyOnA(link, {sourcePath("tree"), sourcePath("big.bin")});
  auto *object = new MSWindowsVirtualFileDataObject(std::make_shared<MSWindowsRemoteFileSource>(link.remoteOfB, list));

  QTemporaryDir destination;
  QVERIFY(destination.isValid());
  const std::wstring destinationPath = QDir::toNativeSeparators(destination.path()).toStdWString();
  IShellItem *folder = nullptr;
  HRESULT result = SHCreateItemFromParsingName(destinationPath.c_str(), nullptr, IID_PPV_ARGS(&folder));
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("no shell item: 0x%1").arg(result, 8, 16)));
  IDropTarget *target = nullptr;
  result = folder->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target));
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("no drop target: 0x%1").arg(result, 8, 16)));

  // what Explorer does when files are pasted into a folder
  const POINTL point = {0, 0};
  DWORD effect = DROPEFFECT_COPY;
  result = target->DragEnter(object, MK_LBUTTON, point, &effect);
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("DragEnter failed: 0x%1").arg(result, 8, 16)));
  effect = DROPEFFECT_COPY;
  result = target->Drop(object, MK_LBUTTON, point, &effect);
  QVERIFY2(SUCCEEDED(result), qPrintable(QStringLiteral("Drop failed: 0x%1").arg(result, 8, 16)));

  QMap<QString, QByteArray> expected;
  describe(sourcePath("tree"), "tree", expected);
  describe(sourcePath("big.bin"), "big.bin", expected);
  QMap<QString, QByteArray> pasted;
  const auto complete = [&] {
    if (object->inAsyncOperation()) {
      return false;
    }
    pasted.clear();
    describe(destination.filePath("tree"), "tree", pasted);
    describe(destination.filePath("big.bin"), "big.bin", pasted);
    return pasted == expected;
  };
  const bool done = waitFor(complete, 120000);
  qInfo("pasted %llu bytes fetched", static_cast<unsigned long long>(link.askedByB.totalData()));
  QCOMPARE(pasted, expected);
  QVERIFY(done);

  target->Release();
  folder->Release();
  object->Release();
}

void MSWindowsClipboardFilesTests::clipboard_holdsTheFilesMarkedAsDeskflows()
{
  Link link;
  const ClipboardFileList list = copyOnA(link, {sourcePath("tree/a.txt"), sourcePath("tree/nested")});
  MSWindowsClipboardFiles clipboard(link.remoteOfB);
  const auto copied = received("a.txt\nnested", &list);
  const DWORD before = GetClipboardSequenceNumber();
  clipboard.set(copied.get());
  QVERIFY(finished(clipboard));
  QVERIFY(GetClipboardSequenceNumber() != before);
  QCOMPARE(clipboard.filesPut(), uint64_t{1});

  // what the computer looks at to tell its own clipboard from a copy of the user
  QVERIFY(MSWindowsClipboard::isOwnedByDeskflow());
  QVERIFY(hasFileDescriptors());
  QCOMPARE(clipboardText(), QStringLiteral("a.txt\r\nnested"));

  IDataObject *pasted = nullptr;
  QCOMPARE(OleGetClipboard(&pasted), S_OK);
  FORMATETC descriptors = {
      static_cast<CLIPFORMAT>(MSWindowsVirtualFileDataObject::fileDescriptorFormat()), nullptr, DVASPECT_CONTENT, -1,
      TYMED_HGLOBAL
  };
  STGMEDIUM medium = {};
  QCOMPARE(pasted->GetData(&descriptors, &medium), S_OK);
  const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(medium.hGlobal));
  QVERIFY(group != nullptr);
  QStringList names;
  for (UINT index = 0; index < group->cItems; ++index) {
    names.append(QString::fromWCharArray(group->fgd[index].cFileName));
  }
  GlobalUnlock(medium.hGlobal);
  ReleaseStgMedium(&medium);
  QCOMPARE(names, QStringList({"a.txt", "nested", "nested\\deeper", "nested\\deeper\\c.txt"}));

  FORMATETC contents = {
      static_cast<CLIPFORMAT>(MSWindowsVirtualFileDataObject::fileContentsFormat()), nullptr, DVASPECT_CONTENT, 3,
      TYMED_ISTREAM
  };
  QCOMPARE(pasted->GetData(&contents, &medium), S_OK);
  QCOMPARE(readStream(medium.pstm), QByteArray("deep down"));
  ReleaseStgMedium(&medium);
  pasted->Release();

  // files of another computer are not files of this one
  std::vector<std::string> paths;
  QVERIFY(!MSWindowsClipboardFiles::readFiles(g_window, paths));
}

void MSWindowsClipboardFilesTests::clipboard_laterClipboardWins()
{
  Link link;
  const ClipboardFileList list = copyOnA(link, {sourcePath("tree/a.txt")});
  MSWindowsClipboardFiles clipboard(link.remoteOfB);
  const auto files = received("files", &list);
  const auto text = received("later", nullptr);
  clipboard.set(files.get());
  clipboard.set(text.get());
  QVERIFY(finished(clipboard));

  QVERIFY(!hasFileDescriptors());
  QCOMPARE(clipboardText(), QStringLiteral("later"));
  QVERIFY(MSWindowsClipboard::isOwnedByDeskflow());
}

void MSWindowsClipboardFilesTests::clipboard_sameClipboardTwice_putsFilesOnce()
{
  Link link;
  const ClipboardFileList list = copyOnA(link, {sourcePath("tree/a.txt")});
  MSWindowsClipboardFiles clipboard(link.remoteOfB);
  const auto files = received("", &list);

  // once for each clipboard ID, one right after the other
  clipboard.set(files.get());
  clipboard.set(files.get());
  QVERIFY(finished(clipboard));
  QCOMPARE(clipboard.filesPut(), uint64_t{1});
  // and once it is there
  clipboard.set(files.get());
  QVERIFY(finished(clipboard));
  QCOMPARE(clipboard.filesPut(), uint64_t{1});

  // replaced by somebody else in between, it is written again
  QVERIFY(openClipboard());
  EmptyClipboard();
  CloseClipboard();
  clipboard.set(files.get());
  QVERIFY(finished(clipboard));
  QCOMPARE(clipboard.filesPut(), uint64_t{2});
  QVERIFY(hasFileDescriptors());
}

void MSWindowsClipboardFilesTests::clipboard_heldOpenByAnother_isWrittenOnceFree()
{
  using enum MSWindowsClipboardFiles::Ownership;
  Link link;
  std::atomic<int> written = 0;
  MSWindowsClipboardFiles clipboard(link.remoteOfB, [&written] { ++written; });
  {
    // emptied by another application, which keeps it open for a while
    ClipboardHolder holder([] { EmptyClipboard(); });
    QVERIFY(holder.open());
    QCOMPARE(clipboard.ownership(), Other);

    const auto text = received("written later", nullptr);
    clipboard.set(text.get());
    QTest::qWait(300);
    // half way nothing can be told, the computer must not take it for a copy of the user
    QCOMPARE(clipboard.ownership(), Unknown);
    QCOMPARE(written.load(), 0);
    holder.release();
    QVERIFY(holder.closed());
  }
  QVERIFY(finished(clipboard));
  QVERIFY(waitFor([&written] { return written.load() > 0; }, 2000));
  QCOMPARE(clipboard.ownership(), Deskflow);
  QCOMPARE(clipboardText(), QStringLiteral("written later"));
}

void MSWindowsClipboardFilesTests::clipboard_readWhileWritten_isRendered()
{
  Link link;
  const ClipboardFileList list = copyOnA(link, {sourcePath("tree/a.txt")});
  MSWindowsClipboardFiles clipboard(link.remoteOfB);
  const auto files = received("with the files", &list);
  clipboard.set(files.get());
  QVERIFY(finished(clipboard));

  // a clipboard tool reads the text just as the next clipboard arrives. it keeps the clipboard
  // open until the thread that put the files there renders the text.
  QString read;
  ClipboardHolder reader({}, [&read] { read = textOfOpenClipboard(); });
  QVERIFY(reader.open());
  const auto text = received("the next one", nullptr);
  clipboard.set(text.get());
  QTest::qWait(200);
  QElapsedTimer timer;
  timer.start();
  reader.release();
  QVERIFY(reader.closed());
  const qint64 rendering = timer.elapsed();
  QVERIFY(finished(clipboard));

  QCOMPARE(read, QStringLiteral("with the files"));
  QVERIFY2(rendering < 1000, qPrintable(QString::number(rendering)));
  QCOMPARE(clipboardText(), QStringLiteral("the next one"));
}

void MSWindowsClipboardFilesTests::clipboard_writeGivenUp_isWrittenNextTime()
{
  Link link;
  MSWindowsClipboardFiles clipboard(link.remoteOfB);
  const auto text = received("written at last", nullptr);
  {
    // kept open for longer than the thread tries
    ClipboardHolder holder;
    QVERIFY(holder.open());
    clipboard.set(text.get());
    QVERIFY(finished(clipboard));
    holder.release();
    QVERIFY(holder.closed());
  }
  QVERIFY(clipboardText() != QStringLiteral("written at last"));

  // the same clipboard comes again, for the other clipboard ID or later: it is written now
  clipboard.set(text.get());
  QVERIFY(finished(clipboard));
  QCOMPARE(clipboardText(), QStringLiteral("written at last"));
}

void MSWindowsClipboardFilesTests::clipboard_emptied_isDeskflows()
{
  Link link;
  MSWindowsClipboardFiles clipboard(link.remoteOfB);
  const auto text = received("something", nullptr);
  clipboard.set(text.get());
  clipboard.set(nullptr);
  QVERIFY(finished(clipboard));
  QVERIFY(MSWindowsClipboard::isOwnedByDeskflow());
  QVERIFY(IsClipboardFormatAvailable(CF_UNICODETEXT) == FALSE);
}

void MSWindowsClipboardFilesTests::clipboard_gone_takesTheFilesOff()
{
  Link link;
  const ClipboardFileList list = copyOnA(link, {sourcePath("tree/a.txt")});
  auto clipboard = std::make_unique<MSWindowsClipboardFiles>(link.remoteOfB);
  const auto files = received("", &list);
  clipboard->set(files.get());
  QVERIFY(finished(*clipboard));
  QVERIFY(hasFileDescriptors());

  // the files can't be fetched once it is gone, they must not stay on the clipboard
  clipboard.reset();
  QVERIFY(!hasFileDescriptors());
}

void MSWindowsClipboardFilesTests::clipboard_goneWhileHeldOpen_takesTheFilesOff()
{
  Link link;
  const ClipboardFileList list = copyOnA(link, {sourcePath("tree/a.txt")});
  auto clipboard = std::make_unique<MSWindowsClipboardFiles>(link.remoteOfB);
  const auto files = received("", &list);
  clipboard->set(files.get());
  QVERIFY(finished(*clipboard));
  QVERIFY(hasFileDescriptors());

  // another application has the clipboard open as it goes, for a moment
  ClipboardHolder holder;
  QVERIFY(holder.open());
  std::thread later([&holder] {
    Sleep(300);
    holder.release();
  });
  clipboard.reset();
  later.join();
  QVERIFY(holder.closed());
  QVERIFY(!hasFileDescriptors());
}

void MSWindowsClipboardFilesTests::readFiles_reportsCopiedPaths()
{
  const QStringList copied = {
      QDir::toNativeSeparators(sourcePath("tree/a.txt")),
      QDir::toNativeSeparators(sourcePath(QStringLiteral("tree/中文 目录")))
  };

  // what Explorer puts on the clipboard: a list of paths, each ended by a zero, and one more zero
  std::wstring list;
  for (const QString &path : copied) {
    list += path.toStdWString();
    list.push_back(L'\0');
  }
  list.push_back(L'\0');
  const size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
  HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  QVERIFY(global != nullptr);
  auto *drop = static_cast<DROPFILES *>(GlobalLock(global));
  drop->pFiles = sizeof(DROPFILES);
  drop->fWide = TRUE;
  std::memcpy(reinterpret_cast<BYTE *>(drop) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
  GlobalUnlock(global);
  QVERIFY(openClipboard());
  EmptyClipboard();
  QVERIFY(SetClipboardData(CF_HDROP, global) != nullptr);
  CloseClipboard();

  std::vector<std::string> paths;
  QVERIFY(MSWindowsClipboardFiles::readFiles(g_window, paths));
  QCOMPARE(paths.size(), size_t{2});
  QCOMPARE(QString::fromStdString(paths[0]), copied[0]);
  QCOMPARE(QString::fromStdString(paths[1]), copied[1]);
  // nothing that marks it as Deskflow's: the user copied it
  QVERIFY(!MSWindowsClipboard::isOwnedByDeskflow());
}

QTEST_MAIN(MSWindowsClipboardFilesTests)
