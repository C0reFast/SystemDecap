#include <algorithm>
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

class PerfGroup {
 public:
  PerfGroup() { open(); }
  PerfGroup(const PerfGroup &) = delete;
  PerfGroup &operator=(const PerfGroup &) = delete;
  ~PerfGroup() {
    for (const int fd : fds_) {
      if (fd >= 0) {
        close(fd);
      }
    }
  }

  bool available() const { return leader_ >= 0 && event_names_.size() >= 2; }
  const std::string &error() const { return error_; }

  void start() const {
    if (!available()) {
      return;
    }
    ioctl(leader_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(leader_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
  }

  PerfCounts stop() const {
    PerfCounts counts;
    if (!available()) {
      counts.error = error_;
      return counts;
    }
    ioctl(leader_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    std::vector<std::uint64_t> buffer(3 + event_names_.size());
    const auto bytes = read(leader_, buffer.data(), buffer.size() * sizeof(std::uint64_t));
    if (bytes < static_cast<ssize_t>((3 + event_names_.size()) * sizeof(std::uint64_t))) {
      counts.error = "short read from perf_event group";
      return counts;
    }
    const auto nr = buffer[0];
    const double enabled = static_cast<double>(buffer[1]);
    const double running = static_cast<double>(buffer[2]);
    if (nr != event_names_.size() || running <= 0.0) {
      counts.error = "invalid perf_event group result";
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

  void open() {
    struct EventSpec {
      const char *name;
      std::uint64_t config;
      bool required;
    };
    const std::vector<EventSpec> specs = {
        {"cycles", PERF_COUNT_HW_CPU_CYCLES, true},
        {"instructions", PERF_COUNT_HW_INSTRUCTIONS, true},
        {"branches", PERF_COUNT_HW_BRANCH_INSTRUCTIONS, false},
        {"branch_misses", PERF_COUNT_HW_BRANCH_MISSES, false},
        {"cache_references", PERF_COUNT_HW_CACHE_REFERENCES, false},
        {"cache_misses", PERF_COUNT_HW_CACHE_MISSES, false},
    };
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
        if (spec.required) {
          error_ = std::string("perf_event_open ") + spec.name + ": " + std::strerror(errno);
          for (const int old_fd : fds_) close(old_fd);
          fds_.clear();
          event_names_.clear();
          leader_ = -1;
          return;
        }
        continue;
      }
      if (fds_.empty()) {
        leader_ = fd;
      }
      fds_.push_back(fd);
      event_names_.push_back(spec.name);
    }
  }
};

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

void emit_json(const Options &options, const std::vector<Observation> &observations,
               const std::vector<std::string> &warnings, bool perf_available,
               const std::string &perf_error, double runtime_seconds) {
  std::cout << "{\n\"schema_version\":\"1.0\",\n\"metadata\":{";
  emit_string(std::cout, "architecture", architecture_name());
  emit_string(std::cout, "platform_family", platform_family());
  emit_string(std::cout, "profile", options.profile);
  std::cout << "\"seed\":" << options.seed << ",\"perf_available\":"
            << (perf_available ? "true" : "false") << ',';
  emit_string(std::cout, "perf_error", perf_error);
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

class MappedBuffer {
 public:
  explicit MappedBuffer(std::size_t bytes) : bytes_(bytes) {
    data_ = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data_ == MAP_FAILED) {
      data_ = nullptr;
      throw std::runtime_error(std::string("mmap failed: ") + std::strerror(errno));
    }
#ifdef MADV_HUGEPAGE
    madvise(data_, bytes_, MADV_HUGEPAGE);
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

std::size_t last_level_cache_bytes(int cpu) {
  const auto root = std::filesystem::path("/sys/devices/system/cpu") /
                    ("cpu" + std::to_string(cpu)) / "cache";
  std::size_t largest = 0;
  std::error_code error;
  for (const auto &entry : std::filesystem::directory_iterator(root, error)) {
    if (entry.path().filename().string().rfind("index", 0) != 0) continue;
    const auto type = read_file(entry.path() / "type");
    if (type == "Unified" || type == "Data")
      largest = std::max(largest, parse_size_bytes(read_file(entry.path() / "size")));
  }
  return largest;
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
    warnings.push_back(std::string("cache-latency probe stopped: ") + error.what());
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
      MappedBuffer buffer(bytes);
      build_random_chain(buffer.bytes(), bytes, page_size, random);
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
                       {"working_set_bytes", std::to_string(bytes)}});
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("TLB probe stopped: ") + error.what());
  }
}

enum class StreamOperation { Read, Write, Copy, Triad };

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
  const std::size_t llc = last_level_cache_bytes(physical.front().cpu);
  const std::size_t automatic_cap = options.profile == "quick" ? 512ULL * 1024ULL * 1024ULL
      : options.profile == "deep" ? 4ULL * 1024ULL * 1024ULL * 1024ULL
                                  : 2ULL * 1024ULL * 1024ULL * 1024ULL;
  const std::size_t bytes = options.memory_explicit
      ? requested_bytes : std::min(automatic_cap, std::max(requested_bytes, llc * 2));
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
        add_observation(observations, "memory_bandwidth", "stream_bandwidth",
                        result.gigabytes_per_second, "GB/s", "high",
                        "parallel pinned streaming kernel; payload bytes",
                        {{"operation", stream_name(operation)},
                         {"threads", std::to_string(count)},
                         {"bytes_per_array", std::to_string(bytes)},
                         {"cpu_scope", "physical-cores"}});
      }
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("memory-bandwidth probe stopped: ") + error.what());
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
    warnings.push_back(std::string("cache-bandwidth sweep stopped: ") + error.what());
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
    warnings.push_back(std::string("stride/prefetch sweep stopped: ") + error.what());
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
    warnings.push_back(std::string("memory-level parallelism probe stopped: ") + error.what());
  }
}

