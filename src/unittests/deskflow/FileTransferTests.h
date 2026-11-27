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

class FileTransferTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void wholeTree_arrivesUnchanged();
  void wholeTree_bothDirectionsAtOnce();
  void read_fromOffset();
  void read_smallestWindow_completes();
  void list_largeDirectory();
  void list_skipsLinks();
  void request_outsideOfTheOffer_refused();
  void request_rawPathTricks_refused();
  void request_wrongKindOfEntry_fails();
  void request_tooManyAtOnce_refused();
  void cancel_stopsTheSender();
  void disconnect_failsTheReaderAtOnce();
  void noAnswer_timesOut();
  void sourceShrinks_readerSeesTheEnd();
  void disabled_nothingIsOfferedOrAnswered();
  void answers_notAskedFor_ignored();
  void sender_ignoringTheWindow_isCutOff();
  void transferDestroyed_readersFail();
  void replacedOffer_runningPasteCompletes();

private:
  QString sourcePath(const QString &relative) const;

  Arch m_arch;
  Log m_log;
  std::unique_ptr<QTemporaryDir> m_source;
  bool m_haveLinks = false;
};
