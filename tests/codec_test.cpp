// Qt-free codec test. This target is deliberately not linked against Qt5::Core:
// if registerCodec.h ever includes a Qt header again, the build fails here.
#include "modbusscope/registerCodec.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string &what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    } else {
        std::cout << "ok: " << what << '\n';
    }
}

} // namespace

int main() {
    using modbusscope::WordOrder64;

    const std::vector<std::uint16_t> two{0x1234, 0x5678};
    check(modbusscope::toUint32(two, 0, true) == 0x12345678U,
          "toUint32 high word first");
    check(modbusscope::toUint32(two, 0, false) == 0x56781234U,
          "toUint32 low word first");

    const std::vector<std::uint16_t> floatWords{0x3FC0, 0x0000};
    const auto floatValue = modbusscope::toFloat32(floatWords, 0, true);
    check(floatValue.has_value() && *floatValue == 1.5F, "toFloat32 1.5");

    const std::vector<std::uint16_t> negative{0xFFFF, 0xFFFE};
    const auto signedValue = modbusscope::toI32(negative, 0, true);
    check(signedValue.has_value() && *signedValue == -2, "toI32 sign bit");

    const std::vector<std::uint16_t> four{0x0001, 0x0002, 0x0003, 0x0004};
    check(modbusscope::toUint64(four, 0, WordOrder64::ABCD) ==
              0x0001000200030004ULL,
          "toUint64 ABCD");
    check(modbusscope::toUint64(four, 0, WordOrder64::CDAB) ==
              0x0004000300020001ULL,
          "toUint64 CDAB");

    const std::vector<std::uint16_t> negative64{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFE};
    const auto signed64 = modbusscope::toI64(negative64, 0, WordOrder64::ABCD);
    check(signed64.has_value() && *signed64 == -2, "toI64 sign bit");

    check(!modbusscope::toUint32({}, 0, true).has_value(),
          "empty input is rejected");
    check(!modbusscope::toUint32(two, 1, true).has_value(),
          "two-word access at the last word is rejected");
    check(!modbusscope::toUint64(four, 1, WordOrder64::ABCD).has_value(),
          "four-word access past the end is rejected");
    check(!modbusscope::toUint32(two, static_cast<std::size_t>(-1), true)
               .has_value(),
          "overflowing index is rejected");

    std::cout << (failures == 0 ? "CODEC PASS" : "CODEC FAIL") << '\n';
    return failures == 0 ? 0 : 1;
}
