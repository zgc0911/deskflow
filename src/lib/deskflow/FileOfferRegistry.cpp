/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/FileOfferRegistry.h"

#include "base/Log.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSet>

#include <algorithm>

namespace deskflow {

namespace {

//! Symbolic links are never offered, following them could lead outside of what was copied
bool isLink(const QFileInfo &info)
{
  return info.isSymbolicLink() || info.isJunction();
}

int64_t monotonicMs()
{
  static const QElapsedTimer s_timer = [] {
    QElapsedTimer timer;
    timer.start();
    return timer;
  }();
  return s_timer.elapsed();
}

} // namespace

FileOfferRegistry::FileOfferRegistry() : m_clock(&monotonicMs)
{
  // makes identifiers unpredictable and different on every run
  m_salt.resize(16);
  QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(m_salt.data()), m_salt.size() / 4);
}

void FileOfferRegistry::setClock(std::function<int64_t()> clock)
{
  m_clock = std::move(clock);
}

uint64_t FileOfferRegistry::makeId(const QByteArray &description) const
{
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(m_salt);
  hash.addData(description);
  const QByteArray digest = hash.result();

  uint64_t id = 0;
  for (int i = 0; i < 8; ++i) {
    id = (id << 8) | static_cast<uint8_t>(digest.at(i));
  }
  // zero means "no offer"
  return (id == 0) ? 1 : id;
}

ClipboardFileList FileOfferRegistry::offer(const std::vector<std::string> &paths)
{
  ClipboardFileList list;
  QStringList roots;
  QSet<QString> names;
  QByteArray description;

  for (const auto &path : paths) {
    const QFileInfo info(QString::fromStdString(path));
    if (isLink(info)) {
      LOG_DEBUG("not offering a link: %s", path.c_str());
      continue;
    }
    if (!info.exists() || (!info.isFile() && !info.isDir())) {
      LOG_DEBUG("not offering, no such file or directory: %s", path.c_str());
      continue;
    }

    const QString canonical = info.canonicalFilePath();
    const QString name = QFileInfo(canonical).fileName();
    const std::string nameUtf8 = name.toStdString();
    if (canonical.isEmpty() || !ClipboardFileList::isValidName(nameUtf8)) {
      LOG_DEBUG("not offering, unusable name: %s", path.c_str());
      continue;
    }
    if (names.contains(name)) {
      LOG_DEBUG("not offering, another copied file has the same name: %s", path.c_str());
      continue;
    }
    names.insert(name);

    ClipboardFileEntry entry;
    entry.name = nameUtf8;
    entry.isDirectory = info.isDir();
    entry.size = entry.isDirectory ? 0 : static_cast<uint64_t>(info.size());
    entry.modifiedTime = info.lastModified().toSecsSinceEpoch();

    description.append(canonical.toUtf8());
    description.append('\0');
    description.append(entry.isDirectory ? 'd' : 'f');
    description.append(QByteArray::number(static_cast<qulonglong>(entry.size)));
    description.append(':');
    description.append(QByteArray::number(static_cast<qlonglong>(entry.modifiedTime)));
    description.append('\0');

    list.entries.push_back(std::move(entry));
    roots.append(canonical);
  }

  if (list.entries.size() > ClipboardFileList::s_maxEntries) {
    LOG_WARN(
        "not sharing copied files, there are too many: %d (limit %d)", static_cast<int>(list.entries.size()),
        static_cast<int>(ClipboardFileList::s_maxEntries)
    );
    list.entries.clear();
    roots.clear();
  }

  if (list.entries.empty()) {
    withdraw();
    return list;
  }

  list.offerId = makeId(description);
  if (m_current.id != list.offerId) {
    withdraw();
    m_current.id = list.offerId;
    m_current.roots = roots;
  }
  m_current.lastUsedMs = m_clock();
  return list;
}

void FileOfferRegistry::withdraw()
{
  if (m_current.id != 0) {
    m_current.lastUsedMs = m_clock();
    m_retired.push_back(m_current);
    m_current = {};
  }
  expireRetired();
}

void FileOfferRegistry::expireRetired()
{
  const int64_t now = m_clock();
  std::erase_if(m_retired, [now](const Offer &offer) { return now - offer.lastUsedMs > s_retiredLifetimeMs; });
}

FileOfferRegistry::Offer *FileOfferRegistry::find(uint64_t offerId)
{
  if (offerId == 0) {
    return nullptr;
  }
  if (m_current.id == offerId) {
    return &m_current;
  }

  expireRetired();
  const auto found =
      std::find_if(m_retired.begin(), m_retired.end(), [offerId](const Offer &offer) { return offer.id == offerId; });
  return (found == m_retired.end()) ? nullptr : &*found;
}

bool FileOfferRegistry::isOffered(uint64_t offerId)
{
  return find(offerId) != nullptr;
}

bool FileOfferRegistry::roots(uint64_t offerId, QStringList &roots)
{
  roots.clear();
  Offer *offer = find(offerId);
  if (offer == nullptr) {
    return false;
  }
  offer->lastUsedMs = m_clock();
  roots = offer->roots;
  return true;
}

bool FileOfferRegistry::isValidPathComponent(std::string_view component)
{
  if (!ClipboardFileList::isValidName(component)) {
    return false;
  }
#if defined(Q_OS_WIN)
  // a backslash separates directories and a colon names a drive or a stream
  if (component.find_first_of("\\:") != std::string_view::npos) {
    return false;
  }
#endif
  return true;
}

FileOfferRegistry::Resolved
FileOfferRegistry::resolve(uint64_t offerId, uint32_t item, std::string_view relativePath, QString &localPath)
{
  localPath.clear();

  Offer *offer = find(offerId);
  if (offer == nullptr) {
    return Resolved::UnknownOffer;
  }
  offer->lastUsedMs = m_clock();

  if (item >= static_cast<uint32_t>(offer->roots.size())) {
    return Resolved::BadPath;
  }
  const QString root = offer->roots.at(static_cast<qsizetype>(item));

  QString path = root;
  while (!relativePath.empty()) {
    const auto separator = relativePath.find('/');
    const std::string_view component = relativePath.substr(0, separator);
    if (!isValidPathComponent(component)) {
      return Resolved::BadPath;
    }
    path.append(QLatin1Char('/'));
    path.append(QString::fromUtf8(component.data(), static_cast<qsizetype>(component.size())));
    if (separator == std::string_view::npos) {
      break;
    }
    relativePath.remove_prefix(separator + 1);
    if (relativePath.empty()) {
      // a trailing separator leaves an empty component
      return Resolved::BadPath;
    }
  }

  const QFileInfo info(path);
  if (!info.exists()) {
    return Resolved::NotFound;
  }

  // the canonical path has every link resolved, so this also catches a link that leads elsewhere
  const QString canonical = info.canonicalFilePath();
  if (canonical.isEmpty()) {
    return Resolved::NotFound;
  }
  if (canonical != root && !canonical.startsWith(root + QLatin1Char('/'))) {
    LOG_WARN("refusing a file request that leads outside of the copied files");
    return Resolved::BadPath;
  }

  localPath = canonical;
  return Resolved::Ok;
}

} // namespace deskflow
