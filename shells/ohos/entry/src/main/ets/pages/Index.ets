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
         setLayerVisible, setLayerOpacity, removeLayer, setLayerName, layerPanelSelfTest,
         adjustTargetSelfTest,
         createGroup, moveLayer, setActiveLayer, activeLayer,
         layerTreeText,
         setBrushColor, setBrushDiameter, setBrushHardness, setBrushErase, brushInfo } from 'libcompositor.so';
import { picker, fileIo } from '@kit.CoreFileKit';

/*
 * 图层列表的一行（来自 napi 侧的 layerTreeText，分隔符格式）。
 *
 * visible / isGroup 是 0/1 数字（跨桥只有标量最稳）。
 */
interface LayerRowItem {
  id: number;
  name: string;
  visible: number;
  opacity: number;
  /*
   * 树形字段（napi 侧算好了树形前序：组后面紧跟它的子层）。
   *
   * depth 是缩进层级——不能靠「parent 是哪个组」自己推：同级的顺序与缩进都要
   * 一致地反映引擎的真实层级，壳自己推就会在「组套组」时错位。
   */
  isGroup: number;
  /*
   * 父级 id 与缩进层级。
   *
   * 名字是 `pp` / `lv` 而不是 `parent` / `nest`：后两个名字在 ArkTS 里读出来
   * **恒为 0**——JSON 文本里明明是 4 和 1（把同一个字符串直接显示到界面上是
   * 对的），`JSON.parse` 之后就变 0。这两个名字与 ArkUI 的内建属性撞车，
   * 值被吞掉。字段名可以换，语义换不了。
   */
  pp: number;
  lv: number;
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
  /* 正在拖的图层 id（0 = 没在拖）；拖到哪一行就高亮哪一行。 */
  @State dragLayerId: number = 0;
  @State dropTargetId: number = 0;

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
    /*
     * 一次取完整棵树，用分隔符切成行 / 字段。
     *
     * 为什么不走 JSON：`JSON.parse(raw) as LayerRowItem[]` 会把 interface 里
     * **靠后的字段抹成 0**——实测同一个字符串直接显示是 `"pp":4`，as 转换后
     * 读到的却是 0，而靠前的 id/name/visible/opacity/isGroup 都正常。
     * 为什么不用「一次 build + 逐字段取」：多次 NAPI 调用之间那份缓存会与渲染
     * 节奏错位（实测不同行读到不同批次的值，出现「组行有父级、子层没有」）。
     *
     * 字符串本身从来没错过，所以就把字段分好放在字符串里。
     */
    const raw: string = layerTreeText();
    const next: LayerRowItem[] = [];
    if (raw.length > 0) {
      const lines: string[] = raw.split('\x1e');
      for (let i = 0; i < lines.length; i++) {
        const fields: string[] = lines[i].split('\x1f');
        if (fields.length < 7) {
          continue;
        }
        const row: LayerRowItem = {
          id: Number(fields[0]),
          name: fields[1],
          visible: Number(fields[2]),
          opacity: Number(fields[3]),
          isGroup: Number(fields[4]),
          pp: Number(fields[5]),
          lv: Number(fields[6])
        };
        next.push(row);
      }
    }
    this.layerRows = next;
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
    /*
     * 选中即写回引擎的活动层。
     *
     * 这一步是「调整面板联动」的关键：applyVignette 之类的操作作用于**当前
     * 选中层**，而那个 id 由 napi 侧的 g_active_layer 保管。不写回的话，用户
     * 在列表里选了 B 层、点「暗角」，被改的还是上次那层。
     */
    setActiveLayer(id);
  }

  /*
   * 建组（原位）：组落在选中层的位置上，选中层成为组的第一个子层。
   *
   * 为什么要「原位」：用户选了一层再点建组，期望的是「把这一层收进一个组」，
   * 不是「在列表最顶上凭空多一个空组」——后者还要再拖一次，等于白点。
   */
  private groupSelectedLayer(): void {
    if (this.selectedLayerId === 0) {
      this.lastOp = '建组：未选中';
      this.refresh();
      return;
    }
    const groupId: number = createGroup('组');
    if (groupId > 0) {
      this.selectedLayerId = groupId;
      setActiveLayer(groupId);
      this.do('建组', 0);
    } else {
      this.do('建组', groupId);
    }
    this.refreshLayers();
  }

  /* 移出组：把选中层移到根级末尾（拖行的按钮兜底——手机上拖拽不一定好使）。 */
  private ungroupSelectedLayer(): void {
    if (this.selectedLayerId === 0) {
      this.lastOp = '移出组：未选中';
      this.refresh();
      return;
    }
    const rc: number = moveLayer(this.selectedLayerId, 0, 0);
    this.do('移出组', rc);
    this.refreshLayers();
  }

  /*
   * 拖行落点：把 A 拖到 B 上。
   *
   * 两种语义，按 B 是不是组来分：
   *   - B 是组 → **移进 B**（放到组末尾）。PS 同款：往组上一放就是收进去。
   *   - B 不是组 → 移到 B **之前**、与 B 同父级。拖到根级行之前就自然出了组，
   *     所以「拖出组」不需要单独的手势。
   *
   * 引擎会拒绝成环（把组拖进自己或自己的子组）——这里不预判，直接把错误码
   * 显示出来：壳侧预判等于把引擎的规则复制一份，迟早分叉。
   */
  private dropLayerOn(target: LayerRowItem): void {
    const dragged: number = this.dragLayerId;
    this.dragLayerId = 0;
    this.dropTargetId = 0;
    if (dragged === 0 || dragged === target.id) {
      return;
    }
    const parent: number = target.isGroup !== 0 ? target.id : target.pp;
    const before: number = target.isGroup !== 0 ? 0 : target.id;
    const rc: number = moveLayer(dragged, parent, before);
    this.do(target.isGroup !== 0 ? '拖入组' : '拖行', rc);
    this.refreshLayers();
  }

  /*
   * 建一层并**立刻选中它**。
   *
   * 为什么要一步做完：新建层后不选中的话，用户点「建组」「暗角」这些「作用于
   * 选中层」的操作会落到空处（或上一层的身上）。PS 里新建层就是自动选中的，
   * 这里跟上。
   *
   * 新层的 id 由 napi 侧写进活动层（g_active_layer），读回来即可——不用再解
   * 一次 JSON 列表去猜哪一行是新的。
   */
  private addLayerAndSelect(r: number, g: number, b: number, a: number, label: string): void {
    /* 返回的是**新层的 id**（>0），不是 0/错误码——成功与否看正负。 */
    const id: number = addSolidLayer(r, g, b, a);
    this.refreshLayers();
    if (id > 0) {
      this.selectLayer(id);
      this.do(label, 0);
    } else {
      this.do(label, id);
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
    this.refreshLayers();
  }

  /*
   * 调整目标自检：applyVignette 到底改的是哪一层。
   *
   * 模拟器上画布上不了屏（OPT-21b），「暗角打在哪一层」光看屏幕验不了。
   * 而这个错误在真机上极难发现——选中 B 层点暗角，被改的是最上面那层 A，
   * 屏幕上看「某个地方暗了」，很容易以为是对的。所以只能读回像素比。
   */
  private runAdjustTargetSelfTest(): void {
    this.layerPanelState = adjustTargetSelfTest();
    this.refreshLayers();
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
        /*
         * 画布高度 200 -> 150：图层面板要固定在画布下方，手机（尤其竖屏）垂直
         * 空间就那么多——画布让出 50vp，换图层面板（最常看的一块）不用滚。
         */
        .height(150)
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
        Button('调自检').fontSize(11).height(28)
          .onClick(() => {
            this.runAdjustTargetSelfTest();
          })
        Text(this.layerPanelState).fontSize(9).fontColor('#8a8a8a').layoutWeight(1)
      }
      .width('100%')

      /*
       * 其余内容放进 Scroll：手机（尤其竖屏）窗口高度有限，内容总高很容易
       * 超出——超出时 Column 末尾的元素会被**挤没**（2026-10-26 实测：图层面板
       * 只剩 40px 高度，列表整块看不见）。放进 Scroll 后内容再长也能滚到。
       */
      /*
       * 图层面板放在 **Scroll 之外**（画布正下方、固定可见）。
       *
       * 为什么：面板本身有 200vp 出头（标题行 + 按钮行 + 列表 + 不透明度行），
       * 而 Scroll 的可视高度在手机上只剩一百多 px（上方还有画布与几行按钮）。
       * 放进 Scroll 里就等于「打开应用看不到图层」——用户得先滚才知道有图层面板，
       * 而图层面板恰恰是这类应用最常看的一块。
       *
       * 调试用的按钮区反而适合放 Scroll：它们不常用、可滚、数量还在长。
       */
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
        }
        .width('100%')
        /*
         * 第二行：分组与整理。
         *
         * 「建组」是原位建组（选中层直接成为新组的子层）；「移出」把选中层搬回
         * 根级——拖拽在手机上不一定顺手，这两个按钮是拖行的兜底入口，功能不缺。
         */
        Row({ space: 6 }) {
          Button('建组').fontSize(11).height(28)
            .onClick(() => {
              this.groupSelectedLayer();
            })
          Button('移出').fontSize(11).height(28)
            .onClick(() => {
              this.ungroupSelectedLayer();
            })
          Button('删除').fontSize(11).height(28)
            .onClick(() => {
              this.deleteSelectedLayer();
            })
          Text(this.dragLayerId !== 0 ? '拖动中…' : '')
            .fontSize(10).fontColor('#7fb0ff').layoutWeight(1)
        }
        .width('100%')

        if (this.layerRows.length > 0) {
          List({ space: 2 }) {
            ForEach(this.layerRows, (item: LayerRowItem) => {
              ListItem() {
                Row({ space: 4 }) {
                  /*
                   * 缩进用**宽度占位**而不是 padding。
                   *
                   * 这个 Row 上的 `padding({left})` 实测不生效（缩进恒为 0），
                   * 而一个固定宽度的空元素是可靠的：层级 × 14vp，根级为 0。
                   */
                  Text('')
                    .width(item.lv * 14)
                    .height(1)
                  /* 组标记：组行给一个图标，子层留同宽占位（不对齐会看着像错位）。 */
                  Text(item.isGroup !== 0 ? '▾' : '')
                    .fontSize(11)
                    .fontColor('#8a93a6')
                    .width(12)
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
                /*
                 * 缩进由 depth 直接乘：树形前序已经把「谁在谁里面」编成了层级，
                 * 壳不再自己推父子关系（推出来的会与引擎的真实层级分叉）。
                 */
                .padding({ left: 6, right: 6, top: 3, bottom: 3 })
                .backgroundColor(item.id === this.selectedLayerId
                  ? '#25334a'
                  : (item.id === this.dropTargetId ? '#2f3a24' : '#1b1d22'))
              }
              /*
               * 拖行：长按拖动一行到另一行上。
               *
               * 落点语义见 dropLayerOn：拖到组上 = 收进组，拖到普通行上 = 插到它
               * 之前（同父级）。系统级拖拽（onDragStart/onDrop）与 List 的滚动
               * 手势不冲突——自己用 PanGesture 抢手势的话，列表一滚就乱。
               */
              .onDragStart(() => {
                this.dragLayerId = item.id;
                const preview: DragItemInfo = { extraInfo: item.name };
                return preview;
              })
              .onDrop((event: DragEvent) => {
                this.dropLayerOn(item);
              })
            },
            /*
             * key 必须带上**会变化的展示字段**（层级 / 父级 / 名字）。
             *
             * 只用 id 做 key 的坑：把一层拖进组之后 id 没变，ForEach 认为还是
             * 同一个节点、**复用旧组件与旧 item**，于是缩进、组标记全是上一次
             * 的样子——数据明明是对的，界面上却看不出分组（实测踩了很久：
             * `layerTreeText` 里已经是 `p4,l1`，界面上还是 `p0,l0`）。
             */
            (item: LayerRowItem) =>
              item.id.toString() + '_' + item.lv.toString() + '_' + item.pp.toString() +
              '_' + item.name)
          }
          .width('100%')
          /*
           * 高度按行数算（上限 176vp ≈ 4 行）：层少时面板矮，把垂直空间留给
           * 下面的按钮区；层多时最多显示 4 行，其余靠滚动。
           *
           * 固定 136vp 的教训：那只有 3 行多一点，而「组 + 组内子层」本身就
           * 占两行——建完组，用户看不到子层，会以为建组没生效（实测）。
           */
          .height(Math.min(this.layerRows.length * 42 + 8, 176))
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
      /*
       * 不设固定高度：内容（标题行 + 按钮行 + 列表 132 + 不透明度行）加起来比
       * 200 高，固定 200 会把 **List 挤成 0 高**——列表整块看不见，但按钮照常
       * 显示，看起来像「图层列表没了」。高度交给内容决定。
       */
      .padding({ top: 6, bottom: 6 })
      .backgroundColor('#14161a')
      Scroll() {
        Column({ space: 8 }) {


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
          this.addLayerAndSelect(64, 128, 255, 255, '蓝层');
        })
        Button('红层').onClick(() => {
          this.addLayerAndSelect(255, 64, 64, 255, '红层');
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
