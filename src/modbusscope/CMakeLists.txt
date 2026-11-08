
#指定所需的最低 Cmkae 版本
cmake_minimum_required(VERSION 3.15)
project(modbusScope C CXX)

message("modbusScope CMake DIR: " ${CMAKE_CURRENT_SOURCE_DIR})

#获取特定文件夹下的.c  .cpp 源文件
set(
    SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

#设置源头文件目录
#创建动态库/静态库文件名，将源文件添加到动态库/静态库文件里
set(appleName "modbusScope")
add_library(${appleName} STATIC ${SOURCES})
target_compile_features(${appleName} PUBLIC cxx_std_17)
target_compile_options(modbusScope PRIVATE -Wswitch-enum -Werror=switch-enum)

#将源头文件的路径传递给建立的动态库/静态库文件里
target_include_directories(${appleName} PRIVATE 
${CMAKE_SOURCE_DIR}/include
${CMAKE_SOURCE_DIR}
)

#对本地动态库/静态库文件链接进应用程序里
target_link_libraries(${appleName}
PRIVATE
third_party_deps
)

#为该库创建一个别名
add_library(modbusScope::modbusScope ALIAS ${appleName})

# 显式检查目标是否存在
if(NOT TARGET modbusScope::modbusScope)
    message(STATUS "Dependency target 'modbusScope' not found")
else()
    message(STATUS "Dependency target 'modbusScope' found")
endif()
