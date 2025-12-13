#ifndef COMMON_H
#define COMMON_H

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <locale>
#include <map>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <queue>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "avis/build_configuration.h"

using namespace std::chrono_literals;

#include "avis/assertions.h"
#include "avis/utils/com_ptr.h"
#include "avis/utils/exceptions.h"
#include "avis/utils/helper_functions.h"
#include "avis/utils/string_functions.h"
#include "avis/utils/unique_handle.h"

template<typename enum_t>
concept enumeration = std::is_enum_v<enum_t>;

#endif
