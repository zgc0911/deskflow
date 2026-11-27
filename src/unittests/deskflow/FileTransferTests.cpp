/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransferTests.h"

#include "FileTransferTestLink.h"

#include "base/Event.h"
#include "base/EventQueue.h"
#include "deskflow/FileTransfer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <thread>

using deskflow::ClipboardFileEntry;
using deskflow::ClipboardFileList;
using deskflow::FileTransfer;
using deskflow::FileTransferStatus;
using deskflow::IFileTransferChannel;
using deskflow::RemoteFileReader;
using deskflow::RemoteFiles;
using namespace deskflow::test;

namespace {

//! Size of the big file, 300 MiB unless overridden to keep a local run short
qint64 bigFileSize()
{
  bool ok = false;
  const int megabytes = qEnvironmentVariableIntValue("DESKFLOW_TEST_BIG_FILE_MB", &ok);
  return (ok && megabytes > 0 ? megabytes : 300) * kMiB;
}

//! Pastes on the computer that reads: lists and downloads with the blocking interface
struct Fetch
{
  RemoteFiles &remote;
  uint64_t offerId = 0;
  uint32_t window = 0;
  FileTransferStatus failure = FileTransferStatus::Ok;
  QString failedAt;
  quint64 bytes = 0;

  bool file(uint32_t item, const std::string &relativePath, const QString &target)
  {
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
      failedAt = target;
      failure = FileTransferStatus::Failed;
      return false;
    }
    auto reader = remote.openFile(offerId, item, relativePath, 0, window);
    QByteArray buffer(256 * 1024, Qt::Uninitialized);
    for (;;) {
      uint32_t got = 0;
      const auto status = reader->read(buffer.data(), static_cast<uint32_t>(buffer.size()), got);
      if (status != FileTransferStatus::Ok) {
        failedAt = QString::fromStdString(relativePath);
        failure = status;
        return false;
      }
      if (got == 0) {
        return true;
      }
      bytes += got;
      if (out.write(buffer.constData(), got) != got) {
        failedAt = target;
        failure = FileTransferStatus::Failed;
        return false;
      }
    }
  }

  bool directory(uint32_t item, const std::string &relativePath, const QString &target)
  {
    if (!QDir().mkpath(target)) {
      failedAt = target;
      failure = FileTransferStatus::Failed;
      return false;
    }
    std::vector<ClipboardFileEntry> entries;
    const auto status = remote.listDirectory(offerId, item, relativePath, entries);
    if (status != FileTransferStatus::Ok) {
      failedAt = QString::fromStdString(relativePath);
      failure = status;
      return false;
    }
    for (const auto &entry : entries) {
      const std::string childPath = relativePath.empty() ? entry.name : relativePath + "/" + entry.name;
      const QString childTarget = QDir(target).filePath(QString::fromStdString(entry.name));
      if (!(entry.isDirectory ? directory(item, childPath, childTarget) : file(item, childPath, childTarget))) {
        return false;
      }
    }
    return true;
  }

  bool all(const ClipboardFileList &list, const QString &targetDirectory)
  {
    offerId = list.offerId;
    for (uint32_t item = 0; item < list.entries.size(); ++item) {
      const auto &entry = list.entries[item];
      const QString target = QDir(targetDirectory).filePath(QString::fromStdString(entry.name));
      if (!(entry.isDirectory ? directory(item, {}, target) : file(item, {}, target))) {
        return false;
      }
    }
    return true;
  }
};

