#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace strata::runtime {
bool decode_extra_image(const std::string &, uint32_t &, uint32_t &, std::vector<unsigned char> &, int &);
}
