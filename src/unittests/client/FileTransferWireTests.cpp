/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransferWireTests.h"

#include "base/Event.h"
#include "base/EventQueue.h"
#include "client/Client.h"
#include "client/ServerProxy.h"
#include "deskflow/Clipboard.h"
#include "deskflow/FileTransfer.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"
#include "server/ClientProxy1_6.h"
#include "server/Server.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>

#include <cstring>
#include <future>
#include <mutex>
#include <random>
#include <thread>

using deskflow::ClipboardFileEntry;
using deskflow::ClipboardFileList;
using deskflow::FileTransfer;
using deskflow::FileTransferStatus;
using deskflow::RemoteFiles;
using Protocol = deskflow::FileTransfer::Protocol;

namespace {

class TestTask : public EventData
{
public:
  explicit TestTask(std::function<void()> task) : m_task(std::move(task))
  {
  }
  std::function<void()> m_task;
};

//! A real event queue running on its own thread, like the one that drives the core
class EventLoop
{
public:
  EventLoop()
  {
    m_events.addHandler(EventTypes::FileTransferTask, this, [](const Event &event) {
      static_cast<const TestTask *>(event.getDataObject())->m_task();
    });
    m_thread = std::thread([this] { m_events.loop(); });
    m_events.waitForReady();
  }

  ~EventLoop()
  {
    m_events.addEvent(Event(EventTypes::Quit));
    m_thread.join();
    m_events.removeHandler(EventTypes::FileTransferTask, this);
  }

  EventQueue &events()
  {
    return m_events;
  }

  void post(std::function<void()> task)
  {
    m_events.addEvent(Event(EventTypes::FileTransferTask, this, new TestTask(std::move(task))));
  }

  template <typename Task> auto call(Task task) -> decltype(task())
  {
    using Result = decltype(task());
    std::promise<Result> promise;
    auto future = promise.get_future();
    post([&promise, &task] {
      if constexpr (std::is_void_v<Result>) {
        task();
        promise.set_value();
      } else {
        promise.set_value(task());
      }
    });
    return future.get();
  }

private:
  EventQueue m_events;
  std::thread m_thread;
};

//! One end of an in memory connection, bytes written here can be read at the other end
class PipeStream : public deskflow::IStream
{
public:
  explicit PipeStream(IEventQueue *events) : m_events(events)
  {
  }

  void connect(PipeStream *peer)
  {
    m_peer = peer;
  }

  //! Everything that was written to this end so far
  QByteArray written()
  {
    std::scoped_lock lock{m_mutex};
    return m_written;
  }

  void close() override
  {
  }

  uint32_t read(void *buffer, uint32_t n) override
  {
    std::scoped_lock lock{m_mutex};
    const auto take = static_cast<uint32_t>(std::min<qsizetype>(n, m_input.size()));
    if (buffer != nullptr && take > 0) {
      std::memcpy(buffer, m_input.constData(), take);
    }
    m_input.remove(0, take);
    return take;
  }

  void write(const void *buffer, uint32_t n) override
  {
    {
      std::scoped_lock lock{m_mutex};
      m_written.append(static_cast<const char *>(buffer), n);
    }
    if (m_peer != nullptr) {
      {
        std::scoped_lock lock{m_peer->m_mutex};
        m_peer->m_input.append(static_cast<const char *>(buffer), n);
      }
      m_events->addEvent(Event(EventTypes::StreamInputReady, m_peer->getEventTarget()));
    }
    // a real connection reports when what was written has left
    m_events->addEvent(Event(EventTypes::StreamOutputFlushed, getEventTarget()));
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
  }

  void shutdownOutput() override
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<PipeStream *>(this);
  }

  bool isReady() const override
  {
    std::scoped_lock lock{m_mutex};
    return !m_input.isEmpty();
  }

  uint32_t getSize() const override
  {
    std::scoped_lock lock{m_mutex};
    return static_cast<uint32_t>(m_input.size());
  }

private:
  IEventQueue *m_events;
  PipeStream *m_peer = nullptr;
  mutable std::mutex m_mutex;
  QByteArray m_input;
  QByteArray m_written;
};