//! Measures how long the event thread keeps unrelated events waiting
class LatencyProbe
{
public:
  explicit LatencyProbe(EventLoop &loop) : m_loop(loop)
  {
    m_thread = std::thread([this] {
      while (!m_stop) {
        const auto posted = std::chrono::steady_clock::now();
        ++m_pending;
        m_loop.post([this, posted] {
          const auto waited =
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - posted).count();
          qint64 worst = m_worstMs.load();
          while (waited > worst && !m_worstMs.compare_exchange_weak(worst, waited)) {
          }
          ++m_samples;
          --m_pending;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    });
  }

  ~LatencyProbe()
  {
    stop();
  }

  void stop()
  {
    if (m_thread.joinable()) {
      m_stop = true;
      m_thread.join();
      // every probe refers to this object, wait for the ones still queued
      while (m_pending != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }

  qint64 worstMs() const
  {
    return m_worstMs;
  }

  int samples() const
  {
    return m_samples;
  }

private:
  EventLoop &m_loop;
  std::thread m_thread;
  std::atomic<bool> m_stop = false;
  std::atomic<qint64> m_worstMs = 0;
  std::atomic<int> m_samples = 0;
  std::atomic<int> m_pending = 0;
};

const char *statusName(FileTransferStatus status)
{
  using enum FileTransferStatus;
  switch (status) {
  case Ok:
    return "Ok";
  case Cancelled:
    return "Cancelled";
  case Disconnected:
    return "Disconnected";
  case TimedOut:
    return "TimedOut";
  case Refused:
    return "Refused";
  case NotFound:
    return "NotFound";
  case Failed:
    return "Failed";
  }
  return "?";
}

//! Read a reader to its end, returns the status that ended it
FileTransferStatus drain(RemoteFileReader &reader, QByteArray *data = nullptr, quint64 *total = nullptr)
{
  QByteArray buffer(128 * 1024, Qt::Uninitialized);
  for (;;) {
    uint32_t got = 0;
    const auto status = reader.read(buffer.data(), static_cast<uint32_t>(buffer.size()), got);
    if (status != FileTransferStatus::Ok || got == 0) {
      return status;
    }
    if (data != nullptr) {
      data->append(buffer.constData(), got);
    }
    if (total != nullptr) {
      *total += got;
    }
  }
}

} // namespace

// evaluates \p actual once: reading or listing a second time would change the outcome
#define COMPARE_STATUS(actual, expected)                                                                               \
  do {                                                                                                                 \
    const FileTransferStatus statusValue = (actual);                                                                   \
    QVERIFY2(statusValue == (expected), qPrintable(QStringLiteral("status is %1").arg(statusName(statusValue))));      \
  } while (false)

QString FileTransferTests::sourcePath(const QString &relative) const
{
  return QDir(m_source->path()).filePath(relative);
}

void FileTransferTests::initTestCase()
{
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Info);

  // tree/                       what gets copied
  //   empty.bin
  //   小文件.txt
  //   one-chunk.bin             exactly one message
  //   one-window.bin            exactly one window
  //   odd.bin                   a window and a bit
  //   big.bin                   several hundred megabytes
  //   nested/deeper/更深/leaf.dat
  //   nested/empty-dir/
  //   way-out -> ../secret      a link that leads outside
  // single.txt                  copied next to the tree
  // secret/s.txt                must never be reachable
  m_source = std::make_unique<QTemporaryDir>();
  QVERIFY(m_source->isValid());
  QDir root(m_source->path());
  QVERIFY(root.mkpath(QStringLiteral("tree/nested/deeper/更深")));
  QVERIFY(root.mkpath("tree/nested/empty-dir"));
  QVERIFY(root.mkpath("secret"));
  QVERIFY(writeFile(sourcePath("tree/empty.bin"), {}));
  QVERIFY(writeFile(sourcePath(QStringLiteral("tree/小文件.txt")), "\xe5\x86\x85\xe5\xae\xb9\n"));
  QVERIFY(writePattern(sourcePath("tree/one-chunk.bin"), Protocol::ChunkSize, 11));
  QVERIFY(writePattern(sourcePath("tree/one-window.bin"), Protocol::DefaultWindow, 22));
  QVERIFY(writePattern(sourcePath("tree/odd.bin"), Protocol::DefaultWindow + 12345, 33));
  QVERIFY(writePattern(sourcePath("tree/big.bin"), bigFileSize(), 44));
  QVERIFY(writePattern(sourcePath(QStringLiteral("tree/nested/deeper/更深/leaf.dat")), 200 * 1024, 55));
  QVERIFY(writeFile(sourcePath("single.txt"), "a single file"));
  QVERIFY(writeFile(sourcePath("secret/s.txt"), "TOP-SECRET-MUST-NOT-LEAK"));

  m_haveLinks = QFile::link(sourcePath("secret"), sourcePath("tree/way-out")) &&
                QFileInfo(sourcePath("tree/way-out")).isSymbolicLink();
  if (!m_haveLinks) {
    // what Windows makes instead is a shortcut: an ordinary file, which is copied like any other
    QFile::remove(sourcePath("tree/way-out"));
  }
}

void FileTransferTests::cleanupTestCase()
{
  m_source.reset();
}

void FileTransferTests::wholeTree_arrivesUnchanged()
{
  Link link;
  const ClipboardFileList list =
      link.copyOnA({sourcePath("tree").toStdString(), sourcePath("single.txt").toStdString()});
  QCOMPARE(list.entries.size(), size_t{2});

  QTemporaryDir target;
  QVERIFY(target.isValid());

  LatencyProbe probe(link.loop);
  QElapsedTimer timer;
  timer.start();
  Fetch fetch{*link.remoteOfB};
  const bool fetched = fetch.all(list, target.path());
  const qint64 elapsed = timer.elapsed();
  probe.stop();
  QVERIFY2(fetched, qPrintable(QStringLiteral("failed at %1: %2").arg(fetch.failedAt, statusName(fetch.failure))));

  QMap<QString, QByteArray> expected;
  describe(sourcePath("tree"), "tree", expected);
  describe(sourcePath("single.txt"), "single.txt", expected);
  QMap<QString, QByteArray> actual;
  describe(QDir(target.path()).filePath("tree"), "tree", actual);
  describe(QDir(target.path()).filePath("single.txt"), "single.txt", actual);
  QCOMPARE(actual.keys(), expected.keys());
  for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
    QVERIFY2(actual.value(it.key()) == it.value(), qPrintable(it.key()));
  }
  QVERIFY(expected.contains(QStringLiteral("tree/nested/deeper/更深/leaf.dat")));
  QVERIFY(expected.contains("tree/nested/empty-dir"));
  QVERIFY(!actual.contains("tree/way-out"));

  // nothing stays open on the computer that sent
  QVERIFY(link.waitUntilAIsIdle());

  // the sender kept within the room it was given, in messages of the agreed size
  QVERIFY(!link.askedByB.windowExceeded);
  QVERIFY(!link.askedByB.chunkTooBig);
  QCOMPARE(link.askedByB.totalData() >= static_cast<uint64_t>(fetch.bytes), true);

  // while hundreds of megabytes went through, other events were never held up for long
  qInfo(
      "transferred %llu MiB in %lld ms, slowest unrelated event waited %lld ms (%d samples)",
      static_cast<unsigned long long>(fetch.bytes / kMiB), static_cast<long long>(elapsed),
      static_cast<long long>(probe.worstMs()), probe.samples()
  );
  QVERIFY(probe.samples() > 0);
  QVERIFY2(probe.worstMs() < 250, qPrintable(QStringLiteral("an event waited %1 ms").arg(probe.worstMs())));
}

void FileTransferTests::wholeTree_bothDirectionsAtOnce()
{
  Link link;
  const ClipboardFileList fromA = link.copyOnA({sourcePath("tree/nested").toStdString()});
  const ClipboardFileList fromB =
      link.copyOnB({sourcePath("tree/odd.bin").toStdString(), sourcePath("single.txt").toStdString()});
  QCOMPARE(fromA.entries.size(), size_t{1});
  QCOMPARE(fromB.entries.size(), size_t{2});

  QTemporaryDir targetOnB;
  QTemporaryDir targetOnA;
  Fetch fetchOnB{*link.remoteOfB};
  Fetch fetchOnA{*link.remoteOfA};
  bool okOnB = false;
  bool okOnA = false;
  std::thread pasteOnB([&] { okOnB = fetchOnB.all(fromA, targetOnB.path()); });
  std::thread pasteOnA([&] { okOnA = fetchOnA.all(fromB, targetOnA.path()); });
  pasteOnB.join();
  pasteOnA.join();
  QVERIFY2(okOnB, statusName(fetchOnB.failure));
  QVERIFY2(okOnA, statusName(fetchOnA.failure));

  QMap<QString, QByteArray> expected;
  QMap<QString, QByteArray> actual;
  describe(sourcePath("tree/nested"), "nested", expected);
  describe(QDir(targetOnB.path()).filePath("nested"), "nested", actual);
  QCOMPARE(actual, expected);

  QCOMPARE(hashOfFile(QDir(targetOnA.path()).filePath("odd.bin")), hashOfFile(sourcePath("tree/odd.bin")));
  QCOMPARE(hashOfFile(QDir(targetOnA.path()).filePath("single.txt")), hashOfFile(sourcePath("single.txt")));
  QVERIFY(!link.askedByA.windowExceeded);
  QVERIFY(!link.askedByB.windowExceeded);
}

void FileTransferTests::read_fromOffset()
{
  Link link;
  const ClipboardFileList list = link.copyOnA({sourcePath("tree").toStdString()});
  QCOMPARE(list.entries.size(), size_t{1});

  QFile source(sourcePath("tree/odd.bin"));
  QVERIFY(source.open(QIODevice::ReadOnly));
  const QByteArray expected = source.readAll();

  for (const qint64 offset :
       {qint64{1}, qint64{Protocol::ChunkSize}, qint64{Protocol::DefaultWindow}, expected.size() - 1}) {
    auto reader = link.remoteOfB->openFile(list.offerId, 0, "odd.bin", static_cast<uint64_t>(offset));
    QByteArray data;
    COMPARE_STATUS(drain(*reader, &data), FileTransferStatus::Ok);
    QVERIFY2(data == expected.mid(offset), qPrintable(QStringLiteral("offset %1").arg(offset)));
  }

  // at and past the end there is nothing to read, which is not an error
  for (const qint64 offset : {expected.size(), expected.size() + 1000}) {
    auto reader = link.remoteOfB->openFile(list.offerId, 0, "odd.bin", static_cast<uint64_t>(offset));
    QByteArray data;
    COMPARE_STATUS(drain(*reader, &data), FileTransferStatus::Ok);
    QVERIFY(data.isEmpty());
  }
  QVERIFY(link.waitUntilAIsIdle());
}

void FileTransferTests::read_smallestWindow_completes()
{
  // sizes around the window, where a sender waiting for room and a reader waiting for the end could block each other
  QTemporaryDir sizes;
  QVERIFY(sizes.isValid());
  const qint64 window = Protocol::ChunkSize;
  const QList<qint64> fileSizes = {0, 1, window - 1, window, window + 1, 2 * window, 3 * window + 7, 10 * window};
  std::vector<std::string> paths;
  for (const qint64 size : fileSizes) {
    const QString path = QDir(sizes.path()).filePath(QStringLiteral("size-%1.bin").arg(size));
    QVERIFY(writePattern(path, size, static_cast<quint64>(size)));
    paths.push_back(path.toStdString());
  }

  Link link;
  // a transfer that does not complete shows as a time out instead of hanging the test
  link.loop.call([&] { link.b->setReplyTimeout(5000); });
  const ClipboardFileList list = link.copyOnA(paths);
  QCOMPARE(list.entries.size(), static_cast<size_t>(fileSizes.size()));

  QTemporaryDir target;
  Fetch fetch{*link.remoteOfB};
  fetch.window = static_cast<uint32_t>(window);
  QVERIFY2(
      fetch.all(list, target.path()),
      qPrintable(QStringLiteral("failed at %1: %2").arg(fetch.failedAt, statusName(fetch.failure)))
  );
  for (const auto &path : paths) {
    const QString name = QFileInfo(QString::fromStdString(path)).fileName();
    QCOMPARE(hashOfFile(QDir(target.path()).filePath(name)), hashOfFile(QString::fromStdString(path)));
  }
  QVERIFY(!link.askedByB.windowExceeded);
  QVERIFY(link.waitUntilAIsIdle());
}

void FileTransferTests::list_largeDirectory()
{
  QTemporaryDir many;
  QVERIFY(many.isValid());
  QSet<QString> expected;
  for (int i = 0; i < 3000; ++i) {
    const QString name = QStringLiteral("文件-%1.txt").arg(i, 5, 10, QLatin1Char('0'));
    QVERIFY(writeFile(QDir(many.path()).filePath(name), QByteArray::number(i)));
    expected.insert(name);
  }

  Link link;
  const ClipboardFileList list = link.copyOnA({many.path().toStdString()});
  QCOMPARE(list.entries.size(), size_t{1});

  LatencyProbe probe(link.loop);
  std::vector<ClipboardFileEntry> entries;
  COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId, 0, {}, entries), FileTransferStatus::Ok);
  probe.stop();

  QSet<QString> actual;
  for (const auto &entry : entries) {
    QVERIFY(!entry.isDirectory);
    QVERIFY(entry.size >= 1 && entry.size <= 4);
    actual.insert(QString::fromStdString(entry.name));
  }
  QCOMPARE(entries.size(), size_t{3000});
  QCOMPARE(actual, expected);
  QVERIFY2(probe.worstMs() < 250, qPrintable(QStringLiteral("an event waited %1 ms").arg(probe.worstMs())));
  QVERIFY(link.waitUntilAIsIdle());
}

