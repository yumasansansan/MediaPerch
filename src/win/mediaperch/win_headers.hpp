// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <avrt.h>
#include <objbase.h>
// CommandLineToArgvW: the only correct splitter for Windows quoting rules.
#include <shellapi.h>
