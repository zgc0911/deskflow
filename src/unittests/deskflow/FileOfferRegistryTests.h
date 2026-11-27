/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>

class FileOfferRegistryTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void offer_listsTopLevelEntries();
  void offer_skipsMissingLinksAndDuplicates();
  void offer_nothingUsable_offersNothing();
  void offer_tooManyEntries_offersNothing();
  void offer_tooManyPaths_notLookedAt();
  void offer_sameFiles_sameIdentifier();
  void offer_differentRegistries_differentIdentifiers();
  void resolve_findsFilesAndDirectories();
  void resolve_badPaths_data();
  void resolve_badPaths();
  void resolve_linkLeadingOutside_rejected();
  void resolve_linkInside_rejected();
  void resolve_junction_rejected();
  void offer_shortcut_isAFile();
  void resolve_missingFile_notFound();
  void resolve_unknownOffer();
  void replacedOffer_expiresWhenUnused();
  void withdrawnOffer_expiresWhenUnused();

private:
  QString path(const QString &relative) const;
  std::string stdPath(const QString &relative) const;

  Log m_log;
  std::unique_ptr<QTemporaryDir> m_dir;
  bool m_haveLinks = false;
};