void FileTransferTests::listOffer_tellsWhatBecameOfTheCopiedEntries()
{
  QTemporaryDir copied;
  QVERIFY(copied.isValid());
  const QDir directory(copied.path());
  QVERIFY(writeFile(directory.filePath("grows.txt"), "short"));
  QVERIFY(writeFile(directory.filePath("goes.txt"), "soon gone"));
  QVERIFY(writeFile(directory.filePath("stays.txt"), "as it is"));
  QVERIFY(directory.mkpath("folder"));

  Link link;
  const ClipboardFileList list = link.copyOnA({
      directory.filePath("grows.txt").toStdString(),
      directory.filePath("goes.txt").toStdString(),
      directory.filePath("folder").toStdString(),
      directory.filePath("stays.txt").toStdString(),
  });
  QCOMPARE(list.entries.size(), size_t{4});
  QCOMPARE(list.entries[0].size, uint64_t{5});

  // right after copying nothing differs from the list
  std::vector<ClipboardFileEntry> entries;
  COMPARE_STATUS(link.remoteOfB->listOffer(list.offerId, entries), FileTransferStatus::Ok);
  QCOMPARE(entries.size(), size_t{4});
  for (size_t i = 0; i < entries.size(); ++i) {
    QCOMPARE(entries[i], list.entries[i]);
  }

  // time passes before the paste
  QVERIFY(writeFile(directory.filePath("grows.txt"), QByteArray(70000, 'x')));
  QVERIFY(QFile::remove(directory.filePath("goes.txt")));

  COMPARE_STATUS(link.remoteOfB->listOffer(list.offerId, entries), FileTransferStatus::Ok);
  QCOMPARE(entries.size(), size_t{4});
  QCOMPARE(entries[0].name, std::string("grows.txt"));
  QCOMPARE(entries[0].size, uint64_t{70000});
  QVERIFY(!entries[0].isMissing);
  QCOMPARE(entries[1].name, std::string("goes.txt"));
  QVERIFY(entries[1].isMissing);
  QCOMPARE(entries[2].name, std::string("folder"));
  QVERIFY(entries[2].isDirectory);
  QVERIFY(!entries[2].isMissing);
  QCOMPARE(entries[3], list.entries[3]);

  // and what is read is what is there now, all of it
  QByteArray contents;
  auto reader = link.remoteOfB->openFile(list.offerId, 0, {}, 0);
  COMPARE_STATUS(drain(*reader, &contents), FileTransferStatus::Ok);
  QCOMPARE(contents, QByteArray(70000, 'x'));

  // a list this computer was never told about
  COMPARE_STATUS(link.remoteOfB->listOffer(list.offerId ^ 0xff, entries), FileTransferStatus::Disconnected);
  QVERIFY(link.waitUntilAIsIdle());
}

