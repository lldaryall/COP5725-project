// Benchmark driver: bulk load an index, run a pre-generated workload, and
// report throughput, latency percentiles, memory, and hardware counters.
//
// Example:
//   ./build/bench --index btree --dataset synthetic:lognormal --n 2000000
//       --init 1000000 --ops 2000000 --mix writeheavy --csv results/runs.csv

#include <sys/resource.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "datasets.h"
#include "indexes.h"
#include "measure.h"
#include "workloads.h"
#ifdef COP5725_ALEX_REF
#include "alex_reference.h"
#endif

using namespace cop5725;

namespace {

const char* kUsage = R"(usage: bench [options]
  --index NAME          stdmap | btree | rmi | alex | alexref   (required)
                        (alexref only in `make reference` builds)
  --dataset SPEC        synthetic:{uniform,lognormal,normal,sequential}
                        or a SOSD file path                     (required)
  --n N                 keys to keep from the dataset (0 = all)  [0]
  --init N|all          keys to bulk load (all = every unique key) (required)
  --ops N               operations after bulk load               [1000000]
  --mix NAME            readonly | readheavy | writeheavy | writeonly
  --insert-frac F       insert fraction (overrides --mix)        [0]
  --lookup DIST         zipf | uniform                           [zipf]
  --zipf-theta F                                                 [0.99]
  --pattern NAME        dataset | hotrange | drift | regime      [dataset]
    --hot-center F --hot-width F --hot-frac-start F --hot-frac-end F
    --drift-start F --drift-from F --drift-rate F --drift-sigma F
    --regime-at F --regime-center F --regime-sigma F --regime-append 0|1
  --seed N                                                       [42]
  --windows N           throughput samples over the run          [100]
  --latency-every N     time every Nth op for percentiles, 0=off [16]
  --verify 0|1          check every key afterwards               [1]
  --csv FILE            append a summary row
  --series FILE         append one row per window (throughput over time)
  --label TEXT          free-form tag copied into the CSV rows
  --rmi-models N        RMI second-stage models, 0 = auto       [0]
  --alex-insert-frac F  ALEX cost model expected insert fraction [1]
  --alex-max-node-bytes N  ALEX max node size                   [16777216]
  --ref-approx 0|1      alexref: sampled model fitting           [0]
  --alex-ref-fit 0|1    alex: fit models exactly like the reference [0]
)";

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "-h" || a == "--help") {
        std::cout << kUsage;
        std::exit(0);
      }
      if (a.rfind("--", 0) != 0 || i + 1 >= argc)
        throw std::runtime_error("bad argument: " + a);
      kv_[a.substr(2)] = argv[++i];
    }
  }
  bool has(const std::string& k) const { return kv_.count(k) != 0; }
  std::string str(const std::string& k, const std::string& def = "") const {
    used_.insert(k);
    auto it = kv_.find(k);
    if (it != kv_.end()) return it->second;
    if (def.empty()) throw std::runtime_error("missing required --" + k);
    return def;
  }
  double num(const std::string& k, double def) const {
    used_.insert(k);
    auto it = kv_.find(k);
    return it == kv_.end() ? def : std::stod(it->second);
  }
  void check_all_used() const {
    for (const auto& [k, v] : kv_)
      if (!used_.count(k)) throw std::runtime_error("unknown option --" + k);
  }

 private:
  std::map<std::string, std::string> kv_;
  mutable std::set<std::string> used_;
};

struct Config {
  std::string index, dataset, label, csv, series;
  size_t n, init, windows, latency_every;
  bool verify;
  WorkloadSpec spec;
  IndexOptions index_options;
};

struct WindowSample {
  size_t ops_done, inserts_done;
  double mops;
  MemoryUsage mem;
  std::vector<Counter> counters;
};

struct Result {
  double bulk_load_s = 0, run_s = 0;
  uint64_t p50 = 0, p99 = 0, p999 = 0, max_lat = 0;
  size_t lookup_misses = 0, insert_failures = 0, verify_failures = 0;
  uint64_t checksum = 0;
  MemoryUsage mem;
  std::vector<Counter> counters;
  PerfReading perf;
  bool perf_ok = false;
  std::vector<WindowSample> windows;
};

