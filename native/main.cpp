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

namespace {

using Clock = std::chrono::steady_clock;
using Labels = std::map<std::string, std::string>;

volatile std::uint64_t global_sink = 0;

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
  std::size_t memory_mib = 0;
  bool memory_explicit = false;
  int duration_ms = 0;
  unsigned seed = 0x5DEC4A9U;
  bool help = false;
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

std::string read_file(const std::filesystem::path &path) {
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

int read_int(const std::filesystem::path &path, int fallback = 0) {
  try {
    const auto text = read_file(path);
    return text.empty() ? fallback : std::stoi(text);
  } catch (...) {
    return fallback;
  }
}

std::vector<int> parse_cpu_list(std::string_view text) {
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

std::vector<int> allowed_cpus() {
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

bool pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

int cpu_node(int cpu) {
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

std::vector<CpuInfo> discover_cpus() {
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

std::vector<CpuInfo> physical_cpus(const std::vector<CpuInfo> &cpus) {
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

std::vector<CpuInfo> numa_balanced_physical_cpus(const std::vector<CpuInfo> &cpus) {
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

std::string architecture_name() {
#if defined(__x86_64__)
  return "x86_64";
#elif defined(__aarch64__)
  return "arm64";
#else
  return "unsupported";
#endif
}

std::string platform_family() {
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

double seconds_between(Clock::time_point begin, Clock::time_point end) {
  return std::chrono::duration<double>(end - begin).count();
}

std::uint64_t cycle_counter() {
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

long perf_event_open(perf_event_attr *event, pid_t pid, int cpu, int group_fd,
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

PerfProbeStatus probe_perf_group(PerfGroupKind kind) {
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

std::string escape_json(std::string_view value) {
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

void emit_string(std::ostream &output, std::string_view key, std::string_view value,
                 bool comma = true) {
  output << '"' << escape_json(key) << "\":\"" << escape_json(value) << '"';
  if (comma) {
    output << ',';
  }
}

void add_observation(std::vector<Observation> &observations, std::string group,
                     std::string metric, double value, std::string unit,
                     std::string confidence, std::string method, Labels labels = {}) {
  if (std::isfinite(value)) {
    observations.push_back({std::move(group), std::move(metric), value, std::move(unit),
                            std::move(confidence), std::move(method), std::move(labels)});
  }
}

void add_warning_once(std::vector<std::string> &warnings, std::string warning) {
  if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end())
    warnings.push_back(std::move(warning));
}

void emit_json(const Options &options, const std::vector<Observation> &observations,
               const std::vector<std::string> &warnings, const PerfEnvironment &perf,
               double runtime_seconds) {
  std::cout << "{\n\"schema_version\":\"1.0\",\n\"metadata\":{";
  emit_string(std::cout, "architecture", architecture_name());
  emit_string(std::cout, "platform_family", platform_family());
  emit_string(std::cout, "profile", options.profile);
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

std::vector<std::size_t> power_sizes(std::size_t first, std::size_t last) {
  std::vector<std::size_t> result;
  for (std::size_t value = first; value <= last; value *= 2) {
    result.push_back(value);
    if (value > std::numeric_limits<std::size_t>::max() / 2) {
      break;
    }
  }
  return result;
}

std::size_t parse_size_bytes(const std::string &text) {
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

std::vector<CacheInstance> unique_data_cache_instances(const std::vector<int> &cpus) {
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

std::size_t aggregate_last_level_cache_bytes(const std::vector<int> &cpus) {
  const auto caches = unique_data_cache_instances(cpus);
  int last_level = 0;
  for (const auto &cache : caches) last_level = std::max(last_level, cache.level);
  std::size_t total = 0;
  for (const auto &cache : caches) {
    if (cache.level == last_level) total += cache.bytes;
  }
  return total;
}

std::size_t last_level_cache_bytes(int cpu) {
  return aggregate_last_level_cache_bytes({cpu});
}

std::size_t memory_available_bytes() {
  std::ifstream input("/proc/meminfo");
  std::string key;
  std::size_t kib = 0;
  std::string unit;
  while (input >> key >> kib >> unit) {
    if (key == "MemAvailable:") return kib * 1024ULL;
  }
  return 0;
}

void benchmark_timers(std::vector<Observation> &observations, int cpu) {
  pin_to_cpu(cpu);
  constexpr std::size_t samples = 200000;
  auto begin = Clock::now();
  for (std::size_t index = 0; index < samples; ++index) {
    asm volatile("" ::: "memory");
    const auto stamp = Clock::now();
    asm volatile("" : : "g"(&stamp) : "memory");
  }
  auto end = Clock::now();
  add_observation(observations, "timer", "steady_clock_call",
                  seconds_between(begin, end) * 1e9 / static_cast<double>(samples),
                  "ns/call", "high", "back-to-back steady_clock calls", {{"cpu", std::to_string(cpu)}});

  std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t total = 0;
  for (std::size_t index = 0; index < samples; ++index) {
    const auto first = cycle_counter();
    const auto second = cycle_counter();
    const auto delta = second - first;
    minimum = std::min(minimum, delta);
    total += delta;
  }
  add_observation(observations, "timer", "cycle_counter_min", static_cast<double>(minimum),
                  "ticks/call-pair", "high", "serialized back-to-back counter reads",
                  {{"cpu", std::to_string(cpu)}});
  add_observation(observations, "timer", "cycle_counter_mean",
                  static_cast<double>(total) / static_cast<double>(samples),
                  "ticks/call-pair", "high", "serialized back-to-back counter reads",
                  {{"cpu", std::to_string(cpu)}});

  const auto wall_begin = Clock::now();
  const auto cycle_begin = cycle_counter();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const auto cycle_end = cycle_counter();
  const auto wall_end = Clock::now();
  const double hz = static_cast<double>(cycle_end - cycle_begin) /
                    seconds_between(wall_begin, wall_end);
  add_observation(observations, "timer", "counter_frequency", hz / 1e9, "GHz",
                  architecture_name() == "x86_64" ? "high" : "medium",
                  "counter delta over monotonic wall interval", {{"cpu", std::to_string(cpu)}});
}

double chase_chain(std::byte *base, std::size_t iterations) {
  std::uint32_t current = 0;
  const auto begin = Clock::now();
  for (std::size_t index = 0; index < iterations; ++index) {
    current = *reinterpret_cast<volatile std::uint32_t *>(base + current);
  }
  const auto end = Clock::now();
  global_sink = global_sink ^ current;
  return seconds_between(begin, end) * 1e9 / static_cast<double>(iterations);
}

double median_double(std::vector<double> values) {
  if (values.empty()) return 0.0;
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

void build_random_chain(std::byte *base, std::size_t bytes, std::size_t stride,
                        std::mt19937 &random) {
  const std::size_t points = std::max<std::size_t>(2, bytes / stride);
  std::vector<std::uint32_t> order(points);
  std::iota(order.begin(), order.end(), 0U);
  std::shuffle(order.begin(), order.end(), random);
  for (std::size_t index = 0; index < points; ++index) {
    const auto here = static_cast<std::size_t>(order[index]) * stride;
    const auto next = static_cast<std::size_t>(order[(index + 1) % points]) * stride;
    *reinterpret_cast<std::uint32_t *>(base + here) = static_cast<std::uint32_t>(next);
  }
}

void build_random_page_chain(std::byte *base, std::size_t pages,
                             std::size_t page_size, std::mt19937 &random) {
  const std::size_t cache_line = 64;
  const std::size_t set_spread = std::max<std::size_t>(1, page_size / cache_line);
  std::vector<std::uint32_t> addresses(pages);
  for (std::size_t page = 0; page < pages; ++page) {
    const std::size_t offset = (page % set_spread) * cache_line;
    addresses[page] = static_cast<std::uint32_t>(page * page_size + offset);
  }
  std::shuffle(addresses.begin(), addresses.end(), random);
  for (std::size_t index = 0; index < addresses.size(); ++index) {
    *reinterpret_cast<std::uint32_t *>(base + addresses[index]) =
        addresses[(index + 1) % addresses.size()];
  }
}

void benchmark_cache_latency(const Options &options, std::vector<Observation> &observations,
                             std::vector<std::string> &warnings, int cpu) {
  pin_to_cpu(cpu);
  const std::size_t llc = last_level_cache_bytes(cpu);
  const std::size_t base_last = options.profile == "smoke" ? 64U * 1024U
      : options.profile == "quick" ? 32U * 1024U * 1024U
      : options.profile == "deep" ? 512U * 1024U * 1024U
                                  : 128U * 1024U * 1024U;
  const std::size_t multiplier = options.profile == "deep" ? 4 : 2;
  const std::size_t cap = options.profile == "quick" ? 512ULL * 1024ULL * 1024ULL
                                                     : 2ULL * 1024ULL * 1024ULL * 1024ULL;
  const std::size_t last = options.profile == "smoke"
      ? base_last : std::min(cap, std::max(base_last, llc * multiplier));
  std::mt19937 random(options.seed);
  try {
    for (const auto bytes : power_sizes(4U * 1024U, last)) {
      MappedBuffer buffer(bytes);
      build_random_chain(buffer.bytes(), bytes, 64, random);
      const std::size_t points = bytes / 64;
      const std::size_t iterations = std::clamp<std::size_t>(points * 12, 100000, 5000000);
      (void)chase_chain(buffer.bytes(), std::min<std::size_t>(iterations, points * 2));
      const int samples = options.profile == "smoke" ? 1 : options.profile == "quick" ? 3 : 5;
      std::vector<double> latencies;
      for (int sample = 0; sample < samples; ++sample)
        latencies.push_back(chase_chain(buffer.bytes(), iterations));
      const double latency = median_double(std::move(latencies));
      add_observation(observations, "cache_latency", "random_load_latency", latency,
                      "ns/access", "high", "random dependent pointer chase",
                      {{"cpu", std::to_string(cpu)},
                       {"working_set_bytes", std::to_string(bytes)},
                       {"stride_bytes", "64"}});
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("缓存延迟探针提前停止：") + error.what());
  }
}

void benchmark_tlb(const Options &options, std::vector<Observation> &observations,
                   std::vector<std::string> &warnings, int cpu) {
  pin_to_cpu(cpu);
  const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  const std::size_t last_pages = options.profile == "smoke" ? 64
      : options.profile == "quick" ? 8192
      : options.profile == "deep" ? 131072
                                  : 32768;
  std::mt19937 random(options.seed ^ 0x71B5U);
  try {
    for (const auto pages : power_sizes(2, last_pages)) {
      const std::size_t bytes = pages * page_size;
      MappedBuffer buffer(bytes, HugePageAdvice::AvoidHuge);
      build_random_page_chain(buffer.bytes(), pages, page_size, random);
      const std::size_t iterations = std::clamp<std::size_t>(pages * 64, 100000, 3000000);
      (void)chase_chain(buffer.bytes(), std::min<std::size_t>(iterations, pages * 2));
      const int samples = options.profile == "smoke" ? 1 : options.profile == "quick" ? 3 : 5;
      std::vector<double> latencies;
      for (int sample = 0; sample < samples; ++sample)
        latencies.push_back(chase_chain(buffer.bytes(), iterations));
      const double latency = median_double(std::move(latencies));
      add_observation(observations, "tlb_latency", "page_random_load_latency", latency,
                      "ns/access", "medium", "one random dependent access per base page",
                      {{"cpu", std::to_string(cpu)},
                       {"pages", std::to_string(pages)},
                       {"page_bytes", std::to_string(page_size)},
                       {"cache_set_spread", std::to_string(page_size / 64)},
                       {"page_policy", "base-page-advised"},
                       {"working_set_bytes", std::to_string(bytes)}});
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("TLB 探针提前停止：") + error.what());
  }
}

enum class StreamOperation { Read, Write, Copy, Triad };

std::size_t stream_array_count(StreamOperation operation) {
  switch (operation) {
    case StreamOperation::Read:
    case StreamOperation::Write: return 1;
    case StreamOperation::Copy: return 2;
    case StreamOperation::Triad: return 3;
  }
  return 1;
}

std::string stream_name(StreamOperation operation) {
  switch (operation) {
    case StreamOperation::Read: return "read";
    case StreamOperation::Write: return "write";
    case StreamOperation::Copy: return "copy";
    case StreamOperation::Triad: return "triad";
  }
  return "unknown";
}

double stream_bytes_per_element(StreamOperation operation) {
  switch (operation) {
    case StreamOperation::Read:
    case StreamOperation::Write: return sizeof(std::uint64_t);
    case StreamOperation::Copy: return 2.0 * sizeof(std::uint64_t);
    case StreamOperation::Triad: return 3.0 * sizeof(std::uint64_t);
  }
  return 0.0;
}

struct StreamResult {
  double gigabytes_per_second = 0.0;
  double elapsed_seconds = 0.0;
  std::uint64_t passes = 0;
};

StreamResult run_stream(StreamOperation operation, const std::vector<int> &cpus,
                        std::size_t bytes_per_array, int duration_ms,
                        int memory_init_cpu = -1) {
  const std::size_t elements = std::max<std::size_t>(cpus.size() * 1024,
                                                      bytes_per_array / sizeof(std::uint64_t));
  MappedBuffer a(elements * sizeof(std::uint64_t));
  std::unique_ptr<MappedBuffer> b;
  std::unique_ptr<MappedBuffer> c;
  if (operation == StreamOperation::Copy || operation == StreamOperation::Triad) {
    b = std::make_unique<MappedBuffer>(elements * sizeof(std::uint64_t));
  }
  if (operation == StreamOperation::Triad) {
    c = std::make_unique<MappedBuffer>(elements * sizeof(std::uint64_t));
  }
  auto *a_ptr = a.as<std::uint64_t>();
  auto *b_ptr = b ? b->as<std::uint64_t>() : nullptr;
  auto *c_ptr = c ? c->as<std::uint64_t>() : nullptr;
  if (memory_init_cpu >= 0) {
    pin_to_cpu(memory_init_cpu);
    for (std::size_t index = 0; index < elements; ++index) {
      a_ptr[index] = 1;
      if (b_ptr != nullptr) b_ptr[index] = 2;
      if (c_ptr != nullptr) c_ptr[index] = 3;
    }
  }

  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::vector<std::uint64_t> passes(cpus.size(), 0);
  std::vector<std::uint64_t> sums(cpus.size(), 0);
  std::vector<std::thread> workers;
  workers.reserve(cpus.size());
  for (std::size_t worker = 0; worker < cpus.size(); ++worker) {
    workers.emplace_back([&, worker] {
      pin_to_cpu(cpus[worker]);
      const std::size_t begin = elements * worker / cpus.size();
      const std::size_t end = elements * (worker + 1) / cpus.size();
      if (memory_init_cpu < 0) {
        for (std::size_t index = begin; index < end; ++index) {
          a_ptr[index] = 1;
          if (b_ptr != nullptr) b_ptr[index] = 2;
          if (c_ptr != nullptr) c_ptr[index] = 3;
        }
      }
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
        cpu_relax();
      }
      std::uint64_t sum = 0;
      std::uint64_t count = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        switch (operation) {
          case StreamOperation::Read:
            for (std::size_t index = begin; index < end; ++index) sum += a_ptr[index];
            break;
          case StreamOperation::Write:
            for (std::size_t index = begin; index < end; ++index) a_ptr[index] = sum + 1;
            break;
          case StreamOperation::Copy:
            for (std::size_t index = begin; index < end; ++index) a_ptr[index] = b_ptr[index];
            break;
          case StreamOperation::Triad:
            for (std::size_t index = begin; index < end; ++index)
              a_ptr[index] = b_ptr[index] + 3 * c_ptr[index];
            break;
        }
        // Make each completed pass observable without turning individual accesses volatile.
        // This preserves vectorization while preventing dead-store/pass collapsing.
        asm volatile("" : : "r"(a_ptr) : "memory");
        ++count;
      }
      passes[worker] = count;
      sums[worker] = sum;
    });
  }
  while (ready.load(std::memory_order_acquire) != static_cast<int>(cpus.size())) {
    std::this_thread::yield();
  }
  const auto begin = Clock::now();
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
  stop.store(true, std::memory_order_release);
  for (auto &worker : workers) worker.join();
  const auto end = Clock::now();
  const double elapsed = seconds_between(begin, end);
  const std::uint64_t total_passes = std::accumulate(passes.begin(), passes.end(), 0ULL);
  double touched_elements = 0.0;
  for (std::size_t worker = 0; worker < cpus.size(); ++worker) {
    const std::size_t begin_index = elements * worker / cpus.size();
    const std::size_t end_index = elements * (worker + 1) / cpus.size();
    touched_elements += static_cast<double>(end_index - begin_index) *
                        static_cast<double>(passes[worker]);
  }
  global_sink = global_sink ^ std::accumulate(sums.begin(), sums.end(), 0ULL);
  return {touched_elements * stream_bytes_per_element(operation) / elapsed / 1e9,
          elapsed, total_passes};
}

std::vector<std::size_t> thread_counts(std::size_t maximum) {
  std::vector<std::size_t> values{1};
  for (std::size_t count = 2; count < maximum; count *= 2) values.push_back(count);
  if (values.back() != maximum) values.push_back(maximum);
  return values;
}

void benchmark_bandwidth(const Options &options, const std::vector<CpuInfo> &cpus,
                         std::vector<Observation> &observations,
                         std::vector<std::string> &warnings) {
  const auto physical = numa_balanced_physical_cpus(cpus);
  const std::size_t max_threads = options.profile == "smoke" ? 1
      : options.profile == "quick" ? std::min<std::size_t>(physical.size(), 8)
      : physical.size();
  const std::size_t requested_bytes = options.memory_mib * 1024U * 1024U;
  std::vector<int> physical_ids;
  physical_ids.reserve(physical.size());
  for (const auto &cpu : physical) physical_ids.push_back(cpu.cpu);
  const std::size_t aggregate_llc = aggregate_last_level_cache_bytes(physical_ids);
  const std::size_t automatic_cap = options.profile == "quick" ? 512ULL * 1024ULL * 1024ULL
      : options.profile == "deep" ? 4ULL * 1024ULL * 1024ULL * 1024ULL
                                  : 2ULL * 1024ULL * 1024ULL * 1024ULL;
  const std::size_t available = memory_available_bytes();
  const std::size_t safe_per_array = available > 0 ? available / 6 : automatic_cap;
  const std::size_t desired_bytes = options.memory_explicit
      ? requested_bytes : std::max(requested_bytes, aggregate_llc * 2);
  const std::size_t bytes = std::max<std::size_t>(4096,
      std::min(desired_bytes, options.memory_explicit
          ? safe_per_array : std::min(automatic_cap, safe_per_array)));
  const bool read_working_set_exceeds_llc =
      aggregate_llc > 0 && bytes >= aggregate_llc * 2;
  if (!read_working_set_exceeds_llc) {
    warnings.push_back(
        "内存带宽工作集未达到整机 LLC 的 2 倍；结果可能包含缓存带宽，已降低置信度"
        "（工作集=" + std::to_string(bytes) + " 字节，整机 LLC=" +
        std::to_string(aggregate_llc) + " 字节）");
  }
  if (bytes < desired_bytes) {
    warnings.push_back("内存带宽工作集受" + std::string(
                           options.memory_explicit ? "MemAvailable 安全预算" :
                                                     "档位上限或 MemAvailable 安全预算") +
                       "限制：期望 " +
                       std::to_string(desired_bytes) + " 字节，实际 " +
                       std::to_string(bytes) + " 字节/数组");
  }
  const std::vector<StreamOperation> operations = options.profile == "smoke"
      ? std::vector<StreamOperation>{StreamOperation::Read}
      : std::vector<StreamOperation>{StreamOperation::Read, StreamOperation::Write,
                                     StreamOperation::Copy, StreamOperation::Triad};
  try {
    for (const auto operation : operations) {
      for (const auto count : thread_counts(max_threads)) {
        std::vector<int> selected;
        for (std::size_t index = 0; index < count; ++index) selected.push_back(physical[index].cpu);
        const auto result = run_stream(operation, selected, bytes, options.duration_ms);
        const std::size_t working_set = bytes * stream_array_count(operation);
        const bool exceeds_llc = aggregate_llc > 0 && working_set >= aggregate_llc * 2;
        add_observation(observations, "memory_bandwidth", "stream_bandwidth",
                        result.gigabytes_per_second, "GB/s", exceeds_llc ? "high" : "low",
                        "parallel pinned streaming kernel; payload bytes",
                        {{"operation", stream_name(operation)},
                         {"threads", std::to_string(count)},
                         {"bytes_per_array", std::to_string(bytes)},
                         {"working_set_bytes", std::to_string(working_set)},
                         {"aggregate_llc_bytes", std::to_string(aggregate_llc)},
                         {"bytes_per_thread", std::to_string(working_set / count)},
                         {"working_set_exceeds_llc", exceeds_llc ? "true" : "false"},
                         {"cpu_scope", "physical-cores"}});
      }
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("内存带宽探针提前停止：") + error.what());
  }
}

void benchmark_cache_bandwidth(const Options &options, const std::vector<CpuInfo> &cpus,
                               std::vector<Observation> &observations,
                               std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  const int cpu = physical_cpus(cpus).front().cpu;
  const std::size_t base_last = options.profile == "quick" ? 64U * 1024U * 1024U
      : options.profile == "deep" ? 512U * 1024U * 1024U
                                  : 256U * 1024U * 1024U;
  const std::size_t cap = options.profile == "quick" ? 256ULL * 1024ULL * 1024ULL
                                                     : 1024ULL * 1024ULL * 1024ULL;
  const std::size_t last = std::min(cap, std::max(base_last, last_level_cache_bytes(cpu) * 2));
  try {
    for (const auto bytes : power_sizes(32U * 1024U, last)) {
      for (const auto operation : {StreamOperation::Read, StreamOperation::Copy}) {
        const auto result = run_stream(operation, {cpu}, bytes,
                                       std::max(20, options.duration_ms / 2));
        add_observation(observations, "cache_bandwidth", "working_set_bandwidth",
                        result.gigabytes_per_second, "GB/s", "medium",
                        "one-core repeated streaming kernel by working-set size",
                        {{"cpu", std::to_string(cpu)},
                         {"operation", stream_name(operation)},
                         {"working_set_bytes", std::to_string(bytes)}});
      }
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("缓存带宽扫描提前停止：") + error.what());
  }
}

void benchmark_stride_prefetch(const Options &options, int cpu,
                               std::vector<Observation> &observations,
                               std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  const std::size_t base_bytes = (options.profile == "quick" ? 64U : 256U) * 1024U * 1024U;
  const std::size_t cap = options.profile == "quick" ? 256ULL * 1024ULL * 1024ULL
                                                     : 1024ULL * 1024ULL * 1024ULL;
  const std::size_t bytes = std::min(cap, std::max(base_bytes, last_level_cache_bytes(cpu) * 2));
  try {
    MappedBuffer buffer(bytes);
    pin_to_cpu(cpu);
    auto *values = buffer.as<std::uint64_t>();
    const std::size_t elements = bytes / sizeof(std::uint64_t);
    for (std::size_t index = 0; index < elements; ++index) values[index] = index + 1;
    for (const std::size_t stride_bytes : {8U, 16U, 32U, 64U, 128U, 256U, 512U,
                                           1024U, 2048U, 4096U}) {
      const std::size_t stride = stride_bytes / sizeof(std::uint64_t);
      std::uint64_t sum = 0;
      std::uint64_t accesses = 0;
      const auto begin = Clock::now();
      double elapsed = 0.0;
      do {
        for (std::size_t index = 0; index < elements; index += stride) sum += values[index];
        accesses += (elements + stride - 1) / stride;
        elapsed = seconds_between(begin, Clock::now());
      } while (elapsed * 1000.0 < std::max(20, options.duration_ms / 2));
      global_sink = global_sink ^ sum;
      add_observation(observations, "memory_access", "stride_access_rate",
                      static_cast<double>(accesses) / elapsed / 1e9, "Gaccess/s", "medium",
                      "one-core sequential fixed-stride load sweep",
                      {{"cpu", std::to_string(cpu)},
                       {"stride_bytes", std::to_string(stride_bytes)},
                       {"working_set_bytes", std::to_string(bytes)}});
      add_observation(observations, "memory_access", "stride_payload_bandwidth",
                      static_cast<double>(accesses * sizeof(std::uint64_t)) / elapsed / 1e9,
                      "GB/s", "medium", "requested 8-byte payload only; cache-line traffic excluded",
                      {{"cpu", std::to_string(cpu)},
                       {"stride_bytes", std::to_string(stride_bytes)},
                       {"working_set_bytes", std::to_string(bytes)}});
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("步长/预取扫描提前停止：") + error.what());
  }
}

void benchmark_memory_parallelism(const Options &options, int cpu,
                                  std::vector<Observation> &observations,
                                  std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  const std::size_t llc = last_level_cache_bytes(cpu);
  const std::size_t base_total = (options.profile == "quick" ? 64U : 256U) * 1024U * 1024U;
  const std::size_t total_cap = options.profile == "quick" ? 256ULL * 1024ULL * 1024ULL
                                                          : 1024ULL * 1024ULL * 1024ULL;
  const std::size_t total_working_set = std::min(total_cap, std::max(base_total, llc * 2));
  const std::vector<std::size_t> chain_counts = options.profile == "quick"
      ? std::vector<std::size_t>{1, 2, 4, 8}
      : std::vector<std::size_t>{1, 2, 4, 8, 16};
  std::mt19937 random(options.seed ^ 0x4D4C5055U);
  pin_to_cpu(cpu);
  try {
    for (const auto chains : chain_counts) {
      const std::size_t per_chain = total_working_set / chains;
      MappedBuffer buffer(per_chain * chains);
      for (std::size_t chain = 0; chain < chains; ++chain)
        build_random_chain(buffer.bytes() + chain * per_chain, per_chain, 64, random);
      std::vector<std::uint32_t> positions(chains, 0);
      const std::size_t total_accesses = options.profile == "quick" ? 1200000 : 4000000;
      const std::size_t rounds = std::max<std::size_t>(10000, total_accesses / chains);
      for (std::size_t warm = 0; warm < std::min<std::size_t>(rounds, 10000); ++warm) {
        for (std::size_t chain = 0; chain < chains; ++chain) {
          positions[chain] = *reinterpret_cast<volatile std::uint32_t *>(
              buffer.bytes() + chain * per_chain + positions[chain]);
        }
      }
      const auto begin = Clock::now();
      for (std::size_t round = 0; round < rounds; ++round) {
        for (std::size_t chain = 0; chain < chains; ++chain) {
          positions[chain] = *reinterpret_cast<volatile std::uint32_t *>(
              buffer.bytes() + chain * per_chain + positions[chain]);
        }
      }
      const double elapsed = seconds_between(begin, Clock::now());
      global_sink = global_sink ^ std::accumulate(positions.begin(), positions.end(), 0ULL);
      const double accesses = static_cast<double>(rounds * chains);
      add_observation(observations, "memory_parallelism", "effective_load_latency",
                      elapsed * 1e9 / accesses, "ns/access", "medium",
                      "interleaved independent random dependent-load chains",
                      {{"cpu", std::to_string(cpu)},
                       {"chains", std::to_string(chains)},
                       {"bytes_per_chain", std::to_string(per_chain)},
                       {"working_set_bytes", std::to_string(total_working_set)}});
      add_observation(observations, "memory_parallelism", "random_load_rate",
                      accesses / elapsed / 1e9, "Gaccess/s", "medium",
                      "interleaved independent random dependent-load chains",
                      {{"cpu", std::to_string(cpu)},
                       {"chains", std::to_string(chains)},
                       {"bytes_per_chain", std::to_string(per_chain)},
                       {"working_set_bytes", std::to_string(total_working_set)}});
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("内存级并行探针提前停止：") + error.what());
  }
}

std::size_t mapping_anon_huge_bytes(const void *address) {
  std::ifstream smaps("/proc/self/smaps");
  if (!smaps) return 0;
  const auto target = reinterpret_cast<std::uintptr_t>(address);
  bool selected = false;
  std::string line;
  while (std::getline(smaps, line)) {
    const auto dash = line.find('-');
    const auto space = line.find(' ');
    if (dash != std::string::npos && space != std::string::npos && dash < space) {
      try {
        const auto begin = static_cast<std::uintptr_t>(std::stoull(line.substr(0, dash), nullptr, 16));
        const auto end = static_cast<std::uintptr_t>(
            std::stoull(line.substr(dash + 1, space - dash - 1), nullptr, 16));
        selected = target >= begin && target < end;
      } catch (...) {
        selected = false;
      }
      continue;
    }
    if (selected && line.rfind("AnonHugePages:", 0) == 0) {
      std::istringstream input(line.substr(std::string("AnonHugePages:").size()));
      std::size_t kib = 0;
      input >> kib;
      return kib * 1024;
    }
  }
  return 0;
}

void benchmark_page_policy(const Options &options, int cpu,
                           std::vector<Observation> &observations,
                           std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  const std::size_t profile_floor = options.profile == "quick" ? 32U * 1024U * 1024U
      : options.profile == "deep" ? 256U * 1024U * 1024U
                                  : 128U * 1024U * 1024U;
  const std::size_t profile_cap = options.profile == "deep" ? 512ULL * 1024ULL * 1024ULL
                                                            : 256ULL * 1024ULL * 1024ULL;
  const std::size_t bytes = std::min(profile_cap,
      std::max(profile_floor, last_level_cache_bytes(cpu) * 2));
  const std::size_t iterations = options.profile == "quick" ? 300000 : 1000000;
  for (const auto &[name, advice] :
       {std::pair{"base-page-advised", HugePageAdvice::AvoidHuge},
        std::pair{"thp-advised", HugePageAdvice::PreferHuge}}) {
    try {
      MappedBuffer buffer(bytes, advice);
      std::mt19937 random(options.seed ^ (advice == HugePageAdvice::PreferHuge ? 0x48554745U
                                                                              : 0x42415345U));
      build_random_chain(buffer.bytes(), bytes, 64, random);
      (void)chase_chain(buffer.bytes(), 10000);
      std::vector<double> samples;
      for (int sample = 0; sample < (options.profile == "quick" ? 2 : 3); ++sample) {
        samples.push_back(chase_chain(buffer.bytes(), iterations));
      }
      const std::size_t huge_bytes = mapping_anon_huge_bytes(buffer.bytes());
      add_observation(observations, "page_policy", "random_load_latency",
                      median_double(std::move(samples)), "ns/access",
                      advice == HugePageAdvice::PreferHuge && huge_bytes == 0 ? "low" : "medium",
                      "same pointer chase with MADV_NOHUGEPAGE versus MADV_HUGEPAGE",
                      {{"cpu", std::to_string(cpu)},
                       {"policy", name},
                       {"working_set_bytes", std::to_string(bytes)},
                       {"anon_huge_bytes", std::to_string(huge_bytes)}});
    } catch (const std::exception &error) {
      warnings.push_back(std::string("页策略对比探针提前停止：") + error.what());
      return;
    }
  }
}

void benchmark_loaded_memory_latency(const Options &options,
                                     const std::vector<CpuInfo> &cpus,
                                     std::vector<Observation> &observations,
                                     std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  const auto physical = numa_balanced_physical_cpus(cpus);
  const int latency_cpu = physical.front().cpu;
  std::vector<int> load_cpus;
  for (const auto &cpu : physical) {
    if (cpu.cpu != latency_cpu) load_cpus.push_back(cpu.cpu);
  }
  const std::size_t max_load_threads = options.profile == "quick"
      ? std::min<std::size_t>(load_cpus.size(), 4) : load_cpus.size();
  std::vector<std::size_t> loads{0};
  if (max_load_threads > 0) {
    const auto nonzero = thread_counts(max_load_threads);
    loads.insert(loads.end(), nonzero.begin(), nonzero.end());
  }

  const std::size_t floor = options.profile == "quick" ? 64U * 1024U * 1024U
      : options.profile == "deep" ? 256U * 1024U * 1024U
                                  : 128U * 1024U * 1024U;
  const std::size_t cap = options.profile == "deep" ? 512ULL * 1024ULL * 1024ULL
                                                    : 256ULL * 1024ULL * 1024ULL;
  const std::size_t bytes = std::min(cap, std::max(floor, last_level_cache_bytes(latency_cpu) * 2));
  const std::size_t latency_iterations = options.profile == "quick" ? 300000
      : options.profile == "deep" ? 2000000
                                  : 1000000;
  try {
    MappedBuffer latency_buffer(bytes);
    MappedBuffer load_buffer(bytes);
    std::mt19937 random(options.seed ^ 0x10ADED1U);
    build_random_chain(latency_buffer.bytes(), latency_buffer.size(), 64, random);
    auto *load_values = load_buffer.as<std::uint64_t>();
    const std::size_t elements = load_buffer.size() / sizeof(std::uint64_t);
    for (std::size_t index = 0; index < elements; ++index) load_values[index] = index + 1;

    for (const std::size_t load_threads : loads) {
      std::atomic<std::size_t> ready{0};
      std::atomic<bool> go{false};
      std::atomic<bool> stop{false};
      std::vector<std::uint64_t> transferred(load_threads, 0);
      std::vector<std::uint64_t> sums(load_threads, 0);
      std::vector<std::thread> workers;
      for (std::size_t worker = 0; worker < load_threads; ++worker) {
        workers.emplace_back([&, worker] {
          pin_to_cpu(load_cpus[worker]);
          const std::size_t begin = elements * worker / load_threads;
          const std::size_t end = elements * (worker + 1) / load_threads;
          std::uint64_t local_sum = 0;
          std::uint64_t local_bytes = 0;
          ready.fetch_add(1, std::memory_order_release);
          while (!go.load(std::memory_order_acquire)) cpu_relax();
          while (!stop.load(std::memory_order_relaxed)) {
            for (std::size_t index = begin; index < end; ++index) local_sum += load_values[index];
            local_bytes += (end - begin) * sizeof(std::uint64_t);
          }
          transferred[worker] = local_bytes;
          sums[worker] = local_sum;
        });
      }
      while (ready.load(std::memory_order_acquire) != load_threads) std::this_thread::yield();
      pin_to_cpu(latency_cpu);
      (void)chase_chain(latency_buffer.bytes(), 10000);
      const auto begin = Clock::now();
      go.store(true, std::memory_order_release);
      if (load_threads > 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      const double latency = chase_chain(latency_buffer.bytes(), latency_iterations);
      stop.store(true, std::memory_order_release);
      for (auto &worker : workers) worker.join();
      const double elapsed = seconds_between(begin, Clock::now());
      const auto total_bytes = std::accumulate(transferred.begin(), transferred.end(), std::uint64_t{0});
      global_sink = global_sink ^ std::accumulate(sums.begin(), sums.end(), std::uint64_t{0});
      const double bandwidth = static_cast<double>(total_bytes) / std::max(elapsed, 1e-12) / 1e9;
      const Labels labels{{"latency_cpu", std::to_string(latency_cpu)},
                          {"load_threads", std::to_string(load_threads)},
                          {"working_set_bytes", std::to_string(bytes)},
                          {"measured_load_gbps", std::to_string(bandwidth)}};
      add_observation(observations, "loaded_memory_latency", "random_load_latency_under_load",
                      latency, "ns/access", "medium",
                      "dependent pointer chase while pinned cores generate streaming read traffic",
                      labels);
      add_observation(observations, "loaded_memory_latency", "concurrent_read_bandwidth",
                      bandwidth, "GB/s", "medium",
                      "payload generated concurrently with the dependent-load latency probe",
                      labels);
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("带载内存延迟探针提前停止：") + error.what());
  }
}

enum class StoreForwardCase { Exact64, Partial32To64, OverlapByOne, SplitLine };

std::pair<double, double> store_forward_measurement(StoreForwardCase test_case,
                                                     std::size_t iterations) {
  alignas(128) std::array<std::byte, 256> storage{};
  auto *base = storage.data() + (test_case == StoreForwardCase::SplitLine ? 60 : 64);
  std::uint64_t value = 0x123456789abcdef0ULL;
  const auto tick_begin = cycle_counter();
  const auto wall_begin = Clock::now();
#if defined(__x86_64__)
  if (test_case == StoreForwardCase::Partial32To64) {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("movl %k[value], (%[address])\n\tmovq (%[address]), %[value]"
                   : [value] "+r"(value) : [address] "r"(base) : "memory");
    }
  } else if (test_case == StoreForwardCase::OverlapByOne) {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("movq %[value], (%[store])\n\tmovq (%[load]), %[value]"
                   : [value] "+r"(value)
                   : [store] "r"(base), [load] "r"(base + 1) : "memory");
    }
  } else {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("movq %[value], (%[address])\n\tmovq (%[address]), %[value]"
                   : [value] "+r"(value) : [address] "r"(base) : "memory");
    }
  }
#elif defined(__aarch64__)
  if (test_case == StoreForwardCase::Partial32To64) {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("str %w[value], [%[address]]\n\tldr %x[value], [%[address]]"
                   : [value] "+r"(value) : [address] "r"(base) : "memory");
    }
  } else if (test_case == StoreForwardCase::OverlapByOne) {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("str %x[value], [%[store]]\n\tldr %x[value], [%[load]]"
                   : [value] "+r"(value)
                   : [store] "r"(base), [load] "r"(base + 1) : "memory");
    }
  } else {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("str %x[value], [%[address]]\n\tldr %x[value], [%[address]]"
                   : [value] "+r"(value) : [address] "r"(base) : "memory");
    }
  }
#endif
  const double seconds = seconds_between(wall_begin, Clock::now());
  const auto ticks = cycle_counter() - tick_begin;
  global_sink = global_sink ^ value;
  return {seconds * 1e9 / static_cast<double>(iterations),
          static_cast<double>(ticks) / static_cast<double>(iterations)};
}

void benchmark_store_forwarding(const Options &options, int cpu,
                                std::vector<Observation> &observations) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  const std::size_t iterations = options.profile == "quick" ? 1000000 : 5000000;
  struct Case {
    StoreForwardCase value;
    const char *name;
    int store_bytes;
    int load_bytes;
    int store_offset;
    int load_offset;
  };
  const std::array<Case, 4> cases{{
      {StoreForwardCase::Exact64, "exact-8-to-8", 8, 8, 0, 0},
      {StoreForwardCase::Partial32To64, "partial-4-to-8", 4, 8, 0, 0},
      {StoreForwardCase::OverlapByOne, "overlap-offset-1", 8, 8, 0, 1},
      {StoreForwardCase::SplitLine, "split-cache-line", 8, 8, 60, 60},
  }};
  for (const auto &test_case : cases) {
    const auto [nanoseconds, ticks] = store_forward_measurement(test_case.value, iterations);
    const Labels labels{{"case", test_case.name},
                        {"store_bytes", std::to_string(test_case.store_bytes)},
                        {"load_bytes", std::to_string(test_case.load_bytes)},
                        {"store_offset", std::to_string(test_case.store_offset)},
                        {"load_offset", std::to_string(test_case.load_offset)},
                        {"cpu", std::to_string(cpu)}};
    add_observation(observations, "store_forwarding", "store_load_latency",
                    nanoseconds, "ns/pair", "medium",
                    "dependent store followed by overlapping load", labels);
    add_observation(observations, "store_forwarding", "store_load_counter_ticks",
                    ticks, "counter-ticks/pair", "low",
                    "platform counter delta for dependent store/load pair", labels);
  }
}

std::string cpu_relation(const CpuInfo &left, const CpuInfo &right) {
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

double ping_pong_latency(int cpu_a, int cpu_b, std::size_t rounds) {
  alignas(128) std::atomic<int> turn{0};
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  double elapsed = 0.0;
  std::thread first([&] {
    pin_to_cpu(cpu_a);
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    const auto begin = Clock::now();
    for (std::size_t round = 0; round < rounds; ++round) {
      while (turn.load(std::memory_order_acquire) != 0) cpu_relax();
      turn.store(1, std::memory_order_release);
      while (turn.load(std::memory_order_acquire) != 0) cpu_relax();
    }
    elapsed = seconds_between(begin, Clock::now());
  });
  std::thread second([&] {
    pin_to_cpu(cpu_b);
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    for (std::size_t round = 0; round < rounds; ++round) {
      while (turn.load(std::memory_order_acquire) != 1) cpu_relax();
      turn.store(0, std::memory_order_release);
    }
  });
  while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
  go.store(true, std::memory_order_release);
  first.join();
  second.join();
  return elapsed * 1e9 / static_cast<double>(rounds) / 2.0;
}

void benchmark_core_latency(const Options &options, const std::vector<CpuInfo> &cpus,
                            std::vector<Observation> &observations,
                            std::vector<std::string> &warnings) {
  if (cpus.size() < 2) {
    warnings.push_back("已跳过核间延迟：当前进程亲和性范围内不足两个 CPU");
    return;
  }
  using Pair = std::tuple<CpuInfo, CpuInfo, std::string>;
  std::vector<Pair> pairs;
  if (options.profile == "deep") {
    for (std::size_t left = 0; left < cpus.size(); ++left) {
      for (std::size_t right = left + 1; right < cpus.size(); ++right) {
        pairs.emplace_back(cpus[left], cpus[right], "logical-cpu");
      }
    }
  } else if (options.profile == "standard") {
    const auto physical = physical_cpus(cpus);
    for (std::size_t left = 0; left < physical.size(); ++left) {
      for (std::size_t right = left + 1; right < physical.size(); ++right) {
        pairs.emplace_back(physical[left], physical[right], "physical-core");
      }
    }

    // Physical-core representatives intentionally omit SMT siblings. Keep one
    // additional pair so the relation summary still describes the SMT path.
    for (std::size_t left = 0; left < cpus.size(); ++left) {
      bool found = false;
      for (std::size_t right = left + 1; right < cpus.size(); ++right) {
        if (cpu_relation(cpus[left], cpus[right]) == "smt-sibling") {
          pairs.emplace_back(cpus[left], cpus[right], "representative");
          found = true;
          break;
        }
      }
      if (found) break;
    }
  } else {
    std::set<std::string> represented;
    for (std::size_t left = 0; left < cpus.size(); ++left) {
      for (std::size_t right = left + 1; right < cpus.size(); ++right) {
        const auto relation = cpu_relation(cpus[left], cpus[right]);
        if (represented.insert(relation).second) {
          pairs.emplace_back(cpus[left], cpus[right], "representative");
        }
      }
    }
  }

  const std::size_t pair_count = std::max<std::size_t>(1, pairs.size());
  const std::size_t rounds = options.profile == "smoke" ? 2000
      : options.profile == "quick" ? 20000
      : options.profile == "deep"
          ? std::clamp<std::size_t>(5000000 / pair_count, 5000, 100000)
          : std::clamp<std::size_t>(2000000 / pair_count, 2000, 50000);
  for (const auto &[left, right, matrix_scope] : pairs) {
    const double latency = ping_pong_latency(left.cpu, right.cpu, rounds);
    add_observation(observations, "core_latency", "cacheline_handoff_latency", latency,
                    "ns/one-way", "high", "release/acquire cache-line ping-pong",
                    {{"cpu_a", std::to_string(left.cpu)},
                     {"cpu_b", std::to_string(right.cpu)},
                     {"node_a", std::to_string(left.node)},
                     {"node_b", std::to_string(right.node)},
                     {"relation", cpu_relation(left, right)},
                     {"matrix_scope", matrix_scope},
                     {"rounds", std::to_string(rounds)}});
  }
}

double false_sharing_rate(int cpu_a, int cpu_b, std::size_t separation, int duration_ms) {
  MappedBuffer buffer(4096);
  auto *base = buffer.bytes();
  auto *first = new (base) std::atomic<std::uint64_t>(0);
  auto *second = new (base + separation) std::atomic<std::uint64_t>(0);
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::uint64_t first_count = 0;
  std::uint64_t second_count = 0;
  auto worker = [&](int cpu, std::atomic<std::uint64_t> *value, std::uint64_t &count) {
    pin_to_cpu(cpu);
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    while (!stop.load(std::memory_order_relaxed)) {
      for (int index = 0; index < 64; ++index) {
        value->fetch_add(1, std::memory_order_relaxed);
      }
      count += 64;
    }
  };
  std::thread one(worker, cpu_a, first, std::ref(first_count));
  std::thread two(worker, cpu_b, second, std::ref(second_count));
  while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
  const auto begin = Clock::now();
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
  stop.store(true, std::memory_order_release);
  one.join();
  two.join();
  const double elapsed = seconds_between(begin, Clock::now());
  std::destroy_at(first);
  std::destroy_at(second);
  return static_cast<double>(first_count + second_count) / elapsed / 1e6;
}

void benchmark_false_sharing(const Options &options, const std::vector<CpuInfo> &cpus,
                             std::vector<Observation> &observations) {
  const auto physical = physical_cpus(cpus);
  if (physical.size() < 2 || options.profile == "smoke") return;
  const int cpu_a = physical[0].cpu;
  const int cpu_b = physical[1].cpu;
  for (const std::size_t separation : {8U, 16U, 32U, 64U, 128U, 256U}) {
    const double rate = false_sharing_rate(cpu_a, cpu_b, separation,
                                           std::max(20, options.duration_ms / 2));
    add_observation(observations, "coherence", "atomic_update_rate", rate, "Mop/s",
                    "medium", "two independent atomics at varying byte separation",
                    {{"cpu_a", std::to_string(cpu_a)},
                     {"cpu_b", std::to_string(cpu_b)},
                     {"separation_bytes", std::to_string(separation)}});
  }
}

bool bind_memory(void *address, std::size_t bytes, int node, std::string &error) {
#if defined(SYS_mbind)
  constexpr std::size_t bits = sizeof(unsigned long) * 8;
  const unsigned long maxnode = static_cast<unsigned long>(node + 1);
  std::vector<unsigned long> mask((maxnode + bits - 1) / bits, 0);
  mask[static_cast<std::size_t>(node) / bits] |=
      1UL << (static_cast<std::size_t>(node) % bits);
  if (syscall(SYS_mbind, address, bytes, MPOL_BIND, mask.data(), maxnode, 0) == 0) {
    return true;
  }
  error = std::strerror(errno);
  return false;
#else
  (void)address;
  (void)bytes;
  (void)node;
  error = "mbind syscall is unavailable";
  return false;
#endif
}

double read_existing(std::uint64_t *data, std::size_t elements, const std::vector<int> &cpus,
                     int duration_ms) {
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::vector<std::uint64_t> passes(cpus.size(), 0);
  std::vector<std::uint64_t> sums(cpus.size(), 0);
  std::vector<std::thread> workers;
  for (std::size_t worker = 0; worker < cpus.size(); ++worker) {
    workers.emplace_back([&, worker] {
      pin_to_cpu(cpus[worker]);
      const std::size_t begin = elements * worker / cpus.size();
      const std::size_t end = elements * (worker + 1) / cpus.size();
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) cpu_relax();
      std::uint64_t sum = 0;
      std::uint64_t count = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        for (std::size_t index = begin; index < end; ++index) sum += data[index];
        ++count;
      }
      passes[worker] = count;
      sums[worker] = sum;
    });
  }
  while (ready.load(std::memory_order_acquire) != static_cast<int>(cpus.size()))
    std::this_thread::yield();
  const auto begin = Clock::now();
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
  stop.store(true, std::memory_order_release);
  for (auto &worker : workers) worker.join();
  const double elapsed = seconds_between(begin, Clock::now());
  double touched = 0.0;
  for (std::size_t worker = 0; worker < cpus.size(); ++worker) {
    const auto begin_index = elements * worker / cpus.size();
    const auto end_index = elements * (worker + 1) / cpus.size();
    touched += static_cast<double>(end_index - begin_index) * passes[worker];
  }
  global_sink = global_sink ^ std::accumulate(sums.begin(), sums.end(), 0ULL);
  return touched * sizeof(std::uint64_t) / elapsed / 1e9;
}

void benchmark_numa(const Options &options, const std::vector<CpuInfo> &cpus,
                    std::vector<Observation> &observations,
                    std::vector<std::string> &warnings) {
  std::map<int, std::vector<CpuInfo>> by_node;
  for (const auto &cpu : physical_cpus(cpus)) by_node[cpu.node].push_back(cpu);
  if (options.profile == "smoke") {
    return;
  }
  if (by_node.size() < 2)
    warnings.push_back("跨 NUMA 路径不可用：可访问 NUMA 节点不足两个；仍测量本地对角线");
  const std::size_t base_bytes = (options.profile == "quick" ? 64U : 256U) * 1024U * 1024U;
  const std::size_t numa_cap = options.profile == "quick" ? 512ULL * 1024ULL * 1024ULL
                                                         : 2ULL * 1024ULL * 1024ULL * 1024ULL;
  std::map<int, std::size_t> node_llc_bytes;
  std::size_t largest_node_llc = 0;
  for (const auto &[node, node_cpus] : by_node) {
    std::vector<int> ids;
    for (const auto &cpu : node_cpus) ids.push_back(cpu.cpu);
    node_llc_bytes[node] = aggregate_last_level_cache_bytes(ids);
    largest_node_llc = std::max(largest_node_llc, node_llc_bytes[node]);
  }
  const std::size_t requested_bytes = options.memory_mib * 1024U * 1024U;
  const std::size_t desired_bytes = options.memory_explicit
      ? requested_bytes : std::max(base_bytes, largest_node_llc * 2);
  const std::size_t available = memory_available_bytes();
  const std::size_t safe_bytes = available > 0 ? available / 3 : numa_cap;
  const std::size_t bytes = std::max<std::size_t>(4096,
      std::min(desired_bytes, options.memory_explicit
          ? safe_bytes : std::min(numa_cap, safe_bytes)));
  if (bytes < desired_bytes) {
    warnings.push_back("NUMA 工作集受" + std::string(
                           options.memory_explicit ? "MemAvailable 安全预算" :
                                                     "档位上限或 MemAvailable 安全预算") +
                       "限制：期望 " + std::to_string(desired_bytes) + " 字节，实际 " +
                       std::to_string(bytes) + " 字节");
  }
  if (largest_node_llc == 0 || bytes < largest_node_llc * 2) {
    warnings.push_back(
        "NUMA 带宽工作集未确认达到读取节点 LLC 的 2 倍；远端带宽可能包含缓存复用，已降低置信度"
        "（工作集=" + std::to_string(bytes) + " 字节，最大节点 LLC=" +
        std::to_string(largest_node_llc) + " 字节）");
  }
  std::mt19937 random(options.seed ^ 0x4E554D41U);
  bool warned_bind = false;
  for (const auto &[memory_node, memory_cpus] : by_node) {
    try {
      MappedBuffer buffer(bytes);
      std::string bind_error;
      const bool bound = bind_memory(buffer.bytes(), buffer.size(), memory_node, bind_error);
      if (!bound && !warned_bind) {
        warnings.push_back("mbind 不可用（" + bind_error +
                           "）；NUMA 放置退化为固定线程首次触碰");
        warned_bind = true;
      }
      for (const auto &[cpu_node, destination_cpus] : by_node) {
        pin_to_cpu(memory_cpus.front().cpu);
        build_random_chain(buffer.bytes(), buffer.size(), 64, random);
        pin_to_cpu(destination_cpus.front().cpu);
        const std::size_t iterations = options.profile == "quick" ? 300000 : 1000000;
        (void)chase_chain(buffer.bytes(), 10000);
        const double latency = chase_chain(buffer.bytes(), iterations);
        const int memory_socket = memory_cpus.front().socket;
        const int cpu_socket = destination_cpus.front().socket;
        const std::string relation = memory_node == cpu_node ? "local"
            : memory_socket == cpu_socket ? "cross-numa-same-socket" : "cross-socket";
        const Labels labels = {
            {"memory_node", std::to_string(memory_node)},
            {"cpu_node", std::to_string(cpu_node)},
            {"memory_socket", std::to_string(memory_socket)},
            {"cpu_socket", std::to_string(cpu_socket)},
            {"relation", relation},
            {"local", memory_node == cpu_node ? "true" : "false"},
            {"placement", bound ? "mbind" : "pinned-first-touch"}};
        add_observation(observations, "numa", "load_latency", latency, "ns/access",
                        bound ? "high" : "medium", "NUMA-placed random pointer chase", labels);

        // Reinitialize as doubles on the source node before the read-bandwidth pass.
        pin_to_cpu(memory_cpus.front().cpu);
        auto *values = buffer.as<std::uint64_t>();
        const auto elements = bytes / sizeof(std::uint64_t);
        for (std::size_t index = 0; index < elements; ++index) values[index] = 1;
        std::vector<int> selected;
        const std::size_t thread_limit = options.profile == "quick"
            ? std::min<std::size_t>(destination_cpus.size(), 4)
            : destination_cpus.size();
        for (std::size_t index = 0; index < thread_limit; ++index)
          selected.push_back(destination_cpus[index].cpu);
        const double bandwidth = read_existing(values, elements, selected, options.duration_ms);
        auto bandwidth_labels = labels;
        const std::size_t reader_llc = node_llc_bytes[cpu_node];
        const bool exceeds_llc = reader_llc > 0 && bytes >= reader_llc * 2;
        bandwidth_labels["threads"] = std::to_string(selected.size());
        bandwidth_labels["working_set_bytes"] = std::to_string(bytes);
        bandwidth_labels["reader_node_llc_bytes"] = std::to_string(reader_llc);
        bandwidth_labels["bytes_per_thread"] = std::to_string(bytes / selected.size());
        bandwidth_labels["working_set_exceeds_llc"] = exceeds_llc ? "true" : "false";
        add_observation(observations, "numa", "read_bandwidth", bandwidth, "GB/s",
                        exceeds_llc ? (bound ? "high" : "medium") : "low",
                        "pinned NUMA aggregate read payload with LLC coverage metadata",
                        bandwidth_labels);
      }
    } catch (const std::exception &error) {
      warnings.push_back("NUMA 节点 " + std::to_string(memory_node) +
                         " 的探针提前停止：" + error.what());
    }
  }
}

struct KernelMeasurement {
  double seconds = 0.0;
  std::uint64_t counter_ticks = 0;
  PerfCounts perf;
};

template <typename Function>
KernelMeasurement measure_kernel(Function &&function) {
  function();
  PerfGroup core(PerfGroupKind::Core);
  PerfGroup branch(PerfGroupKind::Branch);
  PerfGroup cache(PerfGroupKind::Cache);
  const auto wall_begin = Clock::now();
  const auto tick_begin = cycle_counter();
  core.start();
  branch.start();
  cache.start();
  function();
  auto cache_counts = cache.stop();
  auto branch_counts = branch.stop();
  auto counts = core.stop();
  const auto tick_end = cycle_counter();
  const auto wall_end = Clock::now();
  if (branch_counts.available && branch_counts.branch_counters_available) {
    counts.branch_counters_available = true;
    counts.branches = branch_counts.branches;
    counts.branch_misses = branch_counts.branch_misses;
  }
  if (cache_counts.available && cache_counts.cache_counters_available) {
    counts.cache_counters_available = true;
    counts.cache_references = cache_counts.cache_references;
    counts.cache_misses = cache_counts.cache_misses;
  }
  return {seconds_between(wall_begin, wall_end), tick_end - tick_begin, std::move(counts)};
}

void nop_kernel(std::size_t iterations) {
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    asm volatile(".rept 128\n\tnop\n\t.endr" ::: "memory");
  }
}

void add_one_chain(std::size_t iterations) {
  std::uint64_t a0 = 1;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 64\n\taddq $1, %[a0]\n\t.endr"
                 : [a0] "+r"(a0));
#elif defined(__aarch64__)
    asm volatile(".rept 64\n\tadd %[a0], %[a0], #1\n\t.endr"
                 : [a0] "+r"(a0));
#else
    for (int index = 0; index < 64; ++index) a0 += 1;
#endif
  }
  global_sink = global_sink ^ a0;
}

void add_four_chains(std::size_t iterations) {
  std::uint64_t a0 = 1, a1 = 3, a2 = 5, a3 = 7;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 16\n\t"
                 "addq $1, %[a0]\n\taddq $1, %[a1]\n\t"
                 "addq $1, %[a2]\n\taddq $1, %[a3]\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3));
#elif defined(__aarch64__)
    asm volatile(".rept 16\n\t"
                 "add %[a0], %[a0], #1\n\tadd %[a1], %[a1], #1\n\t"
                 "add %[a2], %[a2], #1\n\tadd %[a3], %[a3], #1\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3));
#else
    for (int index = 0; index < 16; ++index) { ++a0; ++a1; ++a2; ++a3; }
#endif
  }
  global_sink = global_sink ^ a0 ^ a1 ^ a2 ^ a3;
}

void add_eight_chains(std::size_t iterations) {
  std::uint64_t a0 = 1, a1 = 3, a2 = 5, a3 = 7;
  std::uint64_t a4 = 11, a5 = 13, a6 = 17, a7 = 19;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 8\n\t"
                 "addq $1, %[a0]\n\taddq $1, %[a1]\n\t"
                 "addq $1, %[a2]\n\taddq $1, %[a3]\n\t"
                 "addq $1, %[a4]\n\taddq $1, %[a5]\n\t"
                 "addq $1, %[a6]\n\taddq $1, %[a7]\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3),
                   [a4] "+r"(a4), [a5] "+r"(a5), [a6] "+r"(a6), [a7] "+r"(a7));
#elif defined(__aarch64__)
    asm volatile(".rept 8\n\t"
                 "add %[a0], %[a0], #1\n\tadd %[a1], %[a1], #1\n\t"
                 "add %[a2], %[a2], #1\n\tadd %[a3], %[a3], #1\n\t"
                 "add %[a4], %[a4], #1\n\tadd %[a5], %[a5], #1\n\t"
                 "add %[a6], %[a6], #1\n\tadd %[a7], %[a7], #1\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3),
                   [a4] "+r"(a4), [a5] "+r"(a5), [a6] "+r"(a6), [a7] "+r"(a7));
#else
    for (int index = 0; index < 8; ++index) {
      ++a0; ++a1; ++a2; ++a3; ++a4; ++a5; ++a6; ++a7;
    }
#endif
  }
  global_sink = global_sink ^ a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7;
}

void multiply_one_chain(std::size_t iterations) {
  std::uint64_t value = 3;
  const std::uint64_t multiplier = 0x9e3779b1U;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 32\n\timulq %[multiplier], %[value]\n\t.endr"
                 : [value] "+r"(value) : [multiplier] "r"(multiplier));
#elif defined(__aarch64__)
    asm volatile(".rept 32\n\tmul %[value], %[value], %[multiplier]\n\t.endr"
                 : [value] "+r"(value) : [multiplier] "r"(multiplier));
#else
    for (int index = 0; index < 32; ++index) value *= multiplier;
#endif
  }
  global_sink = global_sink ^ value;
}

