// Timing, latency percentiles, and hardware performance counters.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace cop5725 {

inline uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Returns the p-th percentile (0..100) of `samples`; reorders the vector.
inline uint64_t percentile(std::vector<uint64_t>& samples, double p) {
  if (samples.empty()) return 0;
  size_t idx = std::min(samples.size() - 1,
                        static_cast<size_t>(p / 100.0 * samples.size()));
  std::nth_element(samples.begin(), samples.begin() + idx, samples.end());
  return samples[idx];
}

// Hardware counters for the calling thread via perf_event_open (Linux only).
// On other platforms, or when perf is not permitted
// (/proc/sys/kernel/perf_event_paranoid), available() is false and every
// reading is zero.
struct PerfReading {
  uint64_t cycles = 0, instructions = 0, llc_misses = 0, branch_misses = 0,
           l1d_misses = 0;
};

class PerfCounters {
 public:
  PerfCounters() {
#ifdef __linux__
    open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
    if (fds_.empty()) return;  // perf unavailable; leave counters disabled
    open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
    open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES);
    open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES);
    open(PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_L1D |
                                 (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                                 (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
#endif
  }
  ~PerfCounters() {
#ifdef __linux__
    for (int fd : fds_) close(fd);
#endif
  }
  PerfCounters(const PerfCounters&) = delete;
  PerfCounters& operator=(const PerfCounters&) = delete;

  bool available() const { return !fds_.empty(); }

  void start() {
#ifdef __linux__
    if (!available()) return;
    ioctl(fds_[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(fds_[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
#endif
  }

  PerfReading stop() {
    PerfReading r;
#ifdef __linux__
    if (!available()) return r;
    ioctl(fds_[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    // PERF_FORMAT_GROUP layout: { u64 nr; u64 values[nr]; }
    uint64_t buf[1 + 8] = {};
    if (read(fds_[0], buf, sizeof(buf)) <= 0) return r;
    uint64_t* v = buf + 1;
    uint64_t* fields[] = {&r.cycles, &r.instructions, &r.llc_misses,
                          &r.branch_misses, &r.l1d_misses};
    for (size_t i = 0; i < buf[0] && i < 5; ++i) *fields[slot_[i]] = v[i];
#endif
    return r;
  }

 private:
#ifdef __linux__
  void open(uint32_t type, uint64_t config) {
    perf_event_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = type;
    attr.config = config;
    attr.disabled = fds_.empty() ? 1 : 0;  // only the group leader starts off
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format = PERF_FORMAT_GROUP;
    int group = fds_.empty() ? -1 : fds_[0];
    int fd = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, group, 0));
    if (fd >= 0) {
      slot_.push_back(next_field_);
      fds_.push_back(fd);
    }
    ++next_field_;  // keep field mapping aligned even if one event is missing
  }
  std::vector<size_t> slot_;
  size_t next_field_ = 0;
#endif
  std::vector<int> fds_;
};

}  // namespace cop5725
