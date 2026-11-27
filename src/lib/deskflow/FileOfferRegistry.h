/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardFileList.h"
#include "deskflow/LocalFileSystem.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace deskflow {

//! The files this computer currently offers to other computers
/*!
Remembers which local files and directories were copied to the clipboard, so
that requests from another computer can be mapped back to local paths. Local
paths never leave this class: other computers only see the names in the
ClipboardFileList and refer to a file by the identifier of the list, the
index of a top level entry and a path below it.

Not thread safe, used from the thread that runs the event queue.
*/
class FileOfferRegistry
{
public:
  enum class Resolved
  {
    Ok,           //!< The request maps to an existing local path
    UnknownOffer, //!< The list is not offered (any more)
    BadPath,      //!< The request does not name something inside the offered files
    NotFound      //!< Nothing exists at the requested path
  };

  //! How long a list that was replaced stays usable after its last request
  static constexpr int64_t s_retiredLifetimeMs = 60 * 1000;

  //! Offering the same paths again within this time does not look at the files again
  static constexpr int64_t s_reuseLookMs = 3 * 1000;

  FileOfferRegistry();

  //! Offer files
  /*!
  Makes \p paths (absolute local paths, UTF-8) the current offer and returns
  the list to tell other computers. Paths that do not exist, links and special
  files are left out. The returned list has no entries if nothing can be
  offered, also when there are more paths than a list may have. Offering the
  same unchanged files again returns the same list with the same identifier.
  */
  ClipboardFileList offer(const std::vector<std::string> &paths);

  //! Stop offering files
  /*!
  The previous offer stays usable for a short while so that a paste that is
  in progress can finish.
  */
  void withdraw();

  //! True if the identifier belongs to a list offered by this computer
  bool isOffered(uint64_t offerId);

  //! Find the local path for a request
  /*!
  \p item is the index of a top level entry of the list \p offerId and
  \p relativePath a path below it with "/" between components (empty for the
  entry itself). On success \p localPath is the local path of what exists
  there, none of the steps to it is a link. Does what locate() and walk() do.
  */
  Resolved resolve(uint64_t offerId, uint32_t item, std::string_view relativePath, QString &localPath);

  //! Check a request without looking at files
  /*!
  The first half of resolve(): gets the local path of the top level entry and
  the checked components of \p relativePath.
  */
  Resolved
  locate(uint64_t offerId, uint32_t item, std::string_view relativePath, QString &root, QStringList &components);

  //! Look at the files on the way from \p root through \p components
  /*!
  The second half of resolve(). Uses no state, so it may run on any thread,
  and may take long on a slow volume. \p entry describes what was found.
  */
  static Resolved walk(const QString &root, const QStringList &components, QString &localPath, LocalEntry &entry);

  //! Get the local paths of the top level entries of a list, in the order of the list
  /*!
  Returns false if the list is not offered by this computer.
  */
  bool roots(uint64_t offerId, QStringList &roots);

  //! Replace the clock, for tests. The function returns milliseconds.
  void setClock(std::function<int64_t()> clock);

  //! Check one component of a requested path
  static bool isValidPathComponent(std::string_view component);

private:
  struct Offer
  {
    uint64_t id = 0;
    //! Local path of each top level entry, absolute and with "/" between directories
    QStringList roots;
    int64_t lastUsedMs = 0;
  };

  //! The last paths that were offered and what came of it
  struct LastOffer
  {
    bool made = false;
    std::vector<std::string> paths;
    ClipboardFileList list;
    int64_t atMs = 0;
  };

  //! Look at the files of \p paths and make them the current offer
  ClipboardFileList look(const std::vector<std::string> &paths);
  Offer *find(uint64_t offerId);
  void expireRetired();
  uint64_t makeId(const QByteArray &description) const;

  QByteArray m_salt;
  Offer m_current;
  std::vector<Offer> m_retired;
  LastOffer m_lastOffer;
  std::function<int64_t()> m_clock;
};

} // namespace deskflow
