// ALEX (Ding et al., SIGMOD '20): read path and bulk loading.
//
// Implemented so far (weeks 3-4):
//   - Data nodes: a Gapped Array of keys and payloads with a bitmap, a linear
//     model, model-based placement at bulk load, and lookup by model
//     prediction plus exponential search.
//   - Model (internal) nodes: a linear model over a power-of-two pointer
//     array, where a child may own 2^k consecutive (duplicate) pointers.
//   - Bulk loading: recursive, cost-model driven, using the fanout tree to
//     choose each model node's fanout and to merge cheap neighbouring
//     children (paper Sec. 4.3; Algorithm 4).
//   - The cost model (paper Sec. 4.2): expected exponential-search iterations
//     and expected shifts per insert for a data node, plus traversal and
//     model-size terms, with the reference implementation's weights.
//
// Still to come (weeks 7-8): inserts into gaps, node expansion, sideways and
// downward splits, and cost-deviation-triggered adaptation.
//
// Keys equal to UINT64_MAX are reserved for the gap sentinel.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "datasets.h"
#include "linear_model.h"

namespace cop5725 {
namespace alex {

using Value = std::pair<Key, uint64_t>;

// Cost model weights and densities, from the reference implementation
// (alex_base.h, alex_nodes.h).
constexpr double kExpSearchIterationsWeight = 20;
constexpr double kShiftsWeight = 0.5;
constexpr double kNodeLookupsWeight = 20;
constexpr double kModelSizeWeight = 5e-7;
constexpr double kInitDensity = 0.7;  // data node density after bulk load
constexpr double kMaxDensity = 0.8;   // expansion threshold (used for inserts)
constexpr double kMinDensity = 0.6;   // density after expansion (inserts)
constexpr Key kEndSentinel = std::numeric_limits<Key>::max();

struct Params {
  // Fraction of operations expected to be inserts; weights the shift term of
  // the cost model. The reference implementation defaults to 1.
  double expected_insert_frac = 1.0;
  // Maximum node size in bytes; bounds data node slots and model node fanout.
  size_t max_node_bytes = size_t(1) << 24;
  // Per-data-node metadata footprint charged by the cost model's model-size
  // term. The weights (kModelSizeWeight etc.) were tuned against the
  // reference's 208-byte AlexDataNode<uint64_t, uint64_t>, so charging our
  // smaller struct would tilt the cost model towards many more, smaller data
  // nodes than ALEX builds. Memory accounting still uses our real sizes.
  double data_node_cost_bytes = 208;
  // Fit data node models exactly as the reference does (raw long double
  // sums) instead of our more precise offset fit. Only for validation: it
  // makes structure comparisons bit-exact.
  bool reference_model_fit = false;
};

// Least-squares data node model for keys v[0..n), positions 0..n-1.
inline LinearModel fit_data_model(const Value* v, int n, const Params& p) {
  auto key_at = [&](size_t i) { return v[i].first; };
  return p.reference_model_fit ? fit_linear_reference(n, key_at) : fit_linear(n, key_at);
}

struct Node {
  explicit Node(bool leaf, int lvl) : is_leaf(leaf), level(lvl) {}
  bool is_leaf;
  uint8_t duplication_factor = 0;  // owns 2^duplication_factor parent slots
  int level;
  double cost = 0;  // expected cost per key as a data node (cost model)
  LinearModel model;
};

struct ModelNode : Node {
  explicit ModelNode(int lvl) : Node(false, lvl) {}
  int num_children = 0;
  std::unique_ptr<Node*[]> children;

  Node* child_for(Key k) const { return children[model.predict_clamped(k, num_children)]; }
};

struct DataNode : Node {
  explicit DataNode(int lvl) : Node(true, lvl) {}

  std::unique_ptr<Key[]> keys;  // gaps hold the next key to their right
  std::unique_ptr<uint64_t[]> payloads;
  std::unique_ptr<uint64_t[]> bitmap;  // bit set = slot holds a real key
  int capacity = 0;
  int num_keys = 0;
  int bitmap_words = 0;
  // Cost model expectations recorded at build time; the adaptation logic
  // (weeks 7-8) compares these against observed costs.
  double expected_avg_exp_search_iterations = 0;
  double expected_avg_shifts = 0;

  static int capacity_for(int n, double density) {
    return std::max(static_cast<int>(n / density), n + 1);
  }

  bool is_occupied(int pos) const { return (bitmap[pos >> 6] >> (pos & 63)) & 1; }

