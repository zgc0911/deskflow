/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXClipboardFilesTests.h"

#include "../deskflow/FileTransferTestLink.h"

#include "platform/OSXClipboard.h"
#include "platform/OSXClipboardFiles.h"
#include "platform/OSXPasteboardFiles.h"
#include "platform/OSXRemoteFileVolume.h"

#include <QElapsedTimer>

#include <sys/mount.h>

using deskflow::ClipboardFileList;
using deskflow::OSXRemoteFileVolume;
using namespace deskflow::test;

namespace {

void unmountEverythingBelow(const QString &directory)
{
  const std::string base = QFileInfo(directory).canonicalFilePath().toStdString() + "/";
  const int count = ::getfsstat(nullptr, 0, MNT_NOWAIT);
  if (count <= 0) {
    return;
  }
  std::vector<struct statfs> mounts(static_cast<size_t>(count) + 8);
  const int filled = ::getfsstat(mounts.data(), static_cast<int>(mounts.size() * sizeof(struct statfs)), MNT_NOWAIT);
  for (int i = 0; i < filled; ++i) {
    const std::string point = mounts[static_cast<size_t>(i)].f_mntonname;
    if (base.size() > 1 && point.rfind(base, 0) == 0) {
      ::unmount(point.c_str(), MNT_FORCE);
    }
  }
}

std::string canonical(const QString &path)
{
  return QFileInfo(path).canonicalFilePath().toStdString();
}

//! The clipboard of a computer that pastes, linked to a computer that copies
struct Clipboard
{
  explicit Clipboard(const std::string &mountCommand = "/sbin/mount_nfs") : base(std::make_unique<QTemporaryDir>())
  {
    OSXRemoteFileVolume::Options options;
    options.directory = QDir(base->path()).filePath("volumes").toStdString();
    options.mountCommand = mountCommand;
    options.limits.waitMs = 2000;
    auto ownPasteboard = std::make_unique<OSXPasteboardFiles>(true);
    pasteboard = ownPasteboard.get();
    auto ownVolume = std::make_unique<OSXRemoteFileVolume>(link.remoteOfB, options);
    volume = ownVolume.get();
    mounted = volume->waitUntilMounted(10000);
    files = std::make_unique<OSXClipboardFiles>(std::move(ownPasteboard), std::move(ownVolume));
  }

  ~Clipboard()
  {
    link.release();
    files.reset();
    unmountEverythingBelow(base->path());
  }

  ClipboardFileList copy(const std::vector<QString> &paths)
  {
    std::vector<std::string> local;
    for (const auto &path : paths) {
      local.push_back(path.toStdString());
    }
    return link.copyOnA(local);
  }

  std::vector<std::string> onPasteboard() const
  {
    std::vector<std::string> paths;
    pasteboard->read(paths);
    return paths;
  }

  std::unique_ptr<QTemporaryDir> base;
  Link link;
  std::unique_ptr<OSXClipboardFiles> files;
  OSXPasteboardFiles *pasteboard = nullptr;
  OSXRemoteFileVolume *volume = nullptr;
  bool mounted = false;
};

QByteArray contentsOf(const std::string &path)
{
  QFile file(QString::fromStdString(path));
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("unreadable");
}

} // namespace

QString OSXClipboardFilesTests::sourcePath(const QString &relative) const
{
  return QDir(m_source->path()).filePath(relative);
}

void OSXClipboardFilesTests::initTestCase()
{
  if (!QFileInfo::exists("/sbin/mount_nfs")) {
    QSKIP("this system has no NFS client");
  }
  m_source = std::make_unique<QTemporaryDir>();
  QVERIFY(m_source->isValid());
  QVERIFY(QDir().mkpath(sourcePath(QStringLiteral("文件夹/inside"))));
  QVERIFY(writeFile(sourcePath("a.txt"), "first"));
  QVERIFY(writeFile(sourcePath("b.txt"), "second"));
  QVERIFY(writeFile(sourcePath(QStringLiteral("文件夹/inside/c.txt")), "third"));
}

void OSXClipboardFilesTests::cleanupTestCase()
{
  m_source.reset();
}

