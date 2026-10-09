// Workload generation. A workload is a pre-generated vector of operations so
// that random number generation never runs inside the timed loop.
//
// Lookup keys are always drawn from keys already in the index (bulk-loaded or
// previously inserted), Zipfian by default as in the ALEX paper (Sec. 6.1).
//
// Insert keys come from one of four patterns:
//   dataset  - the unused remainder of the shuffled dataset (ALEX paper setup)
//   hotrange - a growing fraction of inserts hits a narrow key range
//   drift    - inserts follow a narrow distribution whose centre moves at a
//              controlled rate (the controlled variable of the extension)
//   regime   - inserts switch abruptly to a different distribution
//
// The hot range, drift and regime patterns are positioned in *quantile space*
// of the bulk-loaded keys: q = 0.3 means "where the 30th percentile of the
// existing data is". Quantiles outside [0, 1] extrapolate linearly past the
// smallest/largest key, so a drift with q_to > 1 walks off the end of the
// key domain (the case where ALEX must expand its root).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "datasets.h"

namespace cop5725 {

enum class OpType : uint8_t { Lookup = 0, Insert = 1 };

struct Op {
  OpType type;
  Key key;
};

enum class InsertPattern { Dataset, HotRange, Drift, Regime };

inline InsertPattern parse_pattern(const std::string& s) {
  if (s == "dataset") return InsertPattern::Dataset;
  if (s == "hotrange") return InsertPattern::HotRange;
  if (s == "drift") return InsertPattern::Drift;
  if (s == "regime") return InsertPattern::Regime;
  throw std::runtime_error("unknown insert pattern: " + s);
}

inline const char* pattern_name(InsertPattern p) {
  switch (p) {
    case InsertPattern::Dataset: return "dataset";
    case InsertPattern::HotRange: return "hotrange";
    case InsertPattern::Drift: return "drift";
    case InsertPattern::Regime: return "regime";
  }
  return "?";
}

// The four read/write mixes from the ALEX paper.
inline double mix_insert_fraction(const std::string& mix) {
  if (mix == "readonly") return 0.0;
  if (mix == "readheavy") return 0.05;
  if (mix == "writeheavy") return 0.5;
  if (mix == "writeonly") return 1.0;
  throw std::runtime_error("unknown mix: " + mix);
}

struct WorkloadSpec {
  size_t num_ops = 1'000'000;
  double insert_frac = 0.0;
  bool zipf_lookups = true;
  double zipf_theta = 0.99;
  uint64_t seed = 42;
  InsertPattern pattern = InsertPattern::Dataset;

  // hotrange: the fraction of inserts that hit [center +- width/2] ramps
  // linearly from hot_frac_start to hot_frac_end over the workload; the rest
  // come from the dataset remainder.
  double hot_center = 0.5;
  double hot_width = 0.01;
  double hot_frac_start = 0.0;
  double hot_frac_end = 1.0;

  // drift: before drift_start (fraction of inserts) keys come from the
  // dataset remainder. Afterwards keys ~ Normal(center, drift_sigma), with
  // center = drift_from + drift_rate * (inserts since start) / 1e6.
  double drift_start = 0.0;
  double drift_from = 0.5;
  double drift_rate = 0.1;  // quantile units per million inserts
  double drift_sigma = 0.005;

  // regime: before regime_at (fraction of inserts) keys come from the
  // dataset remainder; afterwards keys ~ Normal(regime_center, regime_sigma),
  // or, with regime_append, strictly increasing keys past the current maximum.
  double regime_at = 0.5;
  double regime_center = 0.1;
  double regime_sigma = 0.005;
  bool regime_append = false;
};

struct Workload {
  std::vector<std::pair<Key, uint64_t>> init;  // sorted, for bulk loading
  std::vector<Op> ops;
  size_t num_inserts = 0;
  // Pattern draws that could not find an unused key and fell back to the
  // dataset remainder (e.g. a hot range so narrow it has run out of free
  // integers). Reported so such runs are not silently mislabelled.
  size_t fallbacks = 0;
};

// Payloads are a cheap function of the key so lookups can be verified.
inline uint64_t payload_for(Key k) { return k * 0x9E3779B97F4A7C15ull + 1; }

// YCSB Zipfian generator (Gray et al.) over ranks [0, n), supporting a
// growing n by extending zeta(n) incrementally.
class ZipfGenerator {
 public:
  explicit ZipfGenerator(double theta) : theta_(theta) {
    zeta2_ = 1.0 + std::pow(0.5, theta_);
    alpha_ = 1.0 / (1.0 - theta_);
  }

