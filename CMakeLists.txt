# CMake minimum and project languages. Keep the standard pinned: the public
# headers and the Qt 5.15 implementation both rely on C++17.
cmake_minimum_required(VERSION 3.15)
project(modbusScope VERSION 0.1.0 LANGUAGES CXX C)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

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

# Integration clients. These need QtCore only for the application object and
# the report formatting; the wrapper library itself is linked through its alias.
function(add_modbus_client name source)
    add_executable(${name} ${source})
    target_compile_features(${name} PRIVATE cxx_std_17)
    target_link_libraries(${name} PRIVATE
        modbusScope::modbusScope
        Qt5::Core
        stdc++
    )
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

add_modbus_client(modbus_client_smoke
                  ${InternalrootDir}/tests/smoke_client.cpp)
add_modbus_client(modbus_client_stress
                  ${InternalrootDir}/tests/stress_client.cpp)
add_modbus_client(modbus_client_recovery
                  ${InternalrootDir}/tests/recovery_client.cpp)

# P1 fault-injection probes. The integration modes need a server; the
# no-QCoreApplication mode is standalone and is wired into CTest.
add_modbus_client(modbus_client_p1_regression
                  ${InternalrootDir}/tests/p1_regression.cpp)

# P2 probes: API/error mapping, write echo, Qt-free codec, and a compile-time
# encapsulation/Rule-of-Five probe. The last two deliberately do not link Qt.
add_modbus_client(modbus_client_p2_regression
                  ${InternalrootDir}/tests/p2_regression.cpp)

add_executable(modbus_codec_test ${InternalrootDir}/tests/codec_test.cpp)
target_compile_features(modbus_codec_test PRIVATE cxx_std_17)
target_link_libraries(modbus_codec_test PRIVATE modbusScope::modbusScope)
set_target_properties(modbus_codec_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(modbus_encapsulation_probe
               ${InternalrootDir}/tests/compile/encapsulation_probe.cpp)
target_compile_features(modbus_encapsulation_probe PRIVATE cxx_std_17)
target_link_libraries(modbus_encapsulation_probe PRIVATE
    modbusScope::modbusScope)
set_target_properties(modbus_encapsulation_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME p1_no_qcoreapp
         COMMAND modbus_client_p1_regression no_qcoreapp)
set_tests_properties(p1_no_qcoreapp PROPERTIES TIMEOUT 10)

add_test(NAME p2_error_mapping
         COMMAND modbus_client_p2_regression error_mapping)
set_tests_properties(p2_error_mapping PROPERTIES TIMEOUT 10)

add_test(NAME p2_codec COMMAND modbus_codec_test)
add_test(NAME p2_encapsulation COMMAND modbus_encapsulation_probe)

add_test(NAME p2_write_echo
         COMMAND bash ${InternalrootDir}/tests/run_p2_regression.sh
                 ${CMAKE_BINARY_DIR})
set_tests_properties(p2_write_echo PROPERTIES TIMEOUT 30)

# Install/export support: consumers use
#   find_package(modbusScope 0.1 CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE modbusScope::modbusScope)
install(TARGETS modbusScope
    EXPORT modbusScopeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY
    ${InternalrootDir}/include/modbusscope
    ${InternalrootDir}/include/Rust_error_deal
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
install(EXPORT modbusScopeTargets
    FILE modbusScopeTargets.cmake
    NAMESPACE modbusScope::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/modbusScope
)
configure_package_config_file(
    ${InternalrootDir}/cmake/modbusScopeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/modbusScopeConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/modbusScope
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/modbusScopeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/modbusScopeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/modbusScopeConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/modbusScope
)
