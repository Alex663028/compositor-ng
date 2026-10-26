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
         layerListJson, setLayerVisible, setLayerOpacity, removeLayer, setLayerName, layerPanelSelfTest,
         setBrushColor, setBrushDiameter, setBrushHardness, setBrushErase, brushInfo } from 'libcompositor.so';
import { picker, fileIo } from '@kit.CoreFileKit';

/*
 * 图层列表的一行（来自 napi 侧的 layerListJson）。
 *
 * 字段名与 JSON 里的键一一对应，visible 是 0/1 数字（JSON 里没有布尔量）。
 */
interface LayerRowItem {
  id: number;
  name: string;
  visible: number;
  opacity: number;
}

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

  /* 图层面板：与 Windows 壳的图层列表同语义（名字 / 显隐 / 透明度 / 删除 / 改名）。 */
  @State layerRows: LayerRowItem[] = [];
  @State selectedLayerId: number = 0;
  @State selectedOpacity: number = 100;
  @State renameText: string = '';
  @State layerPanelState: string = '—';

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
    this.refreshLayers();
    present();
  }

  /*
   * 图层面板：从 napi 侧拿一份 JSON 列表（引擎的堆叠序：上层在前）。
   *
   * 为什么整份取而不是逐层问：ArkTS 侧要画**列表**，逐层跨 NAPI 边界取会变成
   * N 次往返，层一多就明显卡。napi 侧已经把它拼成一份 JSON（名字做了转义）。
   */
  private refreshLayers(): void {
    const raw: string = layerListJson();
    try {
      this.layerRows = JSON.parse(raw) as LayerRowItem[];
    } catch {
      this.layerRows = [];
    }
  }

  private findLayer(id: number): LayerRowItem | undefined {
    for (let i = 0; i < this.layerRows.length; i++) {
      if (this.layerRows[i].id === id) {
        return this.layerRows[i];
      }
    }
    return undefined;
  }

  private selectLayer(id: number): void {
    this.selectedLayerId = id;
    const row: LayerRowItem | undefined = this.findLayer(id);
    if (row !== undefined) {
      this.selectedOpacity = Math.round(row.opacity * 100);
      this.renameText = row.name;
    }
  }

  private toggleLayer(id: number, on: boolean): void {
    const rc: number = setLayerVisible(id, on);
    this.do('显隐', rc);
    this.refreshLayers();
  }

  private applySelectedOpacity(value: number): void {
    this.selectedOpacity = Math.round(value);
    if (this.selectedLayerId === 0) {
      return;
    }
    const rc: number = setLayerOpacity(this.selectedLayerId, this.selectedOpacity / 100.0);
    this.do('层透明度', rc);
    this.refreshLayers();
  }

  private deleteSelectedLayer(): void {
    if (this.selectedLayerId === 0) {
      this.lastOp = '删除层：未选中';
      this.refresh();
      return;
    }
    const rc: number = removeLayer(this.selectedLayerId);
    this.selectedLayerId = 0;
    this.do('删除层', rc);
    this.refreshLayers();
  }

  /*
   * 图层面板自检：把「列表 / 显隐 / 透明度 / 改名 / 删除」在引擎层跑一遍并读回校验。
   *
   * 为什么需要：模拟器上 XComponent 上不了屏（OPT-21b），画布上看不到任何变化——
   * 「显隐有没有真的作用到引擎」光看屏幕验不了，只能读回引擎状态。
   */
  private runLayerPanelSelfTest(): void {
    this.layerPanelState = layerPanelSelfTest();
    this.refresh();
  }

  private renameSelectedLayer(): void {
    if (this.selectedLayerId === 0 || this.renameText.length === 0) {
      this.lastOp = '改名：未选中或名字为空';
      this.refresh();
      return;
    }
    const rc: number = setLayerName(this.selectedLayerId, this.renameText);
    this.do('改名', rc);
    this.refreshLayers();
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
        .height(200)
        .backgroundColor('#101418')
        /* surface 建好之后才读得到真实后端/尺寸；不在这里刷新，界面会一直停在
         * 硬编码的 'None' / 'absent'，看起来像初始化失败（2026-10-26 实测）。 */
        .onLoad(() => {
          this.backend = backendName();
          this.surface = surfaceInfo();
          this.layers = layerCount();
          this.undoState = canUndo() > 0 ? '可撤销' : '空';
          this.brushText = brushInfo();
          this.refreshLayers();
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

      /*
       * 图层面板自检按钮放在 **Scroll 之外**。
       *
       * 为什么：ArkUI 的 Scroll 会把落在它身上的触摸先当成滚动处理，按钮的
       * onClick 常常不触发（实测「层自检」放在 Scroll 内的图层面板标题行上，
       * 连点多次都点不动；移到 Scroll 外一按就响）。控件放在会被滚动容器
       * 吞触摸的位置，等于放了个点不着的按钮。
       */
      Row({ space: 8 }) {
        Button('层自检').fontSize(11).height(28)
          .onClick(() => {
            this.runLayerPanelSelfTest();
          })
        Text(this.layerPanelState).fontSize(9).fontColor('#8a8a8a').layoutWeight(1)
      }
      .width('100%')

      /*
       * 其余内容放进 Scroll：手机（尤其竖屏）窗口高度有限，内容总高很容易
       * 超出——超出时 Column 末尾的元素会被**挤没**（2026-10-26 实测：图层面板
       * 只剩 40px 高度，列表整块看不见）。放进 Scroll 后内容再长也能滚到。
       */
      Scroll() {
        Column({ space: 8 }) {
      /*
       * 图层面板：鸿蒙侧此前只有按钮级演示，**没有图层列表**（与 Windows 壳的
       * 差距）。这里补上与 Windows 侧同语义的一层：图层名、显隐、透明度、
       * 删除、改名。
       *
       * 数据来自引擎（layerListJson 走 comp_layer_list + comp_layer_get_name），
       * 不是壳自己记账——壳记账的话，导入图片加的层就不见了。
       */
      Column({ space: 4 }) {
        Row({ space: 6 }) {
          Text('图层').fontSize(12).fontColor('#9aa4b2')
          TextInput({ text: this.renameText, placeholder: '图层名' })
            .layoutWeight(1)
            .fontSize(12)
            .height(30)
            .onChange((value: string) => {
              this.renameText = value;
            })
          Button('改名').fontSize(11).height(28)
            .onClick(() => {
              this.renameSelectedLayer();
            })
          Button('删除').fontSize(11).height(28)
            .onClick(() => {
              this.deleteSelectedLayer();
            })
        }
        .width('100%')

        if (this.layerRows.length > 0) {
          List({ space: 2 }) {
            ForEach(this.layerRows, (item: LayerRowItem) => {
              ListItem() {
                Row({ space: 6 }) {
                  Text(item.name)
                    .fontSize(13)
                    .fontColor(item.id === this.selectedLayerId ? '#7fb0ff' : '#d8d8de')
                    .maxLines(1)
                    .layoutWeight(1)
                    .onClick(() => {
                      this.selectLayer(item.id);
                    })
                  Toggle({ type: ToggleType.Switch, isOn: item.visible !== 0 })
                    .width(40)
                    .height(22)
                    .selectedColor('#3f7fd0')
                    .onChange((on: boolean) => {
                      this.toggleLayer(item.id, on);
                    })
                }
                .width('100%')
                .padding({ left: 6, right: 6, top: 3, bottom: 3 })
                .backgroundColor(item.id === this.selectedLayerId ? '#25334a' : '#1b1d22')
              }
            }, (item: LayerRowItem) => item.id.toString())
          }
          .width('100%')
          .height(132)
          .scrollBar(BarState.Auto)
        } else {
          Text('（无图层）').fontSize(12).fontColor('#6a7080').width('100%')
        }

        Row({ space: 6 }) {
          Text('不透明度').fontSize(11).fontColor('#9aa4b2')
          Slider({ value: this.selectedOpacity, min: 0, max: 100, step: 1 })
            .layoutWeight(1)
            .onChange((value: number) => {
              this.applySelectedOpacity(value);
            })
          Text(this.selectedOpacity.toString() + '%').fontSize(11).fontColor('#9aa4b2')
        }
        .width('100%')
      }
      .width('100%')
      .height(200)
      .padding({ top: 6 })
      .backgroundColor('#14161a')


        }
        .width('100%')
      }
      .layoutWeight(1)
      .scrollBar(BarState.Auto)

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
