/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerConfigDialogTests.h"

#include "common/Settings.h"
#include "gui/config/ServerConfig.h"
#include "gui/dialogs/ServerConfigDialog.h"
#include "gui/widgets/SettingsDialogButtonBox.h"

#include <QCheckBox>
#include <QDir>
#include <QTabWidget>

namespace {

QCheckBox *checkBox(const ServerConfigDialog &dialog, const QString &name)
{
  return dialog.findChild<QCheckBox *>(name);
}

} // namespace

void ServerConfigDialogTests::initTestCase()
{
  QVERIFY(m_settingsDir.isValid());
  Settings::setSettingsFile(m_settingsDir.filePath("Deskflow.conf"));
}

void ServerConfigDialogTests::fileSharing_onlyWhereTheClipboardIsShared()
{
  ServerConfig config;
  ServerConfigDialog dialog(nullptr, config);
  QCheckBox *clipboard = checkBox(dialog, "cbEnableClipboard");
  QCheckBox *files = checkBox(dialog, "cbEnableFileSharing");
  QVERIFY(clipboard != nullptr);
  QVERIFY(files != nullptr);

  // on unless turned off, and only to be had with the clipboard it goes through
  QVERIFY(files->isChecked());
  QVERIFY(files->isEnabled());
  clipboard->setChecked(false);
  QVERIFY(!files->isEnabled());
  clipboard->setChecked(true);
  QVERIFY(files->isEnabled());

  // a picture of where it is, for a person to look at
  const QString pictures = qEnvironmentVariable("DESKFLOW_TEST_SCREENSHOT_DIR");
  if (!pictures.isEmpty()) {
    auto *tabs = dialog.findChild<QTabWidget *>("tabWidget");
    for (int index = 0; tabs != nullptr && index < tabs->count(); ++index) {
      if (tabs->widget(index)->isAncestorOf(files)) {
        tabs->setCurrentIndex(index);
      }
    }
    dialog.show();
    QTest::qWait(200);
    QVERIFY(dialog.grab().save(QDir(pictures).filePath("server-settings.png")));
    clipboard->setChecked(false);
    QTest::qWait(100);
    QVERIFY(dialog.grab().save(QDir(pictures).filePath("server-settings-clipboard-off.png")));
    clipboard->setChecked(true);
  }
}

void ServerConfigDialogTests::fileSharing_isSaved()
{
  ServerConfig config;
  {
    ServerConfigDialog dialog(nullptr, config);
    checkBox(dialog, "cbEnableFileSharing")->setChecked(false);
    auto *buttons = dialog.findChild<SettingsDialogButtonBox *>();
    QVERIFY(buttons != nullptr);
    Q_EMIT buttons->accepted();
  }
  QVERIFY(!Settings::value(Settings::Server::EnableFileSharing).toBool());

  // shown as it was saved
  {
    ServerConfigDialog dialog(nullptr, config);
    QVERIFY(!checkBox(dialog, "cbEnableFileSharing")->isChecked());
  }
  Settings::setValue(Settings::Server::EnableFileSharing);
  QVERIFY(Settings::value(Settings::Server::EnableFileSharing).toBool());
}

QTEST_MAIN(ServerConfigDialogTests)
