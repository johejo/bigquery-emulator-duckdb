#ifndef THIRD_PARTY_CORE_MATH_CORE_MATH_H_
#define THIRD_PARTY_CORE_MATH_CORE_MATH_H_

// CORE-MATH's correctly rounded functions, which the upstream sources define without a header.
extern "C" {
double cr_atan2(double y, double x);
double cr_cos(double x);
double cr_sin(double x);
}

#endif  // THIRD_PARTY_CORE_MATH_CORE_MATH_H_
