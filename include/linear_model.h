// Linear models shared by the RMI and ALEX.
#pragma once

#include <algorithm>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <emmintrin.h>
#endif
#include <cstddef>
#include <cstdint>

#include "datasets.h"

namespace cop5725 {

// y = a * key + b. Keys are converted to double, as in the ALEX reference
// implementation, so routing at lookup time is bit-for-bit the same
// computation as routing during bulk load.
struct LinearModel {
  double a = 0.0;
  double b = 0.0;

  double predict_double(Key k) const { return a * static_cast<double>(k) + b; }

  // Prediction truncated to an int and clamped to [0, size - 1].
  //
  // This sits on every lookup's hot path (once per node), so it uses a
  // branch-free float-to-int conversion whose result is defined for every
  // input. A plain static_cast<int> is undefined behaviour when the
  // prediction is out of range (it can be, for absent keys), and clamping in
  // double space first costs 10-15% lookup throughput on easy datasets.
  int predict_clamped(Key k, int size) const {
    const double p = predict_double(k);
#if defined(__aarch64__)
    // fcvtzs saturates: huge -> INT64_MAX, tiny -> INT64_MIN, NaN -> 0.
    const int64_t i = vcvtd_s64_f64(p);
    return static_cast<int>(std::min<int64_t>(std::max<int64_t>(i, 0), size - 1));
#elif defined(__x86_64__)
    // Cap huge values first, then cvttsd2si, which returns INT64_MIN for
    // anything still out of range (huge negatives) and for NaN. minsd returns
    // its second operand when either is NaN, so NaN passes through to 0.
    const int64_t i = _mm_cvttsd_si64(_mm_min_sd(_mm_set_sd(0x1p62), _mm_set_sd(p)));
    return static_cast<int>(std::min<int64_t>(std::max<int64_t>(i, 0), size - 1));
#else
    if (!(p > 0.0)) return 0;  // also catches NaN
    if (p >= static_cast<double>(size - 1)) return size - 1;
    return static_cast<int>(p);
#endif
  }

  void expand(double factor) {
    a *= factor;
    b *= factor;
  }
};

// Least-squares fit of position (y) against key (x). Keys are offset by the
// first key in exact integer arithmetic before converting to floating point,
// so a narrow range of huge 64-bit keys keeps its precision in the fit even
// where long double is only a double (e.g. Apple Silicon). Predictions still
// go through double(key), as in the reference implementation.
template <class GetKey>
LinearModel fit_linear(size_t n, GetKey key_at, double y_offset = 0.0) {
  LinearModel m;
  if (n == 0) return m;
  if (n == 1) {
    m.b = y_offset;
    return m;
  }
  const Key x0 = key_at(0);
  auto dx_at = [&](size_t i) {
    Key k = key_at(i);
    return k >= x0 ? static_cast<long double>(k - x0) : -static_cast<long double>(x0 - k);
  };
  long double x_mean = 0, y_mean = (n - 1) / 2.0L;
  for (size_t i = 0; i < n; ++i) x_mean += dx_at(i);
  x_mean /= n;
  long double sxy = 0, sxx = 0;
  for (size_t i = 0; i < n; ++i) {
    long double dx = dx_at(i) - x_mean;
    sxy += dx * (static_cast<long double>(i) - y_mean);
    sxx += dx * dx;
  }
  if (sxx == 0) {  // all keys identical
    m.b = static_cast<double>(y_mean) + y_offset;
    return m;
  }
  const long double slope = sxy / sxx;
  m.a = static_cast<double>(slope);
  // y = slope * (x - x0 - x_mean) + y_mean, rewritten as a * x + b.
  m.b = static_cast<double>(y_mean - slope * (static_cast<long double>(x0) + x_mean)) + y_offset;
  return m;
}

}  // namespace cop5725