Client *noClient()
{
  // the file transfer messages are handled without calling through to the client
  return reinterpret_cast<Client *>(0x1);
}

Server *noServer()
{
  return reinterpret_cast<Server *>(0x1);
}

bool isFileMessage(const uint8_t *code)
{
  return memcmp(code, kMsgDFileRequest, 4) == 0 || memcmp(code, kMsgDFileResponse, 4) == 0 ||
         memcmp(code, kMsgDFileCredit, 4) == 0 || memcmp(code, kMsgDFileCancel, 4) == 0;
}

//! The client side proxy, parsing only what these tests are about
/*!
Everything else the server sends here has no parameters (query info, keep
alive) and would need a real client to answer, so it is skipped.
*/
class TestServerProxy : public ServerProxy
{
public:
  TestServerProxy(deskflow::IStream *stream, IEventQueue *events) : ServerProxy(noClient(), stream, events)
  {
    events->addHandler(EventTypes::StreamInputReady, stream->getEventTarget(), [this](const Event &) { pump(); });
  }

  using ServerProxy::acceptFileList;

  std::atomic<int> parseFailures = 0;

private:
  void pump()
  {
    uint8_t code[4];
    while (getStream()->read(code, 4) == 4) {
      if (isFileMessage(code) && parseMessage(code) != ConnectionResult::Okay) {
        ++parseFailures;
      }
    }
  }
};

//! A client and a server proxy talking to each other, each with its own FileTransfer
struct Connection
{
  Connection()
  {
    loop.call([this] {
      clientStream = std::make_unique<PipeStream>(&loop.events());
      // owned by the server side proxy
      serverStream = new PipeStream(&loop.events());
      clientStream->connect(serverStream);
      serverStream->connect(clientStream.get());

      onClient = std::make_unique<FileTransfer>(&loop.events());
      onServer = std::make_unique<FileTransfer>(&loop.events());

      serverProxy = std::make_unique<TestServerProxy>(clientStream.get(), &loop.events());
      serverProxy->setFileTransfer(onClient.get());
      clientProxy = std::make_unique<ClientProxy1_6>("client", serverStream, noServer(), &loop.events());
      clientProxy->setFileTransfer(onServer.get());

      // the client introduces itself, after that the server side parses ordinary messages
      ProtocolUtil::writef(clientStream.get(), kMsgDInfo, 0, 0, 1920, 1080, 0, 100, 100);
    });
    // let the introduction arrive
    loop.call([] {});
    filesOfServer = onClient->remoteFiles();
    filesOfClient = onServer->remoteFiles();
  }

  ~Connection()
  {
    loop.call([this] {
      clientProxy.reset();
      serverProxy.reset();
      onServer.reset();
      onClient.reset();
      clientStream.reset();
    });
  }

  //! Files are copied on the server and the client is told, as the clipboard message would do
  ClipboardFileList copyOnServer(const std::vector<std::string> &paths)
  {
    ClipboardFileList list;
    const std::string fileList = loop.call([&] { return onServer->offerLocalFiles(paths); });
    ClipboardFileList::unmarshall(fileList, list);
    loop.call([&] { onClient->acceptRemoteFiles(fileList, serverProxy.get()); });
    return list;
  }

  ClipboardFileList copyOnClient(const std::vector<std::string> &paths)
  {
    ClipboardFileList list;
    const std::string fileList = loop.call([&] { return onClient->offerLocalFiles(paths); });
    ClipboardFileList::unmarshall(fileList, list);
    loop.call([&] { onServer->acceptRemoteFiles(fileList, clientProxy.get()); });
    return list;
  }

  EventLoop loop;
  std::unique_ptr<PipeStream> clientStream;
  PipeStream *serverStream = nullptr;
  std::unique_ptr<FileTransfer> onClient;
  std::unique_ptr<FileTransfer> onServer;
  //! The server as seen by the client, and the client as seen by the server
  std::unique_ptr<TestServerProxy> serverProxy;
  std::unique_ptr<ClientProxy1_6> clientProxy;
  std::shared_ptr<RemoteFiles> filesOfServer;
  std::shared_ptr<RemoteFiles> filesOfClient;
};

