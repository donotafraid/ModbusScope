// Public-header compile probe.
//
// This target links only modbusScope::modbusScope, not Qt5::Core. The public
// headers must therefore compile without Qt include directories. It also pins
// the Rule of Five and the state() return type at compile time.
#include "modbusscope/modbusscope.h"

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(!std::is_copy_constructible_v<modbusscope::Client>,
              "Client must not be copyable");
static_assert(!std::is_copy_assignable_v<modbusscope::Client>,
              "Client must not be copy-assignable");
static_assert(!std::is_move_constructible_v<modbusscope::Client>,
              "Client must not be movable");
static_assert(!std::is_move_assignable_v<modbusscope::Client>,
              "Client must not be move-assignable");
static_assert(std::is_nothrow_destructible_v<modbusscope::Client>,
              "~Client must be noexcept");
static_assert(
    std::is_same_v<decltype(std::declval<const modbusscope::Client &>().state()),
                   modbusscope::LifeState>,
    "state() must return LifeState directly");
static_assert(
    std::is_same_v<decltype(modbusscope::toUint32(
                       std::declval<const std::vector<std::uint16_t> &>(), 0,
                       true)),
                   std::optional<std::uint32_t>>,
    "codec must return std::optional and use std::vector");

int main() {
    const std::vector<std::uint16_t> words{0x1234, 0x5678};
    const auto value = modbusscope::toUint32(words, 0, true);
    return value && *value == 0x12345678U ? 0 : 1;
}