void multiply_four_chains(std::size_t iterations) {
  std::uint64_t a0 = 3, a1 = 5, a2 = 7, a3 = 11;
  const std::uint64_t multiplier = 0x9e3779b1U;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 8\n\t"
                 "imulq %[m], %[a0]\n\timulq %[m], %[a1]\n\t"
                 "imulq %[m], %[a2]\n\timulq %[m], %[a3]\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3)
                 : [m] "r"(multiplier));
#elif defined(__aarch64__)
    asm volatile(".rept 8\n\t"
                 "mul %[a0], %[a0], %[m]\n\tmul %[a1], %[a1], %[m]\n\t"
                 "mul %[a2], %[a2], %[m]\n\tmul %[a3], %[a3], %[m]\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3)
                 : [m] "r"(multiplier));
#else
    for (int index = 0; index < 8; ++index) {
      a0 *= multiplier; a1 *= multiplier; a2 *= multiplier; a3 *= multiplier;
    }
#endif
  }
  global_sink = global_sink ^ a0 ^ a1 ^ a2 ^ a3;
}

void fp_add_one_chain(std::size_t iterations) {
  double value = 1.0;
  const double increment = 0.0000001;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 32\n\taddsd %[increment], %[value]\n\t.endr"
                 : [value] "+x"(value) : [increment] "x"(increment));