bool writeFile(const QString &path, const QByteArray &data)
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray randomBytes(qsizetype size, quint64 seed)
{
  std::mt19937_64 generator(seed);
  QByteArray data((size + 7) / 8 * 8, Qt::Uninitialized);
  auto *words = reinterpret_cast<quint64 *>(data.data());
  for (qsizetype i = 0; i < data.size() / 8; ++i) {
    words[i] = generator();
  }
  data.truncate(size);
  return data;
}

QByteArray hashOfFile(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return "unreadable";
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(&file);
  return hash.result().toHex();
}

//! Download one file with the blocking interface, returns its SHA-256 or the failure
QByteArray download(RemoteFiles &remote, uint64_t offerId, uint32_t item, const std::string &relativePath)
{
  auto reader = remote.openFile(offerId, item, relativePath, 0);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  QByteArray buffer(100 * 1000, Qt::Uninitialized);
  for (;;) {
    uint32_t got = 0;
    const auto status = reader->read(buffer.data(), static_cast<uint32_t>(buffer.size()), got);
    if (status != FileTransferStatus::Ok) {
      return "failed " + QByteArray::number(static_cast<int>(status));
    }
    if (got == 0) {
      return hash.result().toHex();
    }
    hash.addData(QByteArrayView(buffer.constData(), got));
  }
}

//! Take apart the clipboard messages a proxy wrote and rebuild the clipboard they carry
bool clipboardFromMessages(QByteArray bytes, Clipboard &clipboard)
{
  std::string marshalled;
  bool ended = false;
  while (!bytes.isEmpty()) {
    if (!bytes.startsWith("DCLP")) {
      // a message without parameters, such as the query for the screen info
      bytes.remove(0, 4);
      continue;
    }
    // code, clipboard id (1), sequence number (4), mark (1), length (4), data
    if (bytes.size() < 14) {
      return false;
    }
    const auto mark = static_cast<uint8_t>(bytes.at(9));
    const auto length = (static_cast<uint32_t>(static_cast<uint8_t>(bytes.at(10))) << 24) |
                        (static_cast<uint32_t>(static_cast<uint8_t>(bytes.at(11))) << 16) |
                        (static_cast<uint32_t>(static_cast<uint8_t>(bytes.at(12))) << 8) |
                        static_cast<uint32_t>(static_cast<uint8_t>(bytes.at(13)));
    if (bytes.size() < 14 + static_cast<qsizetype>(length)) {
      return false;
    }
    if (mark == ChunkType::DataChunk) {
      marshalled.append(bytes.constData() + 14, length);
    } else if (mark == ChunkType::DataEnd) {
      ended = true;
    }
    bytes.remove(0, 14 + static_cast<qsizetype>(length));
  }
  if (!ended) {
    return false;
  }
  clipboard.unmarshall(marshalled, 0);
  return true;
}

} // namespace

QString FileTransferWireTests::sourcePath(const QString &relative) const
{
  return QDir(m_source->path()).filePath(relative);
}

void FileTransferWireTests::initTestCase()
{
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Info);

  m_source = std::make_unique<QTemporaryDir>();
  QVERIFY(m_source->isValid());
  QVERIFY(QDir(m_source->path()).mkpath(QStringLiteral("docs/子目录")));
  QVERIFY(writeFile(sourcePath("docs/big.bin"), randomBytes(5 * 1024 * 1024 + 321, 1)));
  QVERIFY(writeFile(sourcePath("docs/empty.txt"), {}));
  QVERIFY(writeFile(sourcePath(QStringLiteral("docs/子目录/文档.txt")), randomBytes(70000, 2)));
  QVERIFY(writeFile(sourcePath("single.dat"), randomBytes(200000, 3)));
}

