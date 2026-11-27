/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "arch/Arch.h"

#include <QTemporaryDir>
#include <QTest>

class ServerConfigTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void equalityCheck();
  void equalityCheck_diff_options();
  void equalityCheck_diff_alias();
  void equalityCheck_diff_filters();
  //  void equalityCheck_diff_address();
  void equalityCheck_diff_neighbours1();
  void equalityCheck_diff_neighbours2();
  void equalityCheck_diff_neighbours3();
  void options_fileSharing_followsTheSettings_data();
  void options_fileSharing_followsTheSettings();

private:
  //! The options taken from the settings are read from a file of the tests' own
  QTemporaryDir m_settingsDir;
  //! Reading the options resolves the address the server listens on
  Arch m_arch;
};
