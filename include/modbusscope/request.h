#pragma once
#include <cstdint>
#include <vector>

namespace modbusscope {

// Values are the on-wire Modbus function codes (MODBUS Application Protocol
// V1.1b3, sections 6.3 and 6.4). Do not renumber them to match an enum index:
// a consumer that logs or serializes the enum must see the same byte that goes
// into the PDU.
enum class FunctionCode : std::uint8_t {
    ReadHoldingRegisters = 0x03,
    ReadInputRegisters = 0x04,
    WriteSingleRegister = 0x06,
    WriteMultipleRegisters = 0x10,
};

static_assert(static_cast<std::uint8_t>(FunctionCode::ReadHoldingRegisters) ==
                  0x03,
              "0x03 is Read Holding Registers");
static_assert(static_cast<std::uint8_t>(FunctionCode::ReadInputRegisters) ==
                  0x04,
              "0x04 is Read Input Registers");
static_assert(static_cast<std::uint8_t>(FunctionCode::WriteSingleRegister) ==
                  0x06,
              "0x06 is Write Single Register");
static_assert(static_cast<std::uint8_t>(FunctionCode::WriteMultipleRegisters) ==
                  0x10,
              "0x10 is Write Multiple Registers");

// MODBUS Application Protocol V1.1b3 limits:
//   0x03/0x04: 1..125 registers per request
//   0x10:      1..123 registers per request
inline constexpr std::uint16_t kMaxReadRegisters = 125;
inline constexpr std::uint16_t kMaxWriteRegisters = 123;

constexpr bool is_read_function(FunctionCode function) noexcept {
    return function == FunctionCode::ReadHoldingRegisters ||
           function == FunctionCode::ReadInputRegisters;
}

constexpr bool is_write_function(FunctionCode function) noexcept {
    return function == FunctionCode::WriteSingleRegister ||
           function == FunctionCode::WriteMultipleRegisters;
}

struct ReadRequest {
    FunctionCode function{FunctionCode::ReadHoldingRegisters};
    std::uint8_t unitId{1};
    std::uint16_t startAddress{0};
    std::uint16_t quantity{1}; // 1..125; otherwise InvalidArgument
};

struct ReadResponse {
    FunctionCode function{FunctionCode::ReadHoldingRegisters};
    std::vector<std::uint16_t> registers; // move is noexcept
};

struct WriteRequest {
    FunctionCode function{FunctionCode::WriteMultipleRegisters};
    std::uint8_t unitId{1};
    std::uint16_t startAddress{0};
    std::uint16_t quantity{1}; // 0x06: == 1; 0x10: 1..123
    std::vector<std::uint16_t> values; // precondition: values.size() == quantity
};

struct WriteResponse {
    FunctionCode function{FunctionCode::WriteMultipleRegisters};
    std::uint16_t startAddress{0};
    std::uint16_t quantity{1};
};

} // namespace modbusscope