void FileTransferTests::list_skipsLinks()
{
  if (!m_haveLinks) {
    QSKIP("symbolic links can not be created here");
  }

  Link link;
  const ClipboardFileList list = link.copyOnA({sourcePath("tree").toStdString()});
  std::vector<ClipboardFileEntry> entries;
  COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId, 0, {}, entries), FileTransferStatus::Ok);

  QStringList names;
  for (const auto &entry : entries) {
    names.append(QString::fromStdString(entry.name));
  }
  QVERIFY(names.contains("big.bin"));
  QVERIFY(names.contains("nested"));
  QVERIFY2(!names.contains("way-out"), "a link was listed");
}

void FileTransferTests::request_outsideOfTheOffer_refused()
{
  Link link;
  link.askedByB.needle = "TOP-SECRET-MUST-NOT-LEAK";
  const ClipboardFileList list = link.copyOnA({sourcePath("tree").toStdString()});
  QCOMPARE(list.entries.size(), size_t{1});
  std::vector<ClipboardFileEntry> entries;

  // listings that try to leave the copied directory
  for (const char *path : {"..", "../secret", "nested/../..", "/", "nested//deeper", "./nested"}) {
    COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId, 0, path, entries), FileTransferStatus::NotFound);
    QVERIFY(entries.empty());
  }
  COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId, 1, {}, entries), FileTransferStatus::NotFound);

  // reads that try the same
  QStringList paths = {
      "../secret/s.txt", "nested/../../secret/s.txt", "/etc/hosts", "../single.txt", "nested/deeper/../../../single.txt"
  };
  if (m_haveLinks) {
    paths.append("way-out/s.txt");
  }
  for (const QString &path : std::as_const(paths)) {
    auto reader = link.remoteOfB->openFile(list.offerId, 0, path.toStdString(), 0);
    QByteArray data;
    const auto status = drain(*reader, &data);
    QVERIFY2(status == FileTransferStatus::NotFound, qPrintable(path + ": " + statusName(status)));
    QVERIFY(data.isEmpty());
  }
  if (m_haveLinks) {
    COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId, 0, "way-out", entries), FileTransferStatus::NotFound);
  }

  // files of a list that was never offered to this computer
  COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId + 1, 0, {}, entries), FileTransferStatus::Disconnected);

  QVERIFY2(!link.askedByB.needleSeen, "contents of a file outside of the copied directory were sent");
  QCOMPARE(link.askedByB.totalData(), uint64_t{0});
  QVERIFY(link.waitUntilAIsIdle());
}

