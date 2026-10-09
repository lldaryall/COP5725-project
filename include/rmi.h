// Two-stage Recursive Model Index (Kraska et al., SIGMOD '18), configured as
// the "Learned Index" baseline in the ALEX paper: linear models in both
// stages over a dense sorted array, with per-model error bounds for a
// bounded binary search. Read-only: inserts are rejected.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "datasets.h"
#include "linear_model.h"

namespace cop5725 {

class RMI {
 public:
  // num_models == 0 picks a default of roughly one model per 128 keys
  // (rounded to a power of two). The ALEX paper tuned this per dataset;
  // scripts/run_read_path.sh sweeps it and reports the best.
  explicit RMI(size_t num_models = 0) : requested_models_(num_models) {}

  void bulk_load(const std::pair<Key, uint64_t>* sorted, size_t n) {
    keys_.resize(n);
    payloads_.resize(n);
    for (size_t i = 0; i < n; ++i) {
      keys_[i] = sorted[i].first;
      payloads_[i] = sorted[i].second;
    }
    size_t m = requested_models_;
    if (m == 0) {
      m = 1;
      while (m * 128 < n) m *= 2;
    }
    leaves_.assign(m, Leaf{});
    if (n == 0) return;

    // Root: regression on the CDF, scaled to [0, m).
    root_ = fit_linear(n, [&](size_t i) { return keys_[i]; });
    root_.expand(static_cast<double>(m) / n);

    // Partition by the root's prediction (monotone in the key) and fit each
    // leaf on the global positions of its keys.
    size_t begin = 0;
    for (size_t j = 0; j < m; ++j) {
      size_t end = begin;
      while (end < n && static_cast<size_t>(root_.predict_clamped(keys_[end], int(m))) == j) ++end;
      Leaf& leaf = leaves_[j];
      if (end == begin) {
        // No key routes here: any lookup that does is a miss. Point at the
        // next position so the (empty) search window stays in range.
        leaf.model.b = static_cast<double>(std::min(begin, n - 1));
      } else {
        leaf.model = fit_linear(end - begin, [&](size_t i) { return keys_[begin + i]; },
                                static_cast<double>(begin));
        int lo = 0, hi = 0;
        for (size_t i = begin; i < end; ++i) {
          int err = static_cast<int>(i) - leaf.model.predict_clamped(keys_[i], int(n));
          lo = std::min(lo, err);
          hi = std::max(hi, err);
        }
        leaf.min_err = lo;
        leaf.max_err = hi;
      }
      begin = end;
    }
  }

  bool find(Key k, uint64_t& payload) const {
    const size_t n = keys_.size();
    if (n == 0) return false;
    const Leaf& leaf = leaves_[root_.predict_clamped(k, int(leaves_.size()))];
    int pred = leaf.model.predict_clamped(k, int(n));
    size_t lo = static_cast<size_t>(std::max(0, pred + leaf.min_err));
    size_t hi = std::min(n, static_cast<size_t>(pred + leaf.max_err + 1));
    auto it = std::lower_bound(keys_.begin() + lo, keys_.begin() + hi, k);
    if (it == keys_.begin() + hi || *it != k) return false;
    payload = payloads_[it - keys_.begin()];
    return true;
  }

  size_t size() const { return keys_.size(); }
  size_t num_models() const { return leaves_.size(); }

  // Mean log2 of the search window: the expected binary-search iterations.
  double avg_log2_window() const {
    if (keys_.empty()) return 0;
    double sum = 0;
    for (Key k : keys_) {
      const Leaf& l = leaves_[root_.predict_clamped(k, int(leaves_.size()))];
      sum += std::log2(double(l.max_err - l.min_err + 1));
    }
    return sum / keys_.size();
  }

  size_t index_bytes() const { return sizeof(LinearModel) + leaves_.size() * sizeof(Leaf); }
  size_t data_bytes() const { return keys_.size() * (sizeof(Key) + sizeof(uint64_t)); }

 private:
  struct Leaf {
    LinearModel model;
    int32_t min_err = 0;
    int32_t max_err = 0;
  };

  size_t requested_models_;
  LinearModel root_;
  std::vector<Leaf> leaves_;
  std::vector<Key> keys_;
  std::vector<uint64_t> payloads_;
};

}  // namespace cop5725
