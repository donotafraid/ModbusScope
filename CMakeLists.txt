# CMake minimum and project languages. Keep the standard pinned: the public
# headers and the Qt 5.15 implementation both rely on C++17.
cmake_minimum_required(VERSION 3.15)
project(App LANGUAGES CXX C)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "Enable AddressSanitizer + UBSan" OFF)
option(ENABLE_LSAN "Enable AddressSanitizer + LSan (leak only)" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

set(_san_count 0)
foreach(_opt ENABLE_ASAN ENABLE_LSAN ENABLE_TSAN)
    if(${_opt})
        math(EXPR _san_count "${_san_count} + 1")
    endif()
endforeach()
if(_san_count GREATER 1)
    message(FATAL_ERROR "Only one of ENABLE_ASAN/ENABLE_LSAN/ENABLE_TSAN can be ON")
endif()

if(ENABLE_LSAN)
    add_compile_options(-fsanitize=address,leak -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,leak)
elseif(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fno-sanitize=function)
        add_link_options(-fno-sanitize=function)
    endif()
elseif(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fstandalone-debug -Wno-unused-command-line-argument)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt5 5.15.3 EXACT COMPONENTS Core Network SerialBus REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(InternalrootDir "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(
    ${InternalrootDir}/src/modbusscope/
    modbusscope-build
)

# Runnable smoke client. It talks to tests/modbus_server.py; no test framework
# is required to reproduce the integration path.
add_executable(modbus_client_smoke ${InternalrootDir}/tests/smoke_client.cpp)
target_compile_features(modbus_client_smoke PRIVATE cxx_std_17)
target_link_libraries(modbus_client_smoke PRIVATE
    modbusScope::modbusScope
    Qt5::Core
)
set_target_properties(modbus_client_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(modbus_client_stress ${InternalrootDir}/tests/stress_client.cpp)
target_compile_features(modbus_client_stress PRIVATE cxx_std_17)
target_link_libraries(modbus_client_stress PRIVATE
    modbusScope::modbusScope
    Qt5::Core
)
set_target_properties(modbus_client_stress PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(modbus_client_recovery ${InternalrootDir}/tests/recovery_client.cpp)
target_compile_features(modbus_client_recovery PRIVATE cxx_std_17)
target_link_libraries(modbus_client_recovery PRIVATE
    modbusScope::modbusScope
    Qt5::Core
)
set_target_properties(modbus_client_recovery PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# P1 fault-injection probes. The integration modes need a server; the
# no-QCoreApplication mode is standalone and is wired into CTest.
add_executable(modbus_client_p1_regression
    ${InternalrootDir}/tests/p1_regression.cpp)
target_compile_features(modbus_client_p1_regression PRIVATE cxx_std_17)
target_link_libraries(modbus_client_p1_regression PRIVATE
    modbusScope::modbusScope
    Qt5::Core
)
set_target_properties(modbus_client_p1_regression PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME p1_no_qcoreapp
         COMMAND modbus_client_p1_regression no_qcoreapp)
set_tests_properties(p1_no_qcoreapp PROPERTIES TIMEOUT 10)