  // Places sorted keys at their model-predicted slots (or the next free slot
  // to the right), keeping enough room at the end for the remaining keys,
  // then fills each gap with the key to its right. Mirrors the reference
  // AlexDataNode::bulk_load.
  void bulk_load(const Value* v, int n, const LinearModel* trained) {
    capacity = capacity_for(n, kInitDensity);
    num_keys = n;
    keys.reset(new Key[capacity]);
    payloads.reset(new uint64_t[capacity]());
    bitmap_words = (capacity + 63) / 64;
    bitmap.reset(new uint64_t[bitmap_words]());
    model = trained ? *trained : fit_linear(n, [&](size_t i) { return v[i].first; });
    if (n == 0) {
      std::fill(keys.get(), keys.get() + capacity, kEndSentinel);
      return;
    }
    model.expand(static_cast<double>(capacity) / n);

    int last = -1;
    for (int i = 0; i < n; ++i) {
      int pos = std::max(model.predict_clamped(v[i].first, capacity), last + 1);
      if (capacity - pos < n - i) {
        // Not enough room left: pack the remaining keys at the end.
        pos = capacity - (n - i);
        for (int j = last + 1; j < pos; ++j) keys[j] = v[i].first;
        for (int j = i; j < n; ++j, ++pos) place(pos, v[j]);
        last = capacity - 1;
        break;
      }
      for (int j = last + 1; j < pos; ++j) keys[j] = v[i].first;
      place(pos, v[i]);
      last = pos;
    }
    for (int j = last + 1; j < capacity; ++j) keys[j] = kEndSentinel;
  }

  // Index of the slot holding k, or -1.
  int find_key(Key k) const {
    int predicted = model.predict_clamped(k, capacity);
    int pos = exponential_search_upper_bound(predicted, k) - 1;
    return (pos < 0 || keys[pos] != k) ? -1 : pos;
  }

  // First slot in [0, capacity) whose key is > k, searching outward from m.
  int exponential_search_upper_bound(int m, Key k) const {
    int bound = 1, l, r;
    if (keys[m] > k) {
      int size = m;
      while (bound < size && keys[m - bound] > k) bound *= 2;
      l = m - std::min(bound, size);
      r = m - bound / 2;
    } else {
      int size = capacity - m;
      while (bound < size && keys[m + bound] <= k) bound *= 2;
      l = m + bound / 2;
      r = m + std::min(bound, size);
    }
    return static_cast<int>(std::upper_bound(keys.get() + l, keys.get() + r, k) - keys.get());
  }

  size_t data_bytes() const {
    return size_t(capacity) * (sizeof(Key) + sizeof(uint64_t)) + bitmap_words * sizeof(uint64_t);
  }

