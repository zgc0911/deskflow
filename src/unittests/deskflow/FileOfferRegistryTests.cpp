/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileOfferRegistryTests.h"

#include "deskflow/FileOfferRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

using deskflow::ClipboardFileList;
using deskflow::FileOfferRegistry;

namespace {

bool writeFile(const QString &path, const QByteArray &data)
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

} // namespace

QString FileOfferRegistryTests::path(const QString &relative) const
{
  return QDir(m_dir->path()).filePath(relative);
}

std::string FileOfferRegistryTests::stdPath(const QString &relative) const
{
  return path(relative).toStdString();
}

void FileOfferRegistryTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Debug);

  // copied/        what the user copies
  //   a.txt
  //   dir/b.bin
  //   dir/sub/c.txt
  //   dir/way-out   -> ../../secret   (a link that leads outside)
  // secret/s.txt    must never be reachable
  m_dir = std::make_unique<QTemporaryDir>();
  QVERIFY(m_dir->isValid());
  QDir root(m_dir->path());
  QVERIFY(root.mkpath("copied/dir/sub"));
  QVERIFY(root.mkpath("copied/other"));
  QVERIFY(root.mkpath("secret"));
  QVERIFY(writeFile(path("copied/a.txt"), "0123456789"));
  QVERIFY(writeFile(path("copied/dir/b.bin"), QByteArray(1000, 'b')));
  QVERIFY(writeFile(path("copied/dir/sub/c.txt"), "c"));
  QVERIFY(writeFile(path("copied/other/a.txt"), "same name, other folder"));
  QVERIFY(writeFile(path("secret/s.txt"), "secret"));

  // creating links needs a privilege on Windows, the tests that need them are skipped without it
  m_haveLinks = QFile::link(path("secret"), path("copied/dir/way-out")) &&
                QFile::link(path("copied/a.txt"), path("copied/link-to-a")) &&
                QFileInfo(path("copied/dir/way-out")).isSymbolicLink();
}

void FileOfferRegistryTests::offer_listsTopLevelEntries()
{
  FileOfferRegistry registry;
  const ClipboardFileList list = registry.offer({stdPath("copied/a.txt"), stdPath("copied/dir")});

  QVERIFY(list.offerId != 0);
  QCOMPARE(list.entries.size(), size_t{2});

  QCOMPARE(list.entries[0].name, std::string("a.txt"));
  QVERIFY(!list.entries[0].isDirectory);
  QCOMPARE(list.entries[0].size, uint64_t{10});
  QCOMPARE(list.entries[0].modifiedTime, QFileInfo(path("copied/a.txt")).lastModified().toSecsSinceEpoch());

  // a directory is listed by itself, its contents are asked for later
  QCOMPARE(list.entries[1].name, std::string("dir"));
  QVERIFY(list.entries[1].isDirectory);
  QCOMPARE(list.entries[1].size, uint64_t{0});

  QVERIFY(registry.isOffered(list.offerId));
  QVERIFY(!registry.isOffered(list.offerId + 1));
  QVERIFY(!registry.isOffered(0));

  // nothing of the local path shows in what other computers are told
  const std::string marshalled = list.marshall();
  QVERIFY(marshalled.find(m_dir->path().toStdString()) == std::string::npos);
  QVERIFY(marshalled.find("copied") == std::string::npos);
}

void FileOfferRegistryTests::offer_skipsMissingLinksAndDuplicates()
{
  FileOfferRegistry registry;
  std::vector<std::string> paths = {
      stdPath("copied/missing.txt"), stdPath("copied/a.txt"), stdPath("copied/other/a.txt"), stdPath("copied/dir")
  };
  if (m_haveLinks) {
    paths.push_back(stdPath("copied/link-to-a"));
    paths.push_back(stdPath("copied/dir/way-out"));
  }

  const ClipboardFileList list = registry.offer(paths);
  QCOMPARE(list.entries.size(), size_t{2});
  QCOMPARE(list.entries[0].name, std::string("a.txt"));
  QCOMPARE(list.entries[0].size, uint64_t{10});
  QCOMPARE(list.entries[1].name, std::string("dir"));

  // the indexes other computers use follow the entries that were kept
  QString localPath;
  QCOMPARE(registry.resolve(list.offerId, 1, "b.bin", localPath), FileOfferRegistry::Resolved::Ok);
  QCOMPARE(localPath, QFileInfo(path("copied/dir/b.bin")).canonicalFilePath());
}

