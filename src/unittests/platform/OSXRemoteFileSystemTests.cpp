/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXRemoteFileSystemTests.h"

#include "../deskflow/FileTransferTestLink.h"

#include "platform/OSXRemoteFileSystem.h"

#include <QElapsedTimer>
#include <QSet>

#include <atomic>
#include <future>
#include <thread>

using deskflow::ClipboardFileEntry;
using deskflow::ClipboardFileList;
using deskflow::OSXRemoteFileSystem;
using deskflow::nfs::Attributes;
using deskflow::nfs::DirectoryEntry;
using deskflow::nfs::FileHandle;
using deskflow::nfs::Status;
using namespace deskflow::test;

namespace {

const char *statusName(Status status)
{
  switch (status) {
  case Status::Ok:
    return "Ok";
  case Status::NoEntry:
    return "NoEntry";
  case Status::Io:
    return "Io";
  case Status::AccessDenied:
    return "AccessDenied";
  case Status::NotDirectory:
    return "NotDirectory";
  case Status::IsDirectory:
    return "IsDirectory";
  case Status::Invalid:
    return "Invalid";
  case Status::ReadOnly:
    return "ReadOnly";
  case Status::Stale:
    return "Stale";
  case Status::BadHandle:
    return "BadHandle";
  case Status::TryLater:
    return "TryLater";
  }
  return "?";
}

//! Short times, so that waiting, giving up and asking again can be seen within a test
OSXRemoteFileSystem::Limits quickLimits()
{
  OSXRemoteFileSystem::Limits limits;
  limits.window = 512 * 1024;
  limits.readAhead = kMiB;
  limits.keepBehind = kMiB;
  limits.span = 8 * kMiB;
  limits.partialWaitMs = 200;
  limits.waitMs = 1000;
  limits.retries = 2;
  limits.idleMs = 400;
  limits.freshMs = 300;
  limits.refreshWaitMs = 1000;
  limits.failureMs = 300;
  return limits;
}

//! Waits long enough for the next call to start a new session
void pause(const OSXRemoteFileSystem::Limits &limits)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(limits.freshMs + 150));
}

//! The file system of the computer that pastes, over a link to the computer that copied
struct Paste
{
  explicit Paste(const OSXRemoteFileSystem::Limits &limits = quickLimits()) : limits(limits)
  {
    files = std::make_unique<OSXRemoteFileSystem>(link.remoteOfB, limits);
  }

  ~Paste()
  {
    // nothing may wait for the other computer when the link goes
    link.release();
    files->shutdown();
  }

  //! A copies \p paths, the file system shows them
  bool copy(const std::vector<QString> &paths)
  {
    std::vector<std::string> local;
    for (const auto &path : paths) {
      local.push_back(path.toStdString());
    }
    list = link.copyOnA(local);
    if (list.entries.empty()) {
      return false;
    }
    shown = files->addOffer(list);
    return true;
  }

  //! Follow a path from the root, the way the system looks things up
  Status find(const std::string &path, FileHandle &handle, Attributes &attributes)
  {
    handle = files->rootHandle();
    attributes = {};
    attributes.isDirectory = true;
    size_t start = 0;
    while (start < path.size()) {
      const size_t slash = path.find('/', start);
      const std::string name = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
      const Status status = files->lookup(handle, name, handle, attributes);
      if (status != Status::Ok) {
        return status;
      }
      start = (slash == std::string::npos) ? path.size() : slash + 1;
    }
    return Status::Ok;
  }

  FileHandle handleOf(const std::string &path)
  {
    FileHandle handle{};
    Attributes attributes;
    const Status status = find(path, handle, attributes);
    if (status != Status::Ok) {
      qWarning("can't find %s: %s", path.c_str(), statusName(status));
    }
    return handle;
  }

  //! The path of a copied entry on the file system
  std::string shownPath(size_t item, const std::string &below = {}) const
  {
    return below.empty() ? shown.at(item) : shown.at(item) + "/" + below;
  }

