#pragma once
#include <string>
#include <vector>

namespace strata::runtime {
int cluster_main(const std::vector<std::string>& args);
int worker_main(const std::vector<std::string>& args);
int server_main(const std::vector<std::string>& args);
int vision_main(const std::vector<std::string>& args);
} // namespace strata::runtime