void FileOfferRegistryTests::offer_nothingUsable_offersNothing()
{
  FileOfferRegistry registry;
  const ClipboardFileList first = registry.offer({stdPath("copied/a.txt")});
  QVERIFY(first.offerId != 0);

  const ClipboardFileList none = registry.offer({stdPath("copied/missing.txt")});
  QCOMPARE(none.offerId, uint64_t{0});
  QVERIFY(none.entries.empty());

  QVERIFY(registry.offer({}).entries.empty());
}

void FileOfferRegistryTests::offer_tooManyEntries_offersNothing()
{
  QTemporaryDir many;
  QVERIFY(many.isValid());
  std::vector<std::string> paths;
  for (size_t i = 0; i <= ClipboardFileList::s_maxEntries; ++i) {
    const QString file = QDir(many.path()).filePath(QStringLiteral("f%1").arg(i));
    QVERIFY(writeFile(file, "x"));
    paths.push_back(file.toStdString());
  }

  FileOfferRegistry registry;
  const ClipboardFileList tooMany = registry.offer(paths);
  QVERIFY(tooMany.entries.empty());
  QCOMPARE(tooMany.offerId, uint64_t{0});

  paths.pop_back();
  const ClipboardFileList atLimit = registry.offer(paths);
  QCOMPARE(atLimit.entries.size(), ClipboardFileList::s_maxEntries);
  ClipboardFileList decoded;
  QVERIFY(ClipboardFileList::unmarshall(atLimit.marshall(), decoded));
}

void FileOfferRegistryTests::offer_sameFiles_sameIdentifier()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString file = QDir(dir.path()).filePath("grows.txt");
  QVERIFY(writeFile(file, "short"));

  FileOfferRegistry registry;
  const std::vector<std::string> paths = {file.toStdString(), stdPath("copied/dir")};
  const ClipboardFileList first = registry.offer(paths);
  const ClipboardFileList again = registry.offer(paths);
  // the clipboard is only resent when its data changes, so this must be stable
  QCOMPARE(again.offerId, first.offerId);
  QCOMPARE(again.marshall(), first.marshall());

  // other files, another identifier
  const ClipboardFileList fewer = registry.offer({file.toStdString()});
  QVERIFY(fewer.offerId != first.offerId);

  // the same path with other contents, another identifier
  QVERIFY(writeFile(file, "now a good deal longer than before"));
  const ClipboardFileList changed = registry.offer({file.toStdString()});
  QVERIFY(changed.offerId != fewer.offerId);
  QCOMPARE(changed.entries[0].size, uint64_t{34});
}

void FileOfferRegistryTests::offer_differentRegistries_differentIdentifiers()
{
  // identifiers must not be guessable from the files alone
  FileOfferRegistry first;
  FileOfferRegistry second;
  const std::vector<std::string> paths = {stdPath("copied/a.txt")};
  QVERIFY(first.offer(paths).offerId != second.offer(paths).offerId);
}

