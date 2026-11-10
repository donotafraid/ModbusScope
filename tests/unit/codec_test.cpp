#include "modbusscope/registerCodec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace {

using modbusscope::WordOrder64;

} // namespace

TEST(RegisterCodec, Uint32WordOrders) {
    const std::vector<std::uint16_t> words{0x1234, 0x5678};
    ASSERT_TRUE(modbusscope::toUint32(words, 0, true).has_value());
    EXPECT_EQ(*modbusscope::toUint32(words, 0, true), 0x12345678U);
    ASSERT_TRUE(modbusscope::toUint32(words, 0, false).has_value());
    EXPECT_EQ(*modbusscope::toUint32(words, 0, false), 0x56781234U);
}

TEST(RegisterCodec, Float32BitPattern) {
    const std::vector<std::uint16_t> words{0x3F80, 0x0000};
    const auto value = modbusscope::toFloat32(words, 0, true);
    ASSERT_TRUE(value.has_value());
    EXPECT_FLOAT_EQ(*value, 1.0F);
}

TEST(RegisterCodec, Int32TwosComplement) {
    const std::vector<std::uint16_t> words{0xFFFF, 0xFFFE};
    const auto value = modbusscope::toI32(words, 0, true);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, -2);
}

TEST(RegisterCodec, Uint64WordOrders) {
    const std::vector<std::uint16_t> words{0x0123, 0x4567, 0x89AB, 0xCDEF};
    ASSERT_TRUE(modbusscope::toUint64(words, 0, WordOrder64::ABCD).has_value());
    EXPECT_EQ(*modbusscope::toUint64(words, 0, WordOrder64::ABCD),
              0x0123456789ABCDEFULL);
    ASSERT_TRUE(modbusscope::toUint64(words, 0, WordOrder64::CDAB).has_value());
    EXPECT_EQ(*modbusscope::toUint64(words, 0, WordOrder64::CDAB),
              0xCDEF89AB45670123ULL);
}

TEST(RegisterCodec, Int64TwosComplement) {
    const std::vector<std::uint16_t> words{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFE};
    const auto value = modbusscope::toI64(words, 0, WordOrder64::ABCD);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, -2);
}

TEST(RegisterCodec, ShortVectorsReturnNullopt) {
    EXPECT_FALSE(modbusscope::toUint32({}, 0, true).has_value());
    EXPECT_FALSE(modbusscope::toUint32({0x1234}, 0, true).has_value());
    EXPECT_FALSE(modbusscope::toUint32({0x1234, 0x5678}, 1, true).has_value());
    EXPECT_FALSE(modbusscope::toFloat32({0x3F80}, 0, true).has_value());
    EXPECT_FALSE(modbusscope::toI32({0xFFFF}, 0, true).has_value());
    EXPECT_FALSE(modbusscope::toUint64({0x0123, 0x4567}, 0,
                                       WordOrder64::ABCD)
                     .has_value());
    EXPECT_FALSE(modbusscope::toI64({0x0123, 0x4567, 0x89AB}, 0,
                                    WordOrder64::ABCD)
                     .has_value());
}

TEST(RegisterCodec, HugeIndexDoesNotOverflow) {
    const std::vector<std::uint16_t> words{0x1234, 0x5678};
    EXPECT_FALSE(modbusscope::toUint32(
                     words, std::numeric_limits<std::size_t>::max(), true)
                     .has_value());
    EXPECT_FALSE(modbusscope::toUint64(
                     words, std::numeric_limits<std::size_t>::max(),
                     WordOrder64::ABCD)
                     .has_value());
}

TEST(RegisterCodec, OffsetAccessWorks) {
    const std::vector<std::uint16_t> words{0x0000, 0x1234, 0x5678};
    ASSERT_TRUE(modbusscope::toUint32(words, 1, true).has_value());
    EXPECT_EQ(*modbusscope::toUint32(words, 1, true), 0x12345678U);
}