  //! Read like the system does: asks again when told to come back later, goes on after a short answer
  Status read(const FileHandle &handle, uint64_t offset, uint32_t count, QByteArray &data, bool *endOfFile = nullptr)
  {
    data.clear();
    while (static_cast<uint32_t>(data.size()) < count) {
      std::string piece;
      bool end = false;
      const uint64_t at = offset + static_cast<uint64_t>(data.size());
      const Status status = files->read(handle, at, count - static_cast<uint32_t>(data.size()), piece, end);
      if (status == Status::TryLater) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      if (status != Status::Ok) {
        return status;
      }
      data.append(piece.data(), static_cast<qsizetype>(piece.size()));
      if (end) {
        if (endOfFile != nullptr) {
          *endOfFile = true;
        }
        break;
      }
      if (piece.empty()) {
        return Status::Io;
      }
    }
    return Status::Ok;
  }

  //! Read a whole file in pieces of \p block bytes, from the start to the end
  Status readAll(const FileHandle &handle, QByteArray &data, uint32_t block = 1024 * 1024)
  {
    data.clear();
    for (;;) {
      QByteArray piece;
      bool end = false;
      const Status status = read(handle, static_cast<uint64_t>(data.size()), block, piece, &end);
      if (status != Status::Ok) {
        return status;
      }
      data.append(piece);
      if (end || piece.isEmpty()) {
        return Status::Ok;
      }
    }
  }

  //! Names in a directory, read in pages of \p page entries
  Status names(const FileHandle &directory, QStringList &names, size_t page = 64)
  {
    names.clear();
    uint64_t first = 0;
    for (;;) {
      std::vector<DirectoryEntry> entries;
      bool complete = false;
      const Status status = files->readDirectory(directory, first, page, entries, complete);
      if (status != Status::Ok) {
        return status;
      }
      for (const auto &entry : entries) {
        names.append(QString::fromStdString(entry.name));
      }
      first += entries.size();
      if (complete) {
        names.sort();
        return Status::Ok;
      }
      if (entries.empty()) {
        return Status::Io;
      }
    }
  }

  //! Requests to read a file that were sent to the computer that copied
  size_t readRequests()
  {
    std::scoped_lock lock{link.askedByB.mutex};
    return link.askedByB.allowed.size();
  }

  //! Wait until the downloads are gone and the computer that copied answers nothing any more
  bool settles(int timeoutMs = 5000)
  {
    QElapsedTimer timer;
    timer.start();
    while (files->streamCount() != 0 || files->threadCount() != 0 || link.a->servedRequestCount() != 0) {
      if (timer.elapsed() > timeoutMs) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
  }

  OSXRemoteFileSystem::Limits limits;
  Link link;
  std::unique_ptr<OSXRemoteFileSystem> files;
  ClipboardFileList list;
  std::vector<std::string> shown;
};

QByteArray slice(const QString &path, qint64 offset, qint64 size)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) {
    return "unreadable";
  }
  return file.read(size);
}

QByteArray contentsOf(const QString &path)
{
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("unreadable");
}

} // namespace

#define COMPARE_STATUS(actual, expected)                                                                               \
  do {                                                                                                                 \
    const Status statusValue = (actual);                                                                               \
    QVERIFY2(                                                                                                          \
        statusValue == (expected),                                                                                     \
        qPrintable(QStringLiteral("status is %1, expected %2").arg(statusName(statusValue), statusName(expected)))     \
    );                                                                                                                 \
  } while (false)

QString OSXRemoteFileSystemTests::sourcePath(const QString &relative) const
{
  return QDir(m_source->path()).filePath(relative);
}