void FileTransferTests::request_rawPathTricks_refused()
{
  // requests as a modified peer could send them, without the checks of the reading side
  Link link;
  link.askedByB.needle = "TOP-SECRET-MUST-NOT-LEAK";
  const ClipboardFileList list = link.copyOnA({sourcePath("tree").toStdString()});

  struct Raw
  {
    uint8_t kind;
    std::string arguments;
    FileTransfer::Error expected;
  };
  using enum FileTransfer::Error;
  const uint64_t offer = list.offerId;
  std::vector<Raw> requests = {
      {Protocol::RequestRead, FileTransfer::encodeRequest(offer, 0, "../secret/s.txt", 0, 1 << 20), BadPath},
      {Protocol::RequestRead, FileTransfer::encodeRequest(offer, 0, std::string("big.bin\0", 8), 0, 1 << 20), BadPath},
      {Protocol::RequestRead, FileTransfer::encodeRequest(offer, 7, "", 0, 1 << 20), BadPath},
      {Protocol::RequestRead, FileTransfer::encodeRequest(offer ^ 0xff, 0, "big.bin", 0, 1 << 20), UnknownOffer},
      {Protocol::RequestRead, FileTransfer::encodeRequest(0, 0, "big.bin", 0, 1 << 20), UnknownOffer},
      {Protocol::RequestList, FileTransfer::encodeRequest(offer, 0, "nested/../../secret", 0, 0), BadPath},
      {Protocol::RequestList, FileTransfer::encodeRequest(offer, Protocol::AllItems, "nested", 0, 0), BadPath},
      {Protocol::RequestRead, FileTransfer::encodeRequest(offer, Protocol::AllItems, "", 0, 1 << 20), BadPath},
      {Protocol::RequestList, FileTransfer::encodeRequest(offer ^ 0xff, Protocol::AllItems, "", 0, 0), UnknownOffer},
      {Protocol::RequestRead, FileTransfer::encodeRequest(offer, 0, "missing.bin", 0, 1 << 20), NotFound},
      {9, FileTransfer::encodeRequest(offer, 0, "big.bin", 0, 1 << 20), BadPath},
      {Protocol::RequestRead, std::string("short"), BadPath},
      {Protocol::RequestRead, FileTransfer::encodeRequest(offer, 0, "big.bin", 0, 1 << 20) + "extra", BadPath},
      {Protocol::RequestRead, std::string(), BadPath},
  };
  // a path length that claims more bytes than there are
  std::string lying = FileTransfer::encodeRequest(offer, 0, "big.bin", 0, 1 << 20);
  lying[12] = '\x7f';
  requests.push_back({Protocol::RequestRead, lying, BadPath});
  if (m_haveLinks) {
    requests.push_back(
        {Protocol::RequestRead, FileTransfer::encodeRequest(offer, 0, "way-out/s.txt", 0, 1 << 20), BadPath}
    );
  }

  link.loop.call([&] {
    uint32_t id = 5000;
    for (const auto &request : requests) {
      link.bToA.sendFileRequest(id++, request.kind, request.arguments);
    }
  });
  // the answers arrive later: looking for the file is done on another thread
  uint32_t id = 5000;
  for (const auto &request : requests) {
    QTRY_VERIFY2_WITH_TIMEOUT(
        link.askedByB.errorFor(id) == static_cast<uint32_t>(request.expected),
        qPrintable(QStringLiteral("request %1 got error %2, expected %3")
                       .arg(id)
                       .arg(link.askedByB.errorFor(id))
                       .arg(static_cast<uint32_t>(request.expected))),
        3000
    );
    ++id;
  }
  QVERIFY(link.waitUntilAIsIdle());
  QVERIFY2(!link.askedByB.needleSeen, "contents of a file outside of the copied directory were sent");
  QCOMPARE(link.askedByB.totalData(), uint64_t{0});
  QCOMPARE(link.a->servedRequestCount(), size_t{0});
}

void FileTransferTests::request_wrongKindOfEntry_fails()
{
  Link link;
  const ClipboardFileList list = link.copyOnA({sourcePath("tree").toStdString()});

  std::vector<ClipboardFileEntry> entries;
  COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId, 0, "odd.bin", entries), FileTransferStatus::Failed);

  auto reader = link.remoteOfB->openFile(list.offerId, 0, "nested", 0);
  COMPARE_STATUS(drain(*reader), FileTransferStatus::Failed);

  // a path too long to be sent is refused before anything is sent
  const std::string endless(Protocol::MaxPathLength + 1, 'x');
  COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId, 0, endless, entries), FileTransferStatus::NotFound);
  auto unsendable = link.remoteOfB->openFile(list.offerId, 0, endless, 0);
  COMPARE_STATUS(drain(*unsendable), FileTransferStatus::Disconnected);
}

