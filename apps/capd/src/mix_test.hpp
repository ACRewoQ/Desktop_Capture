// "capd mixtest": video mixer self-test on a real PC (Windows). See mix_test.cpp.
#pragma once

#include <string>
#include <vector>

namespace dc::capd {

int cmdMixTest(const std::vector<std::string>& args);

}  // namespace dc::capd
