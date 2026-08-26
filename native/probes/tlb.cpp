#include "../probe.hpp"
#include "../support/pointer_chase.hpp"

namespace sdc {
namespace {

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

void run(ProbeContext &context) {
  benchmark_tlb(context.options, context.observations, context.warnings,
                  context.primary_cpu);
}

}  // namespace

ProbeDefinition tlb_probe() {
  return {"tlb", "TLB/page-walk sweep", "TLB 容量与页表遍历延迟", run};
}

}  // namespace sdc
