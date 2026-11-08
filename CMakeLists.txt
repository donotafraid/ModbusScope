#指定所需的最低 CMake 版本
cmake_minimum_required(VERSION 3.15)

#设置项目名和支持语言,该部分应该在上述设置之后。不然会造成 不断更新设置的死循环
project(App LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# Sanitizer 选项（ASan+UBSan / LSan / TSan 互斥）
# ============================================================
option(ENABLE_ASAN "Enable AddressSanitizer + UBSan" OFF)
option(ENABLE_LSAN "Enable AddressSanitizer + LSan (leak only)" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# 三者互斥
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
    message(STATUS "AddressSanitizer + LSan: ENABLED")
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    endif()
    add_compile_options(-fsanitize=address,leak -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,leak)
endif()

if(ENABLE_ASAN)
    message("=========================================")
    message(STATUS "AddressSanitizer + UBSan: ENABLED")
    message(STATUS "CMAKE_BINARY_DIR = ${CMAKE_BINARY_DIR}")

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
        message(STATUS "CMAKE_BUILD_TYPE set to: ${CMAKE_BUILD_TYPE}")
    else()
        message(STATUS "CMAKE_BUILD_TYPE is: ${CMAKE_BUILD_TYPE}")
    endif()

    add_compile_options(-fsanitize=address,undefined)
    add_compile_options(-fno-omit-frame-pointer)
    add_compile_options(-g)
    add_compile_options(-fno-optimize-sibling-calls)

    add_link_options(-fsanitize=address,undefined)

    # ★ 关键修复：-fno-sanitize=function 是 Clang 专属，GCC 不认
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fno-sanitize=function)
        add_link_options(-fno-sanitize=function)
    endif()

    set(ASAN_LOG_DIR "${CMAKE_BINARY_DIR}/asan_logs")
    file(MAKE_DIRECTORY ${ASAN_LOG_DIR})
    message(STATUS "ASAN logs directory: ${ASAN_LOG_DIR}")
    message(STATUS "To use ASAN at runtime, set:")
    message(STATUS "  export ASAN_OPTIONS='log_path=${ASAN_LOG_DIR}/asan_report:log_exe_name=1:log_pid=1:detect_leaks=1:leak_check_at_exit=1'")

    message("=========================================")

elseif(ENABLE_TSAN)
    message("=========================================")
    message(STATUS "ThreadSanitizer: ENABLED")
    message(STATUS "CMAKE_BINARY_DIR = ${CMAKE_BINARY_DIR}")

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
        message(STATUS "CMAKE_BUILD_TYPE set to: ${CMAKE_BUILD_TYPE}")
    else()
        message(STATUS "CMAKE_BUILD_TYPE is: ${CMAKE_BUILD_TYPE}")
    endif()

    # TSan 推荐 -O1 或 -O2，配合 -g
    add_compile_options(-fsanitize=thread)
    add_compile_options(-fno-omit-frame-pointer)
    add_compile_options(-g)
    add_compile_options(-O1)   # TSan 推荐，避免 -O0 的漏报

    add_link_options(-fsanitize=thread)

    message(STATUS "To use TSan at runtime, set:")
    message(STATUS "  export TSAN_OPTIONS='halt_on_error=0:second_deadlock_stack=1'")

    message("=========================================")

else()
    message(STATUS "Sanitizers: DISABLED")
endif()

# ============================================================
# 编译器专属选项：Clang 与 GCC 分派
# ============================================================
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(
        -fstandalone-debug           # 更好的调试信息（Clang 专属）
        -Wno-unused-command-line-argument
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC 没有 -Wthread-safety / -fstandalone-debug；
    # -Wno-unused-command-line-argument 也不是 GCC 的选项，这里不加。
    # 如需 GCC 下的额外警告，可在此追加，例如：
    # add_compile_options(-Wno-maybe-uninitialized)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    # MSVC 走 /W4 之类的选项；当前项目未声明 MSVC 支持，保留占位
endif()

# 修改构建类型配置（仅 Clang 的 Debug 附加项）
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(
            -glldb                # LLDB 兼容格式（Clang 专属）
            -fno-limit-debug-info # Clang 专属
        )
    endif()
endif()

# 确保生成 compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
# 依赖查找
# ============================================================
find_package(Qt5 COMPONENTS Widgets Network SerialBus REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 兼容别名：CMake 标准 target 是 OpenSSL::SSL / OpenSSL::Crypto，
# 但项目历史代码用 openssl::openssl，这里补一个 alias。
if(NOT TARGET openssl::openssl)
    add_library(openssl::openssl INTERFACE IMPORTED)
    set_target_properties(openssl::openssl PROPERTIES
        INTERFACE_LINK_LIBRARIES "OpenSSL::SSL;OpenSSL::Crypto")
endif()

# ============================================================
# 第三方依赖聚合 target
# ============================================================
add_library(third_party_deps INTERFACE)
target_link_libraries(third_party_deps INTERFACE
    Qt5::Widgets
    Qt5::Network
    Qt5::SerialBus
)

# Debug 下强制 -O0（GCC / Clang 通用）。
# TSan 例外：ENABLE_TSAN 已经显式追加 -O1；这里再追加 -O0 会让编译器
# 最终采用 -O0，和上面的“TSan 推荐 -O1”以及 CI 的意图相反。
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ENABLE_TSAN)
    add_compile_options(-O0)
endif()

# ============================================================
# 子目录
# ============================================================
set(InternalrootDir "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(
    ${InternalrootDir}/src/modbusscope/
    modbusscope-build
)

# ============================================================
# 可执行文件
# ============================================================
set(SRC_DIR "${InternalrootDir}/src")

add_executable(file_platform ${SRC_DIR}/main.cpp)

target_include_directories(file_platform PRIVATE
    "${InternalrootDir}/include"
    "${InternalrootDir}"
)

target_link_libraries(file_platform PRIVATE
    third_party_deps
)

set_target_properties(
    file_platform PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${InternalrootDir}/bin"
)