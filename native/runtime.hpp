#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/mempolicy.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace sdc {

using Clock = std::chrono::steady_clock;
using Labels = std::map<std::string, std::string>;

inline volatile std::uint64_t global_sink = 0;

struct Observation {
  std::string group;
  std::string metric;
  double value{};
  std::string unit;
  std::string confidence;
  std::string method;
  Labels labels;
};

struct Options {
  std::string profile = "standard";
  std::vector<std::string> only_probes;
  std::size_t memory_mib = 0;
  bool memory_explicit = false;
  int duration_ms = 0;
  unsigned seed = 0x5DEC4A9U;
  bool help = false;
  bool list_probes = false;
};

struct CpuInfo {
  int cpu = 0;
  int socket = 0;
  int die = 0;
  int core = 0;
  int node = 0;
  std::vector<int> thread_siblings;
};

struct PerfCounts {
  bool available = false;
  bool branch_counters_available = false;
  bool cache_counters_available = false;
  double cycles = 0.0;
  double instructions = 0.0;
  double branches = 0.0;
  double branch_misses = 0.0;
  double cache_references = 0.0;
  double cache_misses = 0.0;
  std::string error;
};

inline std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    return {};
  }
  std::ostringstream data;
  data << input.rdbuf();
  std::string value = data.str();
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

inline int read_int(const std::filesystem::path &path, int fallback = 0) {
  try {
    const auto text = read_file(path);
    return text.empty() ? fallback : std::stoi(text);
  } catch (...) {
    return fallback;
  }
}

inline std::vector<int> parse_cpu_list(std::string_view text) {
  std::vector<int> values;
  std::stringstream input{std::string(text)};
  std::string part;
  while (std::getline(input, part, ',')) {
    if (part.empty()) {
      continue;
    }
    const auto dash = part.find('-');
    try {
      if (dash == std::string::npos) {
        values.push_back(std::stoi(part));
      } else {
        const int first = std::stoi(part.substr(0, dash));
        const int last = std::stoi(part.substr(dash + 1));
        for (int value = first; value <= last; ++value) {
          values.push_back(value);
        }
      }
    } catch (...) {
    }
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

inline std::vector<int> allowed_cpus() {
  cpu_set_t set;
  CPU_ZERO(&set);
  std::vector<int> cpus;
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (CPU_ISSET(cpu, &set)) {
        cpus.push_back(cpu);
      }
    }
  }
  if (cpus.empty()) {
    cpus.push_back(0);
  }
  return cpus;
}

inline bool pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

inline int cpu_node(int cpu) {
  const std::filesystem::path root =
      std::filesystem::path("/sys/devices/system/cpu") / ("cpu" + std::to_string(cpu));
  std::error_code error;
  for (const auto &entry : std::filesystem::directory_iterator(root, error)) {
    const auto name = entry.path().filename().string();
    if (name.rfind("node", 0) == 0) {
      try {
        return std::stoi(name.substr(4));
      } catch (...) {
      }
    }
  }
  return 0;
}

inline std::vector<CpuInfo> discover_cpus() {
  std::vector<CpuInfo> result;
  for (const int cpu : allowed_cpus()) {
    const auto root = std::filesystem::path("/sys/devices/system/cpu") /
                      ("cpu" + std::to_string(cpu)) / "topology";
    result.push_back({cpu,
                      read_int(root / "physical_package_id"),
                      read_int(root / "die_id"),
                      read_int(root / "core_id", cpu),
                      cpu_node(cpu),
                      parse_cpu_list(read_file(root / "thread_siblings_list"))});
  }
  return result;
}

inline std::vector<CpuInfo> physical_cpus(const std::vector<CpuInfo> &cpus) {
  std::set<std::tuple<int, int, int>> seen;
  std::vector<CpuInfo> result;
  for (const auto &cpu : cpus) {
    auto key = std::make_tuple(cpu.socket, cpu.die, cpu.core);
    if (seen.insert(key).second) {
      result.push_back(cpu);
    }
  }
  return result;
}