void FileTransferWireTests::messages_haveTheAgreedBytes_data()
{
  QTest::addColumn<QString>("message");
  QTest::addColumn<QByteArray>("expected");

  // other builds parse these byte for byte, they can not change once released
  QTest::newRow("request") << "request" << "DFRQ" + QByteArray::fromHex("01020304 02 00000003") + "ABC";
  QTest::newRow("response") << "response" << "DFRS" + QByteArray::fromHex("00000007 01 00000002") + "xy";
  QTest::newRow("empty response") << "empty response" << "DFRS" + QByteArray::fromHex("00000007 02 00000000");
  QTest::newRow("credit") << "credit" << "DFCR" + QByteArray::fromHex("00000007 00010000");
  QTest::newRow("cancel") << "cancel" << "DFCN" + QByteArray::fromHex("00000009");
}

void FileTransferWireTests::messages_haveTheAgreedBytes()
{
  QFETCH(QString, message);
  QFETCH(QByteArray, expected);

  EventLoop loop;
  const auto send = [&message](deskflow::IFileTransferChannel &channel) {
    if (message == "request") {
      channel.sendFileRequest(0x01020304, Protocol::RequestRead, "ABC");
    } else if (message == "response") {
      channel.sendFileResponse(7, Protocol::MarkData, "xy");
    } else if (message == "empty response") {
      channel.sendFileResponse(7, Protocol::MarkEnd, {});
    } else if (message == "credit") {
      channel.sendFileCredit(7, 0x00010000);
    } else {
      channel.sendFileCancel(9);
    }
  };

  // both directions write the same bytes
  const QByteArray fromClient = loop.call([&] {
    PipeStream stream(&loop.events());
    ServerProxy proxy(noClient(), &stream, &loop.events());
    send(proxy);
    return stream.written();
  });
  QCOMPARE(fromClient, expected);

  const QByteArray fromServer = loop.call([&] {
    auto *stream = new PipeStream(&loop.events());
    ClientProxy1_6 proxy("client", stream, noServer(), &loop.events());
    const QByteArray before = stream->written();
    send(proxy);
    return stream->written().mid(before.size());
  });
  QCOMPARE(fromServer, expected);
}

void FileTransferWireTests::clientPastes_filesOfTheServer()
{
  Connection connection;
  const ClipboardFileList list =
      connection.copyOnServer({sourcePath("docs").toStdString(), sourcePath("single.dat").toStdString()});
  QCOMPARE(list.entries.size(), size_t{2});

  std::vector<ClipboardFileEntry> entries;
  QVERIFY(connection.filesOfServer->listDirectory(list.offerId, 0, {}, entries) == FileTransferStatus::Ok);
  QStringList names;
  for (const auto &entry : entries) {
    names.append(QString::fromStdString(entry.name));
  }
  names.sort();
  QCOMPARE(names, QStringList({"big.bin", "empty.txt", QStringLiteral("子目录")}));

  QCOMPARE(download(*connection.filesOfServer, list.offerId, 0, "big.bin"), hashOfFile(sourcePath("docs/big.bin")));
  QCOMPARE(download(*connection.filesOfServer, list.offerId, 0, "empty.txt"), hashOfFile(sourcePath("docs/empty.txt")));
  QCOMPARE(
      download(
          *connection.filesOfServer, list.offerId, 0,
          "\xe5\xad\x90\xe7\x9b\xae\xe5\xbd\x95/\xe6\x96\x87\xe6\xa1\xa3.txt"
      ),
      hashOfFile(sourcePath(QStringLiteral("docs/子目录/文档.txt")))
  );
  QCOMPARE(download(*connection.filesOfServer, list.offerId, 1, {}), hashOfFile(sourcePath("single.dat")));

  // a request the server refuses comes back as a failure, not as silence
  QVERIFY(connection.filesOfServer->listDirectory(list.offerId, 0, "../..", entries) == FileTransferStatus::NotFound);
  QCOMPARE(connection.serverProxy->parseFailures.load(), 0);
  QTRY_COMPARE_WITH_TIMEOUT(connection.onServer->servedRequestCount(), size_t{0}, 3000);
}