void OSXRemoteFileSystemTests::initTestCase()
{
  m_source = std::make_unique<QTemporaryDir>();
  QVERIFY(m_source->isValid());

  // the computer that copies has:
  //   big.bin            24 MiB that do not repeat
  //   tree/a.txt
  //   tree/empty.bin
  //   tree/中文.txt
  //   tree/nested/b.bin  3 MiB
  QVERIFY(QDir().mkpath(sourcePath("tree/nested")));
  QVERIFY(writePattern(sourcePath("big.bin"), 24 * kMiB, 7));
  QVERIFY(writeFile(sourcePath("tree/a.txt"), "first file"));
  QVERIFY(writeFile(sourcePath("tree/empty.bin"), {}));
  QVERIFY(writeFile(sourcePath(QStringLiteral("tree/中文.txt")), "\xe5\x86\x85\xe5\xae\xb9"));
  QVERIFY(writePattern(sourcePath("tree/nested/b.bin"), 3 * kMiB, 8));
}

void OSXRemoteFileSystemTests::cleanupTestCase()
{
  m_source.reset();
}

void OSXRemoteFileSystemTests::nothingIsAsked_untilSomethingIsLookedAt()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("tree"), sourcePath("big.bin")}));
  QCOMPARE(paste.shown.size(), size_t{2});
  QVERIFY(QString::fromStdString(paste.shown[0]).endsWith("/tree"));
  QVERIFY(QString::fromStdString(paste.shown[1]).endsWith("/big.bin"));

  // copying alone, or looking at the root, asks the other computer nothing
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  QStringList names;
  COMPARE_STATUS(paste.names(paste.files->rootHandle(), names), Status::Ok);
  QCOMPARE(names.size(), 1);
  QCOMPARE(paste.link.askedByB.requests, uint64_t{0});
  QCOMPARE(paste.files->listingCount(), uint64_t{0});

  // right after the list arrived what was copied is answered from it
  const FileHandle offer = paste.handleOf(paste.shown[0].substr(0, paste.shown[0].find('/')));
  COMPARE_STATUS(paste.names(offer, names), Status::Ok);
  QCOMPARE(names, QStringList({"big.bin", "tree"}));
  QCOMPARE(paste.files->listingCount(), uint64_t{0});
  QCOMPARE(paste.link.askedByB.requests, uint64_t{0});

  // a look after a while asks how it is now, once
  pause(paste.limits);
  COMPARE_STATUS(paste.names(offer, names), Status::Ok);
  QCOMPARE(names, QStringList({"big.bin", "tree"}));
  QCOMPARE(paste.files->listingCount(), uint64_t{1});

  // a directory is listed when it is looked into, and nothing is read until a file is
  const FileHandle tree = paste.handleOf(paste.shownPath(0));
  COMPARE_STATUS(paste.names(tree, names), Status::Ok);
  QCOMPARE(names, QStringList({"a.txt", "empty.bin", "nested", QStringLiteral("中文.txt")}));
  QCOMPARE(paste.files->listingCount(), uint64_t{2});
  QCOMPARE(paste.files->fetchedBytes(), uint64_t{0});
  QCOMPARE(paste.readRequests(), size_t{0});

  Attributes attributes;
  FileHandle handle;
  COMPARE_STATUS(paste.find(paste.shownPath(0, "nested/b.bin"), handle, attributes), Status::Ok);
  QCOMPARE(attributes.size, uint64_t(3 * kMiB));
  QVERIFY(!attributes.isDirectory);
  QCOMPARE(paste.files->fetchedBytes(), uint64_t{0});
  QCOMPARE(paste.readRequests(), size_t{0});
}

void OSXRemoteFileSystemTests::file_readPieceByPiece_matches()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("tree"), sourcePath("big.bin")}));

  FileHandle handle;
  Attributes attributes;
  COMPARE_STATUS(paste.find(paste.shownPath(1), handle, attributes), Status::Ok);
  QCOMPARE(attributes.size, uint64_t(24 * kMiB));

  QByteArray data;
  COMPARE_STATUS(paste.readAll(handle, data), Status::Ok);
  QCOMPARE(data.size(), 24 * kMiB);
  QCOMPARE(data, contentsOf(sourcePath("big.bin")));

  // the small ones, an empty one and one below a directory too
  for (const char *name : {"a.txt", "empty.bin", "nested/b.bin"}) {
    COMPARE_STATUS(paste.find(paste.shownPath(0, name), handle, attributes), Status::Ok);
    COMPARE_STATUS(paste.readAll(handle, data, 64 * 1024), Status::Ok);
    QCOMPARE(data, contentsOf(sourcePath(QStringLiteral("tree/") + name)));
  }
  // each file was fetched once, not much more
  QVERIFY2(
      paste.files->fetchedBytes() <= uint64_t(27 * kMiB + 2 * kMiB),
      qPrintable(QString::number(paste.files->fetchedBytes()))
  );
  QVERIFY(paste.settles());
}

