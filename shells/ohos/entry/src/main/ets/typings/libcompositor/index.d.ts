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
/*
 * 新建纯色层。
 *
 * @returns **新层的 id**（>0，且已经设为活动层）/ 负的错误码
 *          （以前返回 0/错误码，加入图层组后改成返回 id——壳侧要用它立刻选中新层）
 */
export const addSolidLayer: (r: number, g: number, b: number, a: number) => number;
/** 设置顶层不透明度（0..1）。@returns comp_result */
export const setTopOpacity: (opacity: number) => number;
/** 设置顶层混合模式（数值 = comp_blend_mode 冻结表，3 = Multiply）。 */
export const setTopBlend: (blend: number) => number;
/** 当前文档图层数；-1 无文档。 */
export const layerCount: () => number;

/*
 * 图层列表（JSON 数组字符串），**树形前序**。
 *
 * 每一项：{"id":<number>,"name":"<utf8>","visible":<0|1>,"opacity":<0..1>,
 *          "isGroup":<0|1>,"parent":<number>,"nest":<number>}
 *
 * 顺序 = 树形前序（组后面**紧跟**它的子层），nest 是缩进层级（别叫 depth：
 * 那个名字在 ArkTS 侧读出来恒为 0，值被内建属性吞掉）。不能直接把引擎
 * 的堆叠序发给壳：引擎那边父组与子层混在一起、不保证相邻，照着重就是「子层
 * 散落在列表各处」，用户看不出谁在谁里面、也拖不对。
 *
 * 为什么是字符串而不是数组：跨 NAPI 边界传数组在 ArkTS 侧类型不稳（实测
 * ArrayBuffer / TypedArray 都不被识别），纯文本没这个问题；ArkTS 侧一次
 * JSON.parse 即可。
 */
export const layerListJson: () => string;

/*
 * 新建图层组。
 *
 * @param name 组名（空串用默认「组」）
 * @param layerId 可选：把这一层移进新组（0 = 建空组）
 * @returns 新组的 id（>0）；失败返回负的错误码
 *
 * 「原位建组」：组落在原层的**位置**上，原层成为组的第一个子层。
 */
/*
 * 图层树的**标量接口**（配合 layerTreeBuild 使用）。
 *
 * 为什么不是一把 JSON：ArkTS 侧 `JSON.parse(raw) as Row[]` 会把 interface 里
 * 靠后的字段**抹成 0**（实测：同一个字符串，直接 stringify 出来是 `"pp":4`，
 * as 转换后读到的却是 0；只有前面几个字段正常）。标量没有类型转换这一步。
 *
 * 用法：行数 = layerTreeBuild()，然后 layerRowId(i) / layerRowName(i) / …
 */
/*
 * 整棵图层树，**一次给完**，分隔符格式：
 *   每行 `id\x1fname\x1fvisible\x1fopacity\x1fisGroup\x1fparent\x1flevel`，
 *   行间 `\x1e`。
 *
 * 为什么不用 JSON：`JSON.parse(raw) as Row[]` 会把 interface 靠后的字段抹成 0
 * （同一个字符串直接显示是对的）。字符串本身可靠，就在字符串里分好字段。
 */
export const layerTreeText: () => string;

export const layerTreeBuild: () => number;
export const layerRowId: (index: number) => number;
export const layerRowName: (index: number) => string;
export const layerRowVisible: (index: number) => number;
export const layerRowOpacity: (index: number) => number;
export const layerRowIsGroup: (index: number) => number;
/** 父级 id（0 = 根级）。 */
export const layerRowParent: (index: number) => number;
/** 缩进层级（0 = 根级）。 */
export const layerRowLevel: (index: number) => number;

export const createGroup: (name: string, layerId?: number) => number;

/*
 * 移动图层：换父级 + 定位（拖行 / 拖进组 / 拖出组）。
 *
 * @param id 要移动的层
 * @param parent 目标父组（0 = 根级）
 * @param before 目标位置的前一个兄弟（0 = 同级末尾）
 * @returns 0 成功；引擎侧拒绝成环 / 把组拖进自己
 */
export const moveLayer: (id: number, parent: number, before: number) => number;

