cmake_minimum_required(VERSION 3.15)
project(modbusScopeConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The point of this project is to consume the installed package, not the build
# tree. The prefix is supplied through CMAKE_PREFIX_PATH by the driver script.
find_package(modbusScope 0.2 CONFIG REQUIRED)

add_executable(modbus_consumer main.cpp)
target_link_libraries(modbus_consumer PRIVATE modbusScope::modbusScope)
