#pragma once

#include <cstddef>
#include <cstdint>

inline bool HttpsInternalRamReady() { return true; }
inline size_t HttpsLargestInternalBlock() { return 128 * 1024; }