 private:
  void place(int pos, const Value& kv) {
    keys[pos] = kv.first;
    payloads[pos] = kv.second;
    bitmap[pos >> 6] |= uint64_t(1) << (pos & 63);
  }
};

// ---------------------------------------------------------------------------
// Cost model

struct DataNodeStats {
  double search_iterations = 0;  // expected exponential search iterations
  double shifts = 0;             // expected shifts per insert
};

// Simulates a data node bulk load of v[0..n) with `model` (trained on
// positions 0..n-1) and returns the expected cost per key, mirroring the
// reference's compute_expected_cost / build_node_implicit.
inline double expected_cost(const Value* v, int n, double density, double insert_frac,
                            const LinearModel& trained, DataNodeStats* stats) {
  if (n == 0) {
    if (stats) *stats = {};
    return 0;
  }
  const int capacity = DataNode::capacity_for(n, density);
  LinearModel model = trained;
  model.expand(static_cast<double>(capacity) / n);

  double log_err_sum = 0;
  long long shifts_sum = 0;
  int last = -1, run_start = 0;
  auto accumulate = [&](int actual, int predicted) {
    log_err_sum += std::log2(std::abs(predicted - actual) + 1.0);
    if (actual > last + 1) {  // a gap ends the current dense run
      long long len = last - run_start + 1;
      shifts_sum += len * len / 4;
      run_start = actual;
    }
    last = actual;
  };
  for (int i = 0; i < n; ++i) {
    int predicted = model.predict_clamped(v[i].first, capacity);
    int actual = std::max(predicted, last + 1);
    if (capacity - actual < n - i) {
      actual = capacity - (n - i);
      for (int j = i; j < n; ++j, ++actual)
        accumulate(actual, model.predict_clamped(v[j].first, capacity));
      break;
    }
    accumulate(actual, predicted);
  }
  long long len = last - run_start + 1;
  shifts_sum += len * len / 4;

  DataNodeStats s{log_err_sum / n, static_cast<double>(shifts_sum) / n};
  if (stats) *stats = s;
  return kExpSearchIterationsWeight * s.search_iterations +
         kShiftsWeight * s.shifts * insert_frac;
}

// ---------------------------------------------------------------------------
// Fanout tree (paper Sec. 4.3.2; reference alex_fanout_tree.h)

struct FTNode {
  int level, idx;
  double cost;
  int left, right;  // key range [left, right) within the node's values
  bool use;
  DataNodeStats stats;
  LinearModel model;  // data node model trained on this partition
  int num_keys;
};

// Cost of splitting the node's keys into 2^level equal-width (in the node's
// model space) partitions, each a data node. `node_model` maps the node's
// key domain to [0, 1).
inline double compute_level(const Value* v, int n, const LinearModel& node_model,
                            double total_keys, std::vector<FTNode>& out, int level,
                            int max_data_node_keys, const Params& p) {
  const int fanout = 1 << level;
  LinearModel scaled = node_model;
  scaled.expand(fanout);
  double cost = 0;
  int left = 0;
  for (int i = 0; i < fanout; ++i) {
    int right = left;
    if (i == fanout - 1) {
      right = n;
    } else {
      while (right < n && scaled.predict_clamped(v[right].first, fanout) <= i) ++right;
    }
    FTNode t{level, i, 0, left, right, false, {}, {}, right - left};
    if (right > left) {
      t.model = fit_data_model(v + left, right - left, p);
      t.cost = expected_cost(v + left, right - left, kInitDensity, p.expected_insert_frac,
                             t.model, &t.stats);
      // Too big for one data node: account for the extra level it will need.
      if (right - left > max_data_node_keys) t.cost += kNodeLookupsWeight;
      cost += t.cost * (right - left) / n;
    }
    out.push_back(t);
    left = right;
  }
  const double footprint = p.data_node_cost_bytes + sizeof(void*);
  cost += kNodeLookupsWeight + kModelSizeWeight * fanout * footprint * total_keys / n;
  return cost;
}

// Merges sibling partitions bottom-up while doing so lowers the cost.
inline double merge_nodes_upwards(int start_level, double best_cost, int n, double total_keys,
                                  const Params& p, std::vector<std::vector<FTNode>>& tree) {
  for (int level = start_level; level >= 1; --level) {
    bool merged = false;
    for (int i = 0; i < (1 << level) / 2; ++i) {
      FTNode& l = tree[level][2 * i];
      FTNode& r = tree[level][2 * i + 1];
      FTNode& parent = tree[level - 1][i];
      if (!l.use || !r.use) continue;
      const double size_saving = kModelSizeWeight * p.data_node_cost_bytes * total_keys;
      if (parent.num_keys == 0) {
        l.use = r.use = false;
        parent.use = merged = true;
        best_cost -= size_saving / n;
        continue;
      }
      double saving = l.cost * l.num_keys / parent.num_keys +
                      r.cost * r.num_keys / parent.num_keys - parent.cost +
                      size_saving / parent.num_keys;
      if (saving >= 0) {
        l.use = r.use = false;
        parent.use = merged = true;
        best_cost -= saving * parent.num_keys / n;
      }
    }
    if (!merged) break;
  }
  return best_cost;
}

// Returns {best depth, best cost} and the chosen partitions in key order.
inline std::pair<int, double> find_best_fanout_bottom_up(
    const Value* v, int n, const LinearModel& node_model, double node_cost,
    const LinearModel& node_data_model, const DataNodeStats& node_stats, double total_keys,
    int max_fanout, int max_data_node_keys, const Params& p, std::vector<FTNode>& used) {
  int best_level = 0;
  double best_cost = node_cost + kNodeLookupsWeight;
  std::vector<double> costs{best_cost};
  std::vector<std::vector<FTNode>> tree;
  // Level 0 is "this node as a single child"; it is only used if merging
  // collapses everything back up, in which case it needs a real model.
  tree.push_back({FTNode{0, 0, best_cost, 0, n, false, node_stats, node_data_model, n}});
  for (int fanout = 2, level = 1; fanout <= max_fanout; fanout *= 2, ++level) {
    std::vector<FTNode> nodes;
    double cost = compute_level(v, n, node_model, total_keys, nodes, level, max_data_node_keys, p);
    costs.push_back(cost);
    size_t c = costs.size();
    // Stop once the cost has risen for two consecutive levels.
    if (c >= 3 && costs[c - 1] > costs[c - 2] && costs[c - 2] > costs[c - 3]) break;
    if (cost < best_cost) {
      best_cost = cost;
      best_level = level;
    }
    tree.push_back(std::move(nodes));
  }
  for (FTNode& t : tree[best_level]) t.use = true;
  best_cost = merge_nodes_upwards(best_level, best_cost, n, total_keys, p, tree);

  used.clear();
  for (int level = 0; level <= best_level; ++level)
    for (const FTNode& t : tree[level])
      if (t.use) used.push_back(t);
  std::sort(used.begin(), used.end(), [&](const FTNode& a, const FTNode& b) {
    return (a.idx << (best_level - a.level)) < (b.idx << (best_level - b.level));
  });
  return {best_level, best_cost};
}

// ---------------------------------------------------------------------------
// The index

class Alex {
 public:
  explicit Alex(Params p = {}) : params_(p) {
    if (p.max_node_bytes < 1024) throw std::invalid_argument("max_node_bytes too small");
    max_data_node_slots_ = static_cast<int>(p.max_node_bytes / sizeof(Value));
    max_fanout_ = static_cast<int>(p.max_node_bytes / sizeof(void*));
  }
  ~Alex() { destroy(root_); }
  Alex(const Alex&) = delete;
  Alex& operator=(const Alex&) = delete;

