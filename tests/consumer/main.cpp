// Installed-package consumer. It deliberately avoids Qt and QCoreApplication:
// the core target must link through its exported Qt dependencies without the
// consumer naming them, and the codec path must stay Qt-free.
#include "modbusscope/modbusscope.h"

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

static_assert(!std::is_copy_constructible_v<modbusscope::Client>);
static_assert(!std::is_move_constructible_v<modbusscope::Client>);
static_assert(std::is_nothrow_destructible_v<modbusscope::Client>);
static_assert(std::is_nothrow_destructible_v<modbusscope::ScopeModel>);

int main() {
    const std::vector<std::uint16_t> words{0x1234, 0x5678};
    const auto value = modbusscope::toUint32(words, 0, true);
    if (!value || *value != 0x12345678u) {
        return 1;
    }
    // Force a real symbol from libmodbusScope.a, not just a header-only call.
    if (std::string(modbusscope::to_string(modbusscope::LifeState::READY)) !=
        "READY") {
        return 2;
    }
    if (modbusscope::kVersionString[0] == '\0') {
        return 3;
    }
    return 0;
}