void FileTransferTests::request_tooManyAtOnce_refused()
{
  Link link;
  const ClipboardFileList list = link.copyOnA({sourcePath("tree").toStdString()});

  // readers that never read keep their request open on the other computer
  std::vector<std::unique_ptr<RemoteFileReader>> readers;
  for (int i = 0; i < 16; ++i) {
    readers.push_back(link.remoteOfB->openFile(list.offerId, 0, "big.bin", 0));
  }
  QTRY_COMPARE_WITH_TIMEOUT(link.a->servedRequestCount(), size_t{16}, 3000);

  auto oneTooMany = link.remoteOfB->openFile(list.offerId, 0, "odd.bin", 0);
  COMPARE_STATUS(drain(*oneTooMany), FileTransferStatus::Failed);
  QCOMPARE(link.askedByB.errorFor(17), static_cast<uint32_t>(FileTransfer::Error::Busy));

  // giving them up makes room again
  readers.clear();
  QVERIFY(link.waitUntilAIsIdle());
  auto afterwards = link.remoteOfB->openFile(list.offerId, 0, "odd.bin", 0);
  quint64 total = 0;
  COMPARE_STATUS(drain(*afterwards, nullptr, &total), FileTransferStatus::Ok);
  QCOMPARE(total, static_cast<quint64>(Protocol::DefaultWindow + 12345));
}

void FileTransferTests::cancel_stopsTheSender()
{
  Link link;
  const ClipboardFileList list = link.copyOnA({sourcePath("tree").toStdString()});

  auto reader = link.remoteOfB->openFile(list.offerId, 0, "big.bin", 0);
  QByteArray buffer(100 * 1024, Qt::Uninitialized);
  uint32_t got = 0;
  COMPARE_STATUS(reader->read(buffer.data(), static_cast<uint32_t>(buffer.size()), got), FileTransferStatus::Ok);
  QVERIFY(got > 0);
  reader.reset();

  QVERIFY(link.waitUntilAIsIdle());
  QCOMPARE(link.bToA.cancels.load(), 1);

  // no more than what the window allowed was ever sent, and nothing after the cancel
  const uint64_t sent = link.askedByB.totalData();
  QVERIFY2(sent <= got + uint64_t{Protocol::DefaultWindow}, qPrintable(QString::number(sent)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  QCOMPARE(link.askedByB.totalData(), sent);

  // a cancelled reader keeps saying so
  auto cancelled = link.remoteOfB->openFile(list.offerId, 0, "big.bin", 0);
  cancelled->cancel();
  COMPARE_STATUS(cancelled->read(buffer.data(), 10, got), FileTransferStatus::Cancelled);
  QCOMPARE(got, 0U);
  QVERIFY(link.waitUntilAIsIdle());
}

void FileTransferTests::disconnect_failsTheReaderAtOnce()
{
  Link link;
  const ClipboardFileList list = link.copyOnA({sourcePath("tree").toStdString()});

  // a paste that is slower than the connection: it has read a little when the connection drops
  auto reader = link.remoteOfB->openFile(list.offerId, 0, "big.bin", 0);
  QByteArray buffer(64 * 1024, Qt::Uninitialized);
  uint32_t got = 0;
  COMPARE_STATUS(reader->read(buffer.data(), static_cast<uint32_t>(buffer.size()), got), FileTransferStatus::Ok);
  quint64 total = got;
  QVERIFY(total > 0);

  link.loop.call([&] {
    link.aToB.cut();
    link.bToA.cut();
    link.a->channelClosed(&link.aToB);
    link.b->channelClosed(&link.bToA);
  });

  // what had arrived can still be read, then the reader reports the loss instead of waiting
  auto ended = std::async(std::launch::async, [&] { return drain(*reader, nullptr, &total); });
  QVERIFY(ended.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  COMPARE_STATUS(ended.get(), FileTransferStatus::Disconnected);
  QVERIFY2(total <= got + quint64{Protocol::DefaultWindow}, qPrintable(QString::number(total)));
  QVERIFY(total < static_cast<quint64>(bigFileSize()));
  QCOMPARE(link.a->servedRequestCount(), size_t{0});

  // a reader that is blocked waiting when the connection drops is woken up
  Link second;
  const ClipboardFileList again = second.copyOnA({sourcePath("tree").toStdString()});
  second.loop.call([&] { second.aToB.cut(); });
  auto blocked = second.remoteOfB->openFile(again.offerId, 0, "big.bin", 0);
  auto woken = std::async(std::launch::async, [&] { return drain(*blocked); });
  QVERIFY(woken.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout);
  second.loop.call([&] { second.b->channelClosed(&second.bToA); });
  QVERIFY(woken.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  COMPARE_STATUS(woken.get(), FileTransferStatus::Disconnected);

  // the files of the computer that is gone can not be asked for any more
  std::vector<ClipboardFileEntry> entries;
  COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId, 0, {}, entries), FileTransferStatus::Disconnected);
}

void FileTransferTests::noAnswer_timesOut()
{
  Link link;
  const ClipboardFileList list = link.copyOnA({sourcePath("tree").toStdString()});
  link.loop.call([&] {
    link.b->setReplyTimeout(300);
    // the other computer hangs: nothing arrives, and nobody says the connection is gone
    link.aToB.cut();
  });

  QElapsedTimer timer;
  timer.start();
  std::vector<ClipboardFileEntry> entries;
  COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId, 0, {}, entries), FileTransferStatus::TimedOut);
  QVERIFY(timer.elapsed() >= 250);
  QVERIFY(timer.elapsed() < 3000);

  auto reader = link.remoteOfB->openFile(list.offerId, 0, "big.bin", 0);
  timer.restart();
  COMPARE_STATUS(drain(*reader), FileTransferStatus::TimedOut);
  QVERIFY(timer.elapsed() < 3000);
  // and stays failed
  COMPARE_STATUS(drain(*reader), FileTransferStatus::TimedOut);
}

