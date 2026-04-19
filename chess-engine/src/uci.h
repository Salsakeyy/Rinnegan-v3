#pragma once

#include <string>

namespace UCI {

extern bool useNNUE;
extern int threads;
extern std::string evalFile;

void loop();

} // namespace UCI
