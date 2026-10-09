// Key datasets: SOSD binary files and locally generated synthetic keys.
//
// All keys are uint64_t. Every loader returns a deduplicated, randomly
// shuffled vector, matching the ALEX paper's setup (Sec. 6.1): the dataset is
// shuffled, the first `init` keys are bulk loaded, and the remainder is the
// pool that inserts are drawn from.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace cop5725 {

using Key = uint64_t;

inline void dedup_and_shuffle(std::vector<Key>& keys, uint64_t seed) {
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  std::mt19937_64 rng(seed);
  std::shuffle(keys.begin(), keys.end(), rng);
}

// SOSD file format: an 8-byte little-endian count followed by `count` keys.
// The key width (uint32 or uint64) is taken from the file name suffix, as in
// the SOSD distribution (e.g. books_200M_uint32, fb_200M_uint64).
inline std::vector<Key> load_sosd(const std::string& path) {
  const bool is32 = path.find("uint32") != std::string::npos;
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open dataset: " + path);
  uint64_t count = 0;
  in.read(reinterpret_cast<char*>(&count), sizeof(count));
  if (!in) throw std::runtime_error("truncated SOSD header: " + path);

  std::vector<Key> keys(count);
  if (is32) {
    std::vector<uint32_t> raw(count);
    in.read(reinterpret_cast<char*>(raw.data()), count * sizeof(uint32_t));
    std::copy(raw.begin(), raw.end(), keys.begin());
  } else {
    in.read(reinterpret_cast<char*>(keys.data()), count * sizeof(Key));
  }
  if (!in) throw std::runtime_error("truncated SOSD body: " + path);
  return keys;
}

// Synthetic distributions used by the ALEX paper and the proposal:
//   uniform   - YCSB-style uniformly random 63-bit keys
//   lognormal - lognormal(mu=0, sigma=2) scaled by 1e9 (ALEX paper, Sec. 6.1)
//   normal    - normal centred in the 63-bit key space
//   sequential- dense increasing keys 1..n (a best case for learned indexes)
inline std::vector<Key> generate_synthetic(const std::string& dist, size_t n,
                                           uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<Key> keys;
  keys.reserve(n + n / 8);
  const double kMax = static_cast<double>(std::numeric_limits<int64_t>::max());
  std::lognormal_distribution<double> lognormal(0.0, 2.0);
  std::normal_distribution<double> normal(0.5, 0.125);

  auto draw = [&]() -> Key {
    if (dist == "uniform") {
      return rng() >> 1;
    } else if (dist == "lognormal") {
      double v = lognormal(rng) * 1e9;
      return static_cast<Key>(std::min(v, kMax));
    } else if (dist == "normal") {
      double v = std::clamp(normal(rng), 0.0, 1.0) * kMax;
      return static_cast<Key>(v);
    }
    throw std::runtime_error("unknown synthetic distribution: " + dist);
  };

  if (dist == "sequential") {
    for (size_t i = 1; i <= n; ++i) keys.push_back(i);
  } else {
    // Draw, dedupe, and top up until we have n distinct keys.
    while (keys.size() < n) {
      while (keys.size() < n + n / 16 + 16) keys.push_back(draw());
      std::sort(keys.begin(), keys.end());
      keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    }
  }
  dedup_and_shuffle(keys, seed);
  keys.resize(n);
  return keys;
}

// Resolve a dataset spec: either "synthetic:<dist>" or a path to a SOSD file.
// `n` caps the number of keys kept (a random sample, since keys are shuffled);
// n == 0 keeps everything.
inline std::vector<Key> load_dataset(const std::string& spec, size_t n,
                                     uint64_t seed) {
  const std::string prefix = "synthetic:";
  if (spec.rfind(prefix, 0) == 0) {
    if (n == 0) throw std::runtime_error("synthetic datasets need --n");
    return generate_synthetic(spec.substr(prefix.size()), n, seed);
  }
  std::vector<Key> keys = load_sosd(spec);
  dedup_and_shuffle(keys, seed);
  if (n != 0 && n < keys.size()) keys.resize(n);
  return keys;
}

}  // namespace cop5725