void FileTransferTests::sourceShrinks_readerSeesTheEnd()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = QDir(dir.path()).filePath("shrinks.bin");
  const qint64 size = 20 * kMiB;
  QVERIFY(writePattern(path, size, 77));

  Link link;
  link.loop.call([&] { link.b->setReplyTimeout(5000); });
  const ClipboardFileList list = link.copyOnA({path.toStdString()});
  QCOMPARE(list.entries.at(0).size, static_cast<uint64_t>(size));

  auto reader = link.remoteOfB->openFile(list.offerId, 0, {}, 0);
  QByteArray buffer(64 * 1024, Qt::Uninitialized);
  uint32_t got = 0;
  quint64 total = 0;
  COMPARE_STATUS(reader->read(buffer.data(), static_cast<uint32_t>(buffer.size()), got), FileTransferStatus::Ok);
  total += got;

  // the file is cut down while it is being sent (not possible on every system while it is open)
  const bool shrunk = QFile::resize(path, 4 * kMiB);

  const auto status = drain(*reader, nullptr, &total);
  QVERIFY2(
      status == FileTransferStatus::Ok || status == FileTransferStatus::Failed,
      qPrintable(QStringLiteral("status is %1").arg(statusName(status)))
  );
  QVERIFY(total <= static_cast<quint64>(size));
  if (shrunk && status == FileTransferStatus::Ok) {
    QVERIFY2(total < static_cast<quint64>(size), "the whole file arrived although it was cut down");
  }
  QVERIFY(link.waitUntilAIsIdle());
}

void FileTransferTests::disabled_nothingIsOfferedOrAnswered()
{
  const std::vector<std::string> paths = {sourcePath("tree").toStdString()};
  std::vector<ClipboardFileEntry> entries;

  {
    // off on the computer that copies: no list, so nobody can ask
    Link link;
    link.loop.call([&] { link.a->setEnabled(false); });
    QVERIFY(link.loop.call([&] { return link.a->offerLocalFiles(paths); }).empty());
    QVERIFY(link.copyOnA(paths).entries.empty());
  }

  {
    // turned off after the list went out: requests are refused
    Link link;
    const ClipboardFileList list = link.copyOnA(paths);
    QVERIFY(!list.entries.empty());
    auto reader = link.remoteOfB->openFile(list.offerId, 0, "big.bin", 0);
    QByteArray buffer(1024, Qt::Uninitialized);
    uint32_t got = 0;
    COMPARE_STATUS(reader->read(buffer.data(), 1024, got), FileTransferStatus::Ok);

    link.loop.call([&] { link.a->setEnabled(false); });
    COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId, 0, {}, entries), FileTransferStatus::Refused);
    // the read that was under way ends too, after at most the data already on its way
    const auto status = drain(*reader);
    COMPARE_STATUS(status, FileTransferStatus::Refused);
    QCOMPARE(link.a->servedRequestCount(), size_t{0});

    auto later = link.remoteOfB->openFile(list.offerId, 0, "odd.bin", 0);
    COMPARE_STATUS(drain(*later), FileTransferStatus::Refused);
  }

  {
    // off on the computer that pastes: lists are not taken, nothing is asked
    Link link;
    const std::string fileList = link.loop.call([&] { return link.a->offerLocalFiles(paths); });
    QVERIFY(!fileList.empty());
    link.loop.call([&] { link.b->setEnabled(false); });
    QVERIFY(!link.loop.call([&] { return link.b->acceptRemoteFiles(fileList, &link.bToA); }));

    ClipboardFileList list;
    QVERIFY(ClipboardFileList::unmarshall(fileList, list));
    const auto status = link.remoteOfB->listDirectory(list.offerId, 0, {}, entries);
    COMPARE_STATUS(status, FileTransferStatus::Refused);
    QCOMPARE(link.askedByB.requests, uint64_t{0});

    // back on, with the list taken, it works
    link.loop.call([&] { link.b->setEnabled(true); });
    QVERIFY(link.loop.call([&] { return link.b->acceptRemoteFiles(fileList, &link.bToA); }));
    COMPARE_STATUS(link.remoteOfB->listDirectory(list.offerId, 0, {}, entries), FileTransferStatus::Ok);
    QVERIFY(!entries.empty());
  }
}

void FileTransferTests::answers_notAskedFor_ignored()
{
  Link link;
  const ClipboardFileList list = link.copyOnA({sourcePath("tree").toStdString()});
  LoopbackChannel stranger(link.loop);

  // answers to requests that were never made, and messages for requests that are not served
  link.loop.call([&] {
    std::string junk = "junk";
    link.b->handleResponse(&link.bToA, 4242, Protocol::MarkData, junk);
    link.b->handleResponse(&link.bToA, 4242, Protocol::MarkEnd, junk);
    link.b->handleResponse(&link.bToA, 0, Protocol::MarkError, junk);
    link.a->handleCredit(&link.aToB, 4242, 1 << 30);
    link.a->handleCancel(&link.aToB, 4242);
    link.a->channelFlushed(&link.aToB);
    link.a->channelFlushed(&stranger);
  });

  auto reader = link.remoteOfB->openFile(list.offerId, 0, "odd.bin", 0);
  QByteArray head(1000, Qt::Uninitialized);
  uint32_t got = 0;
  COMPARE_STATUS(reader->read(head.data(), 1000, got), FileTransferStatus::Ok);
  QCOMPARE(got, 1000U);

  // someone else can not end or feed a request made to this computer's peer
  const uint32_t id = link.askedByB.lastRequestId;
  link.loop.call([&] {
    std::string fake = "FAKE";
    link.b->handleResponse(&stranger, id, Protocol::MarkData, fake);
    link.b->handleResponse(&stranger, id, Protocol::MarkEnd, fake);
    link.b->handleResponse(&stranger, id, Protocol::MarkError, fake);
  });

  QByteArray rest;
  COMPARE_STATUS(drain(*reader, &rest), FileTransferStatus::Ok);
  QFile source(sourcePath("tree/odd.bin"));
  QVERIFY(source.open(QIODevice::ReadOnly));
  QVERIFY(head + rest == source.readAll());
}

