// Wrapper around the authors' reference ALEX (github.com/microsoft/ALEX), used
// only to validate this reimplementation. Fetch it with
// scripts/fetch_alex_reference.sh and build with `make reference`. The
// reference uses x86 intrinsics, so on Apple Silicon it is built for x86_64
// and runs under Rosetta.
#pragma once

#include <ALEX/src/core/alex.h>

#include "indexes.h"

namespace cop5725 {

class AlexRefIndex {
 public:
  explicit AlexRefIndex(const IndexOptions& o = {}) {
    alex_.set_expected_insert_frac(o.alex.expected_insert_frac);
    alex_.set_max_node_size(static_cast<int>(o.alex.max_node_bytes));
    // Exact model and cost computation, like this reimplementation. The
    // reference defaults to sampled model fitting; --ref-approx 1 restores it.
    alex_.set_approximate_model_computation(o.ref_approximate);
    alex_.set_approximate_cost_computation(false);
  }
  static const char* name() { return "alexref"; }
  static bool supports_inserts() { return true; }

  void bulk_load(const std::pair<Key, uint64_t>* sorted, size_t n) {
    alex_.bulk_load(sorted, static_cast<int>(n));
  }
  bool find(Key k, uint64_t& payload) const {
    const uint64_t* p = alex_.get_payload(k);
    if (!p) return false;
    payload = *p;
    return true;
  }
  bool insert(Key k, uint64_t payload) { return alex_.insert(k, payload).second; }
  size_t size() const { return alex_.size(); }
  MemoryUsage memory() const {
    return {static_cast<size_t>(alex_.model_size()), static_cast<size_t>(alex_.data_size())};
  }
  std::vector<Counter> counters() const {
    const auto& s = alex_.get_stats();
    uint64_t depth_x1000 =
        s.num_lookups ? uint64_t(1000.0 * s.num_node_lookups / s.num_lookups) : 0;
    return {{"model_nodes", uint64_t(s.num_model_nodes)},
            {"data_nodes", uint64_t(s.num_data_nodes)},
            {"node_lookups_per_lookup_x1000", depth_x1000},
            {"expand_and_scales", uint64_t(s.num_expand_and_scales)},
            {"expand_and_retrains", uint64_t(s.num_expand_and_retrains)},
            {"downward_splits", uint64_t(s.num_downward_splits)},
            {"sideways_splits", uint64_t(s.num_sideways_splits)}};
  }

 private:
  ::alex::Alex<Key, uint64_t> alex_;  // the reference, not cop5725::alex
};

}  // namespace cop5725