#elif defined(__aarch64__)
    asm volatile(".rept 32\n\tfadd %d[value], %d[value], %d[increment]\n\t.endr"
                 : [value] "+w"(value) : [increment] "w"(increment));
#endif
  }
  global_sink = global_sink ^ static_cast<std::uint64_t>(value);
}

void fp_add_four_chains(std::size_t iterations) {
  double a0 = 1.0, a1 = 2.0, a2 = 3.0, a3 = 4.0;
  const double increment = 0.0000001;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 8\n\t"
                 "addsd %[increment], %[a0]\n\taddsd %[increment], %[a1]\n\t"
                 "addsd %[increment], %[a2]\n\taddsd %[increment], %[a3]\n\t.endr"
                 : [a0] "+x"(a0), [a1] "+x"(a1), [a2] "+x"(a2), [a3] "+x"(a3)
                 : [increment] "x"(increment));
#elif defined(__aarch64__)
    asm volatile(".rept 8\n\t"
                 "fadd %d[a0], %d[a0], %d[increment]\n\t"
                 "fadd %d[a1], %d[a1], %d[increment]\n\t"
                 "fadd %d[a2], %d[a2], %d[increment]\n\t"
                 "fadd %d[a3], %d[a3], %d[increment]\n\t.endr"
                 : [a0] "+w"(a0), [a1] "+w"(a1), [a2] "+w"(a2), [a3] "+w"(a3)
                 : [increment] "w"(increment));