void OSXRemoteFileSystemTests::file_readByManyAtOnce_matches()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("big.bin")}));
  const FileHandle handle = paste.handleOf(paste.shownPath(0));
  const qint64 size = 24 * kMiB;
  const qint64 blocks = size / kMiB;

  // like Finder: several reads of 1 MiB at a time, each thread takes the next block
  std::vector<QByteArray> parts(static_cast<size_t>(blocks));
  std::atomic<qint64> next = 0;
  std::atomic<int> failures = 0;
  std::vector<std::thread> readers;
  for (int i = 0; i < 8; ++i) {
    readers.emplace_back([&] {
      for (qint64 block = next++; block < blocks; block = next++) {
        if (paste.read(handle, static_cast<uint64_t>(block * kMiB), kMiB, parts[static_cast<size_t>(block)]) !=
            Status::Ok) {
          ++failures;
        }
      }
    });
  }
  for (auto &reader : readers) {
    reader.join();
  }

  QCOMPARE(failures.load(), 0);
  QByteArray data;
  for (const auto &part : parts) {
    data.append(part);
  }
  QCOMPARE(data.size(), size);
  QCOMPARE(data, contentsOf(sourcePath("big.bin")));
  // the reads shared downloads instead of each starting its own
  QVERIFY2(
      paste.files->fetchedBytes() <= uint64_t(size + 4 * kMiB), qPrintable(QString::number(paste.files->fetchedBytes()))
  );
  QVERIFY2(paste.link.askedByB.requests <= 16, qPrintable(QString::number(paste.link.askedByB.requests)));
  QVERIFY(paste.settles());
}

void OSXRemoteFileSystemTests::file_readFromTheMiddle_fetchesFromThere()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("big.bin")}));
  const FileHandle handle = paste.handleOf(paste.shownPath(0));

  QByteArray data;
  COMPARE_STATUS(paste.read(handle, 12 * kMiB + 5, kMiB, data), Status::Ok);
  QCOMPARE(data, slice(sourcePath("big.bin"), 12 * kMiB + 5, kMiB));
  // what comes before was never fetched
  QVERIFY2(paste.files->fetchedBytes() <= uint64_t(4 * kMiB), qPrintable(QString::number(paste.files->fetchedBytes())));

  // the end, a read that goes past it, and one that starts past it
  bool end = false;
  COMPARE_STATUS(paste.read(handle, 24 * kMiB - 100, 4096, data, &end), Status::Ok);
  QCOMPARE(data, slice(sourcePath("big.bin"), 24 * kMiB - 100, 100));
  QVERIFY(end);
  end = false;
  COMPARE_STATUS(paste.read(handle, 24 * kMiB + 10, 4096, data, &end), Status::Ok);
  QVERIFY(data.isEmpty());
  QVERIFY(end);
  QVERIFY(paste.settles());
}

void OSXRemoteFileSystemTests::file_readBackwards_matches()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("big.bin")}));
  const FileHandle handle = paste.handleOf(paste.shownPath(0));

  // the worst order there is: each read starts just before the one before
  QByteArray data;
  for (qint64 block = 23; block >= 0; --block) {
    QByteArray part;
    COMPARE_STATUS(paste.read(handle, static_cast<uint64_t>(block * kMiB), kMiB, part), Status::Ok);
    data.prepend(part);
  }
  QCOMPARE(data, contentsOf(sourcePath("big.bin")));
  // a download that starts before another one stops where that one began
  QVERIFY2(
      paste.files->fetchedBytes() <= uint64_t(24 * kMiB + 4 * kMiB),
      qPrintable(QString::number(paste.files->fetchedBytes()))
  );
  QVERIFY(paste.settles());
}

