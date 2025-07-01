//===-- null_attrs.h - Nullability attributes for non-clang compilers -----===//
//
// Copyright (c) 2025 Nuclearist <nuclearist@teknology-hub.com>
// Part of tek-s3, under the GNU General Public License v3.0 or later
// See https://github.com/teknology-hub/tek-s3/blob/main/COPYING for license
//    information.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//===----------------------------------------------------------------------===//
///
/// @file
/// Definitions of Clang's `_Nullable`, `_Nonnull`, and `_Null_unspecified`
///    attributes for other compilers
///
//===----------------------------------------------------------------------===//
#pragma once

#ifndef __clang__

#ifndef _Nullable
#define _Nullable
#endif // ndef _Nullable
#ifndef _Nonnull
#define _Nonnull
#endif // ndef _Nonnull
#ifndef _Null_unspecified
#define _Null_unspecified
#endif // ndef _Null_unspecified

#endif // ndef __clang__