inline std::vector<CpuInfo> numa_balanced_physical_cpus(const std::vector<CpuInfo> &cpus) {
  std::map<int, std::vector<CpuInfo>> by_node;
  for (const auto &cpu : physical_cpus(cpus)) by_node[cpu.node].push_back(cpu);
  std::vector<CpuInfo> result;
  std::size_t index = 0;
  while (result.size() < physical_cpus(cpus).size()) {
    bool added = false;
    for (const auto &[node, node_cpus] : by_node) {
      (void)node;
      if (index < node_cpus.size()) {
        result.push_back(node_cpus[index]);
        added = true;
      }
    }
    if (!added) break;
    ++index;
  }
  return result;
}

inline std::string architecture_name() {
#if defined(__x86_64__)
  return "x86_64";
#elif defined(__aarch64__)
  return "arm64";
#else
  return "unsupported";
#endif
}

inline std::string platform_family() {
  const std::string cpuinfo = read_file("/proc/cpuinfo");
#if defined(__x86_64__)
  if (cpuinfo.find("HygonGenuine") != std::string::npos) {
    return "c86-hygon";
  }
  return "x86_64";
#elif defined(__aarch64__)
  return "arm64";
#else
  return "unsupported";
#endif
}

inline std::string stream_read_kernel_name() {
#if defined(__x86_64__)
  return __builtin_cpu_supports("avx2") ? "x86-avx2-vector-inline-assembly"
                                        : "x86-sse2-vector-inline-assembly";
#elif defined(__aarch64__)
  return "arm64-neon-vector-inline-assembly";
#else
  return "portable-scalar";
#endif
}

inline double seconds_between(Clock::time_point begin, Clock::time_point end) {
  return std::chrono::duration<double>(end - begin).count();
}

inline std::uint64_t cycle_counter() {
#if defined(__x86_64__) || defined(__i386__)
  unsigned aux = 0;
  _mm_lfence();
  const auto value = __rdtscp(&aux);
  _mm_lfence();
  return value;
#elif defined(__aarch64__)
  std::uint64_t value = 0;
  asm volatile("isb; mrs %0, cntvct_el0" : "=r"(value));
  return value;
#else
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
#endif
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  std::this_thread::yield();
#endif
}

inline long perf_event_open(perf_event_attr *event, pid_t pid, int cpu, int group_fd,
                     unsigned long flags) {
  return syscall(SYS_perf_event_open, event, pid, cpu, group_fd, flags);
}

enum class PerfGroupKind { Core, Branch, Cache };

class PerfGroup {
 public:
  explicit PerfGroup(PerfGroupKind kind = PerfGroupKind::Core) : kind_(kind) { open(); }
  PerfGroup(const PerfGroup &) = delete;
  PerfGroup &operator=(const PerfGroup &) = delete;
  ~PerfGroup() {
    for (const int fd : fds_) {
      if (fd >= 0) {
        close(fd);
      }
    }
  }

  bool available() const { return leader_ >= 0 && event_names_.size() == 2; }
  const std::string &error() const { return error_; }