  void bulk_load(const Value* v, size_t n_in) {
    if (root_) throw std::logic_error("ALEX bulk_load on a non-empty index");
    if (n_in > static_cast<size_t>(std::numeric_limits<int>::max()))
      throw std::invalid_argument("too many keys for a single bulk load");
    const int n = static_cast<int>(n_in);
    if (n > 0 && v[n - 1].first == kEndSentinel)
      throw std::invalid_argument("key UINT64_MAX is reserved by ALEX");
    num_keys_ = n;
    total_keys_ = n;

    LinearModel root_range;  // maps [min_key, max_key] to [0, 1]
    if (n > 1 && v[n - 1].first > v[0].first) {
      root_range.a = 1.0 / (static_cast<double>(v[n - 1].first) - static_cast<double>(v[0].first));
      root_range.b = -static_cast<double>(v[0].first) * root_range.a;
    }
    LinearModel data_model = fit_data_model(v, n, params_);
    DataNodeStats stats;
    double cost = expected_cost(v, n, kInitDensity, params_.expected_insert_frac, data_model, &stats);
    root_ = build(v, n, root_range, cost, data_model, stats, 0);
  }

  bool find(Key k, uint64_t& payload) const {
    if (k == kEndSentinel) return false;  // reserved; trailing gaps hold it
    const DataNode* leaf = leaf_for(k);
    int pos = leaf->find_key(k);
    if (pos < 0) return false;
    payload = leaf->payloads[pos];
    return true;
  }

  const DataNode* leaf_for(Key k) const {
    const Node* cur = root_;
    while (!cur->is_leaf) cur = static_cast<const ModelNode*>(cur)->child_for(k);
    return static_cast<const DataNode*>(cur);
  }

  size_t size() const { return num_keys_; }

  struct Stats {
    size_t model_nodes = 0, data_nodes = 0, max_depth = 0;
    size_t index_bytes = 0, data_bytes = 0;
    double avg_key_depth = 0;  // model nodes traversed, averaged over keys
    double avg_expected_search_iterations = 0;  // cost-model expectation
  };

  Stats stats() const {
    Stats s;
    double depth_sum = 0, iter_sum = 0;
    visit(root_, 0, [&](const Node* node, size_t depth) {
      s.max_depth = std::max(s.max_depth, depth);
      if (node->is_leaf) {
        auto d = static_cast<const DataNode*>(node);
        ++s.data_nodes;
        s.index_bytes += sizeof(DataNode);
        s.data_bytes += d->data_bytes();
        depth_sum += double(depth) * d->num_keys;
        iter_sum += d->expected_avg_exp_search_iterations * d->num_keys;
      } else {
        auto m = static_cast<const ModelNode*>(node);
        ++s.model_nodes;
        s.index_bytes += sizeof(ModelNode) + m->num_children * sizeof(Node*);
      }
    });
    if (num_keys_) {
      s.avg_key_depth = depth_sum / num_keys_;
      s.avg_expected_search_iterations = iter_sum / num_keys_;
    }
    return s;
  }

