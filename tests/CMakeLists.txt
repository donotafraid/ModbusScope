message("tests CMake DIR: " ${CMAKE_CURRENT_SOURCE_DIR})

# Integration clients. These need QtCore only for the application object and
# the report formatting; the wrapper library itself is linked through its alias.
function(add_modbus_client name source)
    add_executable(${name} ${source})
    target_compile_features(${name} PRIVATE cxx_std_17)
    target_link_libraries(${name} PRIVATE
        modbusScope::modbusScope
        Qt5::Core
    )
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

add_modbus_client(modbus_client_smoke
                  ${CMAKE_CURRENT_SOURCE_DIR}/smoke_client.cpp)
add_modbus_client(modbus_client_stress
                  ${CMAKE_CURRENT_SOURCE_DIR}/stress_client.cpp)
add_modbus_client(modbus_client_recovery
                  ${CMAKE_CURRENT_SOURCE_DIR}/recovery_client.cpp)

# Fault-injection probes. The integration modes need a server; the
# no-QCoreApplication mode is standalone and is wired into CTest.
add_modbus_client(modbus_client_p1_regression
                  ${CMAKE_CURRENT_SOURCE_DIR}/p1_regression.cpp)
add_modbus_client(modbus_client_p2_regression
                  ${CMAKE_CURRENT_SOURCE_DIR}/p2_regression.cpp)

add_executable(modbus_codec_test
               ${CMAKE_CURRENT_SOURCE_DIR}/codec_test.cpp)
target_compile_features(modbus_codec_test PRIVATE cxx_std_17)
target_link_libraries(modbus_codec_test PRIVATE modbusScope::modbusScope)
set_target_properties(modbus_codec_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(modbus_encapsulation_probe
               ${CMAKE_CURRENT_SOURCE_DIR}/compile/encapsulation_probe.cpp)
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
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_p2_regression.sh
                 ${CMAKE_BINARY_DIR})
set_tests_properties(p2_write_echo PROPERTIES TIMEOUT 30)

# --- gtest unit suite ------------------------------------------------------
# The Conan CMakeDeps generator installs GTestConfig.cmake and FindGTest.cmake
# under conan_debug_build/. Prefer that local generated package when present so
# a checkout that ran `conan install` finds GTest without extra flags.
if(EXISTS "${CMAKE_SOURCE_DIR}/conan_debug_build/FindGTest.cmake")
    list(PREPEND CMAKE_MODULE_PATH
         "${CMAKE_SOURCE_DIR}/conan_debug_build")
    list(PREPEND CMAKE_PREFIX_PATH
         "${CMAKE_SOURCE_DIR}/conan_debug_build")
endif()
find_package(GTest QUIET)

if(GTest_FOUND)
    include(GoogleTest)

    add_executable(modbus_unit_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/api_validation_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/client_state_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/codec_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/scope_model_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/scope_presentation_test.cpp
    )
    target_compile_features(modbus_unit_tests PRIVATE cxx_std_17)
    target_compile_definitions(modbus_unit_tests PRIVATE
        MODBUSSCOPE_ENABLE_TEST_HOOKS)
    target_include_directories(modbus_unit_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(modbus_unit_tests PRIVATE
        modbusScope::modbusScope
        GTest::gtest_main
    )
    set_target_properties(modbus_unit_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    gtest_discover_tests(modbus_unit_tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        DISCOVERY_MODE PRE_TEST
    )

    if(TARGET modbusScope::modbusScopeGui)
        add_executable(modbus_scope_widget_test
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/scope_widget_test.cpp
        )
        target_compile_features(modbus_scope_widget_test PRIVATE cxx_std_17)
        target_compile_definitions(modbus_scope_widget_test PRIVATE
            MODBUSSCOPE_ENABLE_TEST_HOOKS)
        target_include_directories(modbus_scope_widget_test PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
        target_link_libraries(modbus_scope_widget_test PRIVATE
            modbusScope::modbusScope
            modbusScope::modbusScopeGui
            Qt5::Widgets
            GTest::gtest
        )
        set_target_properties(modbus_scope_widget_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
        gtest_discover_tests(modbus_scope_widget_test
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            DISCOVERY_MODE PRE_TEST
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
else()
    message(WARNING
        "GTest was not found; the gtest unit suite is disabled. "
        "Run `conan install . -s build_type=Debug --build=missing` "
        "or install the system GTest package.")
endif()
