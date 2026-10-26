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
         backendName, exportPng, exportPngToSandbox, importImage, importImageAsDocument, importImageBytes,
         importState, importVerify,
         setBrushColor, setBrushDiameter, setBrushHardness, setBrushErase, brushInfo } from 'libcompositor.so';
import { picker, fileIo } from '@kit.CoreFileKit';

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
  @State importState: string = '—';

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

  /*
   * P1-2 剩余项：导入图片。
   *
   * 此前鸿蒙壳只能建纯色层，连自己的照片都打不开。
   *
   * 关键一步是**把选中的文件搬进沙箱**：文件选择器给的是 `file://` URI，
   * 而引擎的 comp_io_import 只认**路径**（并且格式是按扩展名识别的——
   * comp_io_format_from_path），所以必须复制成 cacheDir 下的同名文件，
   * 扩展名要保留，否则引擎会判成「未知格式」直接拒绝。
   */
  private async pickAndImport(): Promise<void> {
    this.importTo(false);
  }

  private async pickAndImportAsDocument(): Promise<void> {
    this.importTo(true);
  }

  /*
   * 选文件 -> 搬进沙箱 -> 导入。as_document = 导入为新文档（画布尺寸 =
   * 图像尺寸），否则作为新图层放进当前文档。
   */
  private async importTo(asDocument: boolean): Promise<void> {
    try {
      const photoPicker = new picker.PhotoViewPicker();
      const picked = await photoPicker.select({ maxSelectNumber: 1 });
      if (picked.photoUris.length === 0) {
        this.lastOp = '导入 取消';
        return;
      }
      const uri = picked.photoUris[0];
      /* 保留扩展名：引擎按扩展名判格式，丢了就认不出。 */
      const tail = uri.substring(uri.lastIndexOf('.'));
      const suffix = (tail.length >= 2 && tail.length <= 6) ? tail : '.png';
      const target = getContext(this).cacheDir + '/imported' + suffix;
      const source = await fileIo.open(uri, fileIo.OpenMode.READ_ONLY);
      const dest = await fileIo.open(target, fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.CREATE |
                                            fileIo.OpenMode.TRUNC);
      await fileIo.copyFile(source.fd, dest.fd);
      await fileIo.close(source);
      await fileIo.close(dest);

      const rc: number = asDocument ? importImageAsDocument(target) : importImage(target);
      this.importState = rc === 0 ? ('已导入 ' + target) : ('rc' + rc.toString());
      this.do('导入', rc);
    } catch {
      this.importState = '失败';
      this.lastOp = '导入 ✗';
      this.refresh();
    }
  }

  /*
   * 链路自检：用一张**内嵌的 8x8 PNG** 走完整导入链路。
   *
   * 为什么要有这个按钮：P1-2 的验收是「打开一张真实 PNG 并正确显示」，
   * 但「显示」在模拟器上验不了（OPT-21b：x86_64 模拟器不给 surface 的
   * CPU 可写映射）。所以这里把**能验的部分**验掉：图片字节 -> 落盘 ->
   * comp_io_import_layer -> 图层出现在文档里。图层数 +1 且导入状态里的
   * 尺寸对得上，就说明桥接通了。
   *
   * 图案内嵌 8x8 PNG：左上红、右下绿、其余蓝（**非对称**——对称图案里
   * 「转置」「镜像」这类错误会因像素相同而蒙混过关）。
   *
   * 字节以**十六进制字符串**传给 NAPI：跨 NAPI 边界的二进制传递在 ArkTS
   * 侧不稳定（实测 napi_is_arraybuffer / napi_is_typedarray 都判 false），
   * 纯文本没有这个问题。
   */
  private readonly probePngHex: string =
    '89504e470d0a1a0a0000000d4948445200000008000000080806000000c40fbe8b0000' +
    '001b4944415478da63f8cfc0f01f19a371ff33d04101161da890f60a0073277f819c26' +
    '6fe50000000049454e44ae426082';

  private runProbeImport(): void {
    try {
      const rc: number = importImageBytes(this.probePngHex, 'png');
      /*
       * 导出一份到固定沙箱路径：验证脚本用 hdc 拉回来逐像素核对。
       * 模拟器上不了屏（OPT-21b），「图片真的进引擎且像素正确」只能靠这条路证明。
       */
      let exportRc: number = -1;
      if (rc === 0) {
        exportRc = exportPngToSandbox();
      }
      this.importState = '自检 rc=' + rc.toString() + ' exp=' + exportRc.toString() + ' ' +
                         importState();
      this.do('导入自检', rc);
    } catch {
      this.importState = '自检失败';
      this.lastOp = '导入自检 ✗';
      this.refresh();
    }
  }

  /*
   * 像素核对：导入内嵌 PNG 后**读回像素**与期望图案逐字节比对。
   * 这是「打开一张真实 PNG 并正确显示」在模拟器上能做到的最强验证
   * （显示那条路被 OPT-21b 的平台限制堵着，但像素对不对是能验的）。
   */
  private runPixelVerify(): void {
    const rc: number = importVerify();
    this.importState = '验像素 rc=' + rc.toString() + ' ' + importState();
    this.do('验像素', rc);
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

      /* P1-2 剩余项：导入图片。此前鸿蒙壳只能建纯色层，照片打不开。 */
      Row({ space: 8 }) {
        Button('导入图片').onClick(() => {
          this.pickAndImport();
        })
        Button('导入为文档').onClick(() => {
          this.pickAndImportAsDocument();
        })
        Button('自检').onClick(() => {
          this.runProbeImport();
        })
        Button('验像素').onClick(() => {
          this.runPixelVerify();
        })
        Text(this.importState).fontSize(10).fontColor('#9aa4b2').layoutWeight(1)
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
