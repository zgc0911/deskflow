/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClipboardFileListTests.h"

#include "deskflow/Clipboard.h"
#include "deskflow/ClipboardFileList.h"

using deskflow::ClipboardFileEntry;
using deskflow::ClipboardFileList;

namespace {

ClipboardFileList sampleList()
{
  ClipboardFileList list;
  list.offerId = 0x0123456789abcdefULL;
  list.entries.push_back({"report.pdf", false, 5'000'000'000ULL, 1767323046});
  list.entries.push_back({"照片 2026", true, 0, 1700000000});
  list.entries.push_back({"empty", false, 0, 0});
  return list;
}

} // namespace

void ClipboardFileListTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Debug);
}

void ClipboardFileListTests::marshall_roundTrip()
{
  const ClipboardFileList list = sampleList();
  const std::string data = list.marshall();

  ClipboardFileList decoded;
  QVERIFY(ClipboardFileList::unmarshall(data, decoded));
  QVERIFY(decoded == list);
  // the encoding is stable, the same list always gives the same bytes
  QCOMPARE(decoded.marshall(), data);
}

void ClipboardFileListTests::marshall_directoriesCarryNoSize()
{
  ClipboardFileList list;
  list.offerId = 1;
  list.entries.push_back({"folder", true, 123, 0});

  ClipboardFileList decoded;
  QVERIFY(ClipboardFileList::unmarshall(list.marshall(), decoded));
  QVERIFY(decoded.entries.at(0).isDirectory);
  QCOMPARE(decoded.entries.at(0).size, uint64_t{0});
}

void ClipboardFileListTests::unmarshall_truncatedData_rejected()
{
  const std::string data = sampleList().marshall();

  // every shorter prefix must be refused without reading past the end
  for (size_t length = 0; length < data.size(); ++length) {
    ClipboardFileList decoded;
    decoded.offerId = 99;
    QVERIFY2(
        !ClipboardFileList::unmarshall(std::string_view(data).substr(0, length), decoded),
        qPrintable(QStringLiteral("accepted a list cut to %1 bytes").arg(length))
    );
    QCOMPARE(decoded.offerId, uint64_t{0});
    QVERIFY(decoded.entries.empty());
  }

  // extra bytes after the last entry are not an entry
  ClipboardFileList decoded;
  QVERIFY(!ClipboardFileList::unmarshall(data + "x", decoded));
}

void ClipboardFileListTests::unmarshall_wrongHeader_rejected()
{
  const std::string data = sampleList().marshall();
  ClipboardFileList decoded;

  std::string wrongMagic = data;
  wrongMagic[0] = 'X';
  QVERIFY(!ClipboardFileList::unmarshall(wrongMagic, decoded));

  std::string wrongVersion = data;
  wrongVersion[4] = 2;
  QVERIFY(!ClipboardFileList::unmarshall(wrongVersion, decoded));

  QVERIFY(!ClipboardFileList::unmarshall(std::string(), decoded));
  QVERIFY(!ClipboardFileList::unmarshall("plain text that happens to be on the clipboard", decoded));
}

void ClipboardFileListTests::unmarshall_wrongCount_rejected()
{
  ClipboardFileList decoded;

  // the count sits after the magic (4), the version (1) and the identifier (8)
  std::string fewer = sampleList().marshall();
  fewer[16] = 2;
  QVERIFY(!ClipboardFileList::unmarshall(fewer, decoded));

  std::string more = sampleList().marshall();
  more[16] = 4;
  QVERIFY(!ClipboardFileList::unmarshall(more, decoded));

  ClipboardFileList empty;
  empty.offerId = 7;
  QVERIFY(!ClipboardFileList::unmarshall(empty.marshall(), decoded));

  ClipboardFileList tooMany;
  tooMany.offerId = 7;
  for (size_t i = 0; i <= ClipboardFileList::s_maxEntries; ++i) {
    tooMany.entries.push_back({"f" + std::to_string(i), false, 1, 1});
  }
  QVERIFY(!ClipboardFileList::unmarshall(tooMany.marshall(), decoded));

  tooMany.entries.pop_back();
  QVERIFY(ClipboardFileList::unmarshall(tooMany.marshall(), decoded));
  QCOMPARE(decoded.entries.size(), ClipboardFileList::s_maxEntries);
}

void ClipboardFileListTests::unmarshall_invalidName_rejected()
{
  for (const std::string &name : {std::string(".."), std::string("a/b"), std::string("nul\0byte", 8), std::string()}) {
    ClipboardFileList list;
    list.offerId = 5;
    list.entries.push_back({"good.txt", false, 1, 1});
    list.entries.push_back({name, false, 1, 1});

    ClipboardFileList decoded;
    QVERIFY2(!ClipboardFileList::unmarshall(list.marshall(), decoded), name.c_str());
    QVERIFY(decoded.entries.empty());
  }
}

