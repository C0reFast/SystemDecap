#include "../probe.hpp"
#include "../support/pointer_chase.hpp"

namespace sdc {
namespace {

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

void run(ProbeContext &context) {
  benchmark_memory_parallelism(context.options, context.primary_cpu,
                                 context.observations, context.warnings);
}

}  // namespace

ProbeDefinition memory_parallelism_probe() {
  return {"memory-parallelism", "memory-level parallelism", "随机加载内存级并行度", run};
}

}  // namespace sdc