std::string counters_str(const std::vector<Counter>& cs) {
  std::ostringstream o;
  for (size_t i = 0; i < cs.size(); ++i) o << (i ? ";" : "") << cs[i].name << "=" << cs[i].value;
  return o.str();
}

std::string dataset_name(const std::string& spec) {
  size_t slash = spec.find_last_of('/');
  return slash == std::string::npos ? spec : spec.substr(slash + 1);
}

template <class Index>
Result run(const Workload& w, const Config& cfg) {
  Result r;
  if (!Index::supports_inserts() && w.num_inserts > 0)
    throw std::runtime_error(std::string(Index::name()) +
                             " does not support inserts yet; use --mix readonly");
  Index idx(cfg.index_options);

  uint64_t t0 = now_ns();
  idx.bulk_load(w.init.data(), w.init.size());
  r.bulk_load_s = (now_ns() - t0) / 1e9;

  const auto& ops = w.ops;
  const size_t window = std::max<size_t>(1, (ops.size() + cfg.windows - 1) / cfg.windows);
  std::vector<uint64_t> lat;
  if (cfg.latency_every) lat.reserve(ops.size() / cfg.latency_every + 1);

  PerfCounters perf;
  r.perf_ok = perf.available();
  uint64_t checksum = 0, total_ns = 0;
  size_t misses = 0, insert_failures = 0, inserts_done = 0;
  const size_t every = cfg.latency_every;

  perf.start();
  for (size_t begin = 0; begin < ops.size(); begin += window) {
    const size_t end = std::min(ops.size(), begin + window);
    const uint64_t wstart = now_ns();
    for (size_t i = begin; i < end; ++i) {
      const Op& op = ops[i];
      const bool sample = every != 0 && i % every == 0;
      const uint64_t ts = sample ? now_ns() : 0;
      if (op.type == OpType::Lookup) {
        uint64_t v;
        if (idx.find(op.key, v)) checksum += v;
        else ++misses;
      } else {
        if (!idx.insert(op.key, payload_for(op.key))) ++insert_failures;
        ++inserts_done;
      }
      if (sample) lat.push_back(now_ns() - ts);
    }
    const uint64_t wns = now_ns() - wstart;
    total_ns += wns;
    // Memory and structure stats are read outside the timed region.
    r.windows.push_back({end, inserts_done, (end - begin) / (wns / 1e3),
                         idx.memory(), idx.counters()});
  }
  r.perf = perf.stop();

  r.run_s = total_ns / 1e9;
  r.checksum = checksum;
  r.lookup_misses = misses;
  r.insert_failures = insert_failures;
  r.p50 = percentile(lat, 50);
  r.p99 = percentile(lat, 99);
  r.p999 = percentile(lat, 99.9);
  r.max_lat = percentile(lat, 100);
  r.mem = idx.memory();
  r.counters = idx.counters();

  if (cfg.verify) {
    uint64_t v;
    for (const auto& kv : w.init)
      if (!idx.find(kv.first, v) || v != kv.second) ++r.verify_failures;
    for (const Op& op : ops)
      if (op.type == OpType::Insert &&
          (!idx.find(op.key, v) || v != payload_for(op.key)))
        ++r.verify_failures;
    if (idx.size() != w.init.size() + w.num_inserts) ++r.verify_failures;
  }
  return r;
}

const char* kSummaryHeader =
    "label,index,dataset,pattern,init_keys,ops,inserts,insert_frac,lookup,seed,"
    "bulk_load_s,throughput_mops,p50_ns,p99_ns,p999_ns,max_ns,"
    "index_bytes,data_bytes,cycles_per_op,instr_per_op,llc_miss_per_op,"
    "branch_miss_per_op,l1d_miss_per_op,fallbacks,errors,peak_rss_mb,counters";
const char* kSeriesHeader =
    "label,index,dataset,pattern,window,ops_done,inserts_done,window_mops,"
    "index_bytes,data_bytes,counters";

