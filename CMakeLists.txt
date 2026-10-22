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

# FreeType（P5-0）：裁剪版只编译 14 个聚合翻译单元。glob 必须排除
# third_party/freetype —— 模块目录里其余 .c 已被聚合文件 #include（如 truetype.c
# 包含 ttobjs.c），再单独编译就是重复符号。清单与 scripts/build.py 的
# FREETYPE_SOURCES、build_ohos.sh 是同一份，改一处必须同步三处。
# 详见 engine/third_party/freetype/VENDOR-NOTES.md。
set(COMP_FREETYPE_ROOT "${COMP_ENGINE_ROOT}/third_party/freetype")
set(COMP_FREETYPE_SOURCES "")
if(IS_DIRECTORY "${COMP_FREETYPE_ROOT}")
  list(FILTER COMP_ENGINE_C EXCLUDE REGEX "third_party/freetype/")
  foreach(_ft_src
      src/base/ftsystem.c src/base/ftinit.c src/base/ftdebug.c
      src/base/ftbase.c src/base/ftbbox.c src/base/ftglyph.c src/base/ftbitmap.c
      src/base/ftmm.c
      src/sfnt/sfnt.c src/truetype/truetype.c
      src/cff/cff.c src/psaux/psaux.c src/pshinter/pshinter.c
      src/psnames/psnames.c src/smooth/smooth.c)
    list(APPEND COMP_FREETYPE_SOURCES "${COMP_FREETYPE_ROOT}/${_ft_src}")
  endforeach()
endif()

add_library(compositor_engine STATIC ${COMP_ENGINE_CXX} ${COMP_ENGINE_C} ${COMP_FREETYPE_SOURCES})

target_include_directories(compositor_engine
  PUBLIC
    "${COMP_ENGINE_ROOT}"                 # 内部头入口（core/ domain/ compositing/ ...）
    "${COMP_ENGINE_ROOT}/abi/include"     # C ABI（compositor/comp_*.h）
  PRIVATE
    "${COMP_ENGINE_ROOT}/abi/src")

# 可选的 vendor 源码（engine/third_party：zlib / libpng / stb）。目录存在才加路径，
# 与 scripts/build.py 的 INCLUDE_DIRS 同一条规则——libpng 的 pngpriv.h include
# "zlib.h"，少了这一条 CMake 路径会在 pngstruct.h 处编译失败，而 build.py 能过。
# 这正是「两条构建路径必须等价」要防的那种分叉。
set(COMP_VENDOR_INCLUDE_DIRS "")
foreach(_vendor zlib libpng stb)
  if(IS_DIRECTORY "${COMP_ENGINE_ROOT}/third_party/${_vendor}")
    list(APPEND COMP_VENDOR_INCLUDE_DIRS "${COMP_ENGINE_ROOT}/third_party/${_vendor}")
  endif()
endforeach()
# FreeType 的包含根是 <root>/include（上游 INSTALL.ANY 口径），不是 <root>。
if(IS_DIRECTORY "${COMP_FREETYPE_ROOT}/include")
  list(APPEND COMP_VENDOR_INCLUDE_DIRS "${COMP_FREETYPE_ROOT}/include")
endif()
target_include_directories(compositor_engine PRIVATE ${COMP_VENDOR_INCLUDE_DIRS})

# 第三方源码不参与本仓的 -Wall -Wextra 基线：告警只应来自我们自己的代码，
# 否则「零新增告警」会被上游的写法淹没。build.py 的 C 编译本来不开 -Wall。
if(COMP_FREETYPE_SOURCES)
  if(MSVC)
    set_source_files_properties(${COMP_FREETYPE_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")
  else()
    set_source_files_properties(${COMP_FREETYPE_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
  endif()
  # 上游编库必须的宏：fterrors.h 在未定义它时会 #undef FT_ERR_PREFIX。
  set_source_files_properties(${COMP_FREETYPE_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "FT2_BUILD_LIBRARY")
endif()

find_package(Threads REQUIRED)
target_link_libraries(compositor_engine PUBLIC Threads::Threads)

# 可选：开启后 ABI 变为动态库（Windows 下可用于壳/引擎解耦部署）。
option(COMP_SHARED "Build the C ABI as a shared library" OFF)
if(COMP_SHARED)
  set_target_properties(compositor_engine PROPERTIES
    FRAMEWORK FALSE
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# 可选：编入 D3D12 后端（与 scripts/build.py --gpu 等价）。
# D3D12/编译器入口全部运行期从 d3d12.dll / d3dcompiler_47.dll 解析，
# 因此这里只加编译定义，不引入 d3d12.lib / dxgi.lib 链接依赖。
option(COMP_ENABLE_D3D12 "Compile the D3D12 backend (Windows)" OFF)
if(COMP_ENABLE_D3D12)
  # PUBLIC：单测经 target_link_libraries 继承同一口径（它们要探测同一后端）。
  target_compile_definitions(compositor_engine PUBLIC COMP_HAS_D3D12)
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------

enable_testing()

# 测试源用 glob 而非硬编码列表：历史上硬编码表漏掉 test_layer_merge.cpp 与
# test_layer_mutation.cpp，CMake 路径静默少跑两整个文件（框架靠静态注册，
# 漏链的文件连编译都不会发生）。新增测试文件从此自动进入构建。
file(GLOB COMP_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

add_executable(compositor_tests ${COMP_TEST_SOURCES})

# 测试源必须看到和引擎完全相同的包含路径：test_exr.cpp 用 __has_include("zlib.h")
# 决定是否编译 ZIP/ZIPS 三组用例，test_camera_raw*.cpp 也依赖 abi/src 的内部头。
# 少了 vendor 路径，CMake 路径会静默少注册 3 个用例（341 vs 344），而 build.py 全绿——
# 正是「两条构建路径必须等价」要防的分叉。
target_include_directories(compositor_tests PRIVATE
  "${COMP_ENGINE_ROOT}"
  "${COMP_ENGINE_ROOT}/abi/include"
  "${COMP_ENGINE_ROOT}/abi/src"
  ${COMP_VENDOR_INCLUDE_DIRS})

target_link_libraries(compositor_tests PRIVATE compositor_engine)

# S16：test_rhi.cpp 的窗口直呈用例要真实 HWND（CreateWindowExW / DestroyWindow）。
# 与 scripts/build.py 的 -luser32 同一口径，仅 Windows 目标链接；引擎库本身仍不依赖
# user32（D3D12 交换链只和 DXGI 打交道，不等同于壳层）。
if(WIN32)
  target_link_libraries(compositor_tests PRIVATE user32)
endif()

# 工作目录必须是仓库根：test_camera_raw.cpp 以相对路径 resources/camera_raw_tables.bin
# 读取随仓数据。scripts/build.py 的 run() 也以 ROOT 为 cwd 启动测试；若不指定，
# ctest 默认在 build-cmake/ 下运行，12 个 camera_raw 用例会因找不到资源而失败。
add_test(NAME compositor_tests COMMAND compositor_tests
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
