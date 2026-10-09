// Self-contained tests (no framework). Run: ./build/tests

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "datasets.h"
#include "indexes.h"
#include "measure.h"
#include "workloads.h"

using namespace cop5725;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

static void test_synthetic_datasets() {
  for (const char* d : {"uniform", "lognormal", "normal", "sequential"}) {
    auto keys = generate_synthetic(d, 50'000, 7);
    CHECK(keys.size() == 50'000);
    std::set<Key> uniq(keys.begin(), keys.end());
    CHECK(uniq.size() == keys.size());
    CHECK(!std::is_sorted(keys.begin(), keys.end()));  // shuffled
    CHECK(generate_synthetic(d, 50'000, 7) == keys);    // deterministic
  }
}

static void test_sosd_loader() {
  const std::string p64 = "/tmp/cop5725_test_uint64", p32 = "/tmp/cop5725_test_uint32";
  {
    std::ofstream o(p64, std::ios::binary);
    uint64_t n = 4, k[] = {5, 1, 5, 9};
    o.write((char*)&n, 8);
    o.write((char*)k, sizeof(k));
  }
  {
    std::ofstream o(p32, std::ios::binary);
    uint64_t n = 3;
    uint32_t k[] = {7, 3, 4000000000u};
    o.write((char*)&n, 8);
    o.write((char*)k, sizeof(k));
  }
  auto a = load_sosd(p64);
  CHECK((a == std::vector<Key>{5, 1, 5, 9}));
  auto b = load_dataset(p64, 0, 1);  // deduplicated
  std::sort(b.begin(), b.end());
  CHECK((b == std::vector<Key>{1, 5, 9}));
  auto c = load_sosd(p32);
  CHECK((c == std::vector<Key>{7, 3, 4000000000ull}));
  std::remove(p64.c_str());
  std::remove(p32.c_str());
}

// Checks invariants every workload must satisfy and returns inserted keys.
static std::vector<Key> check_workload(const std::vector<Key>& keys, size_t init,
                                       const Workload& w) {
  std::set<Key> present;
  for (size_t i = 0; i < init; ++i) present.insert(keys[i]);
  CHECK(w.init.size() == init);
  CHECK(std::is_sorted(w.init.begin(), w.init.end()));
  std::vector<Key> inserted;
  for (const Op& op : w.ops) {
    if (op.type == OpType::Lookup) {
      CHECK(present.count(op.key) == 1);  // lookups only hit existing keys
    } else {
      CHECK(present.insert(op.key).second);  // inserts are always new keys
      inserted.push_back(op.key);
    }
  }
  CHECK(inserted.size() == w.num_inserts);
  return inserted;
}

static void test_mixes() {
  auto keys = generate_synthetic("lognormal", 200'000, 3);
  for (auto [mix, lo, hi] : {std::tuple{"readonly", 0.0, 0.0},
                             std::tuple{"readheavy", 0.04, 0.06},
                             std::tuple{"writeheavy", 0.48, 0.52},
                             std::tuple{"writeonly", 1.0, 1.0}}) {
    WorkloadSpec s;
    s.num_ops = 100'000;
    s.insert_frac = mix_insert_fraction(mix);
    Workload w = generate_workload(keys, 100'000, s);
    check_workload(keys, 100'000, w);
    double f = double(w.num_inserts) / s.num_ops;
    CHECK(f >= lo && f <= hi);
  }
}

static void test_zipf_skew() {
  ZipfGenerator z(0.99);
  std::mt19937_64 rng(1);
  std::vector<size_t> hits(1000);
  for (int i = 0; i < 200'000; ++i) ++hits[z.next(rng, 1000)];
  CHECK(hits[0] > hits[10] && hits[10] > hits[500]);
  // Growing n must keep ranks in range.
  ZipfGenerator g(0.99);
  for (size_t n = 1; n < 2000; ++n) CHECK(g.next(rng, n) < n);
}

// Fraction of keys whose quantile among the sorted init keys is in [lo, hi].
static double frac_in_quantiles(const std::vector<Key>& sorted, const std::vector<Key>& ks,
                                double lo, double hi) {
  Key a = sorted[size_t(lo * (sorted.size() - 1))];
  Key b = sorted[size_t(hi * (sorted.size() - 1))];
  size_t c = 0;
  for (Key k : ks) c += (k >= a && k <= b);
  return ks.empty() ? 0 : double(c) / ks.size();
}

static void test_patterns() {
  auto keys = generate_synthetic("lognormal", 400'000, 5);
  const size_t init = 200'000;
  std::vector<Key> sorted(keys.begin(), keys.begin() + init);
  std::sort(sorted.begin(), sorted.end());

  {  // hot range: second half of inserts concentrate more than the first half
    WorkloadSpec s;
    s.num_ops = 100'000;
    s.insert_frac = 1.0;
    s.pattern = InsertPattern::HotRange;
    s.hot_center = 0.3;
    s.hot_width = 0.02;
    Workload w = generate_workload(keys, init, s);
    auto ins = check_workload(keys, init, w);
    std::vector<Key> first(ins.begin(), ins.begin() + ins.size() / 4);
    std::vector<Key> last(ins.end() - ins.size() / 4, ins.end());
    double f1 = frac_in_quantiles(sorted, first, 0.29, 0.31);
    double f2 = frac_in_quantiles(sorted, last, 0.29, 0.31);
    CHECK(f1 < 0.3);
    CHECK(f2 > 0.8);
  }
  {  // drift: the insert centre moves from q=0.1 towards q=0.9
    WorkloadSpec s;
    s.num_ops = 100'000;
    s.insert_frac = 1.0;
    s.pattern = InsertPattern::Drift;
    s.drift_from = 0.1;
    s.drift_rate = 8.0;  // 0.8 quantile units over 100k inserts
    s.drift_sigma = 0.002;
    Workload w = generate_workload(keys, init, s);
    auto ins = check_workload(keys, init, w);
    std::vector<Key> first(ins.begin(), ins.begin() + 1000);
    std::vector<Key> last(ins.end() - 1000, ins.end());
    CHECK(frac_in_quantiles(sorted, first, 0.09, 0.12) > 0.9);
    CHECK(frac_in_quantiles(sorted, last, 0.88, 0.91) > 0.9);
  }
  {  // regime: dataset keys, then a sudden switch to a narrow band
    WorkloadSpec s;
    s.num_ops = 100'000;
    s.insert_frac = 0.5;
    s.pattern = InsertPattern::Regime;
    s.regime_at = 0.5;
    s.regime_center = 0.7;
    s.regime_sigma = 0.001;
    Workload w = generate_workload(keys, init, s);
    auto ins = check_workload(keys, init, w);
    std::vector<Key> before(ins.begin(), ins.begin() + ins.size() / 2 - 100);
    std::vector<Key> after(ins.begin() + ins.size() / 2 + 100, ins.end());
    CHECK(frac_in_quantiles(sorted, before, 0.69, 0.71) < 0.1);
    CHECK(frac_in_quantiles(sorted, after, 0.69, 0.71) > 0.95);
  }
  {  // regime with append: keys beyond the max, strictly increasing
    WorkloadSpec s;
    s.num_ops = 20'000;
    s.insert_frac = 1.0;
    s.pattern = InsertPattern::Regime;
    s.regime_at = 0.0;
    s.regime_append = true;
    Workload w = generate_workload(keys, init, s);
    auto ins = check_workload(keys, init, w);
    CHECK(ins.front() > sorted.back());
    CHECK(std::is_sorted(ins.begin(), ins.end()));
  }
}

template <class Index>
static void test_index_against_oracle() {
  {
    Index idx;
    std::map<Key, uint64_t> oracle;
    auto keys = generate_synthetic("lognormal", 60'000, 11);
    std::vector<std::pair<Key, uint64_t>> init;
    for (size_t i = 0; i < 30'000; ++i) init.emplace_back(keys[i], payload_for(keys[i]));
    std::sort(init.begin(), init.end());
    idx.bulk_load(init.data(), init.size());
    oracle.insert(init.begin(), init.end());

    std::mt19937_64 rng(2);
    for (size_t i = 30'000; i < 60'000; ++i) {
      CHECK(idx.insert(keys[i], payload_for(keys[i])));
      oracle.emplace(keys[i], payload_for(keys[i]));
      CHECK(!idx.insert(keys[i], 0));  // duplicates rejected
      Key probe = keys[rng() % (i + 1)];
      uint64_t v = 0;
      CHECK(idx.find(probe, v) && v == oracle[probe]);
    }
    uint64_t v;
    CHECK(!idx.find(Key(1) << 62, v) || oracle.count(Key(1) << 62));
    CHECK(idx.size() == oracle.size());
    MemoryUsage m = idx.memory();
    CHECK(m.data_bytes >= oracle.size() * 16);
    CHECK(m.index_bytes > 0);
  }
  // All memory returned once the index is destroyed.
  Index empty;
  CHECK(empty.memory().total() == 0);
}


// Builds sorted (key, payload) pairs from keys.
static std::vector<std::pair<Key, uint64_t>> make_init(std::vector<Key> keys) {
  std::sort(keys.begin(), keys.end());
  std::vector<std::pair<Key, uint64_t>> v;
  for (Key k : keys) v.emplace_back(k, payload_for(k));
  return v;
}

// Every present key is found with its payload; keys that are absent (between
// neighbours, below the minimum, above the maximum) are not.
template <class Index>
static void check_read_path(const Index& idx, const std::vector<std::pair<Key, uint64_t>>& init) {
  size_t bad = 0;
  uint64_t v;
  for (const auto& kv : init) bad += !(idx.find(kv.first, v) && v == kv.second);
  CHECK(bad == 0);
  if (init.empty()) {
    CHECK(!idx.find(12345, v));
    return;
  }
  size_t false_hits = 0;
  for (size_t i = 0; i + 1 < init.size(); ++i)
    if (init[i + 1].first - init[i].first > 1) false_hits += idx.find(init[i].first + 1, v);
  if (init.front().first > 0) false_hits += idx.find(init.front().first - 1, v);
  if (init.front().first > 0) false_hits += idx.find(0, v);
  false_hits += idx.find(init.back().first + 1, v);
  const Key near_max = std::numeric_limits<Key>::max() - 1;
  if (!std::binary_search(init.begin(), init.end(), std::make_pair(near_max, payload_for(near_max))))
    false_hits += idx.find(near_max, v);
  CHECK(false_hits == 0);
}

static std::vector<std::vector<Key>> read_path_key_sets() {
  std::vector<std::vector<Key>> sets;
  for (const char* d : {"uniform", "lognormal", "normal", "sequential"})
    for (size_t n : {size_t(1), size_t(2), size_t(3), size_t(1000), size_t(150'000)})
      sets.push_back(generate_synthetic(d, n, 21));
  sets.push_back({});
  sets.push_back({0, 1, 2, 3});
  sets.push_back({0, std::numeric_limits<Key>::max() - 1});  // extreme range
  // Two far-apart dense clusters: a stress case for linear models.
  std::vector<Key> clusters;
  for (Key i = 0; i < 20'000; ++i) clusters.push_back(1000 + i);
  for (Key i = 0; i < 20'000; ++i) clusters.push_back((Key(1) << 62) + i * 7);
  sets.push_back(clusters);
  // Keys clustered around 2^63, where double rounding is coarsest.
  std::vector<Key> high;
  for (Key i = 0; i < 50'000; ++i) high.push_back((Key(1) << 63) + i * 3);
  sets.push_back(high);
  return sets;
}

static void test_rmi() {
  for (const auto& keys : read_path_key_sets()) {
    for (size_t models : {size_t(0), size_t(1), size_t(64), size_t(1) << 16}) {
      auto init = make_init(keys);
      RMIIndex idx(IndexOptions{models, {}, false});
      idx.bulk_load(init.data(), init.size());
      check_read_path(idx, init);
      CHECK(!idx.insert(1, 1));  // read-only
    }
  }
}

// Checks ALEX's structural invariants and returns the number of keys found.
static size_t check_alex_structure(const alex::Alex& a, const alex::Node* node, Key lo, Key hi) {
  using namespace alex;
  if (node->is_leaf) {
    auto d = static_cast<const DataNode*>(node);
    size_t real = 0;
    Key prev = 0;
    for (int i = 0; i < d->capacity; ++i) {
      CHECK(i == 0 || d->keys[i] >= prev);  // non-decreasing, so search works
      prev = d->keys[i];
      if (d->is_occupied(i)) {
        ++real;
        CHECK(d->keys[i] >= lo && d->keys[i] <= hi);
      } else {
        // A gap holds the next real key to its right, or the end sentinel.
        int j = i + 1;
        while (j < d->capacity && !d->is_occupied(j)) ++j;
        CHECK(d->keys[i] == (j < d->capacity ? d->keys[j] : kEndSentinel));
      }
    }
    CHECK(real == size_t(d->num_keys));
    CHECK(d->num_keys <= d->capacity);
    return real;
  }
  auto m = static_cast<const ModelNode*>(node);
  CHECK(m->num_children >= 2 && (m->num_children & (m->num_children - 1)) == 0);
  size_t total = 0;
  for (int i = 0; i < m->num_children;) {
    const Node* c = m->children[i];
    CHECK(c != nullptr);
    if (!c) return total;
    int repeats = 1 << c->duplication_factor;
    CHECK(i % repeats == 0);  // duplicate runs are aligned powers of two
    for (int j = i; j < i + repeats && j < m->num_children; ++j) CHECK(m->children[j] == c);
    CHECK(c->level == node->level + 1);
    total += check_alex_structure(a, c, lo, hi);
    i += repeats;
  }
  return total;
}

static void test_alex_read_path() {
  for (const auto& keys : read_path_key_sets()) {
    // Default 16 MB nodes, and small nodes that force deep trees and the
    // "split to satisfy the size limit" path.
    for (size_t node_bytes : {size_t(1) << 24, size_t(4096), size_t(1024)}) {
      auto init = make_init(keys);
      IndexOptions o;
      o.alex.max_node_bytes = node_bytes;
      AlexIndex idx(o);
      idx.bulk_load(init.data(), init.size());
      check_read_path(idx, init);
      CHECK(!idx.insert(1, 1));  // inserts arrive in weeks 7-8
      CHECK(idx.size() == init.size());
      size_t found = check_alex_structure(idx.tree(), idx.tree().root(), 0,
                                          std::numeric_limits<Key>::max() - 1);
      CHECK(found == init.size());
      // Every key routes to the data node that physically holds it.
      for (size_t i = 0; i < init.size(); i += 97) {
        const alex::DataNode* d = idx.tree().leaf_for(init[i].first);
        CHECK(d->find_key(init[i].first) >= 0);
      }
      std::set<double> as_doubles;
      for (const auto& kv : init) as_doubles.insert(double(kv.first));
      if (node_bytes == 1024 && init.size() > 1000 && as_doubles.size() == init.size()) {
        auto s = idx.tree().stats();
        CHECK(s.max_depth >= 2);  // small nodes really do force a deeper tree
      }
    }
  }
  // The end sentinel is reserved.
  auto bad = make_init({1, std::numeric_limits<Key>::max()});
  AlexIndex idx;
  bool threw = false;
  try {
    idx.bulk_load(bad.data(), bad.size());
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

// The cost model's search-iteration term matches the iterations actually
// recorded for a data node, and is zero for perfectly linear keys.
static void test_alex_cost_model() {
  std::vector<std::pair<Key, uint64_t>> linear;
  for (Key i = 0; i < 10'000; ++i) linear.emplace_back(100 + 5 * i, 0);
  auto model = fit_linear(linear.size(), [&](size_t i) { return linear[i].first; });
  alex::DataNodeStats s;
  double cost = alex::expected_cost(linear.data(), int(linear.size()), alex::kInitDensity, 0.0,
                                    model, &s);
  CHECK(s.search_iterations < 1e-9);
  CHECK(cost < 1e-6);
  // With inserts expected, densely packed runs add shift cost.
  alex::expected_cost(linear.data(), int(linear.size()), 1.0, 1.0, model, &s);
  CHECK(s.shifts > 1000);  // density 1: one long run of 10k keys -> n/4 shifts
  // Lognormal keys are harder to model than linear ones.
  auto ln = make_init(generate_synthetic("lognormal", 10'000, 4));
  auto m2 = fit_linear(ln.size(), [&](size_t i) { return ln[i].first; });
  alex::expected_cost(ln.data(), int(ln.size()), alex::kInitDensity, 0.0, m2, &s);
  CHECK(s.search_iterations > 1.0);
}

static void test_linear_model() {
  // Exact on a line of huge keys. The spacing (2^20) is above the double
  // rounding step at 2^62 (2^10); predictions see keys as doubles, so no
  // model can resolve keys closer together than that.
  std::vector<Key> xs;
  for (Key i = 0; i < 1000; ++i) xs.push_back((Key(1) << 62) + (i << 20));
  auto m = fit_linear(xs.size(), [&](size_t i) { return xs[i]; });
  for (size_t i = 0; i < xs.size(); i += 37) CHECK(std::abs(m.predict_double(xs[i]) - double(i)) < 0.5);
  // A narrow range far from zero.
  std::vector<Key> ys;
  for (Key i = 0; i < 1000; ++i) ys.push_back(Key(1) << 52 | i);
  auto m2 = fit_linear(ys.size(), [&](size_t i) { return ys[i]; });
  for (size_t i = 0; i < ys.size(); i += 37) CHECK(std::abs(m2.predict_double(ys[i]) - double(i)) < 0.5);
  LinearModel big{1e300, 0};
  CHECK(big.predict_clamped(5, 10) == 9);  // no overflow on huge predictions
  LinearModel neg{-1, 0};
  CHECK(neg.predict_clamped(5, 10) == 0);
  LinearModel nan{std::numeric_limits<double>::quiet_NaN(), 0};
  CHECK(nan.predict_clamped(5, 10) == 0);
}

static void test_percentile() {
  std::vector<uint64_t> v;
  for (uint64_t i = 1; i <= 100; ++i) v.push_back(101 - i);
  CHECK(percentile(v, 50) == 51);
  CHECK(percentile(v, 99) == 100);
  CHECK(percentile(v, 0) == 1);
}

int main() {
  struct { const char* name; void (*fn)(); } tests[] = {
      {"synthetic datasets", test_synthetic_datasets},
      {"SOSD loader", test_sosd_loader},
      {"read/write mixes", test_mixes},
      {"zipf skew", test_zipf_skew},
      {"hotrange/drift/regime patterns", test_patterns},
      {"stdmap vs oracle", test_index_against_oracle<StdMapIndex>},
      {"btree vs oracle", test_index_against_oracle<BTreeIndex>},
      {"percentile", test_percentile},
      {"linear model", test_linear_model},
      {"RMI read path", test_rmi},
      {"ALEX cost model", test_alex_cost_model},
      {"ALEX read path + structure", test_alex_read_path},
  };
  for (auto& t : tests) {
    int before = failures;
    t.fn();
    std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
  }
  std::printf(failures ? "\n%d check(s) failed\n" : "\nall tests passed\n", failures);
  return failures ? 1 : 0;
}
