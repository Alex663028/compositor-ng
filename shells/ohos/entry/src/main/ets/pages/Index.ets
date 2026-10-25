// AI生成
//
// shells/ohos/entry/src/main/ets/pages/Index.ets — 鸿蒙外壳的 ArkTS 薄壳页面。
//
// 薄壳原则：ArkTS 只做 UI 编排与事件转发，一切图像能力经 libcompositor.so 下沉到引擎核。
// XComponent 的 libraryname 必须与 napi_module.nm_modname（"compositor"）一致，
// 框架才会把 __NATIVE_XCOMPONENT_OBJ__ 挂到模块 exports 上，从而触发 surface 回调注册。
// napi_init.cpp 用 napi_define_properties 挂命名导出，这里必须用命名导入
// （默认导入拿不到成员，ArkTS 严格模式还会按 arkts-no-any-unknown 报错）。

import { startup, shutdown, backendName, flatten, surfaceInfo, presentSurface } from 'libcompositor.so';

@Entry
@Component
struct Index {
  @State backend: string = 'None';
  @State surface: string = 'absent';
  @State status: string = '未启动';
  @State pixels: number = 0;
  @State present: string = '未上屏';

  private presentSize(): void {
    this.surface = surfaceInfo();
  }

  private presentFrame(): void {
    // presentSurface(w, h)：把合成结果提交给 XComponent 表面。
    // 返回值 >= 0 为 comp_result（0=OK，4=UNSUPPORTED 当前无后端支持上屏，1=参数非法）；
    // -1 表示引擎未启动或表面未就绪。此处如实展示，不伪造上屏成功。
    const code: number = presentSurface(1920, 1080);
    if (code < 0) {
      this.present = '前置缺失(' + code.toString() + ')';
    } else if (code === 0) {
      this.present = '上屏 OK';
    } else {
      this.present = '未上屏 rc=' + code.toString();
    }
    this.presentSize();
  }

  build() {
    Column({ space: 12 }) {
      Text('Compositor · HarmonyOS')
        .fontSize(20)
        .fontWeight(FontWeight.Medium)

      // 呈现面：surface 型 XComponent，绑定 native 模块
      XComponent({
        id: 'compositor_surface',
        type: XComponentType.SURFACE,
        libraryname: 'compositor'
      })
        .width('100%')
        .height(240)
        .backgroundColor('#101418')

      Text('状态：' + this.status).fontSize(14)
      Text('后端：' + this.backend).fontSize(14)
      Text('呈现面：' + this.surface).fontSize(14)
      Text('合成像素：' + this.pixels.toString()).fontSize(14)
      Text('上屏：' + this.present).fontSize(14)

      Row({ space: 10 }) {
        Button('启动引擎')
          .onClick(() => {
            const code: number = startup(0); // 0 = AUTO
            this.status = code === 0 ? '引擎就绪' : ('启动失败 ' + code.toString());
            this.backend = backendName();
            this.presentSize();
          })
        Button('离屏合成')
          .onClick(() => {
            const buffer: ArrayBuffer | null = flatten(1920, 1080);
            this.pixels = buffer === null ? 0 : buffer.byteLength;
            this.presentSize();
          })
        Button('上屏')
          .onClick(() => {
            this.presentFrame();
          })
        Button('释放引擎')
          .onClick(() => {
            shutdown();
            this.status = '已释放';
            this.backend = 'None';
            this.presentSize();
          })
      }
      .width('100%')
      .justifyContent(FlexAlign.SpaceEvenly)
    }
    .width('100%')
    .height('100%')
    .padding(16)
  }
}
