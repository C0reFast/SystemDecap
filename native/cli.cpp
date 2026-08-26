#include "cli.hpp"

namespace sdc {
namespace {

void append_probe_names(std::vector<std::string> &target, std::string_view value) {
  std::stringstream input{std::string(value)};
  std::string name;
  while (std::getline(input, name, ',')) {
    if (!name.empty()) {
      target.push_back(name);
    }
  }
}

}  // namespace

void print_probe_list() {
  for (const auto &probe : probe_registry()) {
    std::cout << probe.name << '\t' << probe.description << '\n';
  }
}

Options parse_options(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto require_value = [&]() -> std::string {
      if (index + 1 >= argc) {
        throw std::runtime_error("missing value after " + argument);
      }
      return argv[++index];
    };
    if (argument == "--profile") {
      options.profile = require_value();
    } else if (argument == "--only") {
      append_probe_names(options.only_probes, require_value());
    } else if (argument == "--list-probes") {
      options.list_probes = true;
    } else if (argument == "--memory-mib") {
      options.memory_mib = static_cast<std::size_t>(std::stoull(require_value()));
      options.memory_explicit = true;
    } else if (argument == "--duration-ms") {
      options.duration_ms = std::stoi(require_value());
    } else if (argument == "--seed") {
      options.seed = static_cast<unsigned>(std::stoul(require_value()));
    } else if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else {
      throw std::runtime_error("unknown argument: " + argument);
    }
  }
  const std::set<std::string> valid_profiles{"smoke", "quick", "standard", "deep"};
  if (!valid_profiles.contains(options.profile)) {
    throw std::runtime_error("profile must be one of: smoke, quick, standard, deep");
  }
  if (options.memory_mib == 0) {
    options.memory_mib = options.profile == "smoke" ? 8
        : options.profile == "quick" ? 64
        : options.profile == "deep" ? 1024
                                    : 8192;
  }
  if (options.duration_ms <= 0) {
    options.duration_ms = options.profile == "smoke" ? 15
        : options.profile == "quick" ? 80
        : options.profile == "deep" ? 500
                                    : 200;
  }
  return options;
}

void print_help() {
  std::cout
      << "sdc-native - low-level probes for System Decap\n\n"
      << "Usage: sdc-native [--profile smoke|quick|standard|deep]\n"
      << "                  [--only PROBE[,PROBE...]] [--list-probes]\n"
      << "                  [--memory-mib N] [--duration-ms N] [--seed N]\n\n"
      << "Writes one JSON document to stdout and progress to stderr.\n"
      << "Architectures: x86_64 (including Hygon C86), ARM64/AArch64.\n";
}

}  // namespace sdc
