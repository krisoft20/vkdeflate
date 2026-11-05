cmake_minimum_required(VERSION 3.20)
project(vkdeflate CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin)
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found: install the vulkan sdk or pass -DGLSLC=path")
endif()

set(VULKAN_INCLUDE "" CACHE PATH "folder that holds vulkan/vulkan.h")
if(NOT VULKAN_INCLUDE AND DEFINED ENV{VULKAN_SDK})
    set(VULKAN_INCLUDE $ENV{VULKAN_SDK}/Include)
endif()
if(NOT EXISTS ${VULKAN_INCLUDE}/vulkan/vulkan.h)
    message(FATAL_ERROR "vulkan headers not found in '${VULKAN_INCLUDE}': pass -DVULKAN_INCLUDE=path")
endif()

set(SPV ${CMAKE_CURRENT_BINARY_DIR}/tile_spv.inc)
add_custom_command(
    OUTPUT ${SPV}
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c ${CMAKE_CURRENT_SOURCE_DIR}/src/tile.comp -o ${SPV}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/tile.comp
    COMMENT "glslc tile.comp"
    VERBATIM)

add_library(vkdeflate STATIC src/gdeflate.cpp src/vkdeflate.cpp ${SPV})
target_include_directories(vkdeflate PUBLIC src ${VULKAN_INCLUDE} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_executable(gdz tools/gdz.cpp)
target_link_libraries(gdz PRIVATE vkdeflate)

add_executable(attach_test tools/attach_test.cpp)
target_link_libraries(attach_test PRIVATE vkdeflate)

add_executable(bench tools/bench.cpp)
target_link_libraries(bench PRIVATE vkdeflate)

add_executable(single_test tools/single_test.cpp)
target_include_directories(single_test PRIVATE ${VULKAN_INCLUDE})
set_target_properties(single_test PROPERTIES CXX_STANDARD 17)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(single_header
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/amalgamate.py --glslc ${GLSLC}
        COMMENT "single_include/vkdeflate.h"
        VERBATIM)
endif()

foreach(t vkdeflate gdz attach_test bench single_test)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endif()
endforeach()
