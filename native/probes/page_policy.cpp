#include "../probe.hpp"
#include "../support/pointer_chase.hpp"

namespace sdc {
namespace {

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

void run(ProbeContext &context) {
  benchmark_page_policy(context.options, context.primary_cpu,
                          context.observations, context.warnings);
}

}  // namespace

ProbeDefinition page_policy_probe() {
  return {"page-policy", "base-page versus transparent-hugepage policy", "基础页与透明大页策略", run};
}

}  // namespace sdc
