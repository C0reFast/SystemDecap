#pragma once

#include "runtime.hpp"

namespace sdc {

struct ProbeContext {
  const Options &options;
  const std::vector<CpuInfo> &cpus;
  int primary_cpu;
  std::vector<Observation> &observations;
  std::vector<std::string> &warnings;
};

using ProbeRunner = void (*)(ProbeContext &);

struct ProbeDefinition {
  std::string name;
  std::string section;
  std::string description;
  ProbeRunner run;
};

const std::vector<ProbeDefinition> &probe_registry();
std::vector<const ProbeDefinition *> selected_probe_definitions(const Options &options);

}  // namespace sdc
