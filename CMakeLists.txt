# AI生成
cmake_minimum_required(VERSION 3.16)
project(compositor_ng LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# compositor-ng 引擎核（M1）
#
#   目标：产出静态库 libcompositor_engine + C ABI 头 + 单元测试。
#   ABI 头位于 engine/abi/include/compositor，是 Windows 壳 / 鸿蒙壳唯一的集成边界。
#
#   与 scripts/build.py 必须保持一致：编译的源集合、语言标准与关键开关不允许分叉，
#   否则「CMake 能编过」与「build.py 能编过」将不再等价。
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# 像素内核用到 M_PI 等 GNU/BSD 扩展宏（严格 ANSI 模式下不可见）→ 开启扩展，得到 -std=gnu11。
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# 编译开关
#
#   -fblocks 必需：engine/imaging 下的 10 组像素内核沿用上游 clang block 语法（^）
#   作为区间回调。Apple 平台的 BlocksRuntime 由 libSystem 提供，非 Apple 平台由
#   engine/core/comp_blocks_rt.c 提供最小实现（链接所需符号）。
#
#   MSVC 注意：cl.exe 不支持 block 语法。Windows 侧请使用 VS 自带的 clang-cl
#   （本文件在 Clang 驱动下自动追加 /clang:-fblocks）。此限制已在 README 记录。
# ---------------------------------------------------------------------------

if(MSVC)
  # /utf-8 必须开启：源码含 UTF-8 中文注释，否则 MSVC 会按本地代码页解析而报错。
  add_compile_options(/utf-8 /W4)
  add_compile_definitions(_USE_MATH_DEFINES)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(/clang:-fblocks)
  else()
    message(WARNING
      "cl.exe 不支持像素内核使用的 clang block 语法；请改用 clang-cl（VS 自带）构建，"
      "或直接使用 scripts/build.py（默认工具链为 zig）。详见 README 第 3 节。")
  endif()
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -fblocks)
endif()

set(COMP_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/engine")

file(GLOB_RECURSE COMP_ENGINE_CXX CONFIGURE_DEPENDS "${COMP_ENGINE_ROOT}/*.cpp")
# 全部 .c 都参与：imaging 下 10 组像素内核 + core/comp_blocks_rt.c（BlocksRuntime 桩）。
file(GLOB_RECURSE COMP_ENGINE_C CONFIGURE_DEPENDS "${COMP_ENGINE_ROOT}/*.c")
# Apple 平台的 BlocksRuntime 由 libSystem 提供，排除自带的桩以免符号冲突。
if(APPLE)
  list(FILTER COMP_ENGINE_C EXCLUDE REGEX "comp_blocks_rt\\.c$")
endif()

add_library(compositor_engine STATIC ${COMP_ENGINE_CXX} ${COMP_ENGINE_C})

target_include_directories(compositor_engine
  PUBLIC
    "${COMP_ENGINE_ROOT}"                 # 内部头入口（core/ domain/ compositing/ ...）
    "${COMP_ENGINE_ROOT}/abi/include"     # C ABI（compositor/comp_*.h）
  PRIVATE
    "${COMP_ENGINE_ROOT}/abi/src")

find_package(Threads REQUIRED)
target_link_libraries(compositor_engine PUBLIC Threads::Threads)

# 可选：开启后 ABI 变为动态库（Windows 下可用于壳/引擎解耦部署）。
option(COMP_SHARED "Build the C ABI as a shared library" OFF)
if(COMP_SHARED)
  set_target_properties(compositor_engine PROPERTIES
    FRAMEWORK FALSE
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------

enable_testing()

add_executable(compositor_tests
  tests/test_main.cpp
  tests/test_core.cpp
  tests/test_domain.cpp
  tests/test_compositing.cpp
  tests/test_imaging.cpp
  tests/test_parallel.cpp
  tests/test_filter_graph.cpp
  tests/test_format.cpp
  tests/test_abi.cpp
  tests/test_resample.cpp
  tests/test_rhi.cpp
  tests/test_codec.cpp
  tests/test_adjustment.cpp
  tests/test_shell_contract.cpp)

target_link_libraries(compositor_tests PRIVATE compositor_engine)

add_test(NAME compositor_tests COMMAND compositor_tests)