  // Calls fn(node, depth) once per distinct node (duplicate pointers skipped).
  template <class Fn>
  void visit(const Node* node, size_t depth, Fn&& fn) const {
    if (!node) return;
    fn(node, depth);
    if (node->is_leaf) return;
    auto m = static_cast<const ModelNode*>(node);
    for (int i = 0; i < m->num_children; ++i)
      if (i == 0 || m->children[i] != m->children[i - 1]) visit(m->children[i], depth + 1, fn);
  }

  const Node* root() const { return root_; }

 private:
  // Builds the subtree for v[0..n). `range` maps this node's key domain to
  // [0, 1); `cost` is the expected cost of making it one data node with
  // `data_model`. Follows the reference bulk_load_node.
  Node* build(const Value* v, int n, const LinearModel& range, double cost,
              const LinearModel& data_model, const DataNodeStats& stats, int level) {
    const int max_data_node_keys = static_cast<int>(max_data_node_slots_ * kInitDensity);
    // Models see keys as doubles, so keys that round to the same double can
    // never be routed apart: near 2^63 consecutive doubles are 2048 apart.
    // Splitting such a node would recurse forever, so it becomes one
    // (possibly oversized) data node; lookups stay exact because the
    // exponential search compares integer keys. Likewise for a zero or
    // non-finite range model.
    const bool can_split = n > 1 && range.a > 0 && std::isfinite(range.a) &&
                           std::isfinite(range.b) &&
                           static_cast<double>(v[0].first) < static_cast<double>(v[n - 1].first);

    if (!can_split || (n <= max_data_node_keys && cost < kNodeLookupsWeight)) {
      return make_data_node(v, n, data_model, stats, cost, level);
    }

    std::vector<FTNode> used;
    auto [depth, ft_cost] = find_best_fanout_bottom_up(
        v, n, range, cost, data_model, stats, total_keys_, max_fanout_, max_data_node_keys,
        params_, used);

    if (!(ft_cost < cost || n > max_data_node_keys)) {
      return make_data_node(v, n, data_model, stats, cost, level);
    }

    if (depth == 0) {
      // Fairly uniform but too big for one data node: split to satisfy the
      // node size limit in expectation.
      depth = static_cast<int>(std::log2(double(n) / max_data_node_slots_)) + 1;
      depth = std::min(depth, static_cast<int>(std::log2(double(max_fanout_))));
      used.clear();
      compute_level(v, n, range, total_keys_, used, depth, max_data_node_keys, params_);
    }

    const int fanout = 1 << depth;
    auto* node = new ModelNode(level);
    node->model = range;
    node->model.expand(fanout);
    node->num_children = fanout;
    node->children.reset(new Node*[fanout]());
    node->cost = cost;

    int cur = 0;
    for (const FTNode& t : used) {
      const int dup = depth - t.level;
      const int repeats = 1 << dup;
      // Child key domain = the slice of this node's domain it is routed.
      double lb = (double(cur) / fanout - range.b) / range.a;
      double rb = (double(cur + repeats) / fanout - range.b) / range.a;
      LinearModel child_range;
      if (rb > lb) {
        child_range.a = 1.0 / (rb - lb);
        child_range.b = -child_range.a * lb;
      }
      Node* child = build(v + t.left, t.num_keys, child_range, t.cost, t.model, t.stats, level + 1);
      child->duplication_factor = static_cast<uint8_t>(dup);
      for (int i = cur; i < cur + repeats; ++i) node->children[i] = child;
      cur += repeats;
    }
    if (cur != fanout) throw std::logic_error("ALEX fanout tree did not cover all slots");
    return node;
  }

  DataNode* make_data_node(const Value* v, int n, const LinearModel& model,
                           const DataNodeStats& stats, double cost, int level) {
    auto* d = new DataNode(level);
    d->bulk_load(v, n, &model);
    d->cost = cost;
    d->expected_avg_exp_search_iterations = stats.search_iterations;
    d->expected_avg_shifts = stats.shifts;
    return d;
  }

  void destroy(Node* node) {
    if (!node) return;
    if (node->is_leaf) {
      delete static_cast<DataNode*>(node);
      return;
    }
    auto* m = static_cast<ModelNode*>(node);
    for (int i = 0; i < m->num_children; ++i)
      if (i == 0 || m->children[i] != m->children[i - 1]) destroy(m->children[i]);
    delete m;
  }

  Params params_;
  int max_data_node_slots_;
  int max_fanout_;
  Node* root_ = nullptr;
  size_t num_keys_ = 0;
  double total_keys_ = 0;
};

}  // namespace alex
}  // namespace cop5725
