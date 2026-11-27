/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "arch/Arch.h"
#include "base/Log.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>

class OSXRemoteFileSystemTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void nothingIsAsked_untilSomethingIsLookedAt();
  void file_readPieceByPiece_matches();
  void file_readByManyAtOnce_matches();
  void file_readFromTheMiddle_fetchesFromThere();
  void file_readBackwards_matches();
  void slowSource_readGetsWhatHasArrived();
  void nothingArrives_callComesBackLater_thenFails();
  void sourceStopsAnswering_readFails();
  void connectionLost_readFails();
  void fileGrewAfterCopy_isSeenInANewSession();
  void fileShrankAfterListing_endIsFound();
  void fileDeletedAfterCopy_isGoneInANewSession();
  void directoryChangedAfterCopy_isSeenInANewSession();
  void names_thatCanNotBeUsed_areLeftOut();
  void olderLists_areDropped();
  void bigDirectory_isReadInPages();
  void shutdown_wakesWhoeverWaits();
  void interrupt_failsWhoWaits_laterCallsWork();
  void download_nobodyReads_isGivenUp();

private:
  QString sourcePath(const QString &relative) const;

  Arch m_arch;
  Log m_log;
  std::unique_ptr<QTemporaryDir> m_source;
};