// Opens `path` for appending, writing `header` if the file is new. Refuses to
// append to a file whose header differs (written by an older version), so
// columns can never silently misalign.
std::ofstream open_csv(const std::string& path, const char* header) {
  std::string first;
  {
    std::ifstream in(path);
    if (in) std::getline(in, first);
  }
  if (!first.empty() && first != header)
    throw std::runtime_error(path + " has a different CSV header (older version?); "
                             "write to a new file");
  std::ofstream out(path, std::ios::app);
  if (!out) throw std::runtime_error("cannot write " + path);
  if (first.empty()) out << header << '\n';
  return out;
}

// Peak resident set size of this process in MB.
double peak_rss_mb() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
  return ru.ru_maxrss / 1e6;  // bytes
#else
  return ru.ru_maxrss / 1e3;  // kilobytes
#endif
}

void write_summary(const Config& c, const Workload& w, const Result& r) {
  const size_t nops = w.ops.size();
  const double per = nops ? 1.0 / nops : 0.0;
  const double mops = nops / r.run_s / 1e6;

  std::fprintf(stderr,
               "%s on %s [%s, %zu init, %zu ops, %.0f%% inserts]\n"
               "  bulk load   %.3f s\n"
               "  throughput  %.3f Mops/s\n"
               "  latency     p50 %llu ns  p99 %llu ns  p99.9 %llu ns\n"
               "  memory      index %.2f MB  data %.2f MB  (process peak %.0f MB)\n",
               c.index.c_str(), dataset_name(c.dataset).c_str(),
               pattern_name(c.spec.pattern), w.init.size(), nops,
               100.0 * w.num_inserts * per, r.bulk_load_s, mops,
               (unsigned long long)r.p50, (unsigned long long)r.p99,
               (unsigned long long)r.p999, r.mem.index_bytes / 1e6,
               r.mem.data_bytes / 1e6, peak_rss_mb());
  if (r.perf_ok)
    std::fprintf(stderr, "  per op      %.0f cycles  %.0f instr  %.2f LLC miss  %.2f br miss\n",
                 r.perf.cycles * per, r.perf.instructions * per,
                 r.perf.llc_misses * per, r.perf.branch_misses * per);
  if (w.fallbacks)
    std::fprintf(stderr, "  note        %zu pattern draws fell back to the dataset pool\n",
                 w.fallbacks);
  if (r.lookup_misses || r.insert_failures || r.verify_failures)
    std::fprintf(stderr, "  ERROR       %zu lookup misses, %zu failed inserts, %zu verify failures\n",
                 r.lookup_misses, r.insert_failures, r.verify_failures);

  if (c.csv.empty()) return;
  std::ofstream out = open_csv(c.csv, kSummaryHeader);
  out << c.label << ',' << c.index << ',' << dataset_name(c.dataset) << ','
      << pattern_name(c.spec.pattern) << ',' << w.init.size() << ',' << nops << ','
      << w.num_inserts << ',' << c.spec.insert_frac << ','
      << (c.spec.zipf_lookups ? "zipf" : "uniform") << ',' << c.spec.seed << ','
      << r.bulk_load_s << ',' << mops << ',' << r.p50 << ',' << r.p99 << ','
      << r.p999 << ',' << r.max_lat << ',' << r.mem.index_bytes << ','
      << r.mem.data_bytes << ',';
  if (r.perf_ok)
    out << r.perf.cycles * per << ',' << r.perf.instructions * per << ','
        << r.perf.llc_misses * per << ',' << r.perf.branch_misses * per << ','
        << r.perf.l1d_misses * per << ',';
  else
    out << ",,,,,";
  out << w.fallbacks << ','
      << (r.lookup_misses + r.insert_failures + r.verify_failures) << ','
      << peak_rss_mb() << ',' << counters_str(r.counters) << '\n';
}

void write_series(const Config& c, const Result& r) {
  if (c.series.empty()) return;
  std::ofstream out = open_csv(c.series, kSeriesHeader);
  for (size_t i = 0; i < r.windows.size(); ++i) {
    const auto& s = r.windows[i];
    out << c.label << ',' << c.index << ',' << dataset_name(c.dataset) << ','
        << pattern_name(c.spec.pattern) << ',' << i << ',' << s.ops_done << ','
        << s.inserts_done << ',' << s.mops << ',' << s.mem.index_bytes << ','
        << s.mem.data_bytes << ',' << counters_str(s.counters) << '\n';
  }
}