  size_t next(std::mt19937_64& rng, size_t n) {
    if (n != n_) grow(n);
    double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
    double uz = u * zetan_;
    if (uz < 1.0) return 0;
    if (uz < 1.0 + std::pow(0.5, theta_)) return std::min<size_t>(1, n - 1);
    size_t r = static_cast<size_t>(n * std::pow(eta_ * u - eta_ + 1.0, alpha_));
    return std::min(r, n - 1);
  }

 private:
  void grow(size_t n) {
    if (n < n_) throw std::logic_error("ZipfGenerator cannot shrink");
    for (size_t i = n_ + 1; i <= n; ++i) zetan_ += 1.0 / std::pow(double(i), theta_);
    n_ = n;
    eta_ = (1.0 - std::pow(2.0 / n_, 1.0 - theta_)) / (1.0 - zeta2_ / zetan_);
  }

  double theta_, zeta2_, alpha_;
  double zetan_ = 0.0, eta_ = 0.0;
  size_t n_ = 0;
};

namespace detail {

// Maps quantile q of the sorted bulk-loaded keys to a fresh key: uniformly
// random inside the gap between the two neighbouring existing keys, or a
// linear extrapolation past either end for q outside [0, 1].
class QuantileSampler {
 public:
  explicit QuantileSampler(const std::vector<std::pair<Key, uint64_t>>& sorted) : s_(sorted) {
    lo_ = static_cast<long double>(s_.front().first);
    hi_ = static_cast<long double>(s_.back().first);
    span_ = std::max<long double>(hi_ - lo_, 1.0L);
  }

  Key sample(double q, std::mt19937_64& rng) const {
    if (q < 0.0 || q > 1.0) {
      // Clamp below 2^64 with margin: on platforms where long double is a
      // plain double, UINT64_MAX - 1 rounds up to 2^64 and the cast would be
      // undefined.
      const long double kTop = 0x1p64L - 0x1p12L;
      long double v = q < 0.0 ? lo_ + q * span_ : hi_ + (q - 1.0) * span_;
      v = std::clamp(v, 0.0L, kTop);
      return static_cast<Key>(v);
    }
    size_t n = s_.size();
    size_t i = std::min(static_cast<size_t>(q * (n - 1)), n - 1);
    Key a = s_[i].first;
    Key b = i + 1 < n ? s_[i + 1].first : a;
    if (b <= a + 1) return a;  // no free integer here; caller resamples
    return std::uniform_int_distribution<Key>(a + 1, b - 1)(rng);
  }

