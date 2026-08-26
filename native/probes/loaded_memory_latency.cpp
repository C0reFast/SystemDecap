#include "../probe.hpp"
#include "../support/pointer_chase.hpp"
#include "../support/stream.hpp"

namespace sdc {
namespace {

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

void run(ProbeContext &context) {
  benchmark_loaded_memory_latency(context.options, context.cpus,
                                    context.observations, context.warnings);
}

}  // namespace

ProbeDefinition loaded_memory_latency_probe() {
  return {"loaded-memory-latency", "loaded memory latency", "带宽压力下的随机内存延迟", run};
}

}  // namespace sdc
