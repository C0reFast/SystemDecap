#include "../probe.hpp"
#include "../support/kernel_measurement.hpp"

namespace sdc {
namespace {

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

void run(ProbeContext &context) {
  benchmark_instruction_fetch(context.options, context.primary_cpu,
                                context.observations, context.warnings);
}

}  // namespace

ProbeDefinition instruction_fetch_probe() {
  return {"instruction-fetch", "instruction-side bandwidth and footprint", "指令侧输送吞吐与代码足迹", run};
}

}  // namespace sdc