 private:
  const std::vector<std::pair<Key, uint64_t>>& s_;
  long double lo_, hi_, span_;
};

}  // namespace detail

// `keys` is a shuffled, deduplicated dataset (see datasets.h). The first
// `init_size` keys are bulk loaded; the rest form the insert pool.
inline Workload generate_workload(const std::vector<Key>& keys, size_t init_size,
                                  const WorkloadSpec& spec) {
  if (init_size == 0 || init_size > keys.size())
    throw std::runtime_error("init size must be in [1, dataset size]");

  Workload w;
  std::mt19937_64 rng(spec.seed);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  std::normal_distribution<double> gauss(0.0, 1.0);

  // w.init doubles as the sorted view of the bulk-loaded keys, and lookups
  // read ranks straight out of `keys`, so no extra copies of the (possibly
  // 200M-key) dataset are made.
  w.init.reserve(init_size);
  for (size_t i = 0; i < init_size; ++i) w.init.emplace_back(keys[i], payload_for(keys[i]));
  std::sort(w.init.begin(), w.init.end());
  detail::QuantileSampler quantile(w.init);

  // Lookups pick existing keys by rank: ranks below init_size are bulk-loaded
  // keys in their shuffled order (so Zipf-popular keys are spread across the
  // key space), higher ranks are inserted keys in insertion order.
  std::vector<Key> inserted_order;
  inserted_order.reserve(static_cast<size_t>(spec.num_ops * spec.insert_frac) + 1);
  auto existing_at = [&](size_t rank) {
    return rank < init_size ? keys[rank] : inserted_order[rank - init_size];
  };
  ZipfGenerator zipf(spec.zipf_theta);

  size_t pool_next = init_size;
  std::unordered_set<Key> inserted;  // keys inserted by this workload
  auto is_used = [&](Key k) {
    auto it = std::lower_bound(w.init.begin(), w.init.end(), k,
                               [](const std::pair<Key, uint64_t>& kv, Key x) { return kv.first < x; });
    return (it != w.init.end() && it->first == k) || inserted.count(k) != 0;
  };
  auto from_pool = [&]() -> Key {
    while (pool_next < keys.size()) {
      Key k = keys[pool_next++];
      if (!inserted.count(k)) return k;  // a pattern draw may have taken it
    }
    throw std::runtime_error(
        "dataset remainder exhausted: use a larger --n or fewer inserts" +
        (w.fallbacks ? " (" + std::to_string(w.fallbacks) +
                           " pattern draws found no free key and fell back to "
                           "the pool; the target range is too dense)"
                     : std::string()));
  };
  auto from_quantile = [&](auto&& draw_q) -> Key {
    for (int attempt = 0; attempt < 64; ++attempt) {
      Key k = quantile.sample(draw_q(), rng);
      if (!is_used(k)) return k;
    }
    ++w.fallbacks;
    return from_pool();
  };

  Key append_next = w.init.back().first;
  const size_t expected_inserts =
      std::max<size_t>(1, static_cast<size_t>(spec.num_ops * spec.insert_frac));

  auto next_insert_key = [&](size_t insert_idx) -> Key {
    double progress = double(insert_idx) / double(expected_inserts);
    switch (spec.pattern) {
      case InsertPattern::Dataset:
        return from_pool();
      case InsertPattern::HotRange: {
        double frac = spec.hot_frac_start +
                      (spec.hot_frac_end - spec.hot_frac_start) * std::min(progress, 1.0);
        if (unit(rng) >= frac) return from_pool();
        return from_quantile([&] {
          return spec.hot_center + (unit(rng) - 0.5) * spec.hot_width;
        });
      }
      case InsertPattern::Drift: {
        if (progress < spec.drift_start) return from_pool();
        double since = insert_idx - spec.drift_start * expected_inserts;
        double center = spec.drift_from + spec.drift_rate * since / 1e6;
        return from_quantile([&] { return center + spec.drift_sigma * gauss(rng); });
      }
      case InsertPattern::Regime: {
        if (progress < spec.regime_at) return from_pool();
        if (spec.regime_append) {
          // Keep strictly increasing past everything inserted so far.
          for (;;) {
            append_next += 1 + rng() % 1024;
            if (!is_used(append_next)) return append_next;
          }
        }
        return from_quantile(
            [&] { return spec.regime_center + spec.regime_sigma * gauss(rng); });
      }
    }
    return from_pool();
  };

  w.ops.reserve(spec.num_ops);
  for (size_t i = 0; i < spec.num_ops; ++i) {
    bool do_insert = spec.insert_frac >= 1.0 ||
                     (spec.insert_frac > 0.0 && unit(rng) < spec.insert_frac);
    if (do_insert) {
      Key k = next_insert_key(w.num_inserts++);
      inserted.insert(k);
      inserted_order.push_back(k);
      append_next = std::max(append_next, k);
      w.ops.push_back({OpType::Insert, k});
    } else {
      const size_t num_existing = init_size + inserted_order.size();
      size_t rank = spec.zipf_lookups
                        ? zipf.next(rng, num_existing)
                        : std::uniform_int_distribution<size_t>(0, num_existing - 1)(rng);
      w.ops.push_back({OpType::Lookup, existing_at(rank)});
    }
  }
  return w;
}

}  // namespace cop5725
