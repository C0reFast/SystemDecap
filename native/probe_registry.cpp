#include "probe.hpp"

#include "probes/probes.hpp"

namespace sdc {

const std::vector<ProbeDefinition> &probe_registry() {
  static const std::vector<ProbeDefinition> probes = {
      timer_probe(),
      cache_latency_probe(),
      tlb_probe(),
      memory_bandwidth_probe(),
      cache_bandwidth_probe(),
      stride_prefetch_probe(),
      memory_parallelism_probe(),
      page_policy_probe(),
      loaded_memory_latency_probe(),
      store_forwarding_probe(),
      instruction_fetch_probe(),
      pipeline_probe(),
      compute_scaling_probe(),
      branch_predictor_probe(),
      branch_structures_probe(),
      core_latency_probe(),
      false_sharing_probe(),
      numa_probe(),
      rob_probe(),
      os_overheads_probe(),
  };
  return probes;
}

std::vector<const ProbeDefinition *> selected_probe_definitions(const Options &options) {
  const std::set<std::string> requested(options.only_probes.begin(),
                                        options.only_probes.end());
  std::vector<const ProbeDefinition *> selected;
  for (const auto &probe : probe_registry()) {
    if (requested.empty() || requested.contains(probe.name)) {
      selected.push_back(&probe);
    }
  }
  if (!requested.empty() && selected.size() != requested.size()) {
    std::vector<std::string> unknown;
    for (const auto &name : requested) {
      const bool known = std::any_of(
          probe_registry().begin(), probe_registry().end(),
          [&](const auto &probe) { return probe.name == name; });
      if (!known) {
        unknown.push_back(name);
      }
    }
    std::ostringstream message;
    message << "unknown probe" << (unknown.size() == 1 ? "" : "s") << ": ";
    for (std::size_t index = 0; index < unknown.size(); ++index) {
      message << unknown[index] << (index + 1 < unknown.size() ? ", " : "");
    }
    throw std::runtime_error(message.str());
  }
  return selected;
}

}  // namespace sdc
