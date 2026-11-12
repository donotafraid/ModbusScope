#include "modbusscope/detail/scopeReadPlan.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

modbusscope::ScopeChannelConfig channel(std::uint16_t address,
                                        std::uint8_t unitId = 1,
                                        modbusscope::FunctionCode function =
                                            modbusscope::FunctionCode::
                                                ReadHoldingRegisters) {
    modbusscope::ScopeChannelConfig value;
    value.name = "ch";
    value.function = function;
    value.unitId = unitId;
    value.address = address;
    return value;
}

} // namespace

TEST(ScopeReadPlan, MergesContiguousChannelsIntoOneRequest) {
    const std::vector<modbusscope::ScopeChannelConfig> channels{
        channel(0), channel(1), channel(2)};
    const auto plan = modbusscope::detail::plan_scope_reads(channels);

    ASSERT_EQ(plan.runs.size(), 1u);
    EXPECT_EQ(plan.runs[0].startAddress, 0);
    EXPECT_EQ(plan.runs[0].quantity, 3);
    EXPECT_EQ(plan.runs[0].channels.size(), 3u);
    EXPECT_EQ(plan.channelRun, (std::vector<std::size_t>{0, 0, 0}));
    EXPECT_EQ(plan.channelOffset, (std::vector<std::uint16_t>{0, 1, 2}));
}

TEST(ScopeReadPlan, SplitsOnAddressGap) {
    const std::vector<modbusscope::ScopeChannelConfig> channels{
        channel(0), channel(2)};
    const auto plan = modbusscope::detail::plan_scope_reads(channels);

    ASSERT_EQ(plan.runs.size(), 2u);
    EXPECT_EQ(plan.runs[0].startAddress, 0);
    EXPECT_EQ(plan.runs[0].quantity, 1);
    EXPECT_EQ(plan.runs[1].startAddress, 2);
    EXPECT_EQ(plan.runs[1].quantity, 1);
}

TEST(ScopeReadPlan, SplitsOnUnitIdAndFunction) {
    const std::vector<modbusscope::ScopeChannelConfig> channels{
        channel(0, 1), channel(0, 2),
        channel(0, 1, modbusscope::FunctionCode::ReadInputRegisters)};
    const auto plan = modbusscope::detail::plan_scope_reads(channels);

    EXPECT_EQ(plan.runs.size(), 3u);
    for (const auto &run : plan.runs) {
        EXPECT_EQ(run.quantity, 1);
    }
}

TEST(ScopeReadPlan, SharesDuplicateAddress) {
    const std::vector<modbusscope::ScopeChannelConfig> channels{
        channel(7), channel(7)};
    const auto plan = modbusscope::detail::plan_scope_reads(channels);

    ASSERT_EQ(plan.runs.size(), 1u);
    EXPECT_EQ(plan.runs[0].quantity, 1);
    EXPECT_EQ(plan.channelRun, (std::vector<std::size_t>{0, 0}));
    EXPECT_EQ(plan.channelOffset, (std::vector<std::uint16_t>{0, 0}));
}

TEST(ScopeReadPlan, CapsOneRunAt125Registers) {
    std::vector<modbusscope::ScopeChannelConfig> channels;
    for (std::uint16_t address = 0; address < 126; ++address) {
        channels.push_back(channel(address));
    }
    const auto plan = modbusscope::detail::plan_scope_reads(channels);

    ASSERT_EQ(plan.runs.size(), 2u);
    EXPECT_EQ(plan.runs[0].startAddress, 0);
    EXPECT_EQ(plan.runs[0].quantity, 125);
    EXPECT_EQ(plan.runs[1].startAddress, 125);
    EXPECT_EQ(plan.runs[1].quantity, 1);
}

TEST(ScopeReadPlan, KeepsOriginalChannelOrderForValues) {
    const std::vector<modbusscope::ScopeChannelConfig> channels{
        channel(2), channel(0), channel(1)};
    const auto plan = modbusscope::detail::plan_scope_reads(channels);

    ASSERT_EQ(plan.runs.size(), 1u);
    EXPECT_EQ(plan.runs[0].startAddress, 0);
    EXPECT_EQ(plan.runs[0].quantity, 3);
    EXPECT_EQ(plan.channelRun, (std::vector<std::size_t>{0, 0, 0}));
    EXPECT_EQ(plan.channelOffset, (std::vector<std::uint16_t>{2, 0, 1}));
}

TEST(ScopeReadPlan, EmptyChannelListProducesNoRuns) {
    const auto plan = modbusscope::detail::plan_scope_reads({});
    EXPECT_TRUE(plan.runs.empty());
    EXPECT_TRUE(plan.channelRun.empty());
    EXPECT_TRUE(plan.channelOffset.empty());
}
