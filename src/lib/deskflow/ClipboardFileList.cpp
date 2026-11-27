/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardFileList.h"

namespace deskflow {

namespace {

const std::string_view kMagic = "DFFL";
const uint8_t kVersion = 1;
const uint8_t kFlagDirectory = 0x01;

} // namespace

//
// wire
//

void wire::putU8(std::string &data, uint8_t value)
{
  data.push_back(static_cast<char>(value));
}

void wire::putU16(std::string &data, uint16_t value)
{
  putU8(data, static_cast<uint8_t>(value >> 8));
  putU8(data, static_cast<uint8_t>(value & 0xff));
}

void wire::putU32(std::string &data, uint32_t value)
{
  putU16(data, static_cast<uint16_t>(value >> 16));
  putU16(data, static_cast<uint16_t>(value & 0xffff));
}

void wire::putU64(std::string &data, uint64_t value)
{
  putU32(data, static_cast<uint32_t>(value >> 32));
  putU32(data, static_cast<uint32_t>(value & 0xffffffffULL));
}

bool wire::getU8(std::string_view &data, uint8_t &value)
{
  if (data.empty()) {
    return false;
  }
  value = static_cast<uint8_t>(data.front());
  data.remove_prefix(1);
  return true;
}

bool wire::getU16(std::string_view &data, uint16_t &value)
{
  uint8_t high = 0;
  uint8_t low = 0;
  if (data.size() < 2 || !getU8(data, high) || !getU8(data, low)) {
    return false;
  }
  value = static_cast<uint16_t>((static_cast<uint16_t>(high) << 8) | low);
  return true;
}

bool wire::getU32(std::string_view &data, uint32_t &value)
{
  uint16_t high = 0;
  uint16_t low = 0;
  if (data.size() < 4 || !getU16(data, high) || !getU16(data, low)) {
    return false;
  }
  value = (static_cast<uint32_t>(high) << 16) | low;
  return true;
}

bool wire::getU64(std::string_view &data, uint64_t &value)
{
  uint32_t high = 0;
  uint32_t low = 0;
  if (data.size() < 8 || !getU32(data, high) || !getU32(data, low)) {
    return false;
  }
  value = (static_cast<uint64_t>(high) << 32) | low;
  return true;
}

bool wire::getBytes(std::string_view &data, size_t size, std::string_view &value)
{
  if (data.size() < size) {
    return false;
  }
  value = data.substr(0, size);
  data.remove_prefix(size);
  return true;
}

//
// ClipboardFileList
//

std::string ClipboardFileList::marshall() const
{
  std::string data;
  data.append(kMagic);
  wire::putU8(data, kVersion);
  wire::putU64(data, offerId);
  wire::putU32(data, static_cast<uint32_t>(entries.size()));
  for (const auto &entry : entries) {
    appendEntry(data, entry);
  }
  return data;
}

bool ClipboardFileList::unmarshall(std::string_view data, ClipboardFileList &list)
{
  list = {};

  std::string_view magic;
  uint8_t version = 0;
  uint64_t offerId = 0;
  uint32_t count = 0;
  if (!wire::getBytes(data, kMagic.size(), magic) || magic != kMagic) {
    return false;
  }
  if (!wire::getU8(data, version) || version != kVersion) {
    return false;
  }
  if (!wire::getU64(data, offerId) || !wire::getU32(data, count)) {
    return false;
  }
  if (count == 0 || count > s_maxEntries) {
    return false;
  }

  std::vector<ClipboardFileEntry> entries;
  if (!readEntries(data, entries) || entries.size() != count) {
    return false;
  }

  list.offerId = offerId;
  list.entries = std::move(entries);
  return true;
}

bool ClipboardFileList::isValidName(std::string_view name)
{
  if (name.empty() || name.size() > s_maxNameLength) {
    return false;
  }
  if (name == "." || name == "..") {
    return false;
  }
  return name.find_first_of(std::string_view("/\0", 2)) == std::string_view::npos;
}

void ClipboardFileList::appendEntry(std::string &data, const ClipboardFileEntry &entry)
{
  wire::putU8(data, entry.isDirectory ? kFlagDirectory : uint8_t{0});
  wire::putU64(data, entry.size);
  wire::putU64(data, static_cast<uint64_t>(entry.modifiedTime));
  wire::putU16(data, static_cast<uint16_t>(entry.name.size()));
  data.append(entry.name);
}

bool ClipboardFileList::readEntries(std::string_view data, std::vector<ClipboardFileEntry> &entries)
{
  entries.clear();
  while (!data.empty()) {
    uint8_t flags = 0;
    uint64_t size = 0;
    uint64_t modifiedTime = 0;
    uint16_t nameLength = 0;
    std::string_view name;
    if (!wire::getU8(data, flags) || !wire::getU64(data, size) || !wire::getU64(data, modifiedTime) ||
        !wire::getU16(data, nameLength) || !wire::getBytes(data, nameLength, name)) {
      entries.clear();
      return false;
    }
    if (!isValidName(name)) {
      entries.clear();
      return false;
    }

    ClipboardFileEntry entry;
    entry.name = std::string(name);
    entry.isDirectory = (flags & kFlagDirectory) != 0;
    entry.size = entry.isDirectory ? 0 : size;
    entry.modifiedTime = static_cast<int64_t>(modifiedTime);
    entries.push_back(std::move(entry));
  }
  return true;
}

} // namespace deskflow