#endif
  }
  global_sink = global_sink ^ static_cast<std::uint64_t>(a0 + a1 + a2 + a3);
}

void fp_multiply_one_chain(std::size_t iterations) {
  double value = 1.0001;
  const double multiplier = 1.0000000001;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 16\n\tmulsd %[multiplier], %[value]\n\t.endr"
                 : [value] "+x"(value) : [multiplier] "x"(multiplier));
#elif defined(__aarch64__)
    asm volatile(".rept 16\n\tfmul %d[value], %d[value], %d[multiplier]\n\t.endr"
                 : [value] "+w"(value) : [multiplier] "w"(multiplier));
#endif
  }
  global_sink = global_sink ^ static_cast<std::uint64_t>(value);
}

void fp_multiply_four_chains(std::size_t iterations) {
  double a0 = 1.0001, a1 = 1.0002, a2 = 1.0003, a3 = 1.0004;
  const double multiplier = 1.0000000001;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 4\n\t"
                 "mulsd %[multiplier], %[a0]\n\tmulsd %[multiplier], %[a1]\n\t"
                 "mulsd %[multiplier], %[a2]\n\tmulsd %[multiplier], %[a3]\n\t.endr"
                 : [a0] "+x"(a0), [a1] "+x"(a1), [a2] "+x"(a2), [a3] "+x"(a3)
                 : [multiplier] "x"(multiplier));
