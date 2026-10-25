// AI生成
//
// shells/ohos/entry/src/main/ets/typings/libcompositor/index.d.ts
// NAPI 桥（libcompositor.so）的 ArkTS 类型声明，与 shells/ohos/src/napi_init.cpp 一一对应。

/** 引擎启动。@param backend 0=AUTO 1=Metal 2=D3D12 3=Vulkan 4=CPU。@returns 0 成功 */
export const startup: (backend?: number) => number;
/** 释放引擎与文档。@returns 0 */
export const shutdown: () => number;
/** 实际生效的后端名（"Vulkan"/"CPU"/…；未启动 "None"）。 */
export const backendName: () => string;

/**
 * 新建文档（旧文档自动关闭；引擎未启动时自动启动）。
 * @returns 0 成功；1 参数非法；负值 = 前置缺失
 */
export const newDocument: (width: number, height: number) => number;
/** 关闭当前文档。@returns 0 */
export const closeDocument: () => number;
/**
 * 叠加一张全画布纯色层（成为新的顶层）。
 * @returns 0 成功；-1 无文档
 */
export const addSolidLayer: (r: number, g: number, b: number, a: number) => number;
/** 设置顶层不透明度（0..1）。@returns comp_result */
export const setTopOpacity: (opacity: number) => number;
/** 设置顶层混合模式（数值 = comp_blend_mode 冻结表，3 = Multiply）。 */
export const setTopBlend: (blend: number) => number;
/** 当前文档图层数；-1 无文档。 */
export const layerCount: () => number;

/** 给顶层加暗角滤镜（comp_filter_apply Vignette）。@returns comp_result */
export const applyVignette: (amount: number) => number;
/** 撤销一步。@returns comp_result（2 = 无可撤销） */
export const undo: () => number;
/** 重做一步。@returns comp_result */
export const redo: () => number;
/** 1 = 可撤销；0 = 不可。 */
export const canUndo: () => number;
/** 1 = 可重做；0 = 不可。 */
export const canRedo: () => number;
/**
 * 导出当前文档为 PNG（引擎编码）。
 * @param path_utf8 应用沙箱内的完整路径（如 cacheDir + '/x.png'）
 * @returns 0 成功；-1 无文档；其余 comp_result
 */
export const exportPng: (path_utf8: string) => number;

/**
 * 上屏（OPT-21 CPU 通路，异步）：投递一帧合成到 XComponent 窗口缓冲，立即返回。
 * @returns 0 已提交；-1 引擎/文档/表面缺失；-2 上一帧仍在进行
 */
export const present: () => number;
/**
 * 最近一次上屏的真实结果（工作线程回填）。
 * @returns 0 成功；-100 尚无结果或进行中；-1 前置缺失；其余 = comp_result/6
 */
export const presentResult: () => number;
/** 呈现面几何："宽x高|ready|absent"。 */
export const surfaceInfo: () => string;
/** 离屏合成临时文档（诊断用）。@returns RGBA8 ArrayBuffer 或 null */
export const flatten: (width: number, height: number) => ArrayBuffer | null;
