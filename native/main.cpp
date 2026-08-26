#include "cli.hpp"

int main(int argc, char **argv) {
  try {
    const sdc::Options options = sdc::parse_options(argc, argv);
    if (options.help) {
      sdc::print_help();
      return 0;
    }
    if (options.list_probes) {
      sdc::print_probe_list();
      return 0;
    }
    if (sdc::architecture_name() == "unsupported") {
      std::cerr << "Unsupported build architecture. Expected x86_64 or ARM64.\n";
      return 2;
    }

    const auto run_begin = sdc::Clock::now();
    const auto selected = sdc::selected_probe_definitions(options);
    std::vector<std::string> selected_names;
    selected_names.reserve(selected.size());
    for (const auto *probe : selected) {
      selected_names.push_back(probe->name);
    }

    std::vector<sdc::Observation> observations;
    std::vector<std::string> warnings;
    const auto cpus = sdc::discover_cpus();
    const int primary_cpu = cpus.front().cpu;
    sdc::pin_to_cpu(primary_cpu);

    sdc::PerfEnvironment perf_environment;
    perf_environment.perf_event_paranoid =
        sdc::read_file("/proc/sys/kernel/perf_event_paranoid");
    perf_environment.nmi_watchdog =
        sdc::read_file("/proc/sys/kernel/nmi_watchdog");
    perf_environment.core = sdc::probe_perf_group(sdc::PerfGroupKind::Core);
    perf_environment.branch = sdc::probe_perf_group(sdc::PerfGroupKind::Branch);
    perf_environment.cache = sdc::probe_perf_group(sdc::PerfGroupKind::Cache);
    if (!perf_environment.core.available) {
      warnings.push_back("核心硬件性能计数器不可用：" + perf_environment.core.error +
                         "；已省略 IPC 与精确的每核心周期估计");
    }
    if (!perf_environment.branch.available) {
      warnings.push_back("分支硬件性能计数器不可用：" + perf_environment.branch.error +
                         "；仍保留墙钟时间分支测试");
    }
    if (!perf_environment.cache.available) {
      warnings.push_back("通用缓存硬件性能计数器不可用：" + perf_environment.cache.error +
                         "；仍保留软件内存层级测试");
    }

    sdc::ProbeContext context{
        options, cpus, primary_cpu, observations, warnings};
    for (const auto *probe : selected) {
      std::cerr << "[system-decap] " << probe->section << "\n";
      probe->run(context);
    }

    sdc::emit_json(options, observations, warnings, perf_environment,
                   sdc::seconds_between(run_begin, sdc::Clock::now()),
                   selected_names);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "sdc-native: " << error.what() << '\n';
    return 1;
  }
}
