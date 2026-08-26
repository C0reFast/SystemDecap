#include "../probe.hpp"

namespace sdc {
namespace {

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

void run(ProbeContext &context) {
  benchmark_rob(context.options, context.observations, context.warnings,
                  context.primary_cpu);
}

}  // namespace

ProbeDefinition rob_probe() {
  return {"rob", "reorder-window / ROB proxy", "ROB/乱序窗口容量代理曲线", run};
}

}  // namespace sdc
