# GeographicLib patches

`hypot.patch` makes GeographicLib's unqualified `hypot` calls on doubles take
[glibc's FMA `hypot`](../glibc_hypot/README.md) rather than the platform's math library. It
declares `GeographicLib::hypot` overloads in `Math.hpp`, which name lookup finds before
`std::hypot`, and removes the `using std::hypot` declarations that would hide them. Other types
still take `std::hypot`. BigQuery's `ST_DISTANCE` with `use_spheroid` matches this `hypot` in
its last digits, where x86_64's glibc and macOS's math library differ; see
`tests/e2e/goclient/testdata/scalars/geography_spheroid.txt`.

`.bazelrc` also builds GeographicLib with `-ffp-contract=off`, since clang otherwise fuses
multiplies and adds on aarch64.
