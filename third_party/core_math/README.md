# CORE-MATH

[CORE-MATH](https://core-math.gitlabpages.inria.fr/) provides correctly rounded math functions
under the MIT license. Geography coordinates convert between degrees and `S2Point` with its
`cr_atan2`, `cr_sin` and `cr_cos` rather than the platform's math library, whose last bit can
differ: glibc's `atan2` turns the longitude 9 of a point to 9 exactly, where BigQuery and
correctly rounded `atan2` give one bit above it.

S2's buffering also steps around circles with its `cr_acos`, `cr_tan` and `cr_sin`, through
[an s2geometry patch](../s2geometry/README.md), and `ST_BUFFER` takes its segments' error with
`cr_cos`: macOS's math library moves a vertex of a buffered linestring's round end cap where
BigQuery and correctly rounded functions agree.

`MODULE.bazel` overrides the `core_math` module with the pinned commit, which
`BUILD.core_math.bazel` builds and a generated `MODULE.bazel` declares, so that the patched
s2geometry module can depend on it. `core_math.h` declares the functions, since upstream has no
header.
