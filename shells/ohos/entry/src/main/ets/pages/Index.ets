// AI生成
//
// shells/ohos/entry/src/main/ets/pages/Index.ets — 鸿蒙外壳的 PS 基础功能演示页。
//
// 演示链（全部经 libcompositor.so 下沉引擎，CPU 直写 XComponent 表面上屏）：
//   新建文档 → 图层（纯色叠加）→ 透明度 / 混合模式 → 滤镜（暗角）→ 撤销 / 重做
//   → 导出 PNG（应用沙箱）。每一步操作后自动 present() 把合成结果刷到屏幕。
//
// napi_init.cpp 用 napi_define_properties 挂命名导出，这里必须用命名导入。

import { newDocument, addSolidLayer, setTopOpacity, setTopBlend, layerCount,
         applyVignette, undo, redo, canUndo, present, presentResult, surfaceInfo,
         backendName, exportPng } from 'libcompositor.so';

@Entry
@Component
struct Index {
  @State backend: string = 'None';
  @State surface: string = 'absent';
  @State layers: number = 0;
  @State lastOp: string = '—';
  @State presentState: string = '未上屏';
  @State undoState: string = '—';
  @State topOpacity: number = 0.5;
  @State exportState: string = '—';

  private refresh(): void {
    this.backend = backendName();
    this.surface = surfaceInfo();
    this.layers = layerCount();
    this.undoState = canUndo() > 0 ? '可撤销' : '空';
    present();  // 异步提交（主线程绝不阻塞）；真实 rc 由 presentResult 读取
    const rc: number = presentResult();
    this.presentState = rc === -100 ? '提交中' : (rc === 0 ? '上屏 OK' : ('rc=' + rc.toString()));
  }

  private do(label: string, code: number): void {
    this.lastOp = label + (code === 0 ? ' ✓' : (' ✗rc' + code.toString()));
    this.refresh();
  }

  build() {
    Column({ space: 10 }) {
      Text('Compositor · PS 基础功能')
        .fontSize(20)
        .fontWeight(FontWeight.Medium)

      XComponent({
        id: 'compositor_surface',
        type: XComponentType.SURFACE,
        libraryname: 'compositor'
      })
        .width('100%')
        .height(300)
        .backgroundColor('#101418')

      Text('后端 ' + this.backend + ' · 表面 ' + this.surface + ' · 上屏 ' + this.presentState)
        .fontSize(12)
      Text('图层 ' + this.layers.toString() + ' · 撤销 ' + this.undoState + ' · 最近 ' + this.lastOp + ' · 导出 ' + this.exportState)
        .fontSize(12)

      Row({ space: 8 }) {
        Button('新建').onClick(() => {
          // 文档尺寸 = 表面原生尺寸（"WxH|ready"）：present 零几何改配、零缩放，
          // 撕裂与协商阻塞的根源都在「文档 ≠ 缓冲」的错配上。
          const info: string = surfaceInfo();
          const match: RegExpMatchArray | null = info.match(/^(\d+)x(\d+)\|ready$/);
          const w: number = match !== null ? Number(match[1]) : 2030;
          const h: number = match !== null ? Number(match[2]) : 456;
          this.do('新建', newDocument(w, h));
        })
        Button('蓝层').onClick(() => {
          this.do('蓝层', addSolidLayer(64, 128, 255, 255));
        })
        Button('红层').onClick(() => {
          this.do('红层', addSolidLayer(255, 64, 64, 255));
        })
        Button('暗角').onClick(() => {
          this.do('暗角', applyVignette(0.7));
        })
      }
      .width('100%')
      .justifyContent(FlexAlign.SpaceEvenly)

      Row({ space: 8 }) {
        Button('撤销').onClick(() => {
          this.do('撤销', undo());
        })
        Button('重做').onClick(() => {
          this.do('重做', redo());
        })
        Button('半透明').onClick(() => {
          this.do('透明度', setTopOpacity(this.topOpacity));
        })
        Button('乘法').onClick(() => {
          this.do('混合', setTopBlend(2) /* COMP_BLEND_MULTIPLY */);
        })
        Button('导出PNG').onClick(() => {
          const path: string = getContext(this).cacheDir + '/compositor_export.png';
          const rc: number = exportPng(path);
          this.exportState = rc === 0 ? path : ('rc' + rc.toString());
          this.do('导出', rc);
        })
      }
      .width('100%')
      .justifyContent(FlexAlign.SpaceEvenly)
    }
    .width('100%')
    .height('100%')
    .padding(14)
  }
}
