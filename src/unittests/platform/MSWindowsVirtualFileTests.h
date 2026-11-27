/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTest>

#include <memory>

class MemoryVirtualFileSource;

class MSWindowsVirtualFileTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  // Test are run in order top to bottom
  void initTestCase();
  void cleanupTestCase();
  void fileDescriptorListsEntries();
  void fileDescriptorListsAgainForAnotherPaste();
  void fileDescriptorStaysWhilePasteUsesIt();
  void setDataKeepsOnlySmallValues();
  void fileDescriptorSkipsTooLongPaths();
  void fileContentsStreamsData();
  void streamSeekRestartsReading();
  void extraFormatsAreServed();
  void clipboardDoesNotReadUntilAsked();
  void shellCopiesVirtualFiles();

private:
  Log m_log;
  std::shared_ptr<MemoryVirtualFileSource> m_source;
  bool m_oleInitialized = false;
};