#elif defined(__aarch64__)
    asm volatile(".rept 4\n\t"
                 "fmul %d[a0], %d[a0], %d[multiplier]\n\t"
                 "fmul %d[a1], %d[a1], %d[multiplier]\n\t"
                 "fmul %d[a2], %d[a2], %d[multiplier]\n\t"
                 "fmul %d[a3], %d[a3], %d[multiplier]\n\t.endr"
                 : [a0] "+w"(a0), [a1] "+w"(a1), [a2] "+w"(a2), [a3] "+w"(a3)
                 : [multiplier] "w"(multiplier));
#endif
  }
  global_sink = global_sink ^ static_cast<std::uint64_t>(a0 + a1 + a2 + a3);
}

void record_kernel(std::vector<Observation> &observations, const std::string &name,
                   std::size_t operations, const KernelMeasurement &measurement,
                   std::vector<std::string> &warnings,
                   const Labels &extra_labels = {}) {
  const double ops = static_cast<double>(operations);
  Labels labels = extra_labels;
  labels["kernel"] = name;
  add_observation(observations, "pipeline", "operation_throughput", ops / measurement.seconds / 1e9,
                  "Gop/s", "medium", "architecture-specific scalar assembly microkernel", labels);
  add_observation(observations, "pipeline", "counter_tick_efficiency",
                  ops / static_cast<double>(measurement.counter_ticks), "ops/counter-tick", "low",
                  "operations divided by invariant/platform counter ticks", labels);
  if (measurement.perf.available && measurement.perf.cycles > 0.0) {
    add_observation(observations, "pipeline", "effective_core_frequency",
                    measurement.perf.cycles / measurement.seconds / 1e9, "GHz", "medium",
                    "perf core cycles divided by wall time during kernel", labels);
    add_observation(observations, "pipeline", "operations_per_cycle",
                    ops / measurement.perf.cycles, "ops/cycle", "high",
                    "known operations divided by perf core cycles", labels);
    add_observation(observations, "pipeline", "ipc",
                    measurement.perf.instructions / measurement.perf.cycles,
                    "instructions/cycle", "high", "perf retired instructions / core cycles", labels);
    add_observation(observations, "pipeline", "cycles_per_operation",
                    measurement.perf.cycles / ops, "cycles/op", "high",
                    "perf core cycles divided by known operations", labels);
    if (measurement.perf.cache_counters_available && measurement.perf.cache_references > 0.0) {
      add_observation(observations, "pipeline", "cache_miss_rate",
                      100.0 * measurement.perf.cache_misses / measurement.perf.cache_references,
                      "%", "medium", "generic perf cache misses/references", labels);
    }
  } else if (!measurement.perf.error.empty()) {
    add_warning_once(warnings, "核心流水线 PMU 测量失败：" + measurement.perf.error +
                               "；IPC、每周期吞吐与周期延迟已标记为不可用");
  }
}

void benchmark_pipeline(const Options &options, std::vector<Observation> &observations,
                        std::vector<std::string> &warnings, int cpu) {
  pin_to_cpu(cpu);
  const std::size_t iterations = options.profile == "smoke" ? 2000
      : options.profile == "quick" ? 100000
      : options.profile == "deep" ? 2000000
                                  : 500000;
  auto nops = measure_kernel([&] { nop_kernel(iterations); });
  record_kernel(observations, "nop_frontend", iterations * 128, nops, warnings,
                {{"chains", "n/a"}, {"bound", "frontend"}});
  auto add1 = measure_kernel([&] { add_one_chain(iterations); });
  record_kernel(observations, "integer_add_dependency", iterations * 64, add1, warnings,
                {{"chains", "1"}, {"bound", "dependency"}});
  auto add4 = measure_kernel([&] { add_four_chains(iterations); });
  record_kernel(observations, "integer_add_parallel4", iterations * 64, add4, warnings,
                {{"chains", "4"}, {"bound", "backend"}});
  auto add8 = measure_kernel([&] { add_eight_chains(iterations); });
  record_kernel(observations, "integer_add_parallel8", iterations * 64, add8, warnings,
                {{"chains", "8"}, {"bound", "backend"}});
  auto mul1 = measure_kernel([&] { multiply_one_chain(iterations); });
  record_kernel(observations, "integer_mul_dependency", iterations * 32, mul1, warnings,
                {{"chains", "1"}, {"bound", "dependency"}});
  auto mul4 = measure_kernel([&] { multiply_four_chains(iterations); });
  record_kernel(observations, "integer_mul_parallel4", iterations * 32, mul4, warnings,
                {{"chains", "4"}, {"bound", "backend"}});
  auto fp_add1 = measure_kernel([&] { fp_add_one_chain(iterations); });
  record_kernel(observations, "fp64_add_dependency", iterations * 32, fp_add1, warnings,
                {{"chains", "1"}, {"bound", "dependency"}, {"class", "floating-point"}});
  auto fp_add4 = measure_kernel([&] { fp_add_four_chains(iterations); });
  record_kernel(observations, "fp64_add_parallel4", iterations * 32, fp_add4, warnings,
                {{"chains", "4"}, {"bound", "backend"}, {"class", "floating-point"}});
  auto fp_mul1 = measure_kernel([&] { fp_multiply_one_chain(iterations); });
  record_kernel(observations, "fp64_mul_dependency", iterations * 16, fp_mul1, warnings,
                {{"chains", "1"}, {"bound", "dependency"}, {"class", "floating-point"}});
  auto fp_mul4 = measure_kernel([&] { fp_multiply_four_chains(iterations); });
  record_kernel(observations, "fp64_mul_parallel4", iterations * 16, fp_mul4, warnings,
                {{"chains", "4"}, {"bound", "backend"}, {"class", "floating-point"}});
}

