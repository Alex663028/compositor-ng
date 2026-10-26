// AI生成
//
// shells/ohos/entry/src/main/ets/pages/Index.ets — 鸿蒙外壳的 PS 基础功能演示页。
//
// 演示链（全部经 libcompositor.so 下沉引擎，CPU 直写 XComponent 表面上屏）：
//   新建文档 → 图层（纯色叠加）→ 透明度 / 混合模式 → 滤镜（暗角）→ 撤销 / 重做
//   → 导出 PNG（应用沙箱）。每一步操作后自动 present() 把合成结果刷到屏幕。
//
// napi_init.cpp 用 napi_define_properties 挂命名导出，这里必须用命名导入。

import { presentStage, presentDiag, touchCount, newDocument, addSolidLayer, setTopOpacity, setTopBlend, layerCount,
         applyVignette, undo, redo, canUndo, present, presentResult, surfaceInfo,
         backendName, exportPng,
         setBrushColor, setBrushDiameter, setBrushHardness, setBrushErase, brushInfo } from 'libcompositor.so';

@Entry
@Component
struct Index {
  @State backend: string = 'None';
  @State surface: string = 'absent';
  @State layers: number = 0;
  @State lastOp: string = '—';
  @State presentState: string = '未上屏';
  @State diag: string = '—';
  @State brushText: string = '';
  @State diameter: number = 70;
  @State softness: number = 85;
  @State erasing: boolean = false;

  /* 画笔参数改完立刻回读一次摘要，让用户看得见当前笔是什么样。 */
  private syncBrush(): void {
    this.brushText = brushInfo();
  }
  @State undoState: string = '—';
  @State topOpacity: number = 0.5;
  @State exportState: string = '—';

  private refresh(): void {
    this.backend = backendName();
    this.surface = surfaceInfo();
    this.layers = layerCount();
    this.undoState = canUndo() > 0 ? '可撤销' : '空';
    /* 先读上一帧的结果与诊断，再提交新一帧：present 是异步的，先提交会把
     * 诊断缓冲清空，读到的永远是空串（2026-10-26 实测踩过）。 */
    const rc: number = presentResult();
    this.presentState = rc === -100 ? '提交中' : (rc === 0 ? '上屏 OK' : ('rc=' + rc.toString()));
    this.diag = presentDiag();
    present();
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
        /* surface 建好之后才读得到真实后端/尺寸；不在这里刷新，界面会一直停在
         * 硬编码的 'None' / 'absent'，看起来像初始化失败（2026-10-26 实测）。 */
        .onLoad(() => {
          this.backend = backendName();
          this.surface = surfaceInfo();
          this.layers = layerCount();
          this.undoState = canUndo() > 0 ? '可撤销' : '空';
          this.brushText = brushInfo();
        })

      Text('后端 ' + this.backend + ' · 表面 ' + this.surface + ' · 上屏 ' + this.presentState)
        .fontSize(12)
      Text('S' + presentStage().toString() + ' 触' + touchCount().toString() + ' · 图层 ' + this.layers.toString() + ' · 撤销 ' + this.undoState + ' · 最近 ' + this.lastOp + ' · 导出 ' + this.exportState)
        .fontSize(12)
      Text(this.diag)
        .fontSize(9)
        .fontColor('#8a8a8a')
        .width('100%')
        .textAlign(TextAlign.Center)

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

      /* P1-2 画笔可调：此前颜色写死黄、直径写死 70px、没有橡皮擦。
       * 这四个色块 + 橡皮开关直接改 napi 侧的全局量，下一笔即生效。 */
      Row({ space: 6 }) {
        Button('黄').onClick(() => { setBrushColor(255, 220, 40); this.syncBrush(); })
        Button('红').onClick(() => { setBrushColor(255, 72, 72); this.syncBrush(); })
        Button('蓝').onClick(() => { setBrushColor(72, 136, 255); this.syncBrush(); })
        Button('绿').onClick(() => { setBrushColor(72, 200, 128); this.syncBrush(); })
        Button(this.erasing ? '橡皮 开' : '橡皮 关').onClick(() => {
          this.erasing = !this.erasing;
          setBrushErase(this.erasing);
          this.syncBrush();
        })
      }
      .width('100%')
      .justifyContent(FlexAlign.SpaceEvenly)

      Row({ space: 10 }) {
        Text('粗细').fontSize(12)
        Slider({ value: this.diameter, min: 1, max: 240, step: 1 })
          .width(170)
          .onChange((value: number) => {
            this.diameter = Math.round(value);
            setBrushDiameter(this.diameter);
            this.syncBrush();
          })
        Text('硬度').fontSize(12)
        Slider({ value: this.softness, min: 0, max: 100, step: 5 })
          .width(130)
          .onChange((value: number) => {
            this.softness = Math.round(value);
            setBrushHardness(this.softness / 100.0);
            this.syncBrush();
          })
        Text(this.brushText).fontSize(11).fontColor('#9aa4b2')
      }
      .width('100%')
      .justifyContent(FlexAlign.Center)
    }
    .width('100%')
    .height('100%')
    .padding(14)
  }
}
