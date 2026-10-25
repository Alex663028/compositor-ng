// AI生成
//
// shells/ohos/entry/src/main/ets/typings/libcompositor/index.d.ts
// NAPI 桥（libcompositor.so）的 ArkTS 类型声明，与 shells/ohos/src/napi_init.cpp 一一对应。

/**
 * 引擎启动。
 * @param backend 期望后端：0=AUTO 1=Metal 2=D3D12 3=Vulkan 4=CPU（ABI 冻结表）
 * @returns 0 表示成功（comp_result 约定：0=COMP_OK）
 */
export const startup: (backend?: number) => number;

/** 释放引擎与其持有资源。@returns 0 表示成功 */
export const shutdown: () => number;

/** 查询实际生效的后端名（"Metal"/"D3D12"/"Vulkan"/"CPU"/"Auto"）。 */
export const backendName: () => string;

/**
 * 离屏合成：新建临时文档并展平导出。
 * @param width  画布宽（1..30000）
 * @param height 画布高（1..30000）
 * @returns RGBA8 像素的 ArrayBuffer；参数非法或引擎未启动时返回 null
 */
export const flatten: (width: number, height: number) => ArrayBuffer | null;

/** 呈现面几何："宽x高|ready|absent"，例如 "1080x2340|ready"。 */
export const surfaceInfo: () => string;

/**
 * 上屏：把合成结果提交给 XComponent 表面（COMP_SURFACE_HARMONY_WINDOW）。
 * @param width  画布宽（1..30000）
 * @param height 画布高（1..30000）
 * @returns >= 0 为 comp_result（0=OK；1=参数非法；4=UNSUPPORTED 当前无后端支持上屏；6=后端错误）；
 *          -1 表示前置缺失（引擎未启动或表面未就绪）
 */
export const presentSurface: (width: number, height: number) => number;
