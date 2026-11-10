# Build the reusable client library. The empty main.cpp that used to be listed
# here made the target build successfully while client.cpp was never compiled.
# The parent project sets InternalrootDir before add_subdirectory().
set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/error.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/realSdk.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/scope.cpp
)

set(appleName "modbusScope")
add_library(${appleName} STATIC ${SOURCES})
add_library(modbusScope::modbusScope ALIAS ${appleName})

target_compile_features(${appleName} PUBLIC cxx_std_17)
target_compile_options(${appleName} PRIVATE -Wswitch-enum -Werror=switch-enum)

target_include_directories(${appleName}
    PUBLIC
        $<BUILD_INTERFACE:${InternalrootDir}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${InternalrootDir}
)

# Qt is an implementation detail: no public header includes it, and the codec
# header is std-only. A static library still needs its Qt link dependencies on
# the consumer link line, so they stay PRIVATE (exported as LINK_ONLY).
target_link_libraries(${appleName}
    PRIVATE Qt5::Core Qt5::Network Qt5::SerialBus
)

set_target_properties(${appleName} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Optional Qt Widgets view. It is a separate target because its public header
# intentionally exposes QWidget; the core headers stay Qt-free.
if(MODBUSSCOPE_BUILD_GUI)
    add_library(modbusScopeGui STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/scopeWidget.cpp
        ${InternalrootDir}/include/modbusscope/scopeWidget.h
    )
    add_library(modbusScope::modbusScopeGui ALIAS modbusScopeGui)
    target_compile_features(modbusScopeGui PUBLIC cxx_std_17)
    target_compile_options(modbusScopeGui PRIVATE
        -Wswitch-enum -Werror=switch-enum)
    target_include_directories(modbusScopeGui
        PUBLIC
            $<BUILD_INTERFACE:${InternalrootDir}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${InternalrootDir}
    )
    target_link_libraries(modbusScopeGui
        PUBLIC modbusScope Qt5::Widgets
    )
    set_target_properties(modbusScopeGui PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
endif()
