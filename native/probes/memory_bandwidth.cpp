#include "../probe.hpp"
#include "../support/stream.hpp"

namespace sdc {
namespace {

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
  const std::size_t automatic_cap = options.profile == "smoke" ? 64ULL * 1024ULL * 1024ULL
      : options.profile == "quick" ? 2ULL * 1024ULL * 1024ULL * 1024ULL
      : options.profile == "deep" ? 16ULL * 1024ULL * 1024ULL * 1024ULL
                                  : 8ULL * 1024ULL * 1024ULL * 1024ULL;
  const std::size_t available = memory_available_bytes();
  const std::size_t safe_per_array = available > 0 ? available / 6 : automatic_cap;
  const std::size_t desired_bytes = options.memory_explicit
      ? requested_bytes : std::max(requested_bytes, aggregate_llc * 4);
  const std::size_t bytes = std::max<std::size_t>(4096,
      std::min(desired_bytes, options.memory_explicit
          ? safe_per_array : std::min(automatic_cap, safe_per_array)));
  const bool read_working_set_exceeds_llc =
      aggregate_llc > 0 && bytes >= aggregate_llc * 4;
  if (!read_working_set_exceeds_llc) {
    warnings.push_back(
        "内存带宽工作集未达到整机 LLC 的 4 倍；结果可能包含缓存带宽，不会进入 DRAM 摘要"
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
        const bool exceeds_llc = aggregate_llc > 0 && working_set >= aggregate_llc * 4;
        add_observation(observations, "memory_bandwidth", "stream_bandwidth",
                        result.gigabytes_per_second, "GB/s", exceeds_llc ? "high" : "low",
                        operation == StreamOperation::Read
                            ? "architecture-specific vector assembly streaming loads; payload bytes"
                            : "parallel pinned streaming kernel; payload bytes",
                        {{"operation", stream_name(operation)},
                         {"threads", std::to_string(count)},
                         {"bytes_per_array", std::to_string(bytes)},
                         {"working_set_bytes", std::to_string(working_set)},
                         {"aggregate_llc_bytes", std::to_string(aggregate_llc)},
                         {"bytes_per_thread", std::to_string(working_set / count)},
                         {"working_set_exceeds_llc", exceeds_llc ? "true" : "false"},
                         {"dram_working_set_threshold", "4x-aggregate-llc"},
                         {"read_kernel", operation == StreamOperation::Read
                                             ? stream_read_kernel_name() : "compiler-vectorized"},
                         {"cache_bypass", "not-guaranteed-for-write-back-memory"},
                         {"cpu_scope", "physical-cores"}});
      }
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("内存带宽探针提前停止：") + error.what());
  }
}

void run(ProbeContext &context) {
  benchmark_bandwidth(context.options, context.cpus, context.observations,
                        context.warnings);
}

}  // namespace

ProbeDefinition memory_bandwidth_probe() {
  return {"memory-bandwidth", "memory bandwidth scaling", "单核与聚合内存流式带宽", run};
}

}  // namespace sdc