  bool start() {
    if (!available()) {
      return false;
    }
    if (ioctl(leader_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0) {
      error_ = "PERF_EVENT_IOC_RESET 失败：" + std::string(std::strerror(errno));
      return false;
    }
    if (ioctl(leader_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
      error_ = "PERF_EVENT_IOC_ENABLE 失败：" + std::string(std::strerror(errno));
      return false;
    }
    started_ = true;
    return true;
  }

  PerfCounts stop() {
    PerfCounts counts;
    if (!available() || !started_) {
      counts.error = error_;
      return counts;
    }
    started_ = false;
    if (ioctl(leader_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) != 0) {
      error_ = "PERF_EVENT_IOC_DISABLE 失败：" + std::string(std::strerror(errno));
      counts.error = error_;
      return counts;
    }
    std::vector<std::uint64_t> buffer(3 + event_names_.size());
    const auto bytes = read(leader_, buffer.data(), buffer.size() * sizeof(std::uint64_t));
    if (bytes < static_cast<ssize_t>((3 + event_names_.size()) * sizeof(std::uint64_t))) {
      error_ = bytes < 0
          ? "读取 perf_event 组失败：" + std::string(std::strerror(errno))
          : "读取 perf_event 组长度不足";
      counts.error = error_;
      return counts;
    }
    const auto nr = buffer[0];
    const double enabled = static_cast<double>(buffer[1]);
    const double running = static_cast<double>(buffer[2]);
    if (nr != event_names_.size()) {
      error_ = "perf_event 返回的事件数量不匹配";
      counts.error = error_;
      return counts;
    }
    if (running <= 0.0) {
      error_ = "time_running 为 0（事件无法调度）";
      counts.error = error_;
      return counts;
    }
    const double scale = enabled / running;
    counts.available = true;
    for (std::size_t index = 0; index < event_names_.size(); ++index) {
      const double value = static_cast<double>(buffer[3 + index]) * scale;
      if (event_names_[index] == "cycles") counts.cycles = value;
      else if (event_names_[index] == "instructions") counts.instructions = value;
      else if (event_names_[index] == "branches") counts.branches = value;
      else if (event_names_[index] == "branch_misses") counts.branch_misses = value;
      else if (event_names_[index] == "cache_references") counts.cache_references = value;
      else if (event_names_[index] == "cache_misses") counts.cache_misses = value;
    }
    counts.branch_counters_available =
        std::find(event_names_.begin(), event_names_.end(), "branches") != event_names_.end() &&
        std::find(event_names_.begin(), event_names_.end(), "branch_misses") != event_names_.end();
    counts.cache_counters_available =
        std::find(event_names_.begin(), event_names_.end(), "cache_references") != event_names_.end() &&
        std::find(event_names_.begin(), event_names_.end(), "cache_misses") != event_names_.end();
    return counts;
  }

 private:
  int leader_ = -1;
  std::vector<int> fds_;
  std::string error_;
  std::vector<std::string> event_names_;
  PerfGroupKind kind_ = PerfGroupKind::Core;
  bool started_ = false;

  void open() {
    struct EventSpec {
      const char *name;
      std::uint64_t config;
    };
    std::vector<EventSpec> specs;
    if (kind_ == PerfGroupKind::Core) {
      specs = {{"cycles", PERF_COUNT_HW_CPU_CYCLES},
               {"instructions", PERF_COUNT_HW_INSTRUCTIONS}};
    } else if (kind_ == PerfGroupKind::Branch) {
      specs = {{"branches", PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
               {"branch_misses", PERF_COUNT_HW_BRANCH_MISSES}};
    } else {
      specs = {{"cache_references", PERF_COUNT_HW_CACHE_REFERENCES},
               {"cache_misses", PERF_COUNT_HW_CACHE_MISSES}};
    }
    for (const auto &spec : specs) {
      perf_event_attr event{};
      event.type = PERF_TYPE_HARDWARE;
      event.size = sizeof(event);
      event.config = spec.config;
      event.disabled = fds_.empty() ? 1U : 0U;
      event.exclude_kernel = 1;
      event.exclude_hv = 1;
      event.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                          PERF_FORMAT_TOTAL_TIME_RUNNING;
      const int fd = static_cast<int>(perf_event_open(&event, 0, -1, leader_, 0));
      if (fd < 0) {
        error_ = std::string("perf_event_open(") + spec.name + ") 失败：" +
                 std::strerror(errno);
        for (const int old_fd : fds_) close(old_fd);
        fds_.clear();
        event_names_.clear();
        leader_ = -1;
        return;
      }
      if (fds_.empty()) {
        leader_ = fd;
      }
      fds_.push_back(fd);
      event_names_.push_back(spec.name);
    }
  }
};

struct PerfProbeStatus {
  bool available = false;
  std::string error;
};

struct PerfEnvironment {
  PerfProbeStatus core;
  PerfProbeStatus branch;
  PerfProbeStatus cache;
  std::string perf_event_paranoid;
  std::string nmi_watchdog;
};

inline PerfProbeStatus probe_perf_group(PerfGroupKind kind) {
  PerfGroup group(kind);
  if (!group.available()) return {false, group.error()};
  if (!group.start()) return {false, group.error()};
  std::uint64_t value = 1;
  for (std::uint64_t index = 0; index < 100000; ++index) {
    value = value * 3 + index;
    asm volatile("" : "+r"(value) :: "memory");
  }
  global_sink = global_sink ^ value;
  const auto counts = group.stop();
  return {counts.available, counts.available ? std::string{} : counts.error};
}

inline std::string escape_json(std::string_view value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
      case '\\': output << "\\\\"; break;
      case '"': output << "\\\""; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (character < 0x20) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<int>(character) << std::dec;
        } else {
          output << character;
        }
    }
  }
  return output.str();
}

