// Index wrappers with a common interface so the benchmark driver is generic.
//
// Every index type provides:
//   explicit Index(const IndexOptions&);
//   static const char* name();
//   static bool supports_inserts();
//   void bulk_load(const std::pair<Key, uint64_t>* sorted, size_t n);
//   bool find(Key k, uint64_t& payload) const;
//   bool insert(Key k, uint64_t payload);
//   size_t size() const;
//   MemoryUsage memory() const;
//   std::vector<Counter> counters() const;   // structure stats, may be empty
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <tlx/container/btree_map.hpp>

#include "alex.h"
#include "datasets.h"
#include "rmi.h"

namespace cop5725 {

// index_bytes: navigation structure only (what the ALEX paper calls "index
// size"; for a B+ tree these are the inner nodes).
// data_bytes: storage holding the keys and payloads (B+ tree leaves).
struct MemoryUsage {
  size_t index_bytes = 0;
  size_t data_bytes = 0;
  size_t total() const { return index_bytes + data_bytes; }
};

struct Counter {
  std::string name;
  uint64_t value;
};

// Per-index tuning knobs, set from the command line.
struct IndexOptions {
  size_t rmi_models = 0;  // 0 = default (about one model per 128 keys)
  alex::Params alex;
  bool ref_approximate = false;  // reference ALEX only: sampled model fitting
};

// ---------------------------------------------------------------------------
// Counting allocator: exact heap accounting per index type. Allocations of
// node types that have a `childid` member (tlx inner nodes) are tracked
// separately so the B+ tree's inner/leaf split is measured, not estimated.
// Counters are per Tag, so only one live instance per index type is measured
// correctly; the benchmark only ever builds one at a time.

template <class Tag>
struct MemCounter {
  static inline size_t inner_bytes = 0;
  static inline size_t other_bytes = 0;
};

template <class T, class = void>
struct is_inner_node : std::false_type {};
template <class T>
struct is_inner_node<T, std::void_t<decltype(&T::childid)>> : std::true_type {};

template <class T, class Tag>
struct CountingAllocator {
  using value_type = T;
  template <class U>
  struct rebind {
    using other = CountingAllocator<U, Tag>;
  };

  CountingAllocator() noexcept = default;
  template <class U>
  CountingAllocator(const CountingAllocator<U, Tag>&) noexcept {}

  T* allocate(size_t n) {
    bytes() += n * sizeof(T);
    return std::allocator<T>{}.allocate(n);
  }
  void deallocate(T* p, size_t n) noexcept {
    bytes() -= n * sizeof(T);
    std::allocator<T>{}.deallocate(p, n);
  }

  static size_t& bytes() {
    return is_inner_node<T>::value ? MemCounter<Tag>::inner_bytes
                                   : MemCounter<Tag>::other_bytes;
  }

  template <class U>
  bool operator==(const CountingAllocator<U, Tag>&) const noexcept { return true; }
  template <class U>
  bool operator!=(const CountingAllocator<U, Tag>&) const noexcept { return false; }
};

// ---------------------------------------------------------------------------
// std::map (red-black tree): the "conventional ordered map" baseline.

class StdMapIndex {
  struct Tag {};
  using Value = std::pair<const Key, uint64_t>;
  using Map = std::map<Key, uint64_t, std::less<Key>, CountingAllocator<Value, Tag>>;

 public:
  explicit StdMapIndex(const IndexOptions& = {}) {}
  static const char* name() { return "stdmap"; }
  static bool supports_inserts() { return true; }

  void bulk_load(const std::pair<Key, uint64_t>* sorted, size_t n) {
    for (size_t i = 0; i < n; ++i) map_.emplace_hint(map_.end(), sorted[i]);
  }
  bool find(Key k, uint64_t& payload) const {
    auto it = map_.find(k);
    if (it == map_.end()) return false;
    payload = it->second;
    return true;
  }
  bool insert(Key k, uint64_t payload) { return map_.emplace(k, payload).second; }
  size_t size() const { return map_.size(); }

  // Every node holds one key/payload; the rest of each node (colour, parent
  // and child pointers) counts as index overhead.
  MemoryUsage memory() const {
    MemoryUsage m;
    m.data_bytes = map_.size() * sizeof(std::pair<Key, uint64_t>);
    m.index_bytes = MemCounter<Tag>::other_bytes - m.data_bytes;
    return m;
  }
  std::vector<Counter> counters() const { return {}; }

