/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXRemoteFileVolumeTests.h"

#include "../deskflow/FileTransferTestLink.h"

#include "platform/OSXNfsServer.h"
#include "platform/OSXRemoteFileVolume.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>

#include <atomic>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <unistd.h>

using deskflow::ClipboardFileList;
using deskflow::OSXRemoteFileSystem;
using deskflow::OSXRemoteFileVolume;
using namespace deskflow::test;

namespace {

OSXRemoteFileVolume::Options quickOptions()
{
  OSXRemoteFileVolume::Options options;
  options.limits.partialWaitMs = 200;
  options.limits.waitMs = 2000;
  options.limits.idleMs = 1000;
  options.limits.freshMs = 500;
  options.checkIntervalMs = 200;
  options.retryAfterMs = 500;
  return options;
}

std::string canonical(const QString &path)
{
  const QString resolved = QFileInfo(path).canonicalFilePath();
  return (resolved.isEmpty() ? path : resolved).toStdString();
}

//! NFS volumes mounted below \p directory, without asking them anything
std::vector<std::string> mountsBelow(const QString &directory)
{
  const std::string base = canonical(directory) + "/";
  std::vector<std::string> found;
  const int count = ::getfsstat(nullptr, 0, MNT_NOWAIT);
  if (count <= 0) {
    return found;
  }
  std::vector<struct statfs> mounts(static_cast<size_t>(count) + 8);
  const int filled = ::getfsstat(mounts.data(), static_cast<int>(mounts.size() * sizeof(struct statfs)), MNT_NOWAIT);
  for (int i = 0; i < filled; ++i) {
    const std::string point = mounts[static_cast<size_t>(i)].f_mntonname;
    if (std::strcmp(mounts[static_cast<size_t>(i)].f_fstypename, "nfs") == 0 && point.rfind(base, 0) == 0) {
      found.push_back(point);
    }
  }
  return found;
}

bool isMountedAt(const std::string &point)
{
  const auto mounts = mountsBelow(QString::fromStdString(point.substr(0, point.rfind('/'))));
  return std::find(mounts.begin(), mounts.end(), point) != mounts.end();
}

//! A test that failed half way must not leave a volume behind
void unmountEverythingBelow(const QString &directory)
{
  for (const std::string &point : mountsBelow(directory)) {
    ::unmount(point.c_str(), MNT_FORCE);
  }
}

int run(const QString &program, const QStringList &arguments, int timeoutMs = 60000, QByteArray *output = nullptr)
{
  QProcess process;
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(program, arguments);
  if (!process.waitForFinished(timeoutMs)) {
    process.kill();
    process.waitForFinished(1000);
    return -1;
  }
  if (output != nullptr) {
    *output = process.readAll();
  }
  return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

//! The number of a process that has ended
long endedProcess()
{
  QProcess process;
  process.start("/usr/bin/true", {});
  process.waitForStarted();
  const long id = static_cast<long>(process.processId());
  process.waitForFinished();
  return id;
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs)
{
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > timeoutMs) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

bool writeScript(const QString &path, const QByteArray &body)
{
  return writeFile(path, "#!/bin/sh\n" + body + "\n") &&
         QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

//! A computer that pastes, with its volume, linked to a computer that copies
struct Pasting
{
  explicit Pasting(OSXRemoteFileVolume::Options options = quickOptions(), bool wait = true)
      : base(std::make_unique<QTemporaryDir>())
  {
    if (options.directory.empty()) {
      options.directory = QDir(base->path()).filePath("volumes").toStdString();
    }
    directory = QString::fromStdString(options.directory);
    volume = std::make_unique<OSXRemoteFileVolume>(link.remoteOfB, options);
    if (wait) {
      mounted = volume->waitUntilMounted(10000);
    }
  }

  ~Pasting()
  {
    link.release();
    if (volume != nullptr) {
      volume->unmount();
    }
    volume.reset();
    unmountEverythingBelow(base->path());
  }

  std::vector<std::string> copy(const std::vector<QString> &paths)
  {
    std::vector<std::string> local;
    for (const auto &path : paths) {
      local.push_back(path.toStdString());
    }
    list = link.copyOnA(local);
    return volume->addFiles(list);
  }

  size_t readRequests()
  {
    std::scoped_lock lock{link.askedByB.mutex};
    return link.askedByB.allowed.size();
  }

  // the directory goes last, after everything that was mounted in it
  std::unique_ptr<QTemporaryDir> base;
  QString directory;
  Link link;
  std::unique_ptr<OSXRemoteFileVolume> volume;
  ClipboardFileList list;
  bool mounted = false;
};

//! Talks to the NFS server the way another program on this computer could
class RpcClient
{
public:
  explicit RpcClient(uint16_t port, const char *address = "127.0.0.1")
  {
    m_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    ::inet_pton(AF_INET, address, &target.sin_addr);
    timeval timeout = {3, 0};
    ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int enable = 1;
    ::setsockopt(m_socket, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));
    m_connected = ::connect(m_socket, reinterpret_cast<sockaddr *>(&target), sizeof(target)) == 0;
  }

  ~RpcClient()
  {
    ::close(m_socket);
  }

  bool connected() const
  {
    return m_connected;
  }

  //! Make a call, returns the status that follows an accepted reply, or -1
  int64_t call(uint32_t program, uint32_t procedure, const QByteArray &arguments)
  {
    QByteArray message;
    for (const uint32_t value : {++m_xid, 0U, 2U, program, 3U, procedure, 0U, 0U, 0U, 0U}) {
      appendU32(message, value);
    }
    message.append(arguments);
    QByteArray record;
    appendU32(record, 0x80000000U | static_cast<uint32_t>(message.size()));
    record.append(message);
    if (::send(m_socket, record.constData(), static_cast<size_t>(record.size()), 0) != record.size()) {
      return -1;
    }

    uint8_t header[4];
    if (!receive(header, 4)) {
      return -1;
    }
    const uint32_t size = ((header[0] & 0x7fU) << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
    QByteArray reply(static_cast<qsizetype>(size), '\0');
    if (!receive(reply.data(), size) || size < 24) {
      return -1;
    }
    // xid, reply, accepted, no verifier, success, then the result
    const auto *bytes = reinterpret_cast<const uint8_t *>(reply.constData());
    auto u32 = [bytes](size_t at) {
      return (static_cast<uint32_t>(bytes[at]) << 24) | (static_cast<uint32_t>(bytes[at + 1]) << 16) |
             (static_cast<uint32_t>(bytes[at + 2]) << 8) | bytes[at + 3];
    };
    if (u32(4) != 1 || u32(8) != 0 || u32(20) != 0) {
      return -1;
    }
    m_result = (size >= 28) ? reply.mid(28) : QByteArray();
    return (size >= 28) ? static_cast<int64_t>(u32(24)) : 0;
  }

  //! What followed the status in the last reply
  const QByteArray &result() const
  {
    return m_result;
  }

  static void appendU64(QByteArray &data, uint64_t value)
  {
    appendU32(data, static_cast<uint32_t>(value >> 32));
    appendU32(data, static_cast<uint32_t>(value));
  }

  static void appendU32(QByteArray &data, uint32_t value)
  {
    const char bytes[4] = {
        static_cast<char>(value >> 24), static_cast<char>(value >> 16), static_cast<char>(value >> 8),
        static_cast<char>(value)
    };
    data.append(bytes, 4);
  }

  static QByteArray opaque(const QByteArray &bytes)
  {
    QByteArray data;
    appendU32(data, static_cast<uint32_t>(bytes.size()));
    data.append(bytes);
    data.append((4 - bytes.size() % 4) % 4, '\0');
    return data;
  }

private:
  bool receive(void *buffer, size_t size)
  {
    auto *target = static_cast<char *>(buffer);
    size_t got = 0;
    while (got < size) {
      const ssize_t n = ::recv(m_socket, target + got, size - got, 0);
      if (n <= 0) {
        return false;
      }
      got += static_cast<size_t>(n);
    }
    return true;
  }

  int m_socket = -1;
  bool m_connected = false;
  uint32_t m_xid = 0;
  QByteArray m_result;
};

//! Takes apart what a reply holds
class ReplyReader
{
public:
  explicit ReplyReader(const QByteArray &data) : m_data(data)
  {
  }

  bool ok() const
  {
    return m_ok;
  }

  uint32_t u32()
  {
    if (m_data.size() - m_position < 4) {
      m_ok = false;
      return 0;
    }
    const auto *bytes = reinterpret_cast<const uint8_t *>(m_data.constData() + m_position);
    m_position += 4;
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
  }

  uint64_t u64()
  {
    const uint64_t high = u32();
    return (high << 32) | u32();
  }

  QByteArray opaque()
  {
    const qsizetype size = u32();
    if (!m_ok || m_data.size() - m_position < size) {
      m_ok = false;
      return {};
    }
    const QByteArray value = m_data.mid(m_position, size);
    m_position += size + (4 - size % 4) % 4;
    return value;
  }

  void skip(qsizetype bytes)
  {
    m_position = std::min(m_position + bytes, m_data.size());
  }

  //! Attributes that may follow
  void skipAttributes()
  {
    if (u32() != 0) {
      skip(84);
    }
  }

private:
  const QByteArray &m_data;
  qsizetype m_position = 0;
  bool m_ok = true;
};

const uint32_t kMountProgram = 100005;
const uint32_t kNfsProgram = 100003;

//! An address of this computer that is not the loopback one, empty if there is none
std::string otherAddress()
{
  ifaddrs *addresses = nullptr;
  if (::getifaddrs(&addresses) != 0) {
    return {};
  }
  std::string found;
  for (const ifaddrs *entry = addresses; entry != nullptr && found.empty(); entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    const auto *address = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
    if (ntohl(address->sin_addr.s_addr) >> 24 == 127) {
      continue;
    }
    char text[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
    found = text;
  }
  ::freeifaddrs(addresses);
  return found;
}

} // namespace

QString OSXRemoteFileVolumeTests::sourcePath(const QString &relative) const
{
  return QDir(m_source->path()).filePath(relative);
}

void OSXRemoteFileVolumeTests::initTestCase()
{
  if (!QFileInfo::exists("/sbin/mount_nfs")) {
    QSKIP("this system has no NFS client");
  }
  // a build machine may not let its user mount, everything here needs it
  if (!Pasting().mounted) {
    QSKIP("this user can not mount an NFS volume here");
  }
  m_source = std::make_unique<QTemporaryDir>();
  QVERIFY(m_source->isValid());

  // the computer that copies has:
  //   big.bin            24 MiB that do not repeat
  //   tree/a.txt
  //   tree/empty.bin
  //   tree/中文 目录/b.bin  3 MiB
  //   tree/nested/deeper/c.txt
  QVERIFY(QDir().mkpath(sourcePath(QStringLiteral("tree/中文 目录"))));
  QVERIFY(QDir().mkpath(sourcePath("tree/nested/deeper")));
  QVERIFY(writePattern(sourcePath("big.bin"), 24 * kMiB, 17));
  QVERIFY(writeFile(sourcePath("tree/a.txt"), "first file"));
  QVERIFY(writeFile(sourcePath("tree/empty.bin"), {}));
  QVERIFY(writePattern(sourcePath(QStringLiteral("tree/中文 目录/b.bin")), 3 * kMiB, 18));
  QVERIFY(writeFile(sourcePath("tree/nested/deeper/c.txt"), "deep down"));
}

void OSXRemoteFileVolumeTests::cleanupTestCase()
{
  m_source.reset();
}

void OSXRemoteFileVolumeTests::copiedTree_copiesBackUnchanged()
{
  Pasting pasting;
  QVERIFY(pasting.mounted);
  const auto paths = pasting.copy({sourcePath("tree"), sourcePath("big.bin")});
  QCOMPARE(paths.size(), size_t{2});
  QVERIFY(!paths[0].empty() && !paths[1].empty());
  QVERIFY(pasting.volume->contains(paths[0]));

  // the way a program that pastes copies: with the files of the system
  QTemporaryDir target;
  QVERIFY(target.isValid());
  for (const auto &path : paths) {
    QByteArray output;
    QCOMPARE(run("/bin/cp", {"-R", QString::fromStdString(path), target.path()}, 60000, &output), 0);
  }

  QMap<QString, QByteArray> expected;
  QMap<QString, QByteArray> actual;
  describe(sourcePath("tree"), "tree", expected);
  describe(sourcePath("big.bin"), "big.bin", expected);
  describe(QDir(target.path()).filePath("tree"), "tree", actual);
  describe(QDir(target.path()).filePath("big.bin"), "big.bin", actual);
  QCOMPARE(actual, expected);

  // what was pasted can be changed like anything else
  for (const char *name : {"tree", "tree/a.txt", "tree/nested/deeper", "big.bin"}) {
    const QFileInfo copied(QDir(target.path()).filePath(name));
    QVERIFY2(copied.permission(QFileDevice::WriteOwner), name);
    QVERIFY2(!copied.isDir() || copied.permission(QFileDevice::ExeOwner), name);
  }
  // the volume itself can't be
  QVERIFY(!QFile(QString::fromStdString(paths[0]) + "/new.txt").open(QIODevice::WriteOnly));
  QVERIFY(!QFile::remove(QString::fromStdString(paths[0]) + "/a.txt"));
  QVERIFY(QFileInfo::exists(QString::fromStdString(paths[0]) + "/a.txt"));
}

void OSXRemoteFileVolumeTests::manyReadersAtOnce_getTheRightBytes()
{
  Pasting pasting;
  QVERIFY(pasting.mounted);
  const auto paths = pasting.copy({sourcePath("big.bin")});
  const int file = ::open(paths[0].c_str(), O_RDONLY);
  QVERIFY(file >= 0);

  // several threads that read blocks at once, as Finder does
  const qint64 blocks = 24;
  std::vector<QByteArray> parts(static_cast<size_t>(blocks));
  std::atomic<qint64> next = 0;
  std::atomic<int> failures = 0;
  std::vector<std::thread> readers;
  for (int i = 0; i < 8; ++i) {
    readers.emplace_back([&] {
      for (qint64 block = next++; block < blocks; block = next++) {
        QByteArray &part = parts[static_cast<size_t>(block)];
        part.resize(kMiB);
        qint64 got = 0;
        while (got < kMiB) {
          const ssize_t n = ::pread(file, part.data() + got, static_cast<size_t>(kMiB - got), block * kMiB + got);
          if (n <= 0) {
            ++failures;
            break;
          }
          got += n;
        }
      }
    });
  }
  for (auto &reader : readers) {
    reader.join();
  }
  ::close(file);

  QCOMPARE(failures.load(), 0);
  QByteArray data;
  for (const auto &part : parts) {
    data.append(part);
  }
  QFile source(sourcePath("big.bin"));
  QVERIFY(source.open(QIODevice::ReadOnly));
  QCOMPARE(data, source.readAll());
}

void OSXRemoteFileVolumeTests::nothingIsFetched_untilAFileIsRead()
{
  Pasting pasting;
  QVERIFY(pasting.mounted);
  const auto paths = pasting.copy({sourcePath("tree"), sourcePath("big.bin")});
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  QCOMPARE(pasting.link.askedByB.requests, uint64_t{0});

  // looking at everything, sizes and dates included
  QByteArray output;
  QCOMPARE(run("/bin/ls", {"-laR", QString::fromStdString(paths[0])}, 30000, &output), 0);
  QVERIFY2(output.contains("deep") && output.contains("b.bin"), output.constData());
  QCOMPARE(run("/bin/ls", {"-la", QString::fromStdString(paths[1])}, 30000, &output), 0);
  QVERIFY2(output.contains(QByteArray::number(24 * kMiB)), output.constData());
  QCOMPARE(pasting.readRequests(), size_t{0});
  QCOMPARE(pasting.volume->fileSystem().fetchedBytes(), uint64_t{0});

  QCOMPARE(run("/bin/cat", {QString::fromStdString(paths[0]) + "/a.txt"}, 30000, &output), 0);
  QCOMPARE(output, QByteArray("first file"));
  QCOMPARE(pasting.readRequests(), size_t{1});
}

void OSXRemoteFileVolumeTests::fileChangedAfterCopy_isCopiedAsItIsNow()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = QDir(directory.path()).filePath("changes.txt");
  QVERIFY(writeFile(path, "as it was"));

  Pasting pasting;
  QVERIFY(pasting.mounted);
  const auto paths = pasting.copy({path});
  QByteArray output;
  QCOMPARE(run("/bin/cat", {QString::fromStdString(paths[0])}, 30000, &output), 0);
  QCOMPARE(output, QByteArray("as it was"));

  // changed between copying and pasting: what is pasted is what is there now, all of it
  const QByteArray now = patternBlock(21, 200 * 1024);
  QVERIFY(writeFile(path, now));
  std::this_thread::sleep_for(std::chrono::milliseconds(1600));
  QCOMPARE(run("/bin/cat", {QString::fromStdString(paths[0])}, 30000, &output), 0);
  QCOMPARE(output.size(), now.size());
  QCOMPARE(output, now);
}

void OSXRemoteFileVolumeTests::slowSource_copyCompletes()
{
  Pasting pasting;
  QVERIFY(pasting.mounted);
  const auto paths = pasting.copy({sourcePath("big.bin")});
  // about 12 MB/s, and every read gets what has arrived
  pasting.link.aToB.setDataDelay(5);

  QTemporaryDir target;
  QVERIFY(target.isValid());
  QCOMPARE(run("/bin/cp", {QString::fromStdString(paths[0]), target.path()}, 120000), 0);
  QMap<QString, QByteArray> expected;
  QMap<QString, QByteArray> actual;
  describe(sourcePath("big.bin"), "big.bin", expected);
  describe(QDir(target.path()).filePath("big.bin"), "big.bin", actual);
  QCOMPARE(actual, expected);
  pasting.link.aToB.setDataDelay(0);
}

void OSXRemoteFileVolumeTests::sourceGone_copyFails_volumeStays()
{
  Pasting pasting;
  QVERIFY(pasting.mounted);
  const auto paths = pasting.copy({sourcePath("big.bin"), sourcePath("tree")});
  pasting.link.aToB.setDataDelay(20);

  QTemporaryDir target;
  QVERIFY(target.isValid());
  QProcess copying;
  copying.start("/bin/cp", {QString::fromStdString(paths[0]), target.path()});
  QVERIFY(copying.waitForStarted());
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // the computer that copied goes away
  QElapsedTimer timer;
  timer.start();
  pasting.link.loop.call([&] {
    pasting.link.aToB.cut();
    pasting.link.bToA.cut();
    pasting.link.b->channelClosed(&pasting.link.bToA);
  });
  QVERIFY(copying.waitForFinished(20000));
  QVERIFY(copying.exitCode() != 0);
  QVERIFY2(timer.elapsed() < 10000, qPrintable(QString::number(timer.elapsed())));

  // the volume is still there and answers, only the files can't be fetched
  QVERIFY(isMountedAt(pasting.volume->mountPoint()));
  QByteArray output;
  QCOMPARE(run("/bin/ls", {QString::fromStdString(pasting.volume->mountPoint())}, 10000, &output), 0);
  timer.restart();
  QVERIFY(run("/bin/cat", {QString::fromStdString(paths[1]) + "/a.txt"}, 30000, &output) != 0);
  QVERIFY2(timer.elapsed() < 10000, qPrintable(QString::number(timer.elapsed())));
}

void OSXRemoteFileVolumeTests::otherClients_areTurnedAway()
{
  Pasting pasting;
  QVERIFY(pasting.mounted);
  pasting.copy({sourcePath("tree")});
  const uint16_t port = pasting.volume->serverPort();
  QVERIFY(port != 0);

  RpcClient client(port);
  QVERIFY(client.connected());
  // knowing the path does not help once the volume is mounted, guessing never does
  QCOMPARE(
      client.call(kMountProgram, 1, RpcClient::opaque(QByteArray::fromStdString(pasting.volume->exportPath()))), 13
  );
  QCOMPARE(client.call(kMountProgram, 1, RpcClient::opaque("/export")), 13);
  QCOMPARE(client.call(kMountProgram, 1, RpcClient::opaque("/")), 13);
  // nothing is told about what is exported or mounted
  QCOMPARE(client.call(kMountProgram, 5, {}), 0);
  QCOMPARE(client.call(kMountProgram, 2, {}), 0);
  // handles can not be guessed, and ones of another size are not even looked at
  QCOMPARE(client.call(kNfsProgram, 1, RpcClient::opaque(QByteArray(16, '\x01'))), 70);
  QCOMPARE(client.call(kNfsProgram, 1, RpcClient::opaque(QByteArray(8, '\x01'))), 10001);
  QByteArray read = RpcClient::opaque(QByteArray(16, '\x02'));
  RpcClient::appendU32(read, 0);
  RpcClient::appendU32(read, 0);
  RpcClient::appendU32(read, 4096);
  QCOMPARE(client.call(kNfsProgram, 6, read), 70);
  // nothing is ever written
  QCOMPARE(client.call(kNfsProgram, 7, RpcClient::opaque(QByteArray(16, '\x01'))), 30);

  // a server that has not been mounted yet answers one mount request, from who knows the path first.
  // should that be another client, the mount of this process fails and is made again.
  OSXRemoteFileSystem files(pasting.link.remoteOfB);
  deskflow::nfs::Server server(files);
  QVERIFY(server.start("/secret-path"));
  RpcClient early(server.port());
  QVERIFY(early.connected());
  QCOMPARE(early.call(kMountProgram, 1, RpcClient::opaque("/wrong")), 13);
  QCOMPARE(early.call(kMountProgram, 1, RpcClient::opaque("/secret-path")), 0);
  ReplyReader mounted(early.result());
  const QByteArray root = mounted.opaque();
  QCOMPARE(root.size(), qsizetype{16});
  QCOMPARE(early.call(kNfsProgram, 1, RpcClient::opaque(root)), 0);
  QCOMPARE(early.call(kMountProgram, 1, RpcClient::opaque("/secret-path")), 13);
  server.stop();

  // the handles of one server mean nothing to the next, the one a volume mounted again gets
  deskflow::nfs::Server next(files);
  QVERIFY(next.start("/next-path"));
  RpcClient later(next.port());
  QVERIFY(later.connected());
  QCOMPARE(later.call(kNfsProgram, 1, RpcClient::opaque(root)), 70);
  QCOMPARE(later.call(kMountProgram, 1, RpcClient::opaque("/next-path")), 0);
  ReplyReader mountedAgain(later.result());
  QVERIFY(mountedAgain.opaque() != root);
  next.stop();

  // and nothing of it is reachable from the network
  const std::string address = otherAddress();
  if (!address.empty()) {
    RpcClient outside(port, address.c_str());
    QVERIFY(!outside.connected());
  }
}

void OSXRemoteFileVolumeTests::listingChangedBetweenPages_positionsAreRefused()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QDir pages(QDir(directory.path()).filePath("pages"));
  QVERIFY(QDir().mkpath(pages.path()));
  for (int i = 0; i < 40; ++i) {
    QVERIFY(writeFile(pages.filePath(QStringLiteral("file-%1.txt").arg(i, 2, 10, QLatin1Char('0'))), "x"));
  }

  Link link;
  OSXRemoteFileSystem::Limits limits;
  limits.freshMs = 200;
  OSXRemoteFileSystem files(link.remoteOfB, limits);
  const ClipboardFileList list = link.copyOnA({pages.path().toStdString()});
  const std::vector<std::string> shown = files.addOffer(list);
  QCOMPARE(shown.size(), size_t{1});
  deskflow::nfs::Server server(files);
  QVERIFY(server.start("/listing"));
  RpcClient client(server.port());
  QVERIFY(client.connected());

  // down to the copied directory, the way the system finds it
  QCOMPARE(client.call(kMountProgram, 1, RpcClient::opaque("/listing")), 0);
  QByteArray handle = ReplyReader(client.result()).opaque();
  for (const QString &name : QString::fromStdString(shown[0]).split('/')) {
    QByteArray lookup = RpcClient::opaque(handle);
    lookup.append(RpcClient::opaque(name.toUtf8()));
    QCOMPARE(client.call(kNfsProgram, 3, lookup), 0);
    handle = ReplyReader(client.result()).opaque();
  }
  const auto readPage = [&](uint64_t cookie, uint64_t verifier, uint32_t count) {
    QByteArray arguments = RpcClient::opaque(handle);
    RpcClient::appendU64(arguments, cookie);
    RpcClient::appendU64(arguments, verifier);
    RpcClient::appendU32(arguments, count);
    return client.call(kNfsProgram, 16, arguments);
  };

  // the first page, it ends before the directory does
  QCOMPARE(readPage(0, 0, 600), 0);
  ReplyReader page(client.result());
  page.skipAttributes();
  const uint64_t verifier = page.u64();
  uint64_t cookie = 0;
  int entries = 0;
  while (page.u32() != 0 && page.ok()) {
    page.u64();
    page.opaque();
    cookie = page.u64();
    ++entries;
  }
  QVERIFY(page.ok());
  QCOMPARE(page.u32(), 0U);
  QVERIFY(entries > 2 && entries < 42);

  // a file goes on the other computer, a pause later the directory is listed again
  QVERIFY(QFile::remove(pages.filePath("file-00.txt")));
  std::this_thread::sleep_for(std::chrono::milliseconds(limits.freshMs + 200));
  // the positions the client has are of the directory as it was: it is told to start over
  QCOMPARE(readPage(cookie, verifier, 600), 10003);

  QCOMPARE(readPage(0, 0, 64 * 1024), 0);
  ReplyReader whole(client.result());
  whole.skipAttributes();
  QVERIFY(whole.u64() != verifier);
  QStringList names;
  while (whole.u32() != 0 && whole.ok()) {
    whole.u64();
    names.append(QString::fromUtf8(whole.opaque()));
    whole.u64();
  }
  QVERIFY(whole.ok());
  QCOMPARE(names.size(), 41);
  QVERIFY(!names.contains("file-00.txt"));
  server.stop();
}

void OSXRemoteFileVolumeTests::tooManyConnections_areClosed()
{
  Link link;
  OSXRemoteFileSystem files(link.remoteOfB);
  deskflow::nfs::Server server(files);
  QVERIFY(server.start("/connections"));

  // the system's client makes one or two, the server takes a few
  std::vector<std::unique_ptr<RpcClient>> clients;
  for (int i = 0; i < 8; ++i) {
    clients.push_back(std::make_unique<RpcClient>(server.port()));
    QVERIFY(clients.back()->connected());
    QCOMPARE(clients.back()->call(kNfsProgram, 0, {}), 0);
  }
  // more are closed as they come
  RpcClient extra(server.port());
  QCOMPARE(extra.call(kNfsProgram, 0, {}), -1);
  QCOMPARE(clients.front()->call(kNfsProgram, 0, {}), 0);

  // once one goes there is room again
  clients.pop_back();
  QElapsedTimer timer;
  timer.start();
  int64_t answered = -1;
  while (answered != 0 && timer.elapsed() < 5000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    RpcClient again(server.port());
    answered = again.call(kNfsProgram, 0, {});
  }
  QCOMPARE(answered, 0);
  clients.clear();
  server.stop();
}

void OSXRemoteFileVolumeTests::unmount_leavesNothingBehind()
{
  Pasting pasting;
  QVERIFY(pasting.mounted);
  const auto paths = pasting.copy({sourcePath("tree")});
  QByteArray output;
  QCOMPARE(run("/bin/cat", {QString::fromStdString(paths[0]) + "/a.txt"}, 30000, &output), 0);
  const std::string point = pasting.volume->mountPoint();
  const uint16_t port = pasting.volume->serverPort();
  QVERIFY(isMountedAt(point));

  QElapsedTimer timer;
  timer.start();
  pasting.volume->unmount();
  QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
  QVERIFY(!isMountedAt(point));
  QVERIFY(!QFileInfo::exists(QString::fromStdString(point)));
  QCOMPARE(QDir(pasting.directory).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList());
  RpcClient client(port);
  QVERIFY(!client.connected());
  // once unmounted, nothing more is shown
  QVERIFY(pasting.volume->addFiles(pasting.list)[0].empty());
  QVERIFY(!pasting.volume->isMounted());
}

void OSXRemoteFileVolumeTests::leftoversOfDeadProcesses_areRemoved()
{
  QTemporaryDir base;
  QVERIFY(base.isValid());
  const QString directory = QDir(base.path()).filePath("volumes");
  Link link;

  // a process that ended without unmounting
  auto options = quickOptions();
  options.directory = directory.toStdString();
  options.owner = endedProcess();
  QVERIFY(options.owner > 0);
  auto dead = std::make_unique<OSXRemoteFileVolume>(link.remoteOfB, options);
  QVERIFY(dead->waitUntilMounted(10000));
  const std::string deadPoint = dead->mountPoint();
  dead->abandon();
  QVERIFY(isMountedAt(deadPoint));

  // and the directory of one that still runs, which must stay
  const QString running = QDir(directory).filePath(QStringLiteral("%1-abcdef01").arg(::getpid()));
  QVERIFY(QDir().mkpath(running + "/Deskflow"));

  options.owner = 0;
  {
    OSXRemoteFileVolume next(link.remoteOfB, options);
    QVERIFY(next.waitUntilMounted(10000));
    QVERIFY(!isMountedAt(deadPoint));
    QVERIFY(!QFileInfo::exists(QString::fromStdString(deadPoint)));
    QVERIFY(!QFileInfo::exists(QFileInfo(QString::fromStdString(deadPoint)).path()));
    QVERIFY(QFileInfo::exists(running));
  }
  dead.reset();
  unmountEverythingBelow(base.path());
}

void OSXRemoteFileVolumeTests::mountCommandSlowOrFailing_neverHoldsUpAddingFiles()
{
  QTemporaryDir scripts;
  QVERIFY(scripts.isValid());
  const QString slow = QDir(scripts.path()).filePath("slow.sh");
  const QString hangs = QDir(scripts.path()).filePath("hangs.sh");
  QVERIFY(writeScript(slow, "sleep 2\nexit 1"));
  QVERIFY(writeScript(hangs, "sleep 30"));

  auto options = quickOptions();
  options.mountCommand = slow.toStdString();
  options.retryAfterMs = 60000;
  Pasting pasting(options, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // while the mount command runs, and after it failed
  for (int round = 0; round < 2; ++round) {
    pasting.list = pasting.link.copyOnA({sourcePath("tree/a.txt").toStdString()});
    QElapsedTimer timer;
    timer.start();
    const auto paths = pasting.volume->addFiles(pasting.list);
    QVERIFY2(timer.elapsed() < 50, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(paths.size(), size_t{1});
    QVERIFY(paths[0].empty());
    QVERIFY(!pasting.volume->isMounted());
    if (round == 0) {
      QVERIFY(!pasting.volume->waitUntilMounted(5000));
    }
  }

  // a command that does not end is not waited for when the volume goes
  options.mountCommand = hangs.toStdString();
  options.mountTimeoutMs = 60000;
  options.directory = QDir(pasting.base->path()).filePath("other").toStdString();
  auto hanging = std::make_unique<OSXRemoteFileVolume>(pasting.link.remoteOfB, options);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  QElapsedTimer timer;
  timer.start();
  hanging.reset();
  QVERIFY2(timer.elapsed() < 1500, qPrintable(QString::number(timer.elapsed())));

  // and one that does not exist fails at once
  options.mountCommand = "/nonexistent/mount_nfs";
  options.directory = QDir(pasting.base->path()).filePath("third").toStdString();
  OSXRemoteFileVolume missing(pasting.link.remoteOfB, options);
  QVERIFY(!missing.waitUntilMounted(3000));
}

void OSXRemoteFileVolumeTests::volumeLost_isMountedAgain()
{
  Pasting pasting;
  QVERIFY(pasting.mounted);
  const auto paths = pasting.copy({sourcePath("tree")});
  const std::string before = pasting.volume->mountPoint();

  // somebody unmounts it
  QCOMPARE(::unmount(before.c_str(), MNT_FORCE), 0);
  QVERIFY(waitFor([&] { return pasting.volume->isMounted() && pasting.volume->mountPoint() != before; }, 10000));
  QVERIFY(!QFileInfo::exists(QString::fromStdString(before)));

  // the files are there again, at the new place
  const auto again = pasting.volume->addFiles(pasting.list);
  QVERIFY(!again[0].empty());
  QVERIFY(QString::fromStdString(again[0]).startsWith(QString::fromStdString(pasting.volume->mountPoint())));
  QByteArray output;
  QCOMPARE(run("/bin/cat", {QString::fromStdString(again[0]) + "/nested/deeper/c.txt"}, 30000, &output), 0);
  QCOMPARE(output, QByteArray("deep down"));
}

void OSXRemoteFileVolumeTests::contains_tellsWhatIsOnAVolume()
{
  Pasting pasting;
  QVERIFY(pasting.mounted);
  const QString given = pasting.directory;
  const QString resolved = QFileInfo(given).canonicalFilePath();
  QVERIFY(given != resolved);

  const auto contains = [&](const QString &path) { return pasting.volume->contains(path.toStdString()); };
  QVERIFY(contains(QString::fromStdString(pasting.volume->mountPoint()) + "/x"));
  QVERIFY(contains(given + "/123-abcd/Deskflow/file"));
  QVERIFY(contains(resolved + "/123-abcd/Deskflow/file"));
  QVERIFY(!contains(given));
  QVERIFY(!contains(given + "x/file"));
  QVERIFY(!contains("/tmp/file"));
  QVERIFY(!contains(sourcePath("tree")));
}

QTEST_MAIN(OSXRemoteFileVolumeTests)
