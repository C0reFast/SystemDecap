#pragma once

#include "probe.hpp"

namespace sdc {

Options parse_options(int argc, char **argv);
void print_help();
void print_probe_list();

}  // namespace sdc
