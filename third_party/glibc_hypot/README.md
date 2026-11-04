# glibc hypot

`Hypot` ports `hypot` from [the GNU C Library](https://www.gnu.org/software/libc/) 2.44
(`sysdeps/ieee754/dbl-64/e_hypot.c`) as it computes where the compiler defines `__FP_FAST_FMA`,
as on aarch64: the square root of one `fma`, without the correction the other branch applies. It
leaves out `errno`, floating-point exceptions and signaling NaNs. `std::fma` keeps the result the
same on platforms without FMA instructions, and `-ffp-contract=off` keeps the compiler from fusing
anything else.

Neither branch is correctly rounded. BigQuery's spheroid distances match this one in their last
digits, where x86_64's glibc, which takes the other branch, macOS's math library and CORE-MATH's
correctly rounded `hypot` differ; see [the GeographicLib patch](../geographiclib/README.md) and
`tests/e2e/goclient/testdata/scalars/geography_spheroid.txt`.

The files in this directory are licensed under the
[GNU Lesser General Public License 2.1 or later](COPYING.LIB), as glibc is.