void OSXRemoteFileSystemTests::slowSource_readGetsWhatHasArrived()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("big.bin")}));
  const FileHandle handle = paste.handleOf(paste.shownPath(0));
  // about 1.3 MB/s
  paste.link.aToB.setDataDelay(50);

  // a read is answered with what has arrived when it waited a moment
  std::string piece;
  bool end = false;
  QElapsedTimer timer;
  timer.start();
  COMPARE_STATUS(paste.files->read(handle, 0, 4 * kMiB, piece, end), Status::Ok);
  const qint64 took = timer.elapsed();
  QVERIFY2(!piece.empty() && piece.size() < size_t(4 * kMiB), qPrintable(QString::number(piece.size())));
  QVERIFY2(took >= 150 && took < 900, qPrintable(QString::number(took)));
  QVERIFY(!end);

  // and the rest follows
  QByteArray data(piece.data(), static_cast<qsizetype>(piece.size()));
  QByteArray rest;
  COMPARE_STATUS(paste.read(handle, piece.size(), 2 * kMiB - static_cast<uint32_t>(piece.size()), rest), Status::Ok);
  data.append(rest);
  QCOMPARE(data, slice(sourcePath("big.bin"), 0, 2 * kMiB));
  paste.link.aToB.setDataDelay(0);
}

void OSXRemoteFileSystemTests::nothingArrives_callComesBackLater_thenFails()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("tree"), sourcePath("big.bin")}));
  const FileHandle file = paste.handleOf(paste.shownPath(1));
  const FileHandle tree = paste.handleOf(paste.shownPath(0));
  paste.link.hold();

  // the system is told to come back later, a few times, then it gets an error
  std::string piece;
  bool end = false;
  QElapsedTimer timer;
  timer.start();
  COMPARE_STATUS(paste.files->read(file, 0, kMiB, piece, end), Status::TryLater);
  QVERIFY2(timer.elapsed() >= 900, qPrintable(QString::number(timer.elapsed())));
  COMPARE_STATUS(paste.files->read(file, 0, kMiB, piece, end), Status::TryLater);
  COMPARE_STATUS(paste.files->read(file, 0, kMiB, piece, end), Status::Io);

  // the same for a directory that was never listed
  FileHandle child;
  Attributes attributes;
  COMPARE_STATUS(paste.files->lookup(tree, "a.txt", child, attributes), Status::TryLater);

  // when the answers come in after all, everything works again
  paste.link.release();
  COMPARE_STATUS(paste.files->lookup(tree, "a.txt", child, attributes), Status::Ok);
  QByteArray data;
  COMPARE_STATUS(paste.read(file, 0, kMiB, data), Status::Ok);
  QCOMPARE(data, slice(sourcePath("big.bin"), 0, kMiB));
}

void OSXRemoteFileSystemTests::sourceStopsAnswering_readFails()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("big.bin")}));
  const FileHandle file = paste.handleOf(paste.shownPath(0));
  paste.link.loop.call([&] { paste.link.b->setReplyTimeout(300); });
  paste.link.hold();

  // the transfer gives up before the system would be told to come back later
  std::string piece;
  bool end = false;
  QElapsedTimer timer;
  timer.start();
  COMPARE_STATUS(paste.files->read(file, 0, kMiB, piece, end), Status::Io);
  QVERIFY2(timer.elapsed() < 900, qPrintable(QString::number(timer.elapsed())));
  // everything that waits for the same thing sees the same failure
  COMPARE_STATUS(paste.files->read(file, 0, kMiB, piece, end), Status::Io);
  paste.link.loop.call([&] { paste.link.b->setReplyTimeout(30000); });
}

