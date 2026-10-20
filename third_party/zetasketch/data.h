// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the
// License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
// express or implied. See the License for the specific language governing permissions and
// limitations under the License.
//
// Ported to C++ from zetasketch's java/com/google/zetasketch/internal/hllplus/Data.java; see
// README.md.

#pragma once

#include <cstdint>

namespace zetasketch {

// alpha_m of the HLL++ paper for m = 2^precision.
double Alpha(int precision);

// The bias of the raw estimate `estimate` at `precision`, from the empirical bias corrections of
// the HLL++ paper, or 0 outside the range they cover.
double EstimateBias(double estimate, int precision);

// The estimate at or below which LinearCounting is more accurate than HyperLogLog.
int64_t LinearCountingThreshold(int precision);

}  // namespace zetasketch