void FileTransferWireTests::serverPastes_filesOfTheClient()
{
  Connection connection;
  const ClipboardFileList list =
      connection.copyOnClient({sourcePath("docs").toStdString(), sourcePath("single.dat").toStdString()});
  QCOMPARE(list.entries.size(), size_t{2});

  std::vector<ClipboardFileEntry> entries;
  QVERIFY(connection.filesOfClient->listDirectory(list.offerId, 0, {}, entries) == FileTransferStatus::Ok);
  QCOMPARE(entries.size(), size_t{3});

  QCOMPARE(download(*connection.filesOfClient, list.offerId, 0, "big.bin"), hashOfFile(sourcePath("docs/big.bin")));
  QCOMPARE(download(*connection.filesOfClient, list.offerId, 1, {}), hashOfFile(sourcePath("single.dat")));
  QVERIFY(connection.filesOfClient->listDirectory(list.offerId, 0, "../..", entries) == FileTransferStatus::NotFound);
  QCOMPARE(connection.serverProxy->parseFailures.load(), 0);
  QTRY_COMPARE_WITH_TIMEOUT(connection.onClient->servedRequestCount(), size_t{0}, 3000);
}

void FileTransferWireTests::cancel_reachesTheOtherComputer()
{
  Connection connection;
  const ClipboardFileList list = connection.copyOnServer({sourcePath("docs").toStdString()});

  // nothing is read, so the server keeps the file open and waits for room
  auto reader = connection.filesOfServer->openFile(list.offerId, 0, "big.bin", 0);
  QTRY_COMPARE_WITH_TIMEOUT(connection.onServer->servedRequestCount(), size_t{1}, 3000);
  reader.reset();
  QTRY_COMPARE_WITH_TIMEOUT(connection.onServer->servedRequestCount(), size_t{0}, 3000);
  QVERIFY(connection.clientStream->written().contains("DFCN"));
}

void FileTransferWireTests::proxyDestroyed_readersFail()
{
  Connection connection;
  const ClipboardFileList list = connection.copyOnServer({sourcePath("docs").toStdString()});

  auto reader = connection.filesOfServer->openFile(list.offerId, 0, "big.bin", 0);
  QByteArray buffer(4096, Qt::Uninitialized);
  uint32_t got = 0;
  QVERIFY(reader->read(buffer.data(), 4096, got) == FileTransferStatus::Ok);
  QTRY_COMPARE_WITH_TIMEOUT(connection.onServer->servedRequestCount(), size_t{1}, 3000);

  // the client goes away: the server stops sending and closes the file
  connection.loop.call([&] { connection.clientProxy.reset(); });
  QCOMPARE(connection.onServer->servedRequestCount(), size_t{0});

  // the connection to the server is dropped on the client: the paste fails instead of waiting
  connection.loop.call([&] { connection.serverProxy.reset(); });
  FileTransferStatus status = FileTransferStatus::Ok;
  do {
    status = reader->read(buffer.data(), 4096, got);
  } while (status == FileTransferStatus::Ok && got > 0);
  QVERIFY(status == FileTransferStatus::Disconnected);
}

