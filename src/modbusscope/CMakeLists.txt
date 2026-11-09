# Build the reusable client library. The empty main.cpp that used to be listed
# here made the target build successfully while client.cpp was never compiled.
cmake_minimum_required(VERSION 3.15)
project(modbusScope CXX)

set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/error.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/realSdk.cpp
)

set(appleName "modbusScope")
add_library(${appleName} STATIC ${SOURCES})
add_library(modbusScope::modbusScope ALIAS ${appleName})

target_compile_features(${appleName} PUBLIC cxx_std_17)
target_compile_options(${appleName} PRIVATE -Wswitch-enum -Werror=switch-enum)

target_include_directories(${appleName}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}
)

# QtSerialBus is only needed by the implementation. The public headers do not
# include Qt, so consumers see no Qt types through modbusscope/modbusscope.h.
target_link_libraries(${appleName}
    PUBLIC Qt5::Core
    PRIVATE Qt5::Network Qt5::SerialBus
)

set_target_properties(${appleName} PROPERTIES POSITION_INDEPENDENT_CODE ON)