 private:
  Map map_;
};

// ---------------------------------------------------------------------------
// tlx B+ tree (successor of the STX B+ tree used in the ALEX paper), with
// tlx's default node sizes (256-byte nodes, 16 slots for 8-byte keys).

class BTreeIndex {
  struct Tag {};
  using Value = std::pair<Key, uint64_t>;
  using Tree = tlx::btree_map<Key, uint64_t, std::less<Key>,
                              tlx::btree_default_traits<Key, Value>,
                              CountingAllocator<Value, Tag>>;

 public:
  explicit BTreeIndex(const IndexOptions& = {}) {}
  static const char* name() { return "btree"; }
  static bool supports_inserts() { return true; }

  void bulk_load(const std::pair<Key, uint64_t>* sorted, size_t n) {
    tree_.bulk_load(sorted, sorted + n);
  }
  bool find(Key k, uint64_t& payload) const {
    auto it = tree_.find(k);
    if (it == tree_.end()) return false;
    payload = it->second;
    return true;
  }
  bool insert(Key k, uint64_t payload) { return tree_.insert(Value(k, payload)).second; }
  size_t size() const { return tree_.size(); }

  MemoryUsage memory() const {
    return {MemCounter<Tag>::inner_bytes, MemCounter<Tag>::other_bytes};
  }
  std::vector<Counter> counters() const {
    const auto& s = tree_.get_stats();
    return {{"leaves", s.leaves}, {"inner_nodes", s.inner_nodes}};
  }

 private:
  Tree tree_;
};

// ---------------------------------------------------------------------------
// Two-stage RMI: the read-only "Learned Index" baseline from the ALEX paper.

class RMIIndex {
 public:
  explicit RMIIndex(const IndexOptions& o = {}) : rmi_(o.rmi_models) {}
  static const char* name() { return "rmi"; }
  static bool supports_inserts() { return false; }

  void bulk_load(const std::pair<Key, uint64_t>* sorted, size_t n) { rmi_.bulk_load(sorted, n); }
  bool find(Key k, uint64_t& payload) const { return rmi_.find(k, payload); }
  bool insert(Key, uint64_t) { return false; }  // read-only
  size_t size() const { return rmi_.size(); }
  MemoryUsage memory() const { return {rmi_.index_bytes(), rmi_.data_bytes()}; }
  std::vector<Counter> counters() const {
    return {{"models", rmi_.num_models()},
            {"avg_log2_window_x1000", uint64_t(rmi_.avg_log2_window() * 1000)}};
  }

 private:
  RMI rmi_;
};

// ---------------------------------------------------------------------------
// ALEX. Inserts arrive in weeks 7-8; until then it is bulk load + lookup.

class AlexIndex {
 public:
  explicit AlexIndex(const IndexOptions& o = {}) : alex_(o.alex) {}
  static const char* name() { return "alex"; }
  static bool supports_inserts() { return false; }

  void bulk_load(const std::pair<Key, uint64_t>* sorted, size_t n) {
    alex_.bulk_load(sorted, n);
    stats_ = alex_.stats();
  }
  bool find(Key k, uint64_t& payload) const { return alex_.find(k, payload); }
  bool insert(Key, uint64_t) { return false; }
  size_t size() const { return alex_.size(); }
  // Same accounting as the reference model_size()/data_size(): node
  // metadata, models and child pointers are index; slots (gaps included)
  // and bitmaps are data.
  MemoryUsage memory() const { return {stats_.index_bytes, stats_.data_bytes}; }
  std::vector<Counter> counters() const {
    return {{"model_nodes", stats_.model_nodes},
            {"data_nodes", stats_.data_nodes},
            {"max_depth", stats_.max_depth},
            {"avg_depth_x1000", uint64_t(stats_.avg_key_depth * 1000)},
            {"exp_search_iters_x1000", uint64_t(stats_.avg_expected_search_iterations * 1000)}};
  }
  const alex::Alex& tree() const { return alex_; }

 private:
  alex::Alex alex_;
  alex::Alex::Stats stats_;
};

}  // namespace cop5725
