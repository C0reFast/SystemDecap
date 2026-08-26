#include "../probe.hpp"
#include "../support/pointer_chase.hpp"
#include "../support/stream.hpp"

namespace sdc {
namespace {

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
        sum ^= stream_read_vector_assembly(data + begin, end - begin);
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
  const std::size_t numa_cap = options.profile == "quick"
      ? 2ULL * 1024ULL * 1024ULL * 1024ULL
      : options.profile == "deep" ? 16ULL * 1024ULL * 1024ULL * 1024ULL
                                  : 8ULL * 1024ULL * 1024ULL * 1024ULL;
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
      ? requested_bytes : std::max(base_bytes, largest_node_llc * 4);
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
  if (largest_node_llc == 0 || bytes < largest_node_llc * 4) {
    warnings.push_back(
        "NUMA 带宽工作集未确认达到读取节点 LLC 的 4 倍；远端带宽可能包含缓存复用，不会进入摘要"
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
        const bool exceeds_llc = reader_llc > 0 && bytes >= reader_llc * 4;
        bandwidth_labels["threads"] = std::to_string(selected.size());
        bandwidth_labels["working_set_bytes"] = std::to_string(bytes);
        bandwidth_labels["reader_node_llc_bytes"] = std::to_string(reader_llc);
        bandwidth_labels["bytes_per_thread"] = std::to_string(bytes / selected.size());
        bandwidth_labels["working_set_exceeds_llc"] = exceeds_llc ? "true" : "false";
        bandwidth_labels["dram_working_set_threshold"] = "4x-reader-node-llc";
        bandwidth_labels["read_kernel"] = stream_read_kernel_name();
        bandwidth_labels["cache_bypass"] = "not-guaranteed-for-write-back-memory";
        add_observation(observations, "numa", "read_bandwidth", bandwidth, "GB/s",
                        exceeds_llc ? (bound ? "high" : "medium") : "low",
                        "pinned NUMA vector-assembly read payload with LLC coverage metadata",
                        bandwidth_labels);
      }
    } catch (const std::exception &error) {
      warnings.push_back("NUMA 节点 " + std::to_string(memory_node) +
                         " 的探针提前停止：" + error.what());
    }
  }
}

void run(ProbeContext &context) {
  benchmark_numa(context.options, context.cpus, context.observations,
                   context.warnings);
}

}  // namespace

ProbeDefinition numa_probe() {
  return {"numa", "NUMA latency and bandwidth matrix", "NUMA 延迟与读取带宽矩阵", run};
}

}  // namespace sdc