std::uint64_t compute_add_batch(std::uint64_t seed) {
  std::uint64_t a0 = seed + 1, a1 = seed + 3, a2 = seed + 5, a3 = seed + 7;
  std::uint64_t a4 = seed + 11, a5 = seed + 13, a6 = seed + 17, a7 = seed + 19;
#if defined(__x86_64__)
  asm volatile(".rept 32\n\t"
               "addq $1, %[a0]\n\taddq $1, %[a1]\n\taddq $1, %[a2]\n\taddq $1, %[a3]\n\t"
               "addq $1, %[a4]\n\taddq $1, %[a5]\n\taddq $1, %[a6]\n\taddq $1, %[a7]\n\t.endr"
               : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3),
                 [a4] "+r"(a4), [a5] "+r"(a5), [a6] "+r"(a6), [a7] "+r"(a7));
#elif defined(__aarch64__)
  asm volatile(".rept 32\n\t"
               "add %[a0], %[a0], #1\n\tadd %[a1], %[a1], #1\n\t"
               "add %[a2], %[a2], #1\n\tadd %[a3], %[a3], #1\n\t"
               "add %[a4], %[a4], #1\n\tadd %[a5], %[a5], #1\n\t"
               "add %[a6], %[a6], #1\n\tadd %[a7], %[a7], #1\n\t.endr"
               : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3),
                 [a4] "+r"(a4), [a5] "+r"(a5), [a6] "+r"(a6), [a7] "+r"(a7));
#endif
  return a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7;
}

void measure_compute_scaling_point(const Options &options, const std::vector<int> &selected,
                                   const std::string &scope,
                                   std::vector<Observation> &observations) {
  struct Result {
    std::uint64_t operations = 0;
    std::uint64_t sink = 0;
    double seconds = 0.0;
    PerfCounts perf;
  };
  std::vector<Result> results(selected.size());
  std::atomic<std::size_t> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  for (std::size_t index = 0; index < selected.size(); ++index) {
    workers.emplace_back([&, index] {
      pin_to_cpu(selected[index]);
      PerfGroup perf;
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) cpu_relax();
      const auto begin = Clock::now();
      perf.start();
      std::uint64_t sink = index + 1;
      std::uint64_t operations = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        sink ^= compute_add_batch(sink);
        operations += 256;
      }
      results[index].perf = perf.stop();
      results[index].seconds = seconds_between(begin, Clock::now());
      results[index].operations = operations;
      results[index].sink = sink;
    });
  }
  while (ready.load(std::memory_order_acquire) != selected.size()) std::this_thread::yield();
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(
      std::max(30, options.profile == "quick" ? options.duration_ms / 2 : options.duration_ms)));
  stop.store(true, std::memory_order_release);
  for (auto &worker : workers) worker.join();

  std::uint64_t total_operations = 0;
  double elapsed = 0.0;
  std::vector<double> frequencies;
  std::ostringstream cpu_list;
  for (std::size_t index = 0; index < results.size(); ++index) {
    if (index) cpu_list << ',';
    cpu_list << selected[index];
    total_operations += results[index].operations;
    elapsed = std::max(elapsed, results[index].seconds);
    global_sink = global_sink ^ results[index].sink;
    if (results[index].perf.available && results[index].seconds > 0.0) {
      frequencies.push_back(results[index].perf.cycles / results[index].seconds / 1e9);
    }
  }
  const Labels labels{{"threads", std::to_string(selected.size())},
                      {"scope", scope}, {"cpus", cpu_list.str()}};
  add_observation(observations, "compute_scaling", "integer_add_throughput",
                  static_cast<double>(total_operations) / std::max(elapsed, 1e-12) / 1e9,
                  "Gop/s", "medium", "parallel pinned independent integer-add chains", labels);
  if (!frequencies.empty()) {
    add_observation(observations, "compute_scaling", "effective_core_frequency",
                    median_double(std::move(frequencies)), "GHz", "medium",
                    "median perf core cycles per wall second across active workers", labels);
  }
}

void benchmark_compute_scaling(const Options &options, const std::vector<CpuInfo> &cpus,
                               std::vector<Observation> &observations) {
  if (options.profile == "smoke") return;
  const auto physical = numa_balanced_physical_cpus(cpus);
  const std::size_t maximum = options.profile == "quick"
      ? std::min<std::size_t>(physical.size(), 4) : physical.size();
  for (const std::size_t count : thread_counts(maximum)) {
    std::vector<int> selected;
    for (std::size_t index = 0; index < count; ++index) selected.push_back(physical[index].cpu);
    measure_compute_scaling_point(options, selected, "physical-cores", observations);
  }
  for (std::size_t left = 0; left < cpus.size(); ++left) {
    bool found = false;
    for (std::size_t right = left + 1; right < cpus.size(); ++right) {
      if (cpu_relation(cpus[left], cpus[right]) == "smt-sibling") {
        measure_compute_scaling_point(options, {cpus[left].cpu, cpus[right].cpu},
                                      "smt-siblings", observations);
        found = true;
        break;
      }
    }
    if (found) break;
  }
}

using GeneratedFunction = void (*)();

void benchmark_instruction_fetch(const Options &options, int cpu,
                                 std::vector<Observation> &observations,
                                 std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  const std::size_t maximum = options.profile == "quick" ? 256U * 1024U
      : options.profile == "deep" ? 4U * 1024U * 1024U
                                  : 1U * 1024U * 1024U;
  const std::size_t target_bytes = options.profile == "quick" ? 32U * 1024U * 1024U
      : options.profile == "deep" ? 256U * 1024U * 1024U
                                  : 128U * 1024U * 1024U;
#if defined(__x86_64__)
  const std::array<std::pair<std::size_t, std::array<std::uint8_t, 8>>, 3> encodings{{
      {1, {0x90, 0, 0, 0, 0, 0, 0, 0}},
      {4, {0x0f, 0x1f, 0x40, 0x00, 0, 0, 0, 0}},
      {8, {0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}},
  }};
#endif
  try {
    for (const std::size_t footprint : power_sizes(4U * 1024U, maximum)) {
#if defined(__x86_64__)
      for (const auto &[instruction_bytes, encoding] : encodings) {
        const std::size_t body_bytes = footprint - (footprint % instruction_bytes);
        ExecutableBuffer code(body_bytes + 1);
        for (std::size_t offset = 0; offset < body_bytes; offset += instruction_bytes) {
          std::memcpy(code.bytes() + offset, encoding.data(), instruction_bytes);
        }
        code.bytes()[body_bytes] = std::byte{0xc3};
        code.seal();
        const auto function = code.function_at<GeneratedFunction>();
        const std::size_t passes = std::max<std::size_t>(1, target_bytes / body_bytes);
        const auto measurement = measure_kernel([&] {
          for (std::size_t pass = 0; pass < passes; ++pass) function();
        });
        const double instructions = static_cast<double>(body_bytes / instruction_bytes) * passes;
        const Labels labels{{"cpu", std::to_string(cpu)},
                            {"working_set_bytes", std::to_string(body_bytes)},
                            {"instruction_bytes", std::to_string(instruction_bytes)},
                            {"passes", std::to_string(passes)}};
        add_observation(observations, "instruction_fetch", "code_delivery_bandwidth",
                        static_cast<double>(body_bytes) * passes / measurement.seconds / 1e9,
                        "GB/s", "medium", "W^X generated NOP body scanned repeatedly", labels);
        add_observation(observations, "instruction_fetch", "instruction_rate",
                        instructions / measurement.seconds / 1e9, "Ginst/s", "medium",
                        "known generated NOP count divided by wall time", labels);
        if (measurement.perf.available && measurement.perf.cycles > 0.0) {
          add_observation(observations, "instruction_fetch", "ipc",
                          measurement.perf.instructions / measurement.perf.cycles,
                          "instructions/cycle", "high",
                          "perf retired instructions / core cycles for generated code", labels);
        }
      }
#elif defined(__aarch64__)
      const std::size_t body_bytes = footprint - (footprint % 4);
      ExecutableBuffer code(body_bytes + 4);
      constexpr std::uint32_t nop = 0xd503201fU;
      constexpr std::uint32_t ret = 0xd65f03c0U;
      for (std::size_t offset = 0; offset < body_bytes; offset += 4)
        std::memcpy(code.bytes() + offset, &nop, sizeof(nop));
      std::memcpy(code.bytes() + body_bytes, &ret, sizeof(ret));
      code.seal();
      const auto function = code.function_at<GeneratedFunction>();
      const std::size_t passes = std::max<std::size_t>(1, target_bytes / body_bytes);
      const auto measurement = measure_kernel([&] {
        for (std::size_t pass = 0; pass < passes; ++pass) function();
      });
      const double instructions = static_cast<double>(body_bytes / 4) * passes;
      const Labels labels{{"cpu", std::to_string(cpu)},
                          {"working_set_bytes", std::to_string(body_bytes)},
                          {"instruction_bytes", "4"}, {"passes", std::to_string(passes)}};
      add_observation(observations, "instruction_fetch", "code_delivery_bandwidth",
                      static_cast<double>(body_bytes) * passes / measurement.seconds / 1e9,
                      "GB/s", "medium", "W^X generated A64 NOP body scanned repeatedly", labels);
      add_observation(observations, "instruction_fetch", "instruction_rate",
                      instructions / measurement.seconds / 1e9, "Ginst/s", "medium",
                      "known generated A64 NOP count divided by wall time", labels);
      if (measurement.perf.available && measurement.perf.cycles > 0.0) {
        add_observation(observations, "instruction_fetch", "ipc",
                        measurement.perf.instructions / measurement.perf.cycles,
                        "instructions/cycle", "high",
                        "perf retired instructions / core cycles for generated code", labels);
      }
#endif
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("指令侧带宽探针提前停止：") + error.what());
  }
}

void benchmark_btb_capacity(const Options &options, int cpu,
                            std::vector<Observation> &observations,
                            std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  const std::size_t maximum = options.profile == "quick" ? 2048
      : options.profile == "deep" ? 32768 : 8192;
  const std::size_t target_branches = options.profile == "quick" ? 500000 :
      options.profile == "deep" ? 4000000 : 2000000;
  try {
    for (const std::string branch_type : {"unconditional", "conditional-taken"}) {
      for (const std::size_t spacing : {std::size_t{4}, std::size_t{8}, std::size_t{16},
                                        std::size_t{32}, std::size_t{64}}) {
        for (const std::size_t count : power_sizes(16, maximum)) {
#if defined(__x86_64__)
        const std::size_t prefix = branch_type == "conditional-taken" ? 2 : 0;
        ExecutableBuffer code(prefix + count * spacing + 4);
        std::memset(code.bytes(), 0x90, code.size());
        if (prefix != 0) {
          code.bytes()[0] = std::byte{0x31};
          code.bytes()[1] = std::byte{0xc0};
        }
        for (std::size_t index = 0; index < count; ++index) {
          const std::size_t offset = prefix + index * spacing;
          code.bytes()[offset] = branch_type == "conditional-taken"
              ? std::byte{0x74} : std::byte{0xeb};
          code.bytes()[offset + 1] = static_cast<std::byte>(spacing - 2);
        }
        code.bytes()[prefix + count * spacing] = std::byte{0xc3};
#elif defined(__aarch64__)
        const std::size_t prefix = branch_type == "conditional-taken" ? 4 : 0;
        ExecutableBuffer code(prefix + count * spacing + 4);
        constexpr std::uint32_t nop = 0xd503201fU;
        constexpr std::uint32_t ret = 0xd65f03c0U;
        for (std::size_t offset = 0; offset < code.size() - 4; offset += 4)
          std::memcpy(code.bytes() + offset, &nop, sizeof(nop));
        if (prefix != 0) {
          constexpr std::uint32_t compare_zero = 0xeb1f03ffU;
          std::memcpy(code.bytes(), &compare_zero, sizeof(compare_zero));
        }
        const std::uint32_t distance = static_cast<std::uint32_t>(spacing / 4);
        const std::uint32_t branch = branch_type == "conditional-taken"
            ? 0x54000000U | (distance << 5) : 0x14000000U | distance;
        for (std::size_t index = 0; index < count; ++index)
          std::memcpy(code.bytes() + prefix + index * spacing, &branch, sizeof(branch));
        std::memcpy(code.bytes() + prefix + count * spacing, &ret, sizeof(ret));
#endif
        code.seal();
        const auto function = code.function_at<GeneratedFunction>();
        const std::size_t passes = std::max<std::size_t>(1, target_branches / count);
        const auto measurement = measure_kernel([&] {
          for (std::size_t pass = 0; pass < passes; ++pass) function();
        });
        const double branches = static_cast<double>(count) * passes;
        const Labels labels{{"cpu", std::to_string(cpu)},
                            {"branch_type", branch_type},
                            {"branch_count", std::to_string(count)},
                            {"spacing_bytes", std::to_string(spacing)},
                            {"code_bytes", std::to_string(count * spacing)}};
        add_observation(observations, "branch_structure", "btb_branch_latency",
                        measurement.seconds * 1e9 / branches, "ns/branch", "low",
                        "generated taken direct branches with independently varied footprint", labels);
        add_observation(observations, "branch_structure", "btb_counter_ticks",
                        static_cast<double>(measurement.counter_ticks) / branches,
                        "counter-ticks/branch", "low",
                        "platform counter ticks per generated taken branch", labels);
        if (measurement.perf.branch_counters_available && measurement.perf.branches > 0.0) {
          add_observation(observations, "branch_structure", "btb_miss_rate",
                          100.0 * measurement.perf.branch_misses / measurement.perf.branches,
                          "%", "medium", "generic perf branch misses during BTB footprint sweep",
                          labels);
        }
      }
    }
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("BTB 容量探针提前停止：") + error.what());
  }
}