inline void emit_string(std::ostream &output, std::string_view key, std::string_view value,
                 bool comma = true) {
  output << '"' << escape_json(key) << "\":\"" << escape_json(value) << '"';
  if (comma) {
    output << ',';
  }
}

inline void add_observation(std::vector<Observation> &observations, std::string group,
                     std::string metric, double value, std::string unit,
                     std::string confidence, std::string method, Labels labels = {}) {
  if (std::isfinite(value)) {
    observations.push_back({std::move(group), std::move(metric), value, std::move(unit),
                            std::move(confidence), std::move(method), std::move(labels)});
  }
}

inline void add_warning_once(std::vector<std::string> &warnings, std::string warning) {
  if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end())
    warnings.push_back(std::move(warning));
}

inline void emit_json(const Options &options, const std::vector<Observation> &observations,
               const std::vector<std::string> &warnings, const PerfEnvironment &perf,
               double runtime_seconds, const std::vector<std::string> &selected_probes) {
  std::cout << "{\n\"schema_version\":\"1.0\",\n\"metadata\":{";
  emit_string(std::cout, "architecture", architecture_name());
  emit_string(std::cout, "platform_family", platform_family());
  emit_string(std::cout, "profile", options.profile);
  emit_string(std::cout, "stream_read_kernel", stream_read_kernel_name());
  std::cout << "\"selected_probes\":[";
  for (std::size_t index = 0; index < selected_probes.size(); ++index) {
    std::cout << '"' << escape_json(selected_probes[index]) << '"'
              << (index + 1 < selected_probes.size() ? "," : "");
  }
  std::cout << "],";
  std::cout << "\"seed\":" << options.seed << ",\"perf_available\":"
            << (perf.core.available ? "true" : "false") << ',';
  emit_string(std::cout, "perf_error", perf.core.error);
  std::cout << "\"pmu_core_available\":" << (perf.core.available ? "true" : "false") << ',';
  emit_string(std::cout, "pmu_core_error", perf.core.error);
  std::cout << "\"pmu_branch_available\":" << (perf.branch.available ? "true" : "false") << ',';
  emit_string(std::cout, "pmu_branch_error", perf.branch.error);
  std::cout << "\"pmu_cache_available\":" << (perf.cache.available ? "true" : "false") << ',';
  emit_string(std::cout, "pmu_cache_error", perf.cache.error);
  emit_string(std::cout, "perf_event_paranoid", perf.perf_event_paranoid);
  emit_string(std::cout, "nmi_watchdog", perf.nmi_watchdog);
  std::cout << "\"requested_memory_mib\":" << options.memory_mib
            << ",\"memory_size_explicit\":" << (options.memory_explicit ? "true" : "false")
            << ",\"duration_ms\":" << options.duration_ms
            << ",\"runtime_seconds\":" << std::setprecision(10) << runtime_seconds
            << "},\n\"observations\":[\n";
  for (std::size_t index = 0; index < observations.size(); ++index) {
    const auto &item = observations[index];
    std::cout << '{';
    emit_string(std::cout, "group", item.group);
    emit_string(std::cout, "metric", item.metric);
    std::cout << "\"value\":" << std::setprecision(12) << item.value << ',';
    emit_string(std::cout, "unit", item.unit);
    emit_string(std::cout, "confidence", item.confidence);
    emit_string(std::cout, "method", item.method);
    std::cout << "\"labels\":{";
    std::size_t label_index = 0;
    for (const auto &[key, value] : item.labels) {
      emit_string(std::cout, key, value, ++label_index < item.labels.size());
    }
    std::cout << "}}" << (index + 1 < observations.size() ? "," : "") << '\n';
  }
  std::cout << "],\n\"warnings\":[";
  for (std::size_t index = 0; index < warnings.size(); ++index) {
    std::cout << '"' << escape_json(warnings[index]) << '"'
              << (index + 1 < warnings.size() ? "," : "");
  }
  std::cout << "]\n}\n";
}

