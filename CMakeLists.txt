# CMake minimum and project languages. Keep the standard pinned: the public
# headers and the Qt 5.15 implementation both rely on C++17.
cmake_minimum_required(VERSION 3.15)
project(modbusScope VERSION 0.2.0 LANGUAGES CXX C)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "Enable AddressSanitizer + UBSan" OFF)
option(ENABLE_LSAN "Enable AddressSanitizer + LSan (leak only)" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(MODBUSSCOPE_BUILD_GUI "Build the Qt Widgets Scope demo" ON)

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

if(MODBUSSCOPE_BUILD_GUI)
    find_package(Qt5 5.15.3 EXACT COMPONENTS Widgets QUIET)
    if(NOT Qt5Widgets_FOUND)
        message(WARNING
            "Qt5Widgets was not found; the Scope demo is disabled. "
            "Install qtbase5-dev or configure with -DMODBUSSCOPE_BUILD_GUI=OFF.")
        set(MODBUSSCOPE_BUILD_GUI OFF)
    endif()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(InternalrootDir "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(
    ${InternalrootDir}/src/modbusscope/
    modbusscope-build
)

if(MODBUSSCOPE_BUILD_GUI)
    add_executable(modbus_scope_demo
                   ${InternalrootDir}/src/modbusscope/scopeDemo.cpp)
    target_compile_features(modbus_scope_demo PRIVATE cxx_std_17)
    target_link_libraries(modbus_scope_demo PRIVATE
        modbusScope::modbusScopeGui
        Qt5::Widgets
    )
    set_target_properties(modbus_scope_demo PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

add_subdirectory(
    ${InternalrootDir}/tests/
    tests-build
)

# Install/export support: consumers use
#   find_package(modbusScope 0.1 CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE modbusScope::modbusScope)
set(_install_targets modbusScope)
if(MODBUSSCOPE_BUILD_GUI)
    list(APPEND _install_targets modbusScopeGui)
endif()
install(TARGETS ${_install_targets}
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
