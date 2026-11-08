#pragma once
#include <cstdint>
#include <vector>

namespace modbusscope {

enum class FunctionCode : std::uint8_t {
  ReadHoldingRegisters = 0x03,
  ReadInputRegisters = 0x04,
  WriteSingleRegister = 0x06,
  WriteMultipleRegisters = 0x10,
};

struct ReadRequest {
    FunctionCode function;
    std::uint8_t unitId;
    std::uint16_t startAddress;
    std::uint16_t quantity;    // 本次读/写多少个寄存器,1..125；否则 InvalidArgument
};

struct ReadResponse {
    FunctionCode function;
    std::vector<std::uint16_t> registers;   // 移动为 noexcept
};

struct WriteRequest {
    FunctionCode function;                  // 仅 0x06 或 0x10；其他 -> UnsupportedFunction
    std::uint8_t unitId;
    std::uint16_t startAddress;
    std::uint16_t quantity;                 // 0x06: 必须 == 1；0x10: 1..123
    std::vector<std::uint16_t> values;      // 前置条件：values.size() == quantity
};

struct WriteResponse {
    FunctionCode function;      // 回显 0x06 或 0x10
    std::uint16_t startAddress; // 回显
    std::uint16_t quantity;     // 0x06 恒为 1；0x10 回显请求数量
};

} // namespace modbusscope