enum class HugePageAdvice { PreferHuge, AvoidHuge, Default };

class MappedBuffer {
 public:
  explicit MappedBuffer(std::size_t bytes,
                        HugePageAdvice advice = HugePageAdvice::PreferHuge)
      : bytes_(bytes) {
    data_ = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data_ == MAP_FAILED) {
      data_ = nullptr;
      throw std::runtime_error(std::string("mmap failed: ") + std::strerror(errno));
    }
#ifdef MADV_HUGEPAGE
    if (advice == HugePageAdvice::PreferHuge) {
      madvise(data_, bytes_, MADV_HUGEPAGE);
    }
#endif
#ifdef MADV_NOHUGEPAGE
    if (advice == HugePageAdvice::AvoidHuge) {
      if (madvise(data_, bytes_, MADV_NOHUGEPAGE) != 0) {
        const std::string error = std::strerror(errno);
        munmap(data_, bytes_);
        data_ = nullptr;
        throw std::runtime_error("MADV_NOHUGEPAGE failed: " + error);
      }
    }
#endif
  }
  MappedBuffer(const MappedBuffer &) = delete;
  MappedBuffer &operator=(const MappedBuffer &) = delete;
  ~MappedBuffer() {
    if (data_ != nullptr) {
      munmap(data_, bytes_);
    }
  }
  std::byte *bytes() { return static_cast<std::byte *>(data_); }
  const std::byte *bytes() const { return static_cast<const std::byte *>(data_); }
  template <typename T> T *as() { return static_cast<T *>(data_); }
  std::size_t size() const { return bytes_; }

 private:
  void *data_ = nullptr;
  std::size_t bytes_ = 0;
};

class ExecutableBuffer {
 public:
  explicit ExecutableBuffer(std::size_t bytes) : bytes_(bytes) {
    data_ = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data_ == MAP_FAILED) {
      data_ = nullptr;
      throw std::runtime_error(std::string("executable mmap failed: ") +
                               std::strerror(errno));
    }
  }
  ExecutableBuffer(const ExecutableBuffer &) = delete;
  ExecutableBuffer &operator=(const ExecutableBuffer &) = delete;
  ~ExecutableBuffer() {
    if (data_ != nullptr) munmap(data_, bytes_);
  }

  std::byte *bytes() { return static_cast<std::byte *>(data_); }
  std::size_t size() const { return bytes_; }

  void seal() {
    auto *begin = static_cast<char *>(data_);
    __builtin___clear_cache(begin, begin + bytes_);
    if (mprotect(data_, bytes_, PROT_READ | PROT_EXEC) != 0) {
      throw std::runtime_error(std::string("mprotect RX failed: ") +
                               std::strerror(errno));
    }
  }

  template <typename Function>
  Function function_at(std::size_t offset = 0) const {
    return reinterpret_cast<Function>(static_cast<std::byte *>(data_) + offset);
  }

 private:
  void *data_ = nullptr;
  std::size_t bytes_ = 0;
};

inline std::vector<std::size_t> power_sizes(std::size_t first, std::size_t last) {
  std::vector<std::size_t> result;
  for (std::size_t value = first; value <= last; value *= 2) {
    result.push_back(value);
    if (value > std::numeric_limits<std::size_t>::max() / 2) {
      break;
    }
  }
  return result;
}

inline std::size_t parse_size_bytes(const std::string &text) {
  if (text.empty()) return 0;
  try {
    std::size_t consumed = 0;
    const auto number = std::stoull(text, &consumed);
    const char suffix = consumed < text.size()
        ? static_cast<char>(std::toupper(static_cast<unsigned char>(text[consumed]))) : '\0';
    if (suffix == 'K') return number * 1024ULL;
    if (suffix == 'M') return number * 1024ULL * 1024ULL;
    if (suffix == 'G') return number * 1024ULL * 1024ULL * 1024ULL;
    return number;
  } catch (...) {
    return 0;
  }
}

