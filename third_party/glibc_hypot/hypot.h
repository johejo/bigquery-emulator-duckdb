// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2021-2026 Free Software Foundation, Inc.
// Ported from the GNU C Library 2.44; see README.md.

#ifndef THIRD_PARTY_GLIBC_HYPOT_HYPOT_H_
#define THIRD_PARTY_GLIBC_HYPOT_HYPOT_H_

namespace glibc_hypot {

// glibc's hypot as it computes where FMA is available, on every platform.
double Hypot(double x, double y);

}  // namespace glibc_hypot

#endif  // THIRD_PARTY_GLIBC_HYPOT_HYPOT_H_
