# bench/CMakeLists.txt
#
# Standalone performance/endurance executables. They are built with the default
# target but are intentionally NOT registered with ctest: a benchmark result is
# an observation, not a pass/fail gate.

set(BENCH_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_latency.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_recovery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_rss.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_server.cpp
)

foreach(bench_src ${BENCH_SOURCES})
    get_filename_component(bench_name ${bench_src} NAME_WE)
    add_executable(${bench_name} ${bench_src})

    target_include_directories(${bench_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_compile_features(${bench_name} PRIVATE cxx_std_17)

    target_link_libraries(${bench_name} PRIVATE
        third_party_deps
        OPCUAPacking::OPCUAPacking
    )

    set_target_properties(${bench_name} PROPERTIES
        BUILD_RPATH "${OPEN62541_BUILD}/bin"
        INSTALL_RPATH "${OPEN62541_BUILD}/bin"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench-build"
    )
endforeach()