void FileTransferWireTests::client_keepsOnlyUsableFileLists()
{
  Connection connection;
  const std::string fileList =
      connection.loop.call([&] { return connection.onServer->offerLocalFiles({sourcePath("docs").toStdString()}); });
  QVERIFY(!fileList.empty());
  ClipboardFileList list;
  QVERIFY(ClipboardFileList::unmarshall(fileList, list));

  const auto fileListAfterAccepting = [&](const std::string &received) {
    return connection.loop.call([&] {
      Clipboard clipboard;
      clipboard.open(0);
      clipboard.add(IClipboard::Format::Text, "docs");
      clipboard.add(IClipboard::Format::FileList, received);
      clipboard.close();
      connection.serverProxy->acceptFileList(clipboard);
      clipboard.open(0);
      const bool textKept = clipboard.get(IClipboard::Format::Text) == "docs";
      const std::string kept = clipboard.has(IClipboard::Format::FileList) ? clipboard.get(IClipboard::Format::FileList)
                                                                           : std::string("(removed)");
      clipboard.close();
      return textKept ? kept : std::string("(text lost)");
    });
  };

  // a list that is not well formed never reaches the system clipboard
  QCOMPARE(fileListAfterAccepting("DFFL not really a list"), std::string("(removed)"));
  QCOMPARE(fileListAfterAccepting(fileList.substr(0, fileList.size() - 1)), std::string("(removed)"));
  std::vector<ClipboardFileEntry> entries;
  QVERIFY(connection.filesOfServer->listDirectory(list.offerId, 0, {}, entries) == FileTransferStatus::Disconnected);

  // a good one is kept, and from then on its files can be asked for over this connection
  QCOMPARE(fileListAfterAccepting(fileList), fileList);
  QVERIFY(connection.filesOfServer->listDirectory(list.offerId, 0, {}, entries) == FileTransferStatus::Ok);
  QCOMPARE(entries.size(), size_t{3});

  // with file sharing off, or unavailable, lists are dropped
  connection.loop.call([&] { connection.onClient->setEnabled(false); });
  QCOMPARE(fileListAfterAccepting(fileList), std::string("(removed)"));
  connection.loop.call([&] {
    connection.onClient->setEnabled(true);
    connection.serverProxy->setFileTransfer(nullptr);
  });
  QCOMPARE(fileListAfterAccepting(fileList), std::string("(removed)"));
}

void FileTransferWireTests::server_sendsOnlyItsOwnFileLists()
{
  EventLoop loop;

  ClipboardFileList foreign;
  foreign.offerId = 0x1111;
  foreign.entries.push_back({"of-another-client.txt", false, 5, 0});

  //! What a client receives when the server sends it a clipboard holding \p fileList
  const auto sentFileList = [&](bool withFileTransfer, bool ownFiles) {
    std::unique_ptr<FileTransfer> transfer;
    std::unique_ptr<ClientProxy1_6> proxy;
    PipeStream *stream = nullptr;
    std::string fileList;
    loop.call([&] {
      transfer = std::make_unique<FileTransfer>(&loop.events());
      fileList = ownFiles ? transfer->offerLocalFiles({sourcePath("single.dat").toStdString()}) : foreign.marshall();
      stream = new PipeStream(&loop.events());
      proxy = std::make_unique<ClientProxy1_6>("client", stream, noServer(), &loop.events());
      if (withFileTransfer) {
        proxy->setFileTransfer(transfer.get());
      }

      Clipboard clipboard;
      clipboard.open(0);
      clipboard.add(IClipboard::Format::Text, "the names");
      clipboard.add(IClipboard::Format::FileList, fileList);
      clipboard.close();
      proxy->setClipboardDirty(kClipboardClipboard, true);
      proxy->setClipboard(kClipboardClipboard, &clipboard);
    });
    // the clipboard is written by events queued behind the call
    loop.call([] {});
    loop.call([] {});

    std::string result;
    Clipboard received;
    if (!clipboardFromMessages(stream->written(), received)) {
      result = "(no clipboard)";
    } else {
      received.open(0);
      if (received.get(IClipboard::Format::Text) != "the names") {
        result = "(text lost)";
      } else if (!received.has(IClipboard::Format::FileList)) {
        result = "(removed)";
      } else {
        result = (received.get(IClipboard::Format::FileList) == fileList) ? "(kept)" : "(changed)";
      }
      received.close();
    }
    loop.call([&] {
      proxy.reset();
      transfer.reset();
    });
    return result;
  };

  // files copied on the server itself are announced to the client
  QCOMPARE(sentFileList(true, true), std::string("(kept)"));
  // files of another client would have to be passed on, their list goes no further
  QCOMPARE(sentFileList(true, false), std::string("(removed)"));
  // and nothing is announced that could not be asked for
  QCOMPARE(sentFileList(false, true), std::string("(removed)"));
}

QTEST_MAIN(FileTransferWireTests)
