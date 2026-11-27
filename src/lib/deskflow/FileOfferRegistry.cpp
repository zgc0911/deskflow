/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/FileOfferRegistry.h"

#include "base/Log.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSet>

#include <algorithm>

namespace deskflow {

namespace {

#if defined(Q_OS_WIN)
//! A name Windows takes for a device, whatever the case and the extension: "nul.txt" is the null device
bool isDeviceName(const QString &name)
{
  QString base = name.section(QLatin1Char('.'), 0, 0).toCaseFolded();
  while (base.endsWith(QLatin1Char(' '))) {
    base.chop(1);
  }
  static const QStringList s_devices = {QStringLiteral("con"), QStringLiteral("prn"),    QStringLiteral("aux"),
                                        QStringLiteral("nul"), QStringLiteral("conin$"), QStringLiteral("conout$")};
  if (s_devices.contains(base)) {
    return true;
  }
  // the ports take a digit, superscript ones too
  if (base.size() == 4 && (base.startsWith(QLatin1String("com")) || base.startsWith(QLatin1String("lpt")))) {
    const char16_t number = base.at(3).unicode();
    return (number >= u'0' && number <= u'9') || number == u'\u00b9' || number == u'\u00b2' || number == u'\u00b3';
  }
  return false;
}
#endif

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
  // asked for both clipboards at once, and each time the cursor leaves this computer: the files
  // are looked at again only once a moment has passed
  const int64_t now = m_clock();
  if (m_lastOffer.made && paths == m_lastOffer.paths && now - m_lastOffer.atMs < s_reuseLookMs &&
      m_current.id == m_lastOffer.list.offerId) {
    if (m_current.id != 0) {
      m_current.lastUsedMs = now;
    }
    return m_lastOffer.list;
  }

  const ClipboardFileList list = look(paths);
  m_lastOffer = {true, paths, list, now};
  return list;
}

ClipboardFileList FileOfferRegistry::look(const std::vector<std::string> &paths)
{
  ClipboardFileList list;
  if (paths.size() > ClipboardFileList::s_maxEntries) {
    // too many to share, not even looked at
    LOG_WARN(
        "not sharing copied files, there are too many: %d (limit %d)", static_cast<int>(paths.size()),
        static_cast<int>(ClipboardFileList::s_maxEntries)
    );
    withdraw();
    return list;
  }

  QStringList roots;
  QSet<QString> names;
  QByteArray description;
  for (const auto &path : paths) {
    const QString local = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromStdString(path)));
    if (!QDir::isAbsolutePath(local)) {
      LOG_DEBUG("not offering, not an absolute path: %s", path.c_str());
      continue;
    }
    // links are never offered, following them could lead anywhere
    const LocalEntry found = LocalFileSystem::inspect(local);
    if (found.type == LocalEntry::Type::Link) {
      LOG_DEBUG("not offering a link: %s", path.c_str());
      continue;
    }
    if (found.type != LocalEntry::Type::File && found.type != LocalEntry::Type::Directory) {
      LOG_DEBUG("not offering, no such file or directory: %s", path.c_str());
      continue;
    }

    const QString name = QFileInfo(local).fileName();
    const std::string nameUtf8 = name.toStdString();
    if (!ClipboardFileList::isValidName(nameUtf8)) {
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
    entry.isDirectory = found.type == LocalEntry::Type::Directory;
    entry.size = entry.isDirectory ? 0 : found.size;
    entry.modifiedTime = found.modifiedTime;

    description.append(local.toUtf8());
    description.append('\0');
    description.append(entry.isDirectory ? 'd' : 'f');
    description.append(QByteArray::number(static_cast<qulonglong>(entry.size)));
    description.append(':');
    description.append(QByteArray::number(static_cast<qlonglong>(entry.modifiedTime)));
    description.append('\0');

    list.entries.push_back(std::move(entry));
    roots.append(local);
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
  // names Windows does not allow: a backslash separates directories, a colon names a drive or a
  // stream, the others are wildcards. Windows drops a dot or a space at the end of a name.
  for (const char character : component) {
    if (static_cast<unsigned char>(character) < 0x20 ||
        std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos) {
      return false;
    }
  }
  if (component.back() == '.' || component.back() == ' ') {
    return false;
  }
  if (isDeviceName(QString::fromUtf8(component.data(), static_cast<qsizetype>(component.size())))) {
    return false;
  }
#endif
  return true;
}

FileOfferRegistry::Resolved FileOfferRegistry::locate(
    uint64_t offerId, uint32_t item, std::string_view relativePath, QString &root, QStringList &components
)
{
  root.clear();
  components.clear();

  Offer *offer = find(offerId);
  if (offer == nullptr) {
    return Resolved::UnknownOffer;
  }
  offer->lastUsedMs = m_clock();

  if (item >= static_cast<uint32_t>(offer->roots.size())) {
    return Resolved::BadPath;
  }

  QStringList found;
  while (!relativePath.empty()) {
    const auto separator = relativePath.find('/');
    const std::string_view component = relativePath.substr(0, separator);
    if (!isValidPathComponent(component)) {
      return Resolved::BadPath;
    }
    found.append(QString::fromUtf8(component.data(), static_cast<qsizetype>(component.size())));
    if (separator == std::string_view::npos) {
      break;
    }
    relativePath.remove_prefix(separator + 1);
    if (relativePath.empty()) {
      // a trailing separator leaves an empty component
      return Resolved::BadPath;
    }
  }

  root = offer->roots.at(static_cast<qsizetype>(item));
  components = found;
  return Resolved::Ok;
}

FileOfferRegistry::Resolved
FileOfferRegistry::walk(const QString &root, const QStringList &components, QString &localPath, LocalEntry &entry)
{
  localPath.clear();
  entry = {};

  // every step is looked at: one that is a link could lead anywhere, inside the copied files or not
  QString path = root;
  LocalEntry found = LocalFileSystem::inspect(path);
  for (const QString &component : components) {
    if (found.type == LocalEntry::Type::Link) {
      LOG_WARN("refusing a file request that goes through a link");
      return Resolved::BadPath;
    }
    if (found.type != LocalEntry::Type::Directory) {
      return Resolved::NotFound;
    }
    path.append(QLatin1Char('/'));
    path.append(component);
    found = LocalFileSystem::inspect(path);
  }

  if (found.type == LocalEntry::Type::Link) {
    LOG_WARN("refusing a file request for a link");
    return Resolved::BadPath;
  }
  if (found.type != LocalEntry::Type::File && found.type != LocalEntry::Type::Directory) {
    return Resolved::NotFound;
  }
  localPath = path;
  entry = found;
  return Resolved::Ok;
}

FileOfferRegistry::Resolved
FileOfferRegistry::resolve(uint64_t offerId, uint32_t item, std::string_view relativePath, QString &localPath)
{
  localPath.clear();
  QString root;
  QStringList components;
  const Resolved located = locate(offerId, item, relativePath, root, components);
  if (located != Resolved::Ok) {
    return located;
  }
  LocalEntry entry;
  return walk(root, components, localPath, entry);
}

} // namespace deskflow