void OSXRemoteFileSystemTests::connectionLost_readFails()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("big.bin")}));
  const FileHandle file = paste.handleOf(paste.shownPath(0));
  paste.link.aToB.setDataDelay(20);

  auto reading = std::async(std::launch::async, [&] {
    QByteArray data;
    return paste.read(file, 0, 16 * kMiB, data);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  QElapsedTimer timer;
  timer.start();
  paste.link.loop.call([&] {
    paste.link.aToB.cut();
    paste.link.bToA.cut();
    paste.link.b->channelClosed(&paste.link.bToA);
  });
  QCOMPARE(reading.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  COMPARE_STATUS(reading.get(), Status::Io);
  QVERIFY2(timer.elapsed() < 900, qPrintable(QString::number(timer.elapsed())));

  // and nothing else can be fetched any more
  QByteArray data;
  COMPARE_STATUS(paste.read(file, 20 * kMiB, kMiB, data), Status::Io);
}

void OSXRemoteFileSystemTests::fileGrewAfterCopy_isSeenInANewSession()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = QDir(directory.path()).filePath("grows.txt");
  QVERIFY(writeFile(path, "ten bytes!"));

  Paste paste;
  QVERIFY(paste.copy({path}));
  FileHandle handle;
  Attributes attributes;
  COMPARE_STATUS(paste.find(paste.shownPath(0), handle, attributes), Status::Ok);
  QCOMPARE(attributes.size, uint64_t{10});
  QByteArray data;
  COMPARE_STATUS(paste.readAll(handle, data), Status::Ok);
  QCOMPARE(data, QByteArray("ten bytes!"));

  // changed after it was copied: the next look after a pause asks again
  const QByteArray grown = patternBlock(3, 300 * 1024);
  QVERIFY(writeFile(path, grown));
  pause(paste.limits);
  COMPARE_STATUS(paste.files->getAttributes(handle, attributes), Status::Ok);
  QCOMPARE(attributes.size, uint64_t(grown.size()));
  COMPARE_STATUS(paste.readAll(handle, data), Status::Ok);
  QCOMPARE(data, grown);
}

void OSXRemoteFileSystemTests::fileShrankAfterListing_endIsFound()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = QDir(directory.path()).filePath("shrinks.bin");
  QVERIFY(writePattern(path, 2 * kMiB, 4));

  Paste paste;
  QVERIFY(paste.copy({path}));
  FileHandle handle;
  Attributes attributes;
  COMPARE_STATUS(paste.find(paste.shownPath(0), handle, attributes), Status::Ok);
  QCOMPARE(attributes.size, uint64_t(2 * kMiB));

  // shorter by the time it is read, within the same session
  QVERIFY(QFile::resize(path, kMiB));
  QByteArray data;
  COMPARE_STATUS(paste.readAll(handle, data, 256 * 1024), Status::Ok);
  QCOMPARE(data.size(), kMiB);
  QCOMPARE(data, contentsOf(path));
  // the system is told the size it really has, it would fill up with zeros otherwise
  COMPARE_STATUS(paste.files->getAttributes(handle, attributes), Status::Ok);
  QCOMPARE(attributes.size, uint64_t(kMiB));
}

void OSXRemoteFileSystemTests::fileDeletedAfterCopy_isGoneInANewSession()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString gone = QDir(directory.path()).filePath("gone.txt");
  const QString stays = QDir(directory.path()).filePath("stays.txt");
  QVERIFY(writeFile(gone, "soon gone"));
  QVERIFY(writeFile(stays, "stays"));

  Paste paste;
  QVERIFY(paste.copy({gone, stays}));
  const FileHandle goneHandle = paste.handleOf(paste.shownPath(0));
  const FileHandle staysHandle = paste.handleOf(paste.shownPath(1));

  QVERIFY(QFile::remove(gone));
  pause(paste.limits);
  FileHandle handle;
  Attributes attributes;
  COMPARE_STATUS(paste.find(paste.shownPath(0), handle, attributes), Status::NoEntry);
  COMPARE_STATUS(paste.files->getAttributes(goneHandle, attributes), Status::Stale);
  COMPARE_STATUS(paste.files->getAttributes(staysHandle, attributes), Status::Ok);
  QByteArray data;
  COMPARE_STATUS(paste.readAll(staysHandle, data), Status::Ok);
  QCOMPARE(data, QByteArray("stays"));
}