void FileOfferRegistryTests::resolve_findsFilesAndDirectories()
{
  FileOfferRegistry registry;
  const ClipboardFileList list = registry.offer({stdPath("copied/a.txt"), stdPath("copied/dir")});
  QString localPath;

  QCOMPARE(registry.resolve(list.offerId, 0, "", localPath), FileOfferRegistry::Resolved::Ok);
  QCOMPARE(localPath, QFileInfo(path("copied/a.txt")).canonicalFilePath());

  QCOMPARE(registry.resolve(list.offerId, 1, "", localPath), FileOfferRegistry::Resolved::Ok);
  QCOMPARE(localPath, QFileInfo(path("copied/dir")).canonicalFilePath());

  QCOMPARE(registry.resolve(list.offerId, 1, "sub", localPath), FileOfferRegistry::Resolved::Ok);
  QVERIFY(QFileInfo(localPath).isDir());

  QCOMPARE(registry.resolve(list.offerId, 1, "sub/c.txt", localPath), FileOfferRegistry::Resolved::Ok);
  QCOMPARE(localPath, QFileInfo(path("copied/dir/sub/c.txt")).canonicalFilePath());
}

void FileOfferRegistryTests::resolve_badPaths_data()
{
  QTest::addColumn<int>("item");
  QTest::addColumn<QByteArray>("relativePath");

  QTest::newRow("parent") << 1 << QByteArray("..");
  QTest::newRow("parent of file") << 0 << QByteArray("..");
  QTest::newRow("sibling through parent") << 1 << QByteArray("../a.txt");
  QTest::newRow("up and out") << 1 << QByteArray("sub/../../../secret/s.txt");
  QTest::newRow("parent in the middle") << 1 << QByteArray("sub/../b.bin");
  QTest::newRow("current directory") << 1 << QByteArray("./b.bin");
  QTest::newRow("trailing dot") << 1 << QByteArray("sub/.");
  QTest::newRow("absolute") << 1 << QByteArray("/etc/passwd");
  QTest::newRow("empty component") << 1 << QByteArray("sub//c.txt");
  QTest::newRow("trailing separator") << 1 << QByteArray("sub/");
  QTest::newRow("only a separator") << 1 << QByteArray("/");
  QTest::newRow("nul byte") << 1 << QByteArray("b.bin\0.txt", 10);
  QTest::newRow("item out of range") << 2 << QByteArray();
  QTest::newRow("item far out of range") << 1000000 << QByteArray("b.bin");
#if defined(Q_OS_WIN)
  QTest::newRow("backslash") << 1 << QByteArray("sub\\c.txt");
  QTest::newRow("backslash parent") << 1 << QByteArray("..\\a.txt");
  QTest::newRow("drive") << 1 << QByteArray("C:");
  QTest::newRow("stream") << 1 << QByteArray("b.bin:stream");
#endif
}

void FileOfferRegistryTests::resolve_badPaths()
{
  QFETCH(int, item);
  QFETCH(QByteArray, relativePath);

  FileOfferRegistry registry;
  const ClipboardFileList list = registry.offer({stdPath("copied/a.txt"), stdPath("copied/dir")});
  QString localPath = QStringLiteral("untouched");
  QCOMPARE(
      registry.resolve(
          list.offerId, static_cast<uint32_t>(item),
          std::string_view(relativePath.constData(), static_cast<size_t>(relativePath.size())), localPath
      ),
      FileOfferRegistry::Resolved::BadPath
  );
  QVERIFY(localPath.isEmpty());
}

void FileOfferRegistryTests::resolve_linkLeadingOutside_rejected()
{
  if (!m_haveLinks) {
    QSKIP("symbolic links can not be created here");
  }

  FileOfferRegistry registry;
  const ClipboardFileList list = registry.offer({stdPath("copied/dir")});
  QString localPath;

  // the link itself and everything behind it is outside of what was copied
  QCOMPARE(registry.resolve(list.offerId, 0, "way-out", localPath), FileOfferRegistry::Resolved::BadPath);
  QVERIFY(localPath.isEmpty());
  QCOMPARE(registry.resolve(list.offerId, 0, "way-out/s.txt", localPath), FileOfferRegistry::Resolved::BadPath);
  QVERIFY(localPath.isEmpty());
}