void OSXClipboardFilesTests::put_showsTheFilesAndWritesTheirPaths()
{
  Clipboard clipboard;
  QVERIFY(clipboard.mounted);
  const ClipboardFileList list = clipboard.copy({sourcePath("a.txt"), sourcePath(QStringLiteral("文件夹"))});
  QCOMPARE(list.entries.size(), size_t{2});

  QVERIFY(clipboard.files->put(list));
  const auto paths = clipboard.onPasteboard();
  QCOMPARE(paths.size(), size_t{2});
  for (const auto &path : paths) {
    QVERIFY2(clipboard.volume->contains(path), path.c_str());
  }
  QVERIFY(QString::fromStdString(paths[0]).endsWith("/a.txt"));
  QVERIFY(QString::fromStdString(paths[1]).endsWith(QStringLiteral("/文件夹")));
  QVERIFY(clipboard.files->holdsOwnWrite());

  // what a program that pastes gets at those paths
  QCOMPARE(contentsOf(paths[0]), QByteArray("first"));
  QCOMPARE(contentsOf(paths[1] + "/inside/c.txt"), QByteArray("third"));
}

void OSXClipboardFilesTests::put_sameFilesAgain_writesNothing()
{
  Clipboard clipboard;
  QVERIFY(clipboard.mounted);
  const ClipboardFileList list = clipboard.copy({sourcePath("a.txt")});
  QVERIFY(clipboard.files->put(list));
  const long count = clipboard.pasteboard->changeCount();

  // once for each clipboard
  QVERIFY(clipboard.files->put(list));
  QCOMPARE(clipboard.pasteboard->changeCount(), count);
  QVERIFY(clipboard.files->holdsOwnWrite());

  // after something else was copied, the same files are written again
  clipboard.pasteboard->writeText("something else");
  QVERIFY(clipboard.files->put(list));
  QVERIFY(clipboard.pasteboard->changeCount() > count + 1);
  QCOMPARE(clipboard.onPasteboard().size(), size_t{1});
}

void OSXClipboardFilesTests::put_otherFiles_replaceThem()
{
  Clipboard clipboard;
  QVERIFY(clipboard.mounted);
  QVERIFY(clipboard.files->put(clipboard.copy({sourcePath("a.txt")})));
  const auto first = clipboard.onPasteboard();
  QVERIFY(clipboard.files->put(clipboard.copy({sourcePath("b.txt"), sourcePath("a.txt")})));
  const auto second = clipboard.onPasteboard();
  QCOMPARE(second.size(), size_t{2});
  QVERIFY(second != first);
  QCOMPARE(contentsOf(second[0]), QByteArray("second"));
}

void OSXClipboardFilesTests::put_volumeNotMounted_changesNothing()
{
  Clipboard clipboard("/nonexistent/mount_nfs");
  QVERIFY(!clipboard.mounted);
  clipboard.pasteboard->writeText("what the user copied");
  const long count = clipboard.pasteboard->changeCount();

  QVERIFY(!clipboard.files->put(clipboard.copy({sourcePath("a.txt")})));
  QCOMPARE(clipboard.pasteboard->changeCount(), count);
  QVERIFY(!clipboard.files->holdsOwnWrite());
}

void OSXClipboardFilesTests::put_namesThatCanNotBeShown_areLeftOut()
{
  Clipboard clipboard;
  QVERIFY(clipboard.mounted);
  std::string tooLong;
  for (int i = 0; i < 100; ++i) {
    tooLong += "\xe4\xb8\xad";
  }

  ClipboardFileList list;
  list.offerId = 0x51;
  list.entries.push_back({"fine.txt", false, 1, 1});
  list.entries.push_back({tooLong, false, 1, 1});
  QVERIFY(clipboard.files->put(list));
  QCOMPARE(clipboard.onPasteboard().size(), size_t{1});

  // nothing that can be shown: the clipboard stays as it is
  clipboard.pasteboard->writeText("what the user copied");
  const long count = clipboard.pasteboard->changeCount();
  ClipboardFileList none;
  none.offerId = 0x52;
  none.entries.push_back({tooLong, false, 1, 1});
  QVERIFY(!clipboard.files->put(none));
  QCOMPARE(clipboard.pasteboard->changeCount(), count);
}

void OSXClipboardFilesTests::put_doesNotWaitForTheVolume()
{
  Clipboard clipboard;
  QVERIFY(clipboard.mounted);
  const ClipboardFileList list = clipboard.copy({sourcePath("a.txt"), sourcePath(QStringLiteral("文件夹"))});

  // the other computer does not answer: putting the files there and reading them back must not
  // look at them, it would wait for the other computer on the thread of the event queue
  clipboard.link.hold();
  QElapsedTimer timer;
  timer.start();
  QVERIFY(clipboard.files->put(list));
  std::vector<std::string> found;
  QVERIFY(!clipboard.files->get(found));
  QCOMPARE(clipboard.onPasteboard().size(), size_t{2});
  QVERIFY2(timer.elapsed() < 200, qPrintable(QString::number(timer.elapsed())));
  clipboard.link.release();
}

