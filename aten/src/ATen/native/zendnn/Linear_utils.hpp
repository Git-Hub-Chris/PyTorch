#pragma once

#include <cpuinfo.h>
#include <cstdint>
#include <functional> // For std::reference_wrapper, std::ref, std::cref
#include <iostream>
#include <optional> // For std::optional, std::nullopt
#include <string>
#include <torch/all.h>
#if AT_ZENDNN_ENABLED()
#include <zendnnl.hpp>

// headerfile content will be added here
#endif // AT_ZENDNN_ENABLED()