// AI生成
//
// shells/ohos/src/CMakeLists.txt — DevEco 原生模块构建脚本（OpenHarmony CMake 版）
//
// 与 scripts/build_ohos_shell.sh 等价，供 DevEco Studio 直接构建：
//   - 目标：libcompositor.so（NAPI 桥 + 引擎核静态编译）
//   - 引擎源集合由 COMPOSITOR_NG_ROOT 下的 engine/** 递归收集
//   - 链接 OHOS NDK 提供的 libace_napi.z / libace_ndk.z / libnative_window

cmake_minimum_required(VERSION 3.5.0)
project(compositor_ohos)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 仓库根（compositor-ng/）；本文件位于 shells/ohos/src/，故上溯三级。
if(NOT DEFINED COMPOSITOR_NG_ROOT)
  set(COMPOSITOR_NG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
endif()
get_filename_component(COMPOSITOR_NG_ROOT "${COMPOSITOR_NG_ROOT}" ABSOLUTE)

set(ENGINE_ROOT "${COMPOSITOR_NG_ROOT}/engine")

# 像素内核沿用上游 clang block 语法，须开启 -fblocks（OHOS 的 clang 支持）。
add_compile_options(-fblocks -fPIC -O2 -Wall -Wextra -Wno-unused-parameter)
add_compile_definitions(_USE_MATH_DEFINES)

file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
  "${ENGINE_ROOT}/*.c"
  "${ENGINE_ROOT}/*.cpp")

add_library(compositor SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/napi_init.cpp"
  ${ENGINE_SOURCES})

target_include_directories(compositor PRIVATE
  "${ENGINE_ROOT}"
  "${ENGINE_ROOT}/abi/include"
  "${ENGINE_ROOT}/abi/src")

# OHOS NDK 系统库：NAPI 运行时 / XComponent 回调 / 呈现面载体。
target_link_libraries(compositor PUBLIC
  libace_napi.z.so
  libace_ndk.z.so
  libnative_window.so)