void ClipboardFileListTests::isValidName_data()
{
  QTest::addColumn<QByteArray>("name");
  QTest::addColumn<bool>("valid");

  QTest::newRow("plain") << QByteArray("notes.txt") << true;
  QTest::newRow("unicode") << QByteArray("\xe6\x96\x87\xe4\xbb\xb6.txt") << true;
  QTest::newRow("spaces") << QByteArray(" leading and trailing ") << true;
  QTest::newRow("hidden") << QByteArray(".hidden") << true;
  QTest::newRow("three dots") << QByteArray("...") << true;
  // legal in a name on macOS and Linux, the Windows side has to rename
  QTest::newRow("backslash") << QByteArray("a\\b") << true;
  QTest::newRow("colon") << QByteArray("a:b") << true;
  QTest::newRow("longest") << QByteArray(1024, 'x') << true;

  QTest::newRow("empty") << QByteArray() << false;
  QTest::newRow("dot") << QByteArray(".") << false;
  QTest::newRow("dot dot") << QByteArray("..") << false;
  QTest::newRow("slash") << QByteArray("a/b") << false;
  QTest::newRow("leading slash") << QByteArray("/etc") << false;
  QTest::newRow("nul") << QByteArray("a\0b", 3) << false;
  QTest::newRow("too long") << QByteArray(1025, 'x') << false;
}

void ClipboardFileListTests::isValidName()
{
  QFETCH(QByteArray, name);
  QFETCH(bool, valid);
  QCOMPARE(ClipboardFileList::isValidName(std::string_view(name.constData(), static_cast<size_t>(name.size()))), valid);
}

void ClipboardFileListTests::wire_roundTrip()
{
  using namespace deskflow::wire;

  std::string data;
  putU8(data, 0xab);
  putU16(data, 0xbeef);
  putU32(data, 0xdeadbeef);
  putU64(data, 0x0102030405060708ULL);
  QCOMPARE(data.size(), size_t{15});
  // big-endian on the wire
  QCOMPARE(static_cast<uint8_t>(data[1]), uint8_t{0xbe});
  QCOMPARE(static_cast<uint8_t>(data[7]), uint8_t{0x01});

  std::string_view view = data;
  uint8_t u8 = 0;
  uint16_t u16 = 0;
  uint32_t u32 = 0;
  uint64_t u64 = 0;
  QVERIFY(getU8(view, u8) && getU16(view, u16) && getU32(view, u32) && getU64(view, u64));
  QCOMPARE(u8, uint8_t{0xab});
  QCOMPARE(u16, uint16_t{0xbeef});
  QCOMPARE(u32, uint32_t{0xdeadbeef});
  QCOMPARE(u64, uint64_t{0x0102030405060708ULL});
  QVERIFY(view.empty());

  // nothing is consumed when there is too little
  std::string_view few = std::string_view(data).substr(0, 3);
  QVERIFY(!getU32(few, u32));
  QCOMPARE(few.size(), size_t{3});
  std::string_view bytes;
  QVERIFY(!getBytes(few, 4, bytes));
  QVERIFY(getBytes(few, 3, bytes));
  QCOMPARE(bytes.size(), size_t{3});
}

void ClipboardFileListTests::clipboard_keepsFileList()
{
  const std::string fileList = sampleList().marshall();

  Clipboard source;
  QVERIFY(source.open(0));
  source.add(IClipboard::Format::Text, "report.pdf");
  source.add(IClipboard::Format::FileList, fileList);
  source.close();

  // through the same encoding the clipboard takes over the network
  Clipboard target;
  target.unmarshall(source.marshall(), 0);
  QVERIFY(target.open(0));
  QVERIFY(target.has(IClipboard::Format::Text));
  QVERIFY(target.has(IClipboard::Format::FileList));
  QCOMPARE(target.get(IClipboard::Format::FileList), fileList);
  target.close();

  // and through a copy between clipboard objects
  Clipboard copied;
  QVERIFY(Clipboard::copy(&copied, &source));
  QVERIFY(copied.open(0));
  QCOMPARE(copied.get(IClipboard::Format::FileList), fileList);
  copied.close();
}

void ClipboardFileListTests::clipboard_removeFormat()
{
  Clipboard clipboard;
  QVERIFY(clipboard.open(0));
  clipboard.add(IClipboard::Format::Text, "kept");
  clipboard.add(IClipboard::Format::FileList, sampleList().marshall());
  clipboard.remove(IClipboard::Format::FileList);
  QVERIFY(!clipboard.has(IClipboard::Format::FileList));
  QVERIFY(clipboard.get(IClipboard::Format::FileList).empty());
  QVERIFY(clipboard.has(IClipboard::Format::Text));
  clipboard.close();

  // a removed format is not sent either
  Clipboard received;
  received.unmarshall(clipboard.marshall(), 0);
  QVERIFY(received.open(0));
  QVERIFY(!received.has(IClipboard::Format::FileList));
  QCOMPARE(received.get(IClipboard::Format::Text), std::string("kept"));
  received.close();
}

QTEST_MAIN(ClipboardFileListTests)
