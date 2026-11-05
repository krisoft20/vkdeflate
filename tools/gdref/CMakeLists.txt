cmake_minimum_required(VERSION 3.20)
project(gdref C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(DS ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party_build/DirectStorage/GDeflate)
set(LD ${DS}/3rdparty/libdeflate)
if(NOT EXISTS ${LD}/libdeflate.h)
    message(FATAL_ERROR "clone the reference implementation first, see README.md (test vectors)")
endif()

add_library(ldef STATIC
    ${LD}/lib/adler32.c
    ${LD}/lib/crc32.c
    ${LD}/lib/deflate_compress.c
    ${LD}/lib/deflate_decompress.c
    ${LD}/lib/gdeflate_compress.c
    ${LD}/lib/gdeflate_decompress.c
    ${LD}/lib/gzip_compress.c
    ${LD}/lib/gzip_decompress.c
    ${LD}/lib/utils.c
    ${LD}/lib/zlib_compress.c
    ${LD}/lib/zlib_decompress.c
    ${LD}/lib/x86/cpu_features.c)
target_include_directories(ldef PUBLIC ${LD})
if(MSVC)
    target_compile_options(ldef PRIVATE /wd4244 /wd4127 /wd4267 /wd4100 /wd4245 /wd4456 /wd4018 /wd4146 /wd4310 /wd4996)
endif()

add_library(gdeflate_ref STATIC ${DS}/GDeflate/GDeflateCompress.cpp ${DS}/GDeflate/GDeflateDecompress.cpp)
target_include_directories(gdeflate_ref PUBLIC ${DS}/GDeflate)
target_link_libraries(gdeflate_ref PUBLIC ldef)

add_executable(gdref gdref.cpp)
target_link_libraries(gdref PRIVATE gdeflate_ref)
