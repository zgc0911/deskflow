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

//! Mounts the volume for real, with the NFS client of the system
class OSXRemoteFileVolumeTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void copiedTree_copiesBackUnchanged();
  void manyReadersAtOnce_getTheRightBytes();
  void nothingIsFetched_untilAFileIsRead();
  void fileChangedAfterCopy_isCopiedAsItIsNow();
  void slowSource_copyCompletes();
  void sourceGone_copyFails_volumeStays();
  void otherClients_areTurnedAway();
  void unmount_leavesNothingBehind();
  void leftoversOfDeadProcesses_areRemoved();
  void mountCommandSlowOrFailing_neverHoldsUpAddingFiles();
  void volumeLost_isMountedAgain();
  void contains_tellsWhatIsOnAVolume();

private:
  QString sourcePath(const QString &relative) const;

  Arch m_arch;
  Log m_log;
  std::unique_ptr<QTemporaryDir> m_source;
};
