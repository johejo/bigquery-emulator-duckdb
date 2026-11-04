// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2021-2026 Free Software Foundation, Inc.
// Ported from the GNU C Library 2.44, sysdeps/ieee754/dbl-64/e_hypot.c, taking the
// __FP_FAST_FMA branch of its kernel and leaving out errno, floating-point exceptions and
// signaling NaNs.
//
// The GNU C Library is free software; you can redistribute it and/or modify it under the terms
// of the GNU Lesser General Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// The GNU C Library is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License along with the GNU
// C Library; if not, see <https://www.gnu.org/licenses/>.

#include "third_party/glibc_hypot/hypot.h"

#include <cmath>
#include <limits>

namespace glibc_hypot {
namespace {

constexpr double kScale = 0x1p-600;
constexpr double kLargeVal = 0x1p+511;
constexpr double kTinyVal = 0x1p-459;
constexpr double kEps = 0x1p-54;

// The inputs must be adjusted so that ax >= ay >= 0 and squaring ax, ay and (ax - ay) does not
// overflow or underflow.
double Kernel(double ax, double ay) {
  const double t1 = ay + ay;
  const double t2 = ax - ay;
  if (t1 >= ax) {
    return std::sqrt(std::fma(t1, ax, t2 * t2));
  }
  return std::sqrt(std::fma(ax, ax, ay * ay));
}

}  // namespace

double Hypot(double x, double y) {
  if (!std::isfinite(x) || !std::isfinite(y)) {
    if (std::isinf(x) || std::isinf(y)) {
      return std::numeric_limits<double>::infinity();
    }
    return x + y;
  }

  x = std::fabs(x);
  y = std::fabs(y);
  const double ax = std::fmax(x, y);
  const double ay = std::fmin(x, y);

  // If ax is huge, scale both inputs down.
  if (ax > kLargeVal) {
    if (ay <= ax * kEps) {
      return ax + ay;
    }
    return Kernel(ax * kScale, ay * kScale) / kScale;
  }

  // If ay is tiny, scale both inputs up.
  if (ay < kTinyVal) {
    if (ax >= ay / kEps) {
      return ax + ay;
    }
    return Kernel(ax / kScale, ay / kScale) * kScale;
  }

  // Common case: ax is not huge and ay is not tiny.
  if (ay <= ax * kEps) {
    return ax + ay;
  }
  return Kernel(ax, ay);
}

}  // namespace glibc_hypot
