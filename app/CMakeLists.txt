# Sims 3 Switch port. Configure with tools/switch/cmake/switch-devkitA64.cmake and REXSDK_DIR set.
cmake_minimum_required(VERSION 3.25)
project(sims3 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT REXGLUE_PLATFORM_SWITCH)
  message(FATAL_ERROR "This project only builds for Switch")
endif()

# GCC's early register allocator miscompiles the recompiled vector code on console (katamari-nx's
# CMakeLists explains it), and out-of-line atomics are slower on the A57, which has no LSE.
add_compile_options(-mearly-ra=none -mno-outline-atomics)
add_link_options(-mearly-ra=none)

# -O1 was the bring-up setting (a full build of 280 MB of generated C++ stays practical). 11-15: -O2 for speed, with
# -fno-strict-aliasing because generated code reads guest memory through casts.
set(SIMS3_RECOMP_OPT "-O2;-fno-strict-aliasing" CACHE STRING "Optimisation level for generated code")

set(REXSDK_DIR "" CACHE PATH "ReXGlue SDK source tree")
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)

include(generated/default/sources.cmake)
set(SIMS3_DEFAULT_SOURCES ${GENERATED_SOURCES})
include(generated/sims3/sources.cmake)
set(SIMS3_DLL_SOURCES ${GENERATED_SOURCES})

function(sims3_recomp_library name dir)
  add_library(${name} OBJECT ${ARGN})
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${dir}")
  target_link_libraries(${name} PRIVATE rex::runtime)
  target_compile_options(${name} PRIVATE ${SIMS3_RECOMP_OPT} -g0 -w)
  target_precompile_headers(${name} PRIVATE "${dir}/sims3_pch.h")
endfunction()

sims3_recomp_library(sims3_default_code "${CMAKE_CURRENT_SOURCE_DIR}/generated/default" ${SIMS3_DEFAULT_SOURCES})
sims3_recomp_library(sims3_dll_code "${CMAKE_CURRENT_SOURCE_DIR}/generated/sims3" ${SIMS3_DLL_SOURCES})

# Both modules generate the same names. They were separate DLLs on PC; here they share one binary.
target_compile_definitions(sims3_dll_code PRIVATE
  PPCImageConfig=Sims3Dll_PPCImageConfig
  PPCFuncMappings=Sims3Dll_PPCFuncMappings
  ReXModule_Register=Sims3Dll_Register
  ReXModule_GetImageInfo=Sims3Dll_GetImageInfo)

add_executable(sims3
  src/main.cpp
  src/deko3d_graphics.cpp
  $<TARGET_OBJECTS:sims3_default_code>
  $<TARGET_OBJECTS:sims3_dll_code>)
target_include_directories(sims3 PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/generated/default"
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

rexglue_switch_configure_target(sims3)
target_link_libraries(sims3 PRIVATE deko3d)
rexglue_switch_add_nro(sims3 NAME "The Sims 3" AUTHOR "Zumoly (Sims3-NX)" VERSION "0.1.0" ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")
