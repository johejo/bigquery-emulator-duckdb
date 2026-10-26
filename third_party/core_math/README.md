# CORE-MATH

[CORE-MATH](https://core-math.gitlabpages.inria.fr/) provides correctly rounded math functions
under the MIT license. Geography coordinates convert between degrees and `S2Point` with its
`cr_atan2`, `cr_sin` and `cr_cos` rather than the platform's math library, whose last bit can
differ: glibc's `atan2` turns the longitude 9 of a point to 9 exactly, where BigQuery and
correctly rounded `atan2` give one bit above it.

`BUILD.core_math.bazel` builds those sources from the commit pinned in `MODULE.bazel`, and
`core_math.h` declares them, since upstream has no header.