void FileOfferRegistryTests::resolve_missingFile_notFound()
{
  FileOfferRegistry registry;
  const ClipboardFileList list = registry.offer({stdPath("copied/a.txt"), stdPath("copied/dir")});
  QString localPath;
  QCOMPARE(registry.resolve(list.offerId, 1, "nope.txt", localPath), FileOfferRegistry::Resolved::NotFound);
  QCOMPARE(registry.resolve(list.offerId, 1, "sub/nope/deeper", localPath), FileOfferRegistry::Resolved::NotFound);
  // a file has nothing below it
  QCOMPARE(registry.resolve(list.offerId, 0, "below", localPath), FileOfferRegistry::Resolved::NotFound);
}

void FileOfferRegistryTests::resolve_unknownOffer()
{
  FileOfferRegistry registry;
  const ClipboardFileList list = registry.offer({stdPath("copied/a.txt")});
  QString localPath;
  QCOMPARE(registry.resolve(list.offerId ^ 1, 0, "", localPath), FileOfferRegistry::Resolved::UnknownOffer);
  QCOMPARE(registry.resolve(0, 0, "", localPath), FileOfferRegistry::Resolved::UnknownOffer);

  FileOfferRegistry empty;
  QCOMPARE(empty.resolve(list.offerId, 0, "", localPath), FileOfferRegistry::Resolved::UnknownOffer);
}

void FileOfferRegistryTests::replacedOffer_expiresWhenUnused()
{
  int64_t now = 1000;
  FileOfferRegistry registry;
  registry.setClock([&now] { return now; });

  const ClipboardFileList first = registry.offer({stdPath("copied/a.txt")});
  const ClipboardFileList second = registry.offer({stdPath("copied/dir")});
  QVERIFY(first.offerId != second.offerId);
  QString localPath;

  // a paste that is under way keeps the replaced offer alive
  for (int i = 0; i < 5; ++i) {
    now += FileOfferRegistry::s_retiredLifetimeMs - 1;
    QCOMPARE(registry.resolve(first.offerId, 0, "", localPath), FileOfferRegistry::Resolved::Ok);
  }

  // left alone for long enough it is gone, the current offer is not affected
  now += FileOfferRegistry::s_retiredLifetimeMs + 1;
  QCOMPARE(registry.resolve(first.offerId, 0, "", localPath), FileOfferRegistry::Resolved::UnknownOffer);
  QVERIFY(!registry.isOffered(first.offerId));
  QCOMPARE(registry.resolve(second.offerId, 0, "b.bin", localPath), FileOfferRegistry::Resolved::Ok);

  now += 100 * FileOfferRegistry::s_retiredLifetimeMs;
  QCOMPARE(registry.resolve(second.offerId, 0, "b.bin", localPath), FileOfferRegistry::Resolved::Ok);
}

void FileOfferRegistryTests::withdrawnOffer_expiresWhenUnused()
{
  int64_t now = 0;
  FileOfferRegistry registry;
  registry.setClock([&now] { return now; });

  const ClipboardFileList list = registry.offer({stdPath("copied/a.txt")});
  registry.withdraw();
  QString localPath;
  now += FileOfferRegistry::s_retiredLifetimeMs;
  QCOMPARE(registry.resolve(list.offerId, 0, "", localPath), FileOfferRegistry::Resolved::Ok);

  now += FileOfferRegistry::s_retiredLifetimeMs + 1;
  QCOMPARE(registry.resolve(list.offerId, 0, "", localPath), FileOfferRegistry::Resolved::UnknownOffer);

  // offering the same files again makes them available again, under the same identifier
  const ClipboardFileList again = registry.offer({stdPath("copied/a.txt")});
  QCOMPARE(again.offerId, list.offerId);
  QCOMPARE(registry.resolve(list.offerId, 0, "", localPath), FileOfferRegistry::Resolved::Ok);
}

QTEST_MAIN(FileOfferRegistryTests)
