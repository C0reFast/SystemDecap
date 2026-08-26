#include "../probe.hpp"

namespace sdc {
namespace {

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

void run(ProbeContext &context) {
  benchmark_os_overheads(context.options, context.cpus, context.observations,
                           context.warnings);
}

}  // namespace

ProbeDefinition os_overheads_probe() {
  return {"os-overheads", "OS and scheduler overheads", "系统调用、缺页与调度交接开销", run};
}

}  // namespace sdc
