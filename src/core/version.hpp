#pragma once

#include <string_view>

namespace dariyanaap {

// Stamped into every CSV header row. A run's numbers are only comparable to
// another run's if the rig that produced them was the same, and "which build
// was this?" is not a question you can answer three repos later from memory.
std::string_view version();

}  // namespace dariyanaap
