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

//! Files of another computer on the Windows clipboard, over a link to that computer
class MSWindowsClipboardFilesTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void windowsName_changesWhatWindowsDoesNotAllow();
  void remoteSource_listsTheWholeTree();
  void remoteSource_namesThatDifferInCase_getANumber();
  void remoteSource_readsFilesAsTheyAreNow();
  void remoteSource_goneFiles_areLeftOut();
  void remoteSource_otherComputerGone_listFails();
  void shell_pastesTheFilesOfTheOtherComputer();
  void clipboard_holdsTheFilesMarkedAsDeskflows();
  void clipboard_laterClipboardWins();
  void clipboard_sameClipboardTwice_putsFilesOnce();
  void clipboard_emptied_isDeskflows();
  void clipboard_gone_takesTheFilesOff();
  void readFiles_reportsCopiedPaths();

private:
  QString sourcePath(const QString &relative) const;

  Arch m_arch;
  Log m_log;
  std::unique_ptr<QTemporaryDir> m_source;
  bool m_oleInitialized = false;
};
