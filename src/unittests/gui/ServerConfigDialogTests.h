/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTemporaryDir>
#include <QTest>

//! The switch for sharing files through the clipboard in the server settings
class ServerConfigDialogTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void fileSharing_onlyWhereTheClipboardIsShared();
  void fileSharing_isSaved();

private:
  //! The dialog reads and writes the settings, those of the tests' own
  QTemporaryDir m_settingsDir;
};
