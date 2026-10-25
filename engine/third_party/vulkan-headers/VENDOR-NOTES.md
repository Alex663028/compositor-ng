# Vulkan Headers（vendored，OPT-10）

- 来源：DevEco NDK sysroot 自带的 Khronos 官方头（`sysroot/usr/include/vulkan/`
  与 `vk_video/`），本地拷贝（GitHub 直连在本机不可用）。许可证 Apache-2.0
  （Khronos Group），与 docs/THIRD-PARTY.md 的登记一致。
- **用途只有一个**：`tools/vulkan_decl_check.cpp` 的编译期交叉检查——vulkan_
  backend.cpp 的手写声明子集（37 结构体 + 52 PFN + 4 常量）与官方头同名共存，
  定义漂移即编译错误。**运行时代码不 include 本目录**（引擎按「自带最小声明」
  口径构建，不引入 SDK 依赖）。
- 检查命令（无设备依赖，~10s）：
  `python -m ziglang c++ -c tools/vulkan_decl_check.cpp -Iengine -Iengine/abi/include -Iengine/abi/src -Iengine/third_party/vulkan-headers/include -std=c++17 -o build/decl_check.o`
- 手写声明被修改时必须跑一次；未来若把运行时声明整体替换为官方头/volk，
  本检查随之退役。