void OSXClipboardFilesTests::get_filesCopiedLikeFinderDoes_areFound()
{
  Clipboard clipboard;
  // Finder refers to the files by their number on the volume
  clipboard.pasteboard->writeFileReferences(
      {sourcePath("a.txt").toStdString(), sourcePath(QStringLiteral("文件夹")).toStdString()}
  );
  std::vector<std::string> paths;
  QVERIFY(clipboard.files->get(paths));
  QCOMPARE(paths.size(), size_t{2});
  QCOMPARE(paths[0], canonical(sourcePath("a.txt")));
  QCOMPARE(paths[1], canonical(sourcePath(QStringLiteral("文件夹"))));

  // and text alone is no file
  clipboard.pasteboard->writeText(sourcePath("a.txt").toStdString());
  QVERIFY(!clipboard.files->get(paths));
  QVERIFY(paths.empty());
}

void OSXClipboardFilesTests::get_filesOfOtherComputers_areLeftOut()
{
  Clipboard clipboard;
  QVERIFY(clipboard.mounted);
  QVERIFY(clipboard.files->put(clipboard.copy({sourcePath("a.txt")})));
  const std::string shown = clipboard.onPasteboard().at(0);

  // a mix that another program copied: one of them is on the volume, of this or of an earlier process
  clipboard.pasteboard->write(
      {shown, sourcePath("b.txt").toStdString(),
       QDir(clipboard.base->path()).filePath("volumes/1-abcd/Deskflow/x.txt").toStdString()},
      {false, false, false}
  );
  std::vector<std::string> paths;
  QVERIFY(clipboard.files->get(paths));
  QCOMPARE(paths, std::vector<std::string>({sourcePath("b.txt").toStdString()}));

  // only files of other computers: nothing is offered
  QVERIFY(clipboard.files->put(clipboard.copy({sourcePath("a.txt")})));
  QVERIFY(!clipboard.files->get(paths));
}

void OSXClipboardFilesTests::holdsOwnWrite_untilSomethingElseIsCopied()
{
  Clipboard clipboard;
  QVERIFY(clipboard.mounted);
  QVERIFY(!clipboard.files->holdsOwnWrite());
  QVERIFY(clipboard.files->put(clipboard.copy({sourcePath("a.txt")})));
  QVERIFY(clipboard.files->holdsOwnWrite());
  clipboard.pasteboard->writeText("copied by the user");
  QVERIFY(!clipboard.files->holdsOwnWrite());
}

void OSXClipboardFilesTests::generalPasteboard_ownWriteIsNotACopy()
{
  // takes over the clipboard of the user for a moment, only when asked to
  if (qEnvironmentVariableIntValue("DESKFLOW_TEST_GENERAL_PASTEBOARD") != 1) {
    QSKIP("set DESKFLOW_TEST_GENERAL_PASTEBOARD=1 to use the clipboard of the user");
  }

  OSXClipboard carbon;
  OSXPasteboardFiles general;
  std::vector<std::string> savedFiles;
  general.read(savedFiles);
  const std::string savedText = carbon.has(IClipboard::Format::Text) ? carbon.get(IClipboard::Format::Text) : "";

  // the check of the computer class looks through the old interface, which sees writes of other
  // references to the pasteboard as changes
  carbon.synchronize();
  const long written = general.write({sourcePath("a.txt").toStdString()}, {false});
  QVERIFY(written >= 0);
  const bool reported = carbon.synchronize();
  qInfo("our own write %s reported as a change", reported ? "is" : "is not");
  QCOMPARE(general.changeCount(), written);
  QVERIFY(!carbon.synchronize());

  // what another program copies is
  OSXPasteboardFiles other;
  other.writeText("copied by someone else");
  QVERIFY(carbon.synchronize());
  QVERIFY(general.changeCount() != written);

  // as it was
  if (!savedFiles.empty()) {
    general.write(savedFiles, std::vector<bool>(savedFiles.size(), false));
  } else {
    other.writeText(savedText);
  }
}

QTEST_MAIN(OSXClipboardFilesTests)
