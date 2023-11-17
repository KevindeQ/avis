#ifndef CORE_COMMON_H
#define CORE_COMMON_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <locale>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

#include "avis/core/configuration/build_configuration.h"

using namespace std::chrono_literals;

#include "avis/core/assertions.h"
#include "avis/core/exceptions.h"
#include "avis/core/helper_functions.h"
#include "avis/core/string_functions.h"
#include "avis/core/unique_handle.h"

#endif
