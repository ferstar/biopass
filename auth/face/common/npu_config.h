#pragma once

#include <string>

namespace biopass::npu {
std::string setting(const char* name, const char* fallback);
std::string prepareCache();
void prepareRuntime(const std::string& cache);
}  // namespace biopass::npu
