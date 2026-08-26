#pragma once

#include "../probe.hpp"

namespace sdc {

ProbeDefinition timer_probe();
ProbeDefinition cache_latency_probe();
ProbeDefinition tlb_probe();
ProbeDefinition memory_bandwidth_probe();
ProbeDefinition cache_bandwidth_probe();
ProbeDefinition stride_prefetch_probe();
ProbeDefinition memory_parallelism_probe();
ProbeDefinition page_policy_probe();
ProbeDefinition loaded_memory_latency_probe();
ProbeDefinition store_forwarding_probe();
ProbeDefinition instruction_fetch_probe();
ProbeDefinition pipeline_probe();
ProbeDefinition compute_scaling_probe();
ProbeDefinition branch_predictor_probe();
ProbeDefinition branch_structures_probe();
ProbeDefinition core_latency_probe();
ProbeDefinition false_sharing_probe();
ProbeDefinition numa_probe();
ProbeDefinition rob_probe();
ProbeDefinition os_overheads_probe();

}  // namespace sdc
