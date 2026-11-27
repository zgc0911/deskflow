/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

//! Let the applications of the user call into this process through COM
/*!
Files copied on another computer are pasted through COM: Explorer calls the
data object this process put on the clipboard. When the core runs as a
service it runs as SYSTEM, and by default COM lets nobody but SYSTEM and
administrators call it. This lets the interactive user in, from medium
integrity up, the level Explorer runs at.

Must come before anything else in the process uses COM, before Qt starts.
Returns what CoInitializeSecurity returned, to be logged once logging works.
*/
HRESULT allowUserToCallThroughCom();