void benchmark_return_stack(const Options &options, int cpu,
                            std::vector<Observation> &observations,
                            std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  const std::size_t maximum = options.profile == "quick" ? 64 : 256;
  const std::size_t target_returns = options.profile == "quick" ? 250000 : 1000000;
  try {
    for (const std::size_t depth : power_sizes(1, maximum)) {
#if defined(__x86_64__)
      constexpr std::size_t slot = 6;
      ExecutableBuffer code(depth * slot + 1);
      for (std::size_t index = 0; index < depth; ++index) {
        const std::size_t offset = index * slot;
        code.bytes()[offset] = std::byte{0xe8};
        const std::int32_t displacement = 1;
        std::memcpy(code.bytes() + offset + 1, &displacement, sizeof(displacement));
        code.bytes()[offset + 5] = std::byte{0xc3};
      }
      code.bytes()[depth * slot] = std::byte{0xc3};
#elif defined(__aarch64__)
      constexpr std::size_t slot = 16;
      ExecutableBuffer code(depth * slot + 4);
      constexpr std::uint32_t save_frame_and_lr = 0xa9bf7bfdU;
      constexpr std::uint32_t call_next = 0x94000003U;
      constexpr std::uint32_t restore_frame_and_lr = 0xa8c17bfdU;
      constexpr std::uint32_t ret = 0xd65f03c0U;
      for (std::size_t index = 0; index < depth; ++index) {
        const std::size_t offset = index * slot;
        std::memcpy(code.bytes() + offset, &save_frame_and_lr, sizeof(save_frame_and_lr));
        std::memcpy(code.bytes() + offset + 4, &call_next, sizeof(call_next));
        std::memcpy(code.bytes() + offset + 8, &restore_frame_and_lr,
                    sizeof(restore_frame_and_lr));
        std::memcpy(code.bytes() + offset + 12, &ret, sizeof(ret));
      }
      std::memcpy(code.bytes() + depth * slot, &ret, sizeof(ret));
#endif
      code.seal();
      const auto function = code.function_at<GeneratedFunction>();
      const std::size_t returns_per_call = depth + 1;
      const std::size_t passes = std::max<std::size_t>(1, target_returns / returns_per_call);
      const auto measurement = measure_kernel([&] {
        for (std::size_t pass = 0; pass < passes; ++pass) function();
      });
      const double returns = static_cast<double>(returns_per_call) * passes;
      const Labels labels{{"cpu", std::to_string(cpu)},
                          {"depth", std::to_string(depth)},
                          {"unique_return_addresses", std::to_string(depth)}};
      add_observation(observations, "branch_structure", "return_stack_latency",
                      measurement.seconds * 1e9 / returns, "ns/return", "low",
                      "generated nested calls with a unique return address at each depth", labels);
      if (measurement.perf.branch_counters_available && measurement.perf.branches > 0.0) {
        add_observation(observations, "branch_structure", "return_stack_miss_rate",
                        100.0 * measurement.perf.branch_misses / measurement.perf.branches,
                        "%", "medium", "generic perf branch misses during nested call/return chain",
                        labels);
      }
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("返回地址栈探针提前停止：") + error.what());
  }
}

void benchmark_indirect_targets(const Options &options, int cpu,
                                std::vector<Observation> &observations,
                                std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  const std::size_t maximum = options.profile == "quick" ? 512
      : options.profile == "deep" ? 16384 : 4096;
  const std::size_t target_calls = options.profile == "quick" ? 250000 :
      options.profile == "deep" ? 2000000 : 1000000;
  constexpr std::size_t slot = 16;
  try {
    ExecutableBuffer code(maximum * slot);
#if defined(__x86_64__)
    std::memset(code.bytes(), 0x90, code.size());
    for (std::size_t index = 0; index < maximum; ++index)
      code.bytes()[index * slot] = std::byte{0xc3};
#elif defined(__aarch64__)
    constexpr std::uint32_t nop = 0xd503201fU;
    constexpr std::uint32_t ret = 0xd65f03c0U;
    for (std::size_t offset = 0; offset < code.size(); offset += 4)
      std::memcpy(code.bytes() + offset, &nop, sizeof(nop));
    for (std::size_t index = 0; index < maximum; ++index)
      std::memcpy(code.bytes() + index * slot, &ret, sizeof(ret));
#endif
    code.seal();
    for (const std::size_t count : power_sizes(1, maximum)) {
      std::vector<GeneratedFunction> targets;
      targets.reserve(count);
      for (std::size_t index = 0; index < count; ++index)
        targets.push_back(code.function_at<GeneratedFunction>(index * slot));
      std::vector<std::size_t> order(count);
      std::iota(order.begin(), order.end(), 0);
      std::mt19937 random(options.seed ^ static_cast<unsigned>(count));
      std::shuffle(order.begin(), order.end(), random);
      const std::size_t rounds = std::max<std::size_t>(1, target_calls / count);
      const auto measurement = measure_kernel([&] {
        for (std::size_t round = 0; round < rounds; ++round)
          for (const std::size_t index : order) targets[index]();
      });
      const double calls = static_cast<double>(count) * rounds;
      const Labels labels{{"cpu", std::to_string(cpu)},
                          {"target_count", std::to_string(count)},
                          {"target_spacing_bytes", std::to_string(slot)}};
      add_observation(observations, "branch_structure", "indirect_call_latency",
                      measurement.seconds * 1e9 / calls, "ns/call", "low",
                      "one indirect call site visits a shuffled set of generated return targets",
                      labels);
      if (measurement.perf.branch_counters_available && measurement.perf.branches > 0.0) {
        add_observation(observations, "branch_structure", "indirect_call_miss_rate",
                        100.0 * measurement.perf.branch_misses / measurement.perf.branches,
                        "%", "medium", "generic perf branch misses during indirect target sweep",
                        labels);
      }
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("间接分支目标容量探针提前停止：") + error.what());
  }
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, optimize("no-if-conversion,no-tree-vectorize")))
#else
__attribute__((noinline))
#endif
std::uint64_t branch_loop(const volatile std::uint8_t *pattern, std::size_t count,
                          std::size_t passes) {
  std::uint64_t value = 1;
  for (std::size_t pass = 0; pass < passes; ++pass) {
    for (std::size_t index = 0; index < count; ++index) {
      if (pattern[index]) {
        asm volatile("" ::: "memory");
        value = value * 3 + 1;
      } else {
        asm volatile("" ::: "memory");
        value = value * 3 - 1;
      }
    }
  }
  return value;
}

void benchmark_branches(const Options &options, std::vector<Observation> &observations,
                        int cpu) {
  pin_to_cpu(cpu);
  constexpr std::size_t count = 65536;
  const std::size_t passes = options.profile == "smoke" ? 2
      : options.profile == "quick" ? 32
                                  : 128;
  std::mt19937 random(options.seed ^ 0xB12A4C4U);
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> patterns;
  patterns.emplace_back("always-taken", std::vector<std::uint8_t>(count, 1));
  patterns.emplace_back("alternating", std::vector<std::uint8_t>(count, 0));
  patterns.emplace_back("random", std::vector<std::uint8_t>(count, 0));
  for (std::size_t index = 0; index < count; ++index) {
    patterns[1].second[index] = static_cast<std::uint8_t>(index & 1U);
    patterns[2].second[index] = static_cast<std::uint8_t>(random() & 1U);
  }
  for (auto &[name, pattern] : patterns) {
    const auto measurement = measure_kernel([&] {
      global_sink = global_sink ^ branch_loop(pattern.data(), pattern.size(), passes);
    });
    const double branches = static_cast<double>(count * passes);
    const Labels labels{{"pattern", name}};
    add_observation(observations, "branch", "time_per_branch",
                    measurement.seconds * 1e9 / branches, "ns/branch", "medium",
                    "forced scalar data-dependent branch loop", labels);
    if (measurement.perf.available && measurement.perf.branch_counters_available &&
        measurement.perf.branches > 0.0) {
      add_observation(observations, "branch", "miss_rate",
                      100.0 * measurement.perf.branch_misses / measurement.perf.branches,
                      "%", "high", "perf branch misses / branch instructions", labels);
      add_observation(observations, "branch", "ipc",
                      measurement.perf.instructions / measurement.perf.cycles,
                      "instructions/cycle", "high", "perf retired instructions / core cycles", labels);
    }
  }
}

void benchmark_branch_history(const Options &options, int cpu,
                              std::vector<Observation> &observations) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  constexpr std::size_t count = 32768;
  const std::size_t maximum = options.profile == "quick" ? 256
      : options.profile == "deep" ? 16384 : 4096;
  const std::size_t target_branches = options.profile == "quick" ? 500000 : 2000000;
  for (const std::size_t period : power_sizes(1, maximum)) {
    std::vector<std::uint8_t> seed_pattern(period, 1);
    if (period > 1) {
      std::mt19937 random(options.seed ^ static_cast<unsigned>(period * 0x9e3779b1U));
      for (auto &value : seed_pattern) value = static_cast<std::uint8_t>(random() & 1U);
      seed_pattern.front() = 1;
      seed_pattern[period / 2] = 0;
    }
    std::vector<std::uint8_t> pattern(count);
    for (std::size_t index = 0; index < count; ++index)
      pattern[index] = seed_pattern[index % period];
    const std::size_t passes = std::max<std::size_t>(1, target_branches / count);
    const auto measurement = measure_kernel([&] {
      global_sink = global_sink ^ branch_loop(pattern.data(), pattern.size(), passes);
    });
    const double branches = static_cast<double>(count) * passes;
    const Labels labels{{"cpu", std::to_string(cpu)}, {"history_period", std::to_string(period)}};
    add_observation(observations, "branch_structure", "history_period_latency",
                    measurement.seconds * 1e9 / branches, "ns/branch", "low",
                    "scalar conditional branch over a repeating pseudo-random direction period",
                    labels);
    if (measurement.perf.branch_counters_available && measurement.perf.branches > 0.0) {
      add_observation(observations, "branch_structure", "history_period_miss_rate",
                      100.0 * measurement.perf.branch_misses / measurement.perf.branches,
                      "%", "medium", "generic perf branch misses during history-period sweep",
                      labels);
    }
  }
}

void benchmark_branch_structures(const Options &options, int cpu,
                                 std::vector<Observation> &observations,
                                 std::vector<std::string> &warnings) {
  benchmark_btb_capacity(options, cpu, observations, warnings);
  benchmark_branch_history(options, cpu, observations);
  benchmark_return_stack(options, cpu, observations, warnings);
  benchmark_indirect_targets(options, cpu, observations, warnings);
}

#if defined(__x86_64__)
using RobTrialFunction = std::uint64_t (*)(const std::uint64_t *, const std::uint64_t *);

std::unique_ptr<ExecutableBuffer> build_rob_trial_function(std::size_t filler_instructions) {
  auto code = std::make_unique<ExecutableBuffer>(128 + filler_instructions * 4);
  std::size_t offset = 0;
  auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
    for (const auto byte : bytes) code->bytes()[offset++] = static_cast<std::byte>(byte);
  };
  // Six caller-saved independent chains. Every generated add is exactly one x86 instruction
  // and one simple integer uop on the supported x86/C86 targets.
  emit({0x45, 0x31, 0xc0});  // xor r8d,r8d
  emit({0x45, 0x31, 0xc9});  // xor r9d,r9d
  emit({0x45, 0x31, 0xd2});  // xor r10d,r10d
  emit({0x45, 0x31, 0xdb});  // xor r11d,r11d
  emit({0x31, 0xc9});        // xor ecx,ecx
  emit({0x31, 0xd2});        // xor edx,edx
  emit({0x48, 0x8b, 0x07});  // mov rax,[rdi] -- first cold load
  const std::array<std::array<std::uint8_t, 4>, 6> adds{{
      {{0x49, 0x83, 0xc0, 0x01}}, {{0x49, 0x83, 0xc1, 0x01}},
      {{0x49, 0x83, 0xc2, 0x01}}, {{0x49, 0x83, 0xc3, 0x01}},
      {{0x48, 0x83, 0xc1, 0x01}}, {{0x48, 0x83, 0xc2, 0x01}},
  }};
  for (std::size_t index = 0; index < filler_instructions; ++index) {
    for (const auto byte : adds[index % adds.size()])
      code->bytes()[offset++] = static_cast<std::byte>(byte);
  }
  emit({0x48, 0x8b, 0x3e});  // mov rdi,[rsi] -- second independent cold load
  emit({0x48, 0x31, 0xf8});  // xor rax,rdi
  emit({0x4c, 0x31, 0xc0});  // xor rax,r8
  emit({0x4c, 0x31, 0xc8});  // xor rax,r9
  emit({0x4c, 0x31, 0xd0});  // xor rax,r10
  emit({0x4c, 0x31, 0xd8});  // xor rax,r11
  emit({0x48, 0x31, 0xc8});  // xor rax,rcx
  emit({0x48, 0x31, 0xd0});  // xor rax,rdx
  emit({0xc3});              // ret
  code->seal();
  return code;
}

std::uint64_t rob_trial(RobTrialFunction function, const std::uint64_t *first,
                        const std::uint64_t *second, bool flush) {
  if (flush) {
    _mm_clflush(first);
    _mm_clflush(second);
    _mm_mfence();
  }
  const auto begin = cycle_counter();
  const auto value = function(first, second);
  const auto end = cycle_counter();
  global_sink = global_sink ^ value;
  return end - begin;
}