std::string cpu_relation(const CpuInfo &left, const CpuInfo &right) {
  if (left.socket == right.socket && left.die == right.die && left.core == right.core) {
    return "smt-sibling";
  }
  if (left.node == right.node) {
    return "same-numa-different-core";
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
    warnings.push_back("core-to-core latency skipped: process affinity exposes fewer than two CPUs");
    return;
  }
  std::vector<std::pair<CpuInfo, CpuInfo>> pairs;
  std::set<std::string> represented;
  for (std::size_t left = 0; left < cpus.size(); ++left) {
    for (std::size_t right = left + 1; right < cpus.size(); ++right) {
      const auto relation = cpu_relation(cpus[left], cpus[right]);
      if (options.profile == "deep" || represented.insert(relation).second) {
        pairs.emplace_back(cpus[left], cpus[right]);
      }
    }
  }
  if (options.profile == "deep" && pairs.size() > 2016) {
    pairs.resize(2016);
    warnings.push_back("deep core-latency matrix capped at 2016 CPU pairs");
  }
  const std::size_t rounds = options.profile == "smoke" ? 2000
      : options.profile == "quick" ? 20000
      : options.profile == "deep" ? 100000
                                  : 50000;
  for (const auto &[left, right] : pairs) {
    const double latency = ping_pong_latency(left.cpu, right.cpu, rounds);
    add_observation(observations, "core_latency", "cacheline_handoff_latency", latency,
                    "ns/one-way", "high", "release/acquire cache-line ping-pong",
                    {{"cpu_a", std::to_string(left.cpu)},
                     {"cpu_b", std::to_string(right.cpu)},
                     {"node_a", std::to_string(left.node)},
                     {"node_b", std::to_string(right.node)},
                     {"relation", cpu_relation(left, right)}});
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
    warnings.push_back("remote NUMA paths unavailable: fewer than two accessible NUMA nodes; local diagonal still measured");
  const std::size_t llc = last_level_cache_bytes(cpus.front().cpu);
  const std::size_t base_bytes = (options.profile == "quick" ? 64U : 256U) * 1024U * 1024U;
  const std::size_t numa_cap = options.profile == "quick" ? 512ULL * 1024ULL * 1024ULL
                                                         : 2ULL * 1024ULL * 1024ULL * 1024ULL;
  const std::size_t bytes = std::min(numa_cap, std::max(base_bytes, llc * 2));
  std::mt19937 random(options.seed ^ 0x4E554D41U);
  bool warned_bind = false;
  for (const auto &[memory_node, memory_cpus] : by_node) {
    try {
      MappedBuffer buffer(bytes);
      std::string bind_error;
      const bool bound = bind_memory(buffer.bytes(), buffer.size(), memory_node, bind_error);
      if (!bound && !warned_bind) {
        warnings.push_back("mbind unavailable (" + bind_error +
                           "); NUMA placement falls back to pinned first-touch");
        warned_bind = true;
      }
      for (const auto &[cpu_node, destination_cpus] : by_node) {
        pin_to_cpu(memory_cpus.front().cpu);
        build_random_chain(buffer.bytes(), buffer.size(), 64, random);
        pin_to_cpu(destination_cpus.front().cpu);
        const std::size_t iterations = options.profile == "quick" ? 300000 : 1000000;
        (void)chase_chain(buffer.bytes(), 10000);
        const double latency = chase_chain(buffer.bytes(), iterations);
        const Labels labels = {
            {"memory_node", std::to_string(memory_node)},
            {"cpu_node", std::to_string(cpu_node)},
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
        bandwidth_labels["threads"] = std::to_string(selected.size());
        add_observation(observations, "numa", "read_bandwidth", bandwidth, "GB/s",
                        bound ? "high" : "medium", "pinned NUMA aggregate read payload",
                        bandwidth_labels);
      }
    } catch (const std::exception &error) {
      warnings.push_back("NUMA node " + std::to_string(memory_node) +
                         " probe stopped: " + error.what());
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
  PerfGroup perf;
  const auto wall_begin = Clock::now();
  const auto tick_begin = cycle_counter();
  perf.start();
  function();
  auto counts = perf.stop();
  const auto tick_end = cycle_counter();
  const auto wall_end = Clock::now();
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

void record_kernel(std::vector<Observation> &observations, const std::string &name,
                   std::size_t operations, const KernelMeasurement &measurement,
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
  }
}

void benchmark_pipeline(const Options &options, std::vector<Observation> &observations,
                        int cpu) {
  pin_to_cpu(cpu);
  const std::size_t iterations = options.profile == "smoke" ? 2000
      : options.profile == "quick" ? 100000
      : options.profile == "deep" ? 2000000
                                  : 500000;
  auto nops = measure_kernel([&] { nop_kernel(iterations); });
  record_kernel(observations, "nop_frontend", iterations * 128, nops,
                {{"chains", "n/a"}, {"bound", "frontend"}});
  auto add1 = measure_kernel([&] { add_one_chain(iterations); });
  record_kernel(observations, "integer_add_dependency", iterations * 64, add1,
                {{"chains", "1"}, {"bound", "dependency"}});
  auto add4 = measure_kernel([&] { add_four_chains(iterations); });
  record_kernel(observations, "integer_add_parallel4", iterations * 64, add4,
                {{"chains", "4"}, {"bound", "backend"}});
  auto add8 = measure_kernel([&] { add_eight_chains(iterations); });
  record_kernel(observations, "integer_add_parallel8", iterations * 64, add8,
                {{"chains", "8"}, {"bound", "backend"}});
  auto mul1 = measure_kernel([&] { multiply_one_chain(iterations); });
  record_kernel(observations, "integer_mul_dependency", iterations * 32, mul1,
                {{"chains", "1"}, {"bound", "dependency"}});
  auto mul4 = measure_kernel([&] { multiply_four_chains(iterations); });
  record_kernel(observations, "integer_mul_parallel4", iterations * 32, mul4,
                {{"chains", "4"}, {"bound", "backend"}});
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

#if defined(__x86_64__)
std::uint64_t rob_trial(volatile std::uint64_t *first, volatile std::uint64_t *second,
                        std::size_t filler_iterations, bool flush) {
  if (flush) {
    _mm_clflush(const_cast<std::uint64_t *>(first));
    _mm_clflush(const_cast<std::uint64_t *>(second));
    _mm_mfence();
  }
  std::uint64_t one = 0, two = 0, filler = 1;
  const auto begin = cycle_counter();
  asm volatile("movq (%[address]), %[value]"
               : [value] "=r"(one) : [address] "r"(first) : "memory");
  for (std::size_t index = 0; index < filler_iterations; ++index) {
    asm volatile("addq $1, %[filler]" : [filler] "+r"(filler));
  }
  asm volatile("movq (%[address]), %[value]"
               : [value] "=r"(two) : [address] "r"(second) : "memory");
  const auto end = cycle_counter();
  global_sink = global_sink ^ one ^ two ^ filler;
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
  auto *first = reinterpret_cast<volatile std::uint64_t *>(buffer.bytes());
  auto *second = reinterpret_cast<volatile std::uint64_t *>(buffer.bytes() + 8U * 1024U * 1024U);
  *const_cast<std::uint64_t *>(first) = 1;
  *const_cast<std::uint64_t *>(second) = 2;
  const std::size_t maximum = options.profile == "quick" ? 256 : 768;
  const std::size_t step = options.profile == "quick" ? 16 : 8;
  const int trials = options.profile == "quick" ? 15 : 31;
  std::vector<std::pair<std::size_t, double>> penalties;
  for (std::size_t filler = 0; filler <= maximum; filler += step) {
    std::vector<std::uint64_t> cold;
    std::vector<std::uint64_t> hot;
    for (int trial = 0; trial < trials; ++trial) {
      hot.push_back(rob_trial(first, second, filler, false));
      cold.push_back(rob_trial(first, second, filler, true));
    }
    const double penalty = std::max(0.0, median(cold) - median(hot));
    penalties.emplace_back(filler, penalty);
    add_observation(observations, "reorder_window", "cold_load_overlap_penalty", penalty,
                    "counter-ticks", "low",
                    "two flushed loads separated by a dynamic independent-uop window",
                    {{"cpu", std::to_string(cpu)},
                     {"filler_iterations", std::to_string(filler)},
                     {"estimated_uops", std::to_string(filler * 2)}});
  }
  double baseline = 0.0;
  const std::size_t baseline_points = std::min<std::size_t>(4, penalties.size());
  for (std::size_t index = 0; index < baseline_points; ++index) baseline += penalties[index].second;
  baseline /= static_cast<double>(baseline_points);
  std::optional<std::size_t> knee;
  for (std::size_t index = baseline_points; index < penalties.size(); ++index) {
    if (penalties[index].second > baseline * 1.45 && penalties[index].second > baseline + 30.0) {
      knee = penalties[index].first;
      break;
    }
  }
  if (knee.has_value()) {
    add_observation(observations, "reorder_window", "rob_capacity_proxy",
                    static_cast<double>(*knee * 2), "estimated-uops", "low",
                    "first loss of overlap; loop body is approximately two fused-domain uops",
                    {{"lower_bound", std::to_string((*knee > step ? *knee - step : 0) * 2)},
                     {"upper_bound", std::to_string((*knee + step) * 3)}});
  } else {
    warnings.push_back("ROB/reorder-window knee was inconclusive; see raw overlap curve");
  }
#else
  (void)options;
  (void)observations;
  (void)cpu;
  warnings.push_back("ROB/reorder-window probe is currently x86/C86-only; ARM64 reports it as unavailable");
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
    warnings.push_back(std::string("page-fault probe stopped: ") + error.what());
  }

  if (cpus.size() < 2) return;
  int to_second[2] = {-1, -1};
  int to_first[2] = {-1, -1};
  if (pipe2(to_second, O_CLOEXEC) != 0 || pipe2(to_first, O_CLOEXEC) != 0) {
    warnings.push_back(std::string("scheduler handoff probe pipe setup failed: ") + std::strerror(errno));
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
    PerfGroup perf_probe;
    if (!perf_probe.available()) {
      warnings.push_back("hardware performance counters unavailable: " + perf_probe.error() +
                         "; IPC and exact per-core-cycle estimates are omitted");
    }

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
    section("core pipeline and IPC");
    benchmark_pipeline(options, observations, primary_cpu);
    section("branch predictor");
    benchmark_branches(options, observations, primary_cpu);
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

    emit_json(options, observations, warnings, perf_probe.available(), perf_probe.error(),
              seconds_between(run_begin, Clock::now()));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "sdc-native: " << error.what() << '\n';
    return 1;
  }
}