Config parse(const Args& a) {
  Config c;
  c.index = a.str("index");
  c.dataset = a.str("dataset");
  c.n = static_cast<size_t>(a.num("n", 0));
  c.init = a.str("init") == "all" ? SIZE_MAX : static_cast<size_t>(a.num("init", 0));
  if (c.init == 0) throw std::runtime_error("--init must be positive or 'all'");
  c.windows = static_cast<size_t>(a.num("windows", 100));
  c.latency_every = static_cast<size_t>(a.num("latency-every", 16));
  c.verify = a.num("verify", 1) != 0;
  c.csv = a.has("csv") ? a.str("csv") : "";
  c.series = a.has("series") ? a.str("series") : "";
  c.label = a.has("label") ? a.str("label") : "";
  c.index_options.rmi_models = static_cast<size_t>(a.num("rmi-models", 0));
  c.index_options.alex.expected_insert_frac = a.num("alex-insert-frac", 1.0);
  c.index_options.alex.max_node_bytes =
      static_cast<size_t>(a.num("alex-max-node-bytes", double(size_t(1) << 24)));
  c.index_options.ref_approximate = a.num("ref-approx", 0) != 0;
  c.index_options.alex.reference_model_fit = a.num("alex-ref-fit", 0) != 0;

  WorkloadSpec& s = c.spec;
  s.num_ops = static_cast<size_t>(a.num("ops", 1'000'000));
  s.insert_frac = a.has("mix") ? mix_insert_fraction(a.str("mix")) : 0.0;
  s.insert_frac = a.num("insert-frac", s.insert_frac);
  s.zipf_lookups = a.str("lookup", "zipf") == "zipf";
  s.zipf_theta = a.num("zipf-theta", s.zipf_theta);
  s.seed = static_cast<uint64_t>(a.num("seed", 42));
  s.pattern = parse_pattern(a.str("pattern", "dataset"));
  s.hot_center = a.num("hot-center", s.hot_center);
  s.hot_width = a.num("hot-width", s.hot_width);
  s.hot_frac_start = a.num("hot-frac-start", s.hot_frac_start);
  s.hot_frac_end = a.num("hot-frac-end", s.hot_frac_end);
  s.drift_start = a.num("drift-start", s.drift_start);
  s.drift_from = a.num("drift-from", s.drift_from);
  s.drift_rate = a.num("drift-rate", s.drift_rate);
  s.drift_sigma = a.num("drift-sigma", s.drift_sigma);
  s.regime_at = a.num("regime-at", s.regime_at);
  s.regime_center = a.num("regime-center", s.regime_center);
  s.regime_sigma = a.num("regime-sigma", s.regime_sigma);
  s.regime_append = a.num("regime-append", 0) != 0;
  a.check_all_used();
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Args args(argc, argv);
    Config cfg = parse(args);
    // Fail fast on an incompatible output file, before the expensive run.
    if (!cfg.csv.empty()) open_csv(cfg.csv, kSummaryHeader);
    if (!cfg.series.empty()) open_csv(cfg.series, kSeriesHeader);

    std::vector<Key> keys = load_dataset(cfg.dataset, cfg.n, cfg.spec.seed);
    Workload w = generate_workload(keys, std::min(cfg.init, keys.size()), cfg.spec);
    keys.clear();
    keys.shrink_to_fit();

    Result r;
    if (cfg.index == "stdmap") r = run<StdMapIndex>(w, cfg);
    else if (cfg.index == "btree") r = run<BTreeIndex>(w, cfg);
    else if (cfg.index == "rmi") r = run<RMIIndex>(w, cfg);
    else if (cfg.index == "alex") r = run<AlexIndex>(w, cfg);
#ifdef COP5725_ALEX_REF
    else if (cfg.index == "alexref") r = run<AlexRefIndex>(w, cfg);
#endif
    else throw std::runtime_error("unknown index: " + cfg.index);

    write_summary(cfg, w, r);
    write_series(cfg, r);
    // Keep the lookup results observable so the compiler cannot drop them.
    if (r.checksum == 42) std::fprintf(stderr, " ");
    return (r.lookup_misses || r.insert_failures || r.verify_failures) ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n\n%s", e.what(), kUsage);
    return 2;
  }
}
