/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsComSecurity.h"

#include <objbase.h>
#include <sddl.h>

#include <vector>

namespace {

// who may call: the interactive user, the system and administrators, with the rights to call
// from this computer (COM_RIGHTS_EXECUTE and COM_RIGHTS_EXECUTE_LOCAL). the label lets callers
// of medium integrity in, which is where Explorer runs, and keeps those of low integrity out.
const wchar_t *const kAccess = L"O:BAG:BAD:(A;;0x3;;;IU)(A;;0x3;;;SY)(A;;0x3;;;BA)S:(ML;;NX;;;ME)";

//! COM keeps the descriptor for the life of the process
struct AbsoluteDescriptor
{
  std::vector<BYTE> descriptor;
  std::vector<BYTE> dacl;
  std::vector<BYTE> sacl;
  std::vector<BYTE> owner;
  std::vector<BYTE> group;
};

} // namespace

HRESULT allowUserToCallThroughCom()
{
  // the same kind of apartment Qt wants for this thread, it joins this one later
  HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(result)) {
    return result;
  }

  PSECURITY_DESCRIPTOR relative = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kAccess, SDDL_REVISION_1, &relative, nullptr)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }

  // CoInitializeSecurity takes the absolute form
  DWORD descriptorSize = 0;
  DWORD daclSize = 0;
  DWORD saclSize = 0;
  DWORD ownerSize = 0;
  DWORD groupSize = 0;
  MakeAbsoluteSD(
      relative, nullptr, &descriptorSize, nullptr, &daclSize, nullptr, &saclSize, nullptr, &ownerSize, nullptr,
      &groupSize
  );
  static AbsoluteDescriptor absolute;
  absolute.descriptor.resize(descriptorSize);
  absolute.dacl.resize(daclSize);
  absolute.sacl.resize(saclSize);
  absolute.owner.resize(ownerSize);
  absolute.group.resize(groupSize);
  const BOOL made = MakeAbsoluteSD(
      relative, absolute.descriptor.data(), &descriptorSize, reinterpret_cast<PACL>(absolute.dacl.data()), &daclSize,
      reinterpret_cast<PACL>(absolute.sacl.data()), &saclSize, absolute.owner.data(), &ownerSize, absolute.group.data(),
      &groupSize
  );
  const DWORD error = made ? ERROR_SUCCESS : GetLastError();
  LocalFree(relative);
  if (!made) {
    return HRESULT_FROM_WIN32(error);
  }

  return CoInitializeSecurity(
      absolute.descriptor.data(), -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, nullptr,
      EOAC_NONE, nullptr
  );
}