void OSXRemoteFileSystemTests::directoryChangedAfterCopy_isSeenInANewSession()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QDir changing(QDir(directory.path()).filePath("changing"));
  QVERIFY(QDir().mkpath(changing.path()));
  QVERIFY(writeFile(changing.filePath("a.txt"), "a"));
  QVERIFY(writeFile(changing.filePath("b.txt"), "b"));

  Paste paste;
  QVERIFY(paste.copy({changing.path()}));
  const FileHandle folder = paste.handleOf(paste.shownPath(0));
  QStringList names;
  COMPARE_STATUS(paste.names(folder, names), Status::Ok);
  QCOMPARE(names, QStringList({"a.txt", "b.txt"}));
  const FileHandle kept = paste.handleOf(paste.shownPath(0, "b.txt"));

  QVERIFY(QFile::remove(changing.filePath("a.txt")));
  QVERIFY(writeFile(changing.filePath("c.txt"), "c"));
  // within the session nothing changes
  COMPARE_STATUS(paste.names(folder, names), Status::Ok);
  QCOMPARE(names, QStringList({"a.txt", "b.txt"}));

  pause(paste.limits);
  COMPARE_STATUS(paste.names(folder, names), Status::Ok);
  QCOMPARE(names, QStringList({"b.txt", "c.txt"}));
  // what stayed keeps its handle
  Attributes attributes;
  COMPARE_STATUS(paste.files->getAttributes(kept, attributes), Status::Ok);
  QByteArray data;
  COMPARE_STATUS(paste.readAll(paste.handleOf(paste.shownPath(0, "c.txt")), data), Status::Ok);
  QCOMPARE(data, QByteArray("c"));
}

void OSXRemoteFileSystemTests::names_thatCanNotBeUsed_areLeftOut()
{
  // names from another system that this one can not show
  ClipboardFileList list;
  list.offerId = 0x1234;
  list.entries.push_back({"fine.txt", false, 1, 1});
  // 100 Chinese characters are 300 bytes, more than a name may have here
  std::string longName;
  for (int i = 0; i < 100; ++i) {
    longName += "\xe4\xb8\xad";
  }
  list.entries.push_back({longName, false, 1, 1});
  list.entries.push_back({"\xff\xfe.txt", false, 1, 1});
  // the same name twice, once with a composed and once with a decomposed letter
  list.entries.push_back({"caf\xc3\xa9", false, 1, 1});
  list.entries.push_back({"cafe\xcc\x81", false, 1, 1});
  list.entries.push_back({std::string(255, 'x'), false, 1, 1});

  Paste paste;
  const std::vector<std::string> shown = paste.files->addOffer(list);
  QCOMPARE(shown.size(), size_t{6});
  QVERIFY(QString::fromStdString(shown[0]).endsWith("/fine.txt"));
  QVERIFY(shown[1].empty());
  QVERIFY(shown[2].empty());
  QVERIFY(!shown[3].empty());
  QVERIFY(shown[4].empty());
  QVERIFY(!shown[5].empty());
  // adding the same list again changes nothing
  QCOMPARE(paste.files->addOffer(list), shown);
}

