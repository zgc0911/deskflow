/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace deskflow {

//! One file or directory in a list of copied files
struct ClipboardFileEntry
{
  //! Name without any directory part, UTF-8
  std::string name;
  bool isDirectory = false;
  //! Size in bytes, zero for directories
  uint64_t size = 0;
  //! Seconds since the Unix epoch, zero when unknown
  int64_t modifiedTime = 0;
  //! Was copied but is gone now. Only in answers that tell what became of the copied entries, never in a list.
  bool isMissing = false;

  bool operator==(const ClipboardFileEntry &) const = default;
};

//! The files on a clipboard, as told to other computers
/*!
Holds what was copied, not the contents: the top level files and directories
and an identifier that the other computer quotes when it asks for more. This
is the data of the clipboard format IClipboard::Format::FileList.
*/
struct ClipboardFileList
{
  //! Most top level entries a list may hold
  static constexpr size_t s_maxEntries = 4096;
  //! Longest name of an entry, in bytes
  static constexpr size_t s_maxNameLength = 1024;

  //! Identifies the list on the computer that offers the files
  uint64_t offerId = 0;
  std::vector<ClipboardFileEntry> entries;

  bool operator==(const ClipboardFileList &) const = default;

  //! Encode the list as clipboard data
  std::string marshall() const;

  //! Decode clipboard data
  /*!
  Returns false, leaving \p list empty, if \p data is not a well formed list.
  Data from another computer is not trusted, every length and name is checked.
  */
  static bool unmarshall(std::string_view data, ClipboardFileList &list);

  //! Check a name of a file or directory received from another computer
  /*!
  A name is a single path component: not empty, not too long, not "." or
  "..", and free of path separators and NUL characters.
  */
  static bool isValidName(std::string_view name);

  //! Append the encoding of one entry to \p data
  static void appendEntry(std::string &data, const ClipboardFileEntry &entry);

  //! Decode entries until \p data is used up
  /*!
  Returns false if the data is malformed. Entries with names that are not
  valid are a reason to reject the data as a whole.
  */
  static bool readEntries(std::string_view data, std::vector<ClipboardFileEntry> &entries);
};

//! Helpers for the big-endian encoding used by the file transfer messages
namespace wire {

void putU8(std::string &data, uint8_t value);
void putU16(std::string &data, uint16_t value);
void putU32(std::string &data, uint32_t value);
void putU64(std::string &data, uint64_t value);

//! Each getter consumes its bytes from the front of \p data and returns false if there are too few
bool getU8(std::string_view &data, uint8_t &value);
bool getU16(std::string_view &data, uint16_t &value);
bool getU32(std::string_view &data, uint32_t &value);
bool getU64(std::string_view &data, uint64_t &value);
bool getBytes(std::string_view &data, size_t size, std::string_view &value);

} // namespace wire

} // namespace deskflow
