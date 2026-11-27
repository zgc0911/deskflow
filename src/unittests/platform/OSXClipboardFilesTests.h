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

//! The files part of the clipboard, on a pasteboard of its own and with a mounted volume
class OSXClipboardFilesTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void put_showsTheFilesAndWritesTheirPaths();
  void put_sameFilesAgain_writesNothing();
  void put_otherFiles_replaceThem();
  void put_clipboardOfAnotherComputer_takesItsFiles();
  void put_volumeNotMounted_changesNothing();
  void put_namesThatCanNotBeShown_areLeftOut();
  void put_doesNotWaitForTheVolume();
  void get_filesCopiedLikeFinderDoes_areFound();
  void get_filesOfOtherComputers_areLeftOut();
  void holdsOwnWrite_untilSomethingElseIsCopied();
  void generalPasteboard_ownWriteIsNotACopy();
  void manual_privacyProbe();
  void manual_finderPasteAndCopy();

private:
  QString sourcePath(const QString &relative) const;

  Arch m_arch;
  Log m_log;
  std::unique_ptr<QTemporaryDir> m_source;
};