/*
 * 设置当前选中层（图层面板点选即写回）。
 *
 * 同时写回引擎：.comp 的 activeLayerID 就是它，重开文档时选中的还是这一层。
 * 「作用于选中层」的操作（applyVignette / 调整面板）都以它为目标。
 */
export const setActiveLayer: (id: number) => number;

/** 当前选中层 id（0 = 没选）。 */
export const activeLayer: () => number;

/** 设置图层可见性。@returns 0 成功；1 参数错；-1 无文档。 */
export const setLayerVisible: (id: number, visible: boolean) => number;

/** 设置图层不透明度（0..1，超范围会被夹住）。 */
export const setLayerOpacity: (id: number, opacity: number) => number;

/** 删除图层。 */
export const removeLayer: (id: number) => number;

/** 重命名图层（UTF-8）。 */
export const setLayerName: (id: number, name_utf8: string) => number;

/**
 * 图层面板自检：把「列表 / 显隐 / 透明度 / 改名 / 删除」在引擎层跑一遍并读回校验。
 *
 * 模拟器上 XComponent 上不了屏（画布上看不到变化），所以「操作有没有真的作用到
 * 引擎」只能靠读回引擎状态来证明。
 *
 * @returns "list=1 hide=0 opacity=0.50 name=renamed_probe group=1 ungroup=1
 *           active=1 del=1 layers=N->N"
 */
export const layerPanelSelfTest: () => string;

/** 给顶层加暗角滤镜（comp_filter_apply Vignette）。@returns comp_result */
export const applyVignette: (amount: number) => number;

/**
 * 调整目标自检：建两层纯色（红/蓝），选中红层打暗角，读回像素比。
 *
 * 为什么需要：模拟器上画布上不了屏，「暗角打在哪一层」光看屏幕验不了；
 * 而这错误在真机上极难发现（选中 B 层、被改的是最上面那层 A，屏幕上看
 * 「某个地方暗了」，很容易以为是对的）。
 *
 * @returns "target=red ok=1 red=R->R blue=R->R"（ok=1 表示只改了选中层）
 */
export const adjustTargetSelfTest: () => string;
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
 * 导出到固定沙箱路径（自动化验证用）：
 * /data/storage/el2/base/cache/compositor_verify.png
 * @returns 0 成功；-1 无文档
 */
export const exportPngToSandbox: () => number;
/**
 * 导入图片为**新图层**（文档尺寸不变）。
 * @param path_utf8 应用沙箱内的完整路径（如 cacheDir + '/x.png'）
 * @returns 0 成功；-1 无引擎/无文档/路径为空；其余 comp_result
 */
export const importImage: (path_utf8: string) => number;
/**
 * 导入图片为**新文档**（画布尺寸 = 图像尺寸；旧文档被关闭）。
 * @param path_utf8 应用沙箱内的完整路径
 * @returns 0 成功；-1 无引擎/路径为空；其余 comp_result
 */
export const importImageAsDocument: (path_utf8: string) => number;
/**
 * 从**十六进制字符串**导入图片为新图层（不经文件路径）。
 *
 * 用 hex 文本而不是二进制：跨 NAPI 边界的二进制传递在 ArkTS 侧不稳定
 * （实测 napi_is_arraybuffer / napi_is_typedarray 都判 false）。
 * 引擎按扩展名识别格式，所以必须给 format（"png" / "jpg" / …）。
 * @param hex 图片字节的十六进制文本（可含空白）
 * @param format 格式名（png / jpg / jpeg / bmp / webp / tga / psd）
 * @returns 0 成功；1 参数错；-1 无引擎/无文档；-3 落地临时文件失败
 */
export const importImageBytes: (hex: string, format: string) => number;
/**
 * 导入链路状态（诊断用）："layers=<n>;doc=<w>x<h>;top=<id>"。
 */
export const importState: () => string;
/**
 * 导入 + 像素核对（一次性自检）：把内嵌 8x8 PNG 导进当前文档，
 * 读回像素与期望图案（左上红 / 右下绿 / 其余蓝）逐字节比对。
 * @returns 0 全部通过；-1 无引擎/无文档；-2 落地失败；-3 导入失败；
 *          -4 读回失败；-5 尺寸不符；-6 像素不符（错误数见 importState 的 last）
 */
export const importVerify: () => number;

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
