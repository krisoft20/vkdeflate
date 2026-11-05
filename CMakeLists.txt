cmake_minimum_required(VERSION 3.20)
project(vkdeflate CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(VULKAN_INCLUDE "" CACHE PATH "folder that holds vulkan/vulkan.h")
if(NOT VULKAN_INCLUDE AND DEFINED ENV{VULKAN_SDK})
    set(VULKAN_INCLUDE $ENV{VULKAN_SDK}/Include)
endif()
if(NOT EXISTS ${VULKAN_INCLUDE}/vulkan/vulkan.h)
    set(HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/third_party_build/Vulkan-Headers)
    if(NOT EXISTS ${HEADERS}/include/vulkan/vulkan.h)
        find_package(Git REQUIRED)
        message(STATUS "no vulkan headers: cloning KhronosGroup/Vulkan-Headers v1.4.357 into third_party_build")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --depth 1 --branch v1.4.357 https://github.com/KhronosGroup/Vulkan-Headers ${HEADERS}
            RESULT_VARIABLE HEADERS_FAILED)
        if(HEADERS_FAILED)
            message(FATAL_ERROR "could not fetch the vulkan headers: pass -DVULKAN_INCLUDE=path")
        endif()
    endif()
    set(VULKAN_INCLUDE ${HEADERS}/include)
endif()

set(SPV ${CMAKE_CURRENT_BINARY_DIR}/tile_spv.inc)
set(PREBUILT ${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/tile_spv.inc)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/tile.comp TILE_SRC)
string(REPLACE "\r" "" TILE_SRC "${TILE_SRC}")
string(SHA256 TILE_HASH "${TILE_SRC}")
set(PREBUILT_FRESH -1)
if(EXISTS ${PREBUILT})
    file(STRINGS ${PREBUILT} PREBUILT_HEAD LIMIT_COUNT 1)
    string(FIND "${PREBUILT_HEAD}" "${TILE_HASH}" PREBUILT_FRESH)
endif()

find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin)
if(GLSLC)
    if(PREBUILT_FRESH EQUAL -1)
        message(STATUS "prebuilt/tile_spv.inc is older than src/tile.comp: build the single_header target before a commit")
    endif()
    add_custom_command(
        OUTPUT ${SPV}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c ${CMAKE_CURRENT_SOURCE_DIR}/src/tile.comp -o ${SPV}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/tile.comp
        COMMENT "glslc tile.comp"
        VERBATIM)
else()
    if(PREBUILT_FRESH EQUAL -1)
        message(FATAL_ERROR "src/tile.comp differs from the prebuilt shader and glslc was not found: install the vulkan sdk or pass -DGLSLC=path")
    endif()
    message(STATUS "glslc not found: using prebuilt/tile_spv.inc")
    add_custom_command(
        OUTPUT ${SPV}
        COMMAND ${CMAKE_COMMAND} -E copy ${PREBUILT} ${SPV}
        DEPENDS ${PREBUILT}
        COMMENT "prebuilt tile_spv.inc"
        VERBATIM)
endif()

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
if(Python3_FOUND AND GLSLC)
    add_custom_target(single_header
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/amalgamate.py --glslc ${GLSLC}
        COMMENT "single_include/vkdeflate.h, prebuilt/tile_spv.inc"
        VERBATIM)
endif()

foreach(t vkdeflate gdz attach_test bench single_test)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endif()
endforeach()