void OSXRemoteFileSystemTests::olderLists_areDropped()
{
  auto limits = quickLimits();
  limits.offers = 2;
  Paste paste(limits);

  QVERIFY(paste.copy({sourcePath("tree/a.txt")}));
  const std::vector<std::string> first = paste.shown;
  const FileHandle firstHandle = paste.handleOf(first[0]);
  QVERIFY(paste.copy({sourcePath("tree/empty.bin")}));
  const std::vector<std::string> second = paste.shown;
  QVERIFY(paste.copy({sourcePath("tree/nested")}));

  FileHandle handle;
  Attributes attributes;
  COMPARE_STATUS(paste.find(first[0], handle, attributes), Status::NoEntry);
  COMPARE_STATUS(paste.files->getAttributes(firstHandle, attributes), Status::Stale);
  COMPARE_STATUS(paste.find(second[0], handle, attributes), Status::Ok);
  COMPARE_STATUS(paste.find(paste.shownPath(0), handle, attributes), Status::Ok);
  QStringList names;
  COMPARE_STATUS(paste.names(paste.files->rootHandle(), names), Status::Ok);
  QCOMPARE(names.size(), 2);
}

void OSXRemoteFileSystemTests::bigDirectory_isReadInPages()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QDir many(QDir(directory.path()).filePath("many"));
  QVERIFY(QDir().mkpath(many.path()));
  QStringList expected;
  for (int i = 0; i < 600; ++i) {
    const QString name = QStringLiteral("文件-%1.txt").arg(i, 4, 10, QLatin1Char('0'));
    QVERIFY(writeFile(many.filePath(name), QByteArray::number(i)));
    expected.append(name);
  }
  expected.sort();

  Paste paste;
  QVERIFY(paste.copy({many.path()}));
  const FileHandle folder = paste.handleOf(paste.shownPath(0));
  QStringList names;
  COMPARE_STATUS(paste.names(folder, names, 37), Status::Ok);
  QCOMPARE(names, expected);
  // a page that starts past the end is empty and complete
  std::vector<DirectoryEntry> entries;
  bool complete = false;
  COMPARE_STATUS(paste.files->readDirectory(folder, 600, 10, entries, complete), Status::Ok);
  QVERIFY(entries.empty());
  QVERIFY(complete);
}

void OSXRemoteFileSystemTests::shutdown_wakesWhoeverWaits()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("big.bin")}));
  const FileHandle file = paste.handleOf(paste.shownPath(0));
  paste.link.hold();

  auto reading = std::async(std::launch::async, [&] {
    std::string piece;
    bool end = false;
    return paste.files->read(file, 0, kMiB, piece, end);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  QElapsedTimer timer;
  timer.start();
  QVERIFY(paste.files->shutdown());
  QCOMPARE(reading.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
  COMPARE_STATUS(reading.get(), Status::Io);
  QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));

  // and every call after it fails at once
  Attributes attributes;
  COMPARE_STATUS(paste.files->getAttributes(file, attributes), Status::Io);
}

void OSXRemoteFileSystemTests::interrupt_failsWhoWaits_laterCallsWork()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("big.bin")}));
  const FileHandle file = paste.handleOf(paste.shownPath(0));
  paste.link.hold();

  auto reading = std::async(std::launch::async, [&] {
    std::string piece;
    bool end = false;
    return paste.files->read(file, 0, kMiB, piece, end);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  paste.files->interrupt();
  QCOMPARE(reading.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
  COMPARE_STATUS(reading.get(), Status::Io);

  // for a volume that is mounted again: the files are still there
  paste.link.release();
  QByteArray data;
  COMPARE_STATUS(paste.read(file, 0, kMiB, data), Status::Ok);
  QCOMPARE(data, slice(sourcePath("big.bin"), 0, kMiB));
}

void OSXRemoteFileSystemTests::download_nobodyReads_isGivenUp()
{
  Paste paste;
  QVERIFY(paste.copy({sourcePath("big.bin")}));
  const FileHandle file = paste.handleOf(paste.shownPath(0));

  QByteArray data;
  COMPARE_STATUS(paste.read(file, 0, kMiB, data), Status::Ok);
  QVERIFY(paste.files->streamCount() > 0);

  // nobody reads on: the download stops and the other computer is told so
  QVERIFY(paste.settles(paste.limits.idleMs + 3000));
  QVERIFY2(paste.files->fetchedBytes() <= uint64_t(4 * kMiB), qPrintable(QString::number(paste.files->fetchedBytes())));
}

QTEST_MAIN(OSXRemoteFileSystemTests)
