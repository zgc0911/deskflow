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

class FileTransferWireTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void messages_haveTheAgreedBytes_data();
  void messages_haveTheAgreedBytes();
  void clientPastes_filesOfTheServer();
  void serverPastes_filesOfTheClient();
  void cancel_reachesTheOtherComputer();
  void proxyDestroyed_readersFail();
  void client_keepsOnlyUsableFileLists();
  void server_sendsOnlyItsOwnFileLists();

private:
  QString sourcePath(const QString &relative) const;

  Arch m_arch;
  Log m_log;
  std::unique_ptr<QTemporaryDir> m_source;
};
