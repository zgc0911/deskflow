/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTest>

class ClipboardFileListTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void marshall_roundTrip();
  void marshall_directoriesCarryNoSize();
  void unmarshall_truncatedData_rejected();
  void unmarshall_wrongHeader_rejected();
  void unmarshall_wrongCount_rejected();
  void unmarshall_invalidName_rejected();
  void isValidName_data();
  void isValidName();
  void wire_roundTrip();
  void clipboard_keepsFileList();
  void clipboard_removeFormat();

private:
  Log m_log;
};