void FileTransferTests::sender_ignoringTheWindow_isCutOff()
{
  //! Answers every read with far more data than was asked for
  class Flooding : public IFileTransferChannel
  {
  public:
    Flooding(EventLoop &loop, FileTransfer *&target) : m_loop(loop), m_target(target)
    {
    }

    void sendFileRequest(uint32_t requestId, uint8_t, const std::string &) override
    {
      for (int i = 0; i < 64; ++i) {
        m_loop.post([this, requestId] {
          if (m_target != nullptr) {
            std::string chunk(Protocol::ChunkSize, 'x');
            m_target->handleResponse(this, requestId, Protocol::MarkData, chunk);
          }
        });
      }
    }
    void sendFileResponse(uint32_t, uint8_t, const std::string &) override
    {
    }
    void sendFileCredit(uint32_t, uint32_t) override
    {
    }
    void sendFileCancel(uint32_t) override
    {
      ++cancels;
    }

    std::atomic<int> cancels = 0;

  private:
    EventLoop &m_loop;
    FileTransfer *&m_target;
  };

  EventLoop loop;
  FileTransfer *target = nullptr;
  Flooding flooding(loop, target);
  std::unique_ptr<FileTransfer> transfer;

  ClipboardFileList list;
  list.offerId = 77;
  list.entries.push_back({"flood.bin", false, 1ULL << 40, 0});
  loop.call([&] {
    transfer = std::make_unique<FileTransfer>(&loop.events());
    target = transfer.get();
    QVERIFY(transfer->acceptRemoteFiles(list.marshall(), &flooding));
  });
  auto remote = transfer->remoteFiles();

  // 64 messages of 64 KiB against a window of 256 KiB that is never read
  auto reader = remote->openFile(list.offerId, 0, {}, 0, 256 * 1024);
  QTRY_COMPARE_WITH_TIMEOUT(flooding.cancels.load(), 1, 3000);

  quint64 total = 0;
  COMPARE_STATUS(drain(*reader, nullptr, &total), FileTransferStatus::Failed);
  QVERIFY2(total <= 256 * 1024, qPrintable(QString::number(total)));

  reader.reset();
  loop.call([&] {
    target = nullptr;
    transfer->channelClosed(&flooding);
    transfer.reset();
  });
}

void FileTransferTests::transferDestroyed_readersFail()
{
  auto link = std::make_unique<Link>();
  const ClipboardFileList list = link->copyOnA({sourcePath("tree").toStdString()});
  std::shared_ptr<RemoteFiles> remote = link->remoteOfB;

  // a reader that waits for data that will never come
  link->loop.call([&] { link->aToB.cut(); });
  auto reader = remote->openFile(list.offerId, 0, "big.bin", 0);
  auto ended = std::async(std::launch::async, [&] { return drain(*reader); });
  QVERIFY(ended.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout);

  // the core shuts down while the paste code still holds on to its objects
  link.reset();
  QVERIFY(ended.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  COMPARE_STATUS(ended.get(), FileTransferStatus::Disconnected);

  std::vector<ClipboardFileEntry> entries;
  COMPARE_STATUS(remote->listDirectory(list.offerId, 0, {}, entries), FileTransferStatus::Disconnected);
  auto late = remote->openFile(list.offerId, 0, "odd.bin", 0);
  COMPARE_STATUS(drain(*late), FileTransferStatus::Disconnected);
  late->cancel();
  reader.reset();
}

void FileTransferTests::replacedOffer_runningPasteCompletes()
{
  Link link;
  const ClipboardFileList first = link.copyOnA({sourcePath("tree").toStdString()});

  auto reader = link.remoteOfB->openFile(first.offerId, 0, "odd.bin", 0);
  QByteArray head(4096, Qt::Uninitialized);
  uint32_t got = 0;
  COMPARE_STATUS(reader->read(head.data(), 4096, got), FileTransferStatus::Ok);

  // something else is copied on A while B is still pasting the first files
  const std::string second =
      link.loop.call([&] { return link.a->offerLocalFiles({sourcePath("single.txt").toStdString()}); });
  QVERIFY(!second.empty());
  QVERIFY(!link.loop.call([&] { return link.a->isLocalOffer(std::string("not a list")); }));
  QVERIFY(link.loop.call([&] { return link.a->isLocalOffer(second); }));

  quint64 total = got;
  COMPARE_STATUS(drain(*reader, nullptr, &total), FileTransferStatus::Ok);
  QCOMPARE(total, static_cast<quint64>(Protocol::DefaultWindow + 12345));

  // the rest of the paste, files it had not started on, still goes through
  std::vector<ClipboardFileEntry> entries;
  COMPARE_STATUS(link.remoteOfB->listDirectory(first.offerId, 0, "nested", entries), FileTransferStatus::Ok);
  QCOMPARE(entries.size(), size_t{2});
}

QTEST_MAIN(FileTransferTests)