double median(std::vector<std::uint64_t> values) {
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return static_cast<double>(*middle);
}
#endif

void benchmark_rob(const Options &options, std::vector<Observation> &observations,
                   std::vector<std::string> &warnings, int cpu) {
#if defined(__x86_64__)
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  MappedBuffer buffer(16U * 1024U * 1024U);
  auto *first = reinterpret_cast<std::uint64_t *>(buffer.bytes());
  auto *second = reinterpret_cast<std::uint64_t *>(buffer.bytes() + 8U * 1024U * 1024U);
  *first = 1;
  *second = 2;
  const std::size_t maximum = options.profile == "quick" ? 640 : 1024;
  const std::size_t step = options.profile == "quick" ? 16 : 8;
  const int trials = options.profile == "quick" ? 15 : 31;
  std::vector<std::pair<std::size_t, double>> penalties;
  for (std::size_t filler = 0; filler <= maximum; filler += step) {
    auto code = build_rob_trial_function(filler);
    const auto function = code->function_at<RobTrialFunction>();
    std::vector<std::uint64_t> cold;
    std::vector<std::uint64_t> hot;
    for (int trial = 0; trial < trials; ++trial) {
      hot.push_back(rob_trial(function, first, second, false));
      cold.push_back(rob_trial(function, first, second, true));
    }
    const double penalty = std::max(0.0, median(cold) - median(hot));
    penalties.emplace_back(filler, penalty);
    add_observation(observations, "reorder_window", "cold_load_overlap_penalty", penalty,
                    "counter-ticks", "low",
                    "two flushed loads separated by an exact static one-uop instruction window",
                    {{"cpu", std::to_string(cpu)},
                     {"filler_instructions", std::to_string(filler)},
                     {"filler_uops", std::to_string(filler)},
                     {"fixed_instructions_before_second_load", "7"}});
  }
  double baseline = 0.0;
  const std::size_t baseline_points = std::min<std::size_t>(4, penalties.size());
  for (std::size_t index = 0; index < baseline_points; ++index) baseline += penalties[index].second;
  baseline /= static_cast<double>(baseline_points);
  std::optional<std::size_t> knee;
  for (std::size_t index = baseline_points; index < penalties.size(); ++index) {
    constexpr std::size_t sustain = 3;
    if (index + sustain <= penalties.size() &&
        penalties[index].second > baseline * 1.35 &&
        penalties[index].second > baseline + 20.0 &&
        std::all_of(penalties.begin() + static_cast<std::ptrdiff_t>(index),
                    penalties.begin() + static_cast<std::ptrdiff_t>(index + sustain),
                    [&](const auto &point) {
                      return point.second > baseline * 1.35 && point.second > baseline + 20.0;
                    })) {
      knee = penalties[index].first;
      break;
    }
  }
  if (knee.has_value()) {
    constexpr std::size_t fixed_before_second_load = 7;
    add_observation(observations, "reorder_window", "rob_capacity_proxy",
                    static_cast<double>(*knee + fixed_before_second_load), "static-instructions", "low",
                    "first sustained loss of overlap in an exact static one-uop filler sequence",
                    {{"exact_filler_instructions", std::to_string(*knee)},
                     {"fixed_instructions_before_second_load",
                      std::to_string(fixed_before_second_load)},
                     {"lower_bound", std::to_string(
                         (*knee > step ? *knee - step : 0) + fixed_before_second_load)},
                     {"upper_bound", std::to_string(*knee + step + fixed_before_second_load)}});
  } else {
    warnings.push_back("ROB/乱序窗口拐点不明确；请检查原始重叠曲线");
  }
#else
  (void)options;
  (void)observations;
  (void)cpu;
  warnings.push_back("ROB/乱序窗口探针目前仅支持 x86/C86；ARM64 将该指标标记为不可用");
#endif
}

void benchmark_os_overheads(const Options &options, const std::vector<CpuInfo> &cpus,
                            std::vector<Observation> &observations,
                            std::vector<std::string> &warnings) {
  const int primary = cpus.front().cpu;
  pin_to_cpu(primary);
  const std::size_t syscall_iterations = options.profile == "smoke" ? 10000 : 300000;
  const auto syscall_begin = Clock::now();
  long pid_sum = 0;
  for (std::size_t index = 0; index < syscall_iterations; ++index)
    pid_sum += syscall(SYS_getpid);
  const double syscall_elapsed = seconds_between(syscall_begin, Clock::now());
  global_sink = global_sink ^ static_cast<std::uint64_t>(pid_sum);
  add_observation(observations, "os_overhead", "getpid_syscall",
                  syscall_elapsed * 1e9 / static_cast<double>(syscall_iterations),
                  "ns/call", "high", "direct SYS_getpid loop", {{"cpu", std::to_string(primary)}});

  const std::size_t fault_bytes = (options.profile == "smoke" ? 4U : 64U) * 1024U * 1024U;
  try {
    MappedBuffer buffer(fault_bytes);
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    rusage usage_before{};
    rusage usage_after{};
    getrusage(RUSAGE_SELF, &usage_before);
    const auto fault_begin = Clock::now();
    for (std::size_t offset = 0; offset < fault_bytes; offset += page)
      buffer.bytes()[offset] = std::byte{1};
    const double fault_elapsed = seconds_between(fault_begin, Clock::now());
    getrusage(RUSAGE_SELF, &usage_after);
    const double pages = static_cast<double>((fault_bytes + page - 1) / page);
    add_observation(observations, "os_overhead", "anonymous_page_first_touch",
                    fault_elapsed * 1e9 / pages, "ns/page", "high",
                    "write one byte per anonymous base page",
                    {{"bytes", std::to_string(fault_bytes)}, {"page_bytes", std::to_string(page)}});
    add_observation(observations, "os_overhead", "minor_faults",
                    static_cast<double>(usage_after.ru_minflt - usage_before.ru_minflt),
                    "faults", "high", "getrusage delta around anonymous first touch",
                    {{"bytes", std::to_string(fault_bytes)}});
  } catch (const std::exception &error) {
    warnings.push_back(std::string("缺页探针提前停止：") + error.what());
  }

  if (cpus.size() < 2) return;
  int to_second[2] = {-1, -1};
  int to_first[2] = {-1, -1};
  if (pipe2(to_second, O_CLOEXEC) != 0 || pipe2(to_first, O_CLOEXEC) != 0) {
    warnings.push_back(std::string("调度交接探针创建管道失败：") + std::strerror(errno));
    for (const int fd : to_second) if (fd >= 0) close(fd);
    for (const int fd : to_first) if (fd >= 0) close(fd);
    return;
  }
  const std::size_t rounds = options.profile == "smoke" ? 200
      : options.profile == "quick" ? 3000
                                  : 10000;
  std::atomic<bool> ready{false};
  std::thread second([&] {
    pin_to_cpu(cpus[1].cpu);
    char token = 1;
    ready.store(true, std::memory_order_release);
    for (std::size_t round = 0; round < rounds; ++round) {
      if (read(to_second[0], &token, 1) != 1) return;
      if (write(to_first[1], &token, 1) != 1) return;
    }
  });
  while (!ready.load(std::memory_order_acquire)) cpu_relax();
  pin_to_cpu(primary);
  char token = 1;
  const auto switch_begin = Clock::now();
  bool io_ok = true;
  for (std::size_t round = 0; round < rounds; ++round) {
    if (write(to_second[1], &token, 1) != 1 || read(to_first[0], &token, 1) != 1) {
      io_ok = false;
      break;
    }
  }
  const double switch_elapsed = seconds_between(switch_begin, Clock::now());
  second.join();
  for (const int fd : to_second) close(fd);
  for (const int fd : to_first) close(fd);
  if (io_ok) {
    add_observation(observations, "os_overhead", "scheduler_pipe_handoff",
                    switch_elapsed * 1e9 / static_cast<double>(rounds) / 2.0,
                    "ns/one-way", "medium", "blocking pipe ping-pong between pinned threads",
                    {{"cpu_a", std::to_string(primary)},
                     {"cpu_b", std::to_string(cpus[1].cpu)},
                     {"relation", cpu_relation(cpus[0], cpus[1])}});
  }
}

Options parse_options(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto require_value = [&]() -> std::string {
      if (index + 1 >= argc) throw std::runtime_error("missing value after " + argument);
      return argv[++index];
    };
    if (argument == "--profile") {
      options.profile = require_value();
    } else if (argument == "--memory-mib") {
      options.memory_mib = static_cast<std::size_t>(std::stoull(require_value()));
      options.memory_explicit = true;
    } else if (argument == "--duration-ms") {
      options.duration_ms = std::stoi(require_value());
    } else if (argument == "--seed") {
      options.seed = static_cast<unsigned>(std::stoul(require_value()));
    } else if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else {
      throw std::runtime_error("unknown argument: " + argument);
    }
  }
  const std::set<std::string> valid_profiles{"smoke", "quick", "standard", "deep"};
  if (!valid_profiles.contains(options.profile)) {
    throw std::runtime_error("profile must be one of: smoke, quick, standard, deep");
  }
  if (options.memory_mib == 0) {
    options.memory_mib = options.profile == "smoke" ? 8
        : options.profile == "quick" ? 64
        : options.profile == "deep" ? 1024
                                    : 256;
  }
  if (options.duration_ms <= 0) {
    options.duration_ms = options.profile == "smoke" ? 15
        : options.profile == "quick" ? 80
        : options.profile == "deep" ? 500
                                    : 200;
  }
  return options;
}

void print_help() {
  std::cout
      << "sdc-native - low-level probes for System Decap\n\n"
      << "Usage: sdc-native [--profile smoke|quick|standard|deep]\n"
      << "                  [--memory-mib N] [--duration-ms N] [--seed N]\n\n"
      << "Writes one JSON document to stdout and progress to stderr.\n"
      << "Architectures: x86_64 (including Hygon C86), ARM64/AArch64.\n";
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parse_options(argc, argv);
    if (options.help) {
      print_help();
      return 0;
    }
    if (architecture_name() == "unsupported") {
      std::cerr << "Unsupported build architecture. Expected x86_64 or ARM64.\n";
      return 2;
    }
    const auto run_begin = Clock::now();
    std::vector<Observation> observations;
    std::vector<std::string> warnings;
    const auto cpus = discover_cpus();
    const int primary_cpu = cpus.front().cpu;
    pin_to_cpu(primary_cpu);
    PerfEnvironment perf_environment;
    perf_environment.perf_event_paranoid = read_file("/proc/sys/kernel/perf_event_paranoid");
    perf_environment.nmi_watchdog = read_file("/proc/sys/kernel/nmi_watchdog");
    perf_environment.core = probe_perf_group(PerfGroupKind::Core);
    perf_environment.branch = probe_perf_group(PerfGroupKind::Branch);
    perf_environment.cache = probe_perf_group(PerfGroupKind::Cache);
    if (!perf_environment.core.available) {
      warnings.push_back("核心硬件性能计数器不可用：" + perf_environment.core.error +
                         "；已省略 IPC 与精确的每核心周期估计");
    }
    if (!perf_environment.branch.available)
      warnings.push_back("分支硬件性能计数器不可用：" + perf_environment.branch.error +
                         "；仍保留墙钟时间分支测试");
    if (!perf_environment.cache.available)
      warnings.push_back("通用缓存硬件性能计数器不可用：" + perf_environment.cache.error +
                         "；仍保留软件内存层级测试");

    auto section = [](std::string_view name) { std::cerr << "[system-decap] " << name << "\n"; };
    section("timer calibration");
    benchmark_timers(observations, primary_cpu);
    section("cache hierarchy latency sweep");
    benchmark_cache_latency(options, observations, warnings, primary_cpu);
    section("TLB/page-walk sweep");
    benchmark_tlb(options, observations, warnings, primary_cpu);
    section("memory bandwidth scaling");
    benchmark_bandwidth(options, cpus, observations, warnings);
    section("cache bandwidth by working set");
    benchmark_cache_bandwidth(options, cpus, observations, warnings);
    section("stride and prefetch sensitivity");
    benchmark_stride_prefetch(options, primary_cpu, observations, warnings);
    section("memory-level parallelism");
    benchmark_memory_parallelism(options, primary_cpu, observations, warnings);
    section("base-page versus transparent-hugepage policy");
    benchmark_page_policy(options, primary_cpu, observations, warnings);
    section("loaded memory latency");
    benchmark_loaded_memory_latency(options, cpus, observations, warnings);
    section("store-to-load forwarding");
    benchmark_store_forwarding(options, primary_cpu, observations);
    section("instruction-side bandwidth and footprint");
    benchmark_instruction_fetch(options, primary_cpu, observations, warnings);
    section("core pipeline and IPC");
    benchmark_pipeline(options, observations, warnings, primary_cpu);
    section("multi-core and SMT compute scaling");
    benchmark_compute_scaling(options, cpus, observations);
    section("branch predictor");
    benchmark_branches(options, observations, primary_cpu);
    section("BTB, history, return-stack and indirect-target sweeps");
    benchmark_branch_structures(options, primary_cpu, observations, warnings);
    section("core-to-core cache-line latency");
    benchmark_core_latency(options, cpus, observations, warnings);
    section("false sharing / coherence line");
    benchmark_false_sharing(options, cpus, observations);
    section("NUMA latency and bandwidth matrix");
    benchmark_numa(options, cpus, observations, warnings);
    section("reorder-window / ROB proxy");
    benchmark_rob(options, observations, warnings, primary_cpu);
    section("OS and scheduler overheads");
    benchmark_os_overheads(options, cpus, observations, warnings);

    emit_json(options, observations, warnings, perf_environment,
              seconds_between(run_begin, Clock::now()));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "sdc-native: " << error.what() << '\n';
    return 1;
  }
}
