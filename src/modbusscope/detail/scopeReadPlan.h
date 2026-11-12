#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "modbusscope/request.h"
#include "modbusscope/scope.h"

namespace modbusscope::detail {

// One Modbus request covering one or more Scope channels. Channels are grouped
// by (function, unitId) and merged only when their register addresses are
// contiguous, so a read never touches an address the caller did not configure.
struct ScopeReadRun {
    FunctionCode function = FunctionCode::ReadHoldingRegisters;
    std::uint8_t unitId = 1;
    std::uint16_t startAddress = 0;
    std::uint16_t quantity = 1;
    std::vector<std::size_t> channels; // channel indexes in request order
};

// Deterministic map from each channel to the run that carries it. `channelRun`
// and `channelOffset` have channels.size() entries and are parallel.
struct ScopeReadPlan {
    std::vector<ScopeReadRun> runs;
    std::vector<std::size_t> channelRun;
    std::vector<std::uint16_t> channelOffset;
};

// Plans the minimum number of contiguous-register reads for the channel list.
// Duplicate addresses share a run; a run is split at kMaxReadRegisters. The
// caller must have validated the channels first.
ScopeReadPlan plan_scope_reads(
    const std::vector<ScopeChannelConfig> &channels);

} // namespace modbusscope::detail