struct CacheInstance {
  int level = 0;
  std::size_t bytes = 0;
  std::vector<int> shared_cpus;
};

inline std::vector<CacheInstance> unique_data_cache_instances(const std::vector<int> &cpus) {
  std::map<std::string, CacheInstance> unique;
  for (const int cpu : cpus) {
    const auto root = std::filesystem::path("/sys/devices/system/cpu") /
                      ("cpu" + std::to_string(cpu)) / "cache";
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(root, error)) {
      if (entry.path().filename().string().rfind("index", 0) != 0) continue;
      const auto type = read_file(entry.path() / "type");
      if (type != "Unified" && type != "Data") continue;
      const int level = read_int(entry.path() / "level");
      const std::size_t bytes = parse_size_bytes(read_file(entry.path() / "size"));
      auto shared = parse_cpu_list(read_file(entry.path() / "shared_cpu_list"));
      if (shared.empty()) shared.push_back(cpu);
      std::ostringstream key;
      key << level << ':' << type << ':' << bytes << ':';
      for (const int shared_cpu : shared) key << shared_cpu << ',';
      unique.emplace(key.str(), CacheInstance{level, bytes, std::move(shared)});
    }
  }
  std::vector<CacheInstance> result;
  result.reserve(unique.size());
  for (auto &[key, cache] : unique) {
    (void)key;
    result.push_back(std::move(cache));
  }
  return result;
}

inline std::size_t aggregate_last_level_cache_bytes(const std::vector<int> &cpus) {
  const auto caches = unique_data_cache_instances(cpus);
  int last_level = 0;
  for (const auto &cache : caches) last_level = std::max(last_level, cache.level);
  std::size_t total = 0;
  for (const auto &cache : caches) {
    if (cache.level == last_level) total += cache.bytes;
  }
  return total;
}

inline std::size_t last_level_cache_bytes(int cpu) {
  return aggregate_last_level_cache_bytes({cpu});
}

inline std::size_t memory_available_bytes() {
  std::ifstream input("/proc/meminfo");
  std::string key;
  std::size_t kib = 0;
  std::string unit;
  while (input >> key >> kib >> unit) {
    if (key == "MemAvailable:") return kib * 1024ULL;
  }
  return 0;
}


inline std::string cpu_relation(const CpuInfo &left, const CpuInfo &right) {
  if (left.socket == right.socket && left.die == right.die && left.core == right.core) {
    return "smt-sibling";
  }
  if (left.node == right.node) {
    static std::map<int, std::vector<int>> llc_siblings_by_cpu;
    auto llc_siblings = [&](int cpu) -> const std::vector<int> & {
      const auto found = llc_siblings_by_cpu.find(cpu);
      if (found != llc_siblings_by_cpu.end()) return found->second;
      std::vector<int> siblings;
      int highest_level = -1;
      const auto root = std::filesystem::path("/sys/devices/system/cpu") /
                        ("cpu" + std::to_string(cpu)) / "cache";
      std::error_code error;
      for (const auto &entry : std::filesystem::directory_iterator(root, error)) {
        if (entry.path().filename().string().rfind("index", 0) != 0) continue;
        const auto type = read_file(entry.path() / "type");
        if (type != "Unified" && type != "Data") continue;
        const int level = read_int(entry.path() / "level");
        if (level >= highest_level) {
          highest_level = level;
          siblings = parse_cpu_list(read_file(entry.path() / "shared_cpu_list"));
        }
      }
      return llc_siblings_by_cpu.emplace(cpu, std::move(siblings)).first->second;
    };
    const auto &shared = llc_siblings(left.cpu);
    if (std::binary_search(shared.begin(), shared.end(), right.cpu))
      return "same-llc-different-core";
    return "cross-llc-same-numa";
  }
  if (left.socket == right.socket) {
    return "cross-numa-same-socket";
  }
  return "cross-socket";
}


}  // namespace sdc
