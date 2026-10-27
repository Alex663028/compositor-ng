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
         layerTreeText, adjustKindsText, adjustParamsText, layerAdjustmentsText,
         addLayerAdjustment, updateLayerAdjustmentAt, removeLayerAdjustment,
         layerAdjustmentValuesText,
         blendModesText, setLayerBlend, layerClipping, toggleLayerClipping,
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
  /*
   * 混合模式（引擎枚举值）与剪贴基底 id（0 = 不剪贴）。
   *
   * 两者都是**已参与像素求值**的属性——面板不显示的话，用户看到「画面不对，
   * 但每层的透明度都对」，这类问题最难查。剪贴基底只用来显示标记，改它要走
   * toggleLayerClipping（基底由引擎侧规则算，壳不指定）。
   */
  blend: number;
  clip: number;
}

/* 一个调整类型（kind + 规范名）。 */
interface AdjustKindItem {
  kind: number;
  name: string;
}

/*
 * 调整的一个可编辑参数。
 *
 * 范围与缺省**来自引擎**（comp_adjustment_param），壳不做参数表——壳自己维护
 * 一份的话，引擎加了参数/改了范围就会分叉。
 */
interface AdjustParamItem {
  path: string;
  label: string;
  type: number; /* 0 = 数值（滑杆）/ 1 = 布尔（开关） */
  min: number;
  max: number;
  def: number;
}

/* 图层上已有的一条调整。 */
interface AdjustEntry {
  index: number;
  kind: number;
  name: string;
}

/* 一个混合模式（引擎枚举值 + 界面名）。清单来自引擎，壳不维护枚举表。 */
interface BlendModeItem {
  mode: number;
  name: string;
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

  /*
   * 页面出现时载入**调整类型清单**。
   *
   * 这份清单是引擎的静态表（comp_layer_adjustment_kind_name），不依赖文档，
   * 所以可以在这里载入。不在这里载的话，用户得先点一次按钮才会出现——
   * 「调整」区的下拉一直是空的，看着像没做。
   */
  aboutToAppear(): void {
    this.loadAdjustKinds();
    /*
     * 混合模式清单同样在这里载入：它也是引擎的静态表（comp_blend_mode_name），
     * 不依赖文档。放 refresh 里的话首次进页面下拉是空的，看着像没做。
     */
    this.loadBlendModes();
  }

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

  /*
   * 折叠的组 id。
   *
   * 折叠是**纯 UI 状态**：层级的真相在引擎里，这里只决定「哪些行不显示」。
   * 所以不写回引擎、不进撤销栈——收起一个组不该能被撤销。
   */
  @State collapsedGroups: number[] = [];
  /* 折叠过滤后的显示行。ForEach 遍历这个，而不是 layerRows（全量树）。 */
  @State displayRows: LayerRowItem[] = [];

  /*
   * 多选集合（不含主选中 selectedLayerId）。
   *
   * 手机上没有 Shift/Ctrl，所以行首放一个勾选点：点它 = 加入/移出多选，
   * 点名字 = 单选（并清空多选）。长按不能用来做多选——长按已经归拖行了。
   */
  @State multiIds: number[] = [];

  /* 可编辑调整层。 */
  @State adjustKinds: AdjustKindItem[] = [];
  @State adjustKindIndex: number = 0;
  @State adjustParams: AdjustParamItem[] = [];
  @State adjustValues: number[] = [];
  @State layerAdjusts: AdjustEntry[] = [];
  @State editingAdjustIndex: number = -1;
  @State adjustState: string = '—';
  /* Select 的选项表（只在载入类型时重建一次）。 */
  @State adjustKindOptions: SelectOption[] = [];

  /*
   * 混合模式（图层属性，已参与像素求值）。
   *
   * 此前鸿蒙侧只有一个写死「乘法」的演示按钮，用户没法把一层改成 Screen/Overlay，
   * 也没法看出某层当前是什么模式。清单与名字都来自引擎（blendModesText），
   * 壳不维护 24 个常量——引擎加档位时下拉自动多一项。
   */
  @State blendModes: BlendModeItem[] = [];
  @State blendOptions: SelectOption[] = [];
  @State blendIndex: number = 0;
  @State clipState: string = '—';

  private refresh(): void {
    /*
     * 调整类型清单惰性载入（引擎起来之前调会拿到空串）。
     *
     * 放在 refresh 里而不是 aboutToAppear：这份清单要等引擎就绪，而且只需要
     * 成功一次（loadAdjustKinds 内部有「已有就不重复」的判断）。
     */
    if (this.adjustKinds.length === 0) {
      this.loadAdjustKinds();
    }
    if (this.blendModes.length === 0) {
      this.loadBlendModes();
    }
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
          lv: Number(fields[6]),
          /* 第 8/9 列是 2026-10-27 追加的：旧版本只给 7 列，缺列按「普通/不剪贴」。 */
          blend: fields.length > 7 ? Number(fields[7]) : 0,
          clip: fields.length > 8 ? Number(fields[8]) : 0
        };
        next.push(row);
      }
    }
    this.layerRows = next;
    /* 组被删掉后它的 id 还留在折叠集合里：不清掉的话，哪天 id 被复用会「凭空收起」。 */
    const alive: number[] = [];
    for (let i = 0; i < this.collapsedGroups.length; i++) {
      const id: number = this.collapsedGroups[i];
      if (this.findLayer(id) !== undefined) {
        alive.push(id);
      }
    }
    this.collapsedGroups = alive;
    this.rebuildDisplayRows();
    this.refreshLayerAdjusts();
    /* 混合模式下拉与列表同一份真相：列表刷完就把下拉对齐（别处改了模式也跟上）。 */
    this.syncBlendIndex(this.selectedLayerId);
  }

  /* 折叠过滤：祖先链上只要有一个组是收起的，这一行就不显示。 */
  private rebuildDisplayRows(): void {
    const out: LayerRowItem[] = [];
    for (let i = 0; i < this.layerRows.length; i++) {
      if (!this.hasCollapsedAncestor(this.layerRows[i])) {
        out.push(this.layerRows[i]);
      }
    }
    this.displayRows = out;
  }

  private hasCollapsedAncestor(row: LayerRowItem): boolean {
    let parent: number = row.pp;
    let guard = 0;
    while (parent !== 0 && guard < 64) {
      if (this.collapsedGroups.indexOf(parent) >= 0) {
        return true;
      }
      const ancestor: LayerRowItem | undefined = this.findLayer(parent);
      if (ancestor === undefined) {
        break;
      }
      parent = ancestor.pp;
      guard++;
    }
    return false;
  }

  private isCollapsed(id: number): boolean {
    return this.collapsedGroups.indexOf(id) >= 0;
  }

  private toggleCollapse(id: number): void {
    const next: number[] = [];
    let removed = false;
    for (let i = 0; i < this.collapsedGroups.length; i++) {
      if (this.collapsedGroups[i] === id) {
        removed = true;
      } else {
        next.push(this.collapsedGroups[i]);
      }
    }
    if (!removed) {
      next.push(id);
    }
    this.collapsedGroups = next;
    this.rebuildDisplayRows();
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
    this.refreshLayerAdjusts();
    /* 混合模式下拉跟着选中层走（否则显示的是上一层的档位）。 */
    this.syncBlendIndex(id);
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

  /*
   * 多选：行首的勾选点切换。
   *
   * 为什么不用长按：长按已经归拖行了（拖拽起手就是长按）。同一个手势不能既做
   * 拖行又做多选——用户拖到一半发现自己"选中"了一堆层，是最糟的体验。
   */
  private toggleMulti(id: number): void {
    const next: number[] = [];
    let removed = false;
    for (let i = 0; i < this.multiIds.length; i++) {
      if (this.multiIds[i] === id) {
        removed = true;
      } else {
        next.push(this.multiIds[i]);
      }
    }
    if (!removed) {
      next.push(id);
    }
    this.multiIds = next;
  }

  private clearMulti(): void {
    this.multiIds = [];
  }

  /* 批量操作的「作用对象」：有多选就用多选，否则就主选中那一个。 */
  private targetLayerIds(): number[] {
    if (this.multiIds.length > 0) {
      return this.multiIds;
    }
    return this.selectedLayerId !== 0 ? [this.selectedLayerId] : [];
  }

  /*
   * 批量收进新建的组（原位）。
   *
   * 顺序：以第一个层原地建组（组落在它的位置上），再把其余的逐个移进组。
   * 建组读的是**活动层**，所以先把活动层设成第一个——否则建出来是个空组。
   */
  private batchGroup(): void {
    const ids: number[] = this.targetLayerIds();
    if (ids.length === 0) {
      this.lastOp = '建组：没有目标';
      this.refresh();
      return;
    }
    setActiveLayer(ids[0]);
    const groupId: number = createGroup('组');
    if (groupId <= 0) {
      this.do('建组', groupId);
      return;
    }
    let failed = 0;
    for (let i = 1; i < ids.length; i++) {
      if (moveLayer(ids[i], groupId, 0) !== 0) {
        failed++;
      }
    }
    this.selectedLayerId = groupId;
    setActiveLayer(groupId);
    this.clearMulti();
    this.lastOp = '建组 ' + (ids.length - failed).toString() + '/' + ids.length.toString() +
      (failed === 0 ? ' ✓' : ' ✗');
    this.refreshLayers();
    this.refresh();
  }

  /* 批量移出到根级末尾。 */
  private batchUngroup(): void {
    const ids: number[] = this.targetLayerIds();
    if (ids.length === 0) {
      this.lastOp = '移出：没有目标';
      this.refresh();
      return;
    }
    let failed = 0;
    for (let i = 0; i < ids.length; i++) {
      if (moveLayer(ids[i], 0, 0) !== 0) {
        failed++;
      }
    }
    this.clearMulti();
    this.lastOp = '移出 ' + (ids.length - failed).toString() + '/' + ids.length.toString() +
      (failed === 0 ? ' ✓' : ' ✗');
    this.refreshLayers();
    this.refresh();
  }

  /*
   * 批量删除。
   *
   * 从**后往前**删：删一个组会连子树一起删，先删后面的层可以避免「按 id 删到
   * 一半发现前面的树已经没了」。结果只报成功数——部分失败比假成功好，但把
   * 每一层的错误码刷在状态栏没人看。
   */
  private batchDelete(): void {
    const ids: number[] = this.targetLayerIds();
    if (ids.length === 0) {
      this.lastOp = '删除：没有目标';
      this.refresh();
      return;
    }
    let failed = 0;
    for (let i = ids.length - 1; i >= 0; i--) {
      if (removeLayer(ids[i]) !== 0) {
        failed++;
      }
    }
    this.selectedLayerId = 0;
    this.clearMulti();
    this.lastOp = '删除 ' + (ids.length - failed).toString() + '/' + ids.length.toString() +
      (failed === 0 ? ' ✓' : ' ✗');
    this.refreshLayers();
    this.refresh();
  }

  /* ------------------------------------------------- 混合模式 / 剪贴蒙版 */

  /*
   * 载入全部混合模式（只做一次）。
   *
   * 清单与显示名都来自引擎（blendModesText -> comp_blend_mode_name），壳里没有
   * 「24 档」这个数字：引擎加一种混合方式，这里的下拉自动多一项。此前鸿蒙侧只有
   * 一个写死「乘法」的演示按钮——用户既改不了别的模式，也看不出某层当前是什么。
   */
  private loadBlendModes(): void {
    if (this.blendModes.length > 0) {
      return;
    }
    const raw: string = blendModesText();
    const list: BlendModeItem[] = [];
    if (raw.length > 0) {
      const lines: string[] = raw.split('\x1e');
      for (let i = 0; i < lines.length; i++) {
        const fields: string[] = lines[i].split('\x1f');
        if (fields.length < 2) {
          continue;
        }
        const item: BlendModeItem = { mode: Number(fields[0]), name: fields[1] };
        list.push(item);
      }
    }
    this.blendModes = list;
    const options: SelectOption[] = [];
    for (let i = 0; i < list.length; i++) {
      options.push({ value: list[i].name });
    }
    this.blendOptions = options;
  }

  private blendModeName(): string {
    if (this.blendIndex >= 0 && this.blendIndex < this.blendModes.length) {
      return this.blendModes[this.blendIndex].name;
    }
    return '';
  }

  /*
   * 把下拉指到某一层**当前的**混合模式上。
   *
   * 不做这一步的话，选中层切换后下拉仍停在上一层的档位——用户以为这层就是那个
   * 模式，一拖滑杆才发现改的是别的东西。
   */
  private syncBlendIndex(id: number): void {
    const row: LayerRowItem | undefined = this.findLayer(id);
    const mode: number = row !== undefined ? row.blend : 0;
    let index = 0;
    for (let i = 0; i < this.blendModes.length; i++) {
      if (this.blendModes[i].mode === mode) {
        index = i;
        break;
      }
    }
    this.blendIndex = index;
  }

  /* 改混合模式：与建组/移出/删除同一套批量语义（有多选就全改）。 */
  private applyBlend(index: number): void {
    if (index < 0 || index >= this.blendModes.length) {
      return;
    }
    this.blendIndex = index;
    const ids: number[] = this.targetLayerIds();
    if (ids.length === 0) {
      this.lastOp = '混合：未选中';
      this.refresh();
      return;
    }
    const mode: number = this.blendModes[index].mode;
    let failed = 0;
    for (let i = 0; i < ids.length; i++) {
      if (setLayerBlend(ids[i], mode) !== 0) {
        failed++;
      }
    }
    this.do('混合 ' + this.blendModes[index].name + (failed === 0 ? '' : ' ✗' + failed.toString()),
            failed === 0 ? 0 : -1);
    this.refreshLayers();
  }

  /*
   * 剪贴蒙版开关。
   *
   * 基底（挂到哪一层上）由 napi 侧按引擎的成栈规则算：**下方紧邻的同父图层**。
   * 壳不指定 id——让用户从一堆 id 里挑是错的，让壳自己算又会与引擎规则分叉。
   */
  private toggleClipping(): void {
    if (this.selectedLayerId === 0) {
      this.clipState = '先选一个图层';
      this.lastOp = '剪贴：未选中';
      this.refresh();
      return;
    }
    const rc: number = toggleLayerClipping(this.selectedLayerId);
    if (rc === 0) {
      const base: number = layerClipping(this.selectedLayerId);
      this.clipState = base > 0 ? ('剪贴 → #' + base.toString()) : '不剪贴';
    } else if (rc === -2) {
      this.clipState = '下方没有同组图层';
    } else if (rc === -3) {
      this.clipState = '下方图层不在同一组';
    } else {
      this.clipState = '失败 rc' + rc.toString();
    }
    this.lastOp = '剪贴 ' + this.clipState;
    this.refreshLayers();
    this.refresh();
  }

  /* ------------------------------------------------------------ 可编辑调整层 */

  /*
   * 载入全部调整类型（只做一次；失败就留空，UI 自然不显示选择器）。
   *
   * 类型清单**来自引擎**（comp_layer_adjustment_kind_name 能报出名字的就是合法
   * kind），壳不维护枚举表——上游加一种调整，壳不用改。
   */
  private loadAdjustKinds(): void {
    if (this.adjustKinds.length > 0) {
      return;
    }
    const raw: string = adjustKindsText();
    const list: AdjustKindItem[] = [];
    if (raw.length > 0) {
      const lines: string[] = raw.split('\x1e');
      for (let i = 0; i < lines.length; i++) {
        const fields: string[] = lines[i].split('\x1f');
        if (fields.length < 2) {
          continue;
        }
        const item: AdjustKindItem = { kind: Number(fields[0]), name: fields[1] };
        list.push(item);
      }
    }
    this.adjustKinds = list;
    const options: SelectOption[] = [];
    for (let i = 0; i < list.length; i++) {
      options.push({ value: list[i].name });
    }
    this.adjustKindOptions = options;
    if (list.length > 0) {
      this.selectAdjustKind(0);
    }
  }

  /* 切到第 index 个调整类型：拉参数元数据 + 用缺省值填满滑块。 */
  private selectAdjustKind(index: number): void {
    if (index < 0 || index >= this.adjustKinds.length) {
      return;
    }
    this.adjustKindIndex = index;
    this.editingAdjustIndex = -1;
    const kind: number = this.adjustKinds[index].kind;
    const raw: string = adjustParamsText(kind);
    const params: AdjustParamItem[] = [];
    const values: number[] = [];
    if (raw.length > 0) {
      const lines: string[] = raw.split('\x1e');
      for (let i = 0; i < lines.length; i++) {
        const fields: string[] = lines[i].split('\x1f');
        if (fields.length < 6) {
          continue;
        }
        const item: AdjustParamItem = {
          path: fields[0],
          label: fields[1],
          type: Number(fields[2]),
          min: Number(fields[3]),
          max: Number(fields[4]),
          def: Number(fields[5])
        };
        params.push(item);
        values.push(item.def);
      }
    }
    this.adjustParams = params;
    this.adjustValues = values;
  }

  /* 刷新选中层已有的调整列表。 */
  private refreshLayerAdjusts(): void {
    const id: number = this.selectedLayerId;
    if (id === 0) {
      this.layerAdjusts = [];
      return;
    }
    const raw: string = layerAdjustmentsText(id);
    const list: AdjustEntry[] = [];
    if (raw.length > 0) {
      const lines: string[] = raw.split('\x1e');
      for (let i = 0; i < lines.length; i++) {
        const fields: string[] = lines[i].split('\x1f');
        if (fields.length < 3) {
          continue;
        }
        const item: AdjustEntry = {
          index: Number(fields[0]),
          kind: Number(fields[1]),
          name: fields[2]
        };
        list.push(item);
      }
    }
    this.layerAdjusts = list;
  }

  /*
   * 点已有调整 = 进入编辑：切到它的 kind，并把当前值回填到滑块。
   *
   * 回填走引擎的 read_value（按 json_path 取值），壳不解析 settings JSON。
   */
  private editExistingAdjust(entry: AdjustEntry): void {
    let target = -1;
    for (let i = 0; i < this.adjustKinds.length; i++) {
      if (this.adjustKinds[i].kind === entry.kind) {
        target = i;
        break;
      }
    }
    if (target < 0) {
      this.adjustState = '这个调整类型当前不可编辑';
      return;
    }
    this.selectAdjustKind(target);
    this.editingAdjustIndex = entry.index;
    const raw: string = layerAdjustmentValuesText(this.selectedLayerId, entry.index, entry.kind);
    if (raw.length === 0) {
      this.adjustState = '回填失败（沿用缺省值）';
      return;
    }
    /*
     * 按 **json_path** 匹配回填，不按下标。
     *
     * 按下标的坑是隐性的：引擎的调整参数表调整了顺序、或在中间插入一个新参数时，
     * 回填值就会整体错位一格。每个值单看都在合法范围内，界面上完全看不出异常，
     * 只有「调过的值怎么变了」这种模糊抱怨。
     *
     * 引擎回报的第三列是 used_default：文档里没有这个键（旧版本存的 settings、
     * 或引擎新加了参数）时用了缺省值——这件事必须说出来，否则用户看到的是一个
     * 凭空变了的滑块。
     */
    const lines: string[] = raw.split('\x1e');
    const byPath: Map<string, string> = new Map<string, string>();
    const defaults: Map<string, number> = new Map<string, number>();
    for (let i = 0; i < lines.length; i++) {
      const fields: string[] = lines[i].split('\x1f');
      if (fields.length < 2) {
        continue;
      }
      byPath.set(fields[0], fields[1]);
      defaults.set(fields[0], fields.length > 2 && fields[2] === '1' ? 1 : 0);
    }
    const values: number[] = [];
    let defaulted = 0;
    for (let i = 0; i < this.adjustParams.length; i++) {
      const param: AdjustParamItem = this.adjustParams[i];
      const text: string | undefined = byPath.get(param.path);
      if (text === undefined) {
        /* 引擎里没有这个参数（版本不一致）：用缺省，并计入「走了缺省」。 */
        values.push(param.def);
        defaulted++;
        continue;
      }
      const parsed: number = Number(text);
      values.push(Number.isFinite(parsed) ? parsed : param.def);
      if (defaults.get(param.path) === 1) {
        defaulted++;
      }
    }
    this.adjustValues = values;
    this.adjustState = '编辑第 ' + (entry.index + 1).toString() + ' 条：' + entry.name +
      (defaulted > 0 ? ('（' + defaulted.toString() + ' 项缺省）') : '');
  }

  /* 扁平值拼成 \x1f 分隔的串（空串 = 让引擎用缺省）。 */
  /* 滑杆步长：把范围切成 200 格（整数范围也不至于拖不动）。 */
  private adjustStep(param: AdjustParamItem): number {
    const span: number = param.max - param.min;
    return span > 0 ? span / 200 : 1;
  }

  private adjustKindName(): string {
    if (this.adjustKindIndex >= 0 && this.adjustKindIndex < this.adjustKinds.length) {
      return this.adjustKinds[this.adjustKindIndex].name;
    }
    return '';
  }

  /*
   * 改一个参数值。
   *
   * 必须**整体替换数组**（`this.adjustValues = next`）而不是 `this.adjustValues[i] = v`：
   * 后者在 ArkUI 里不一定触发重渲染，滑块会「拖了不跟手」。
   */
  private setAdjustValue(index: number, value: number): void {
    const next: number[] = this.adjustValues.slice();
    next[index] = value;
    this.adjustValues = next;
  }

  private adjustValuesText(): string {
    let text = '';
    for (let i = 0; i < this.adjustValues.length; i++) {
      if (i > 0) {
        text += '\x1f';
      }
      text += this.adjustValues[i].toString();
    }
    return text;
  }

  private applyAdjustment(): void {
    if (this.selectedLayerId === 0) {
      this.adjustState = '先选一个图层';
      return;
    }
    if (this.adjustParams.length === 0) {
      this.adjustState = '这个类型没有可编辑参数';
      return;
    }
    const kind: number = this.adjustKinds[this.adjustKindIndex].kind;
    const values: string = this.adjustValuesText();
    const rc: number = this.editingAdjustIndex >= 0
      ? updateLayerAdjustmentAt(this.selectedLayerId, this.editingAdjustIndex, kind, values)
      : addLayerAdjustment(this.selectedLayerId, kind, values);
    this.adjustState = (this.editingAdjustIndex >= 0 ? '更新' : '新增') +
      (rc === 0 ? ' ✓' : (' ✗rc' + rc.toString()));
    this.editingAdjustIndex = -1;
    this.refreshLayers();
    this.refresh();
  }

  private removeAdjustmentAt(entry: AdjustEntry): void {
    if (this.selectedLayerId === 0) {
      return;
    }
    const rc: number = removeLayerAdjustment(this.selectedLayerId, entry.index);
    this.adjustState = '删除' + (rc === 0 ? ' ✓' : (' ✗rc' + rc.toString()));
    this.editingAdjustIndex = -1;
    this.refreshLayers();
    this.refresh();
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
          /*
           * 三个按钮都走**批量**语义：有多选就作用于多选，否则作用于主选中。
           * 这样「选几个层一起建组」不需要额外的模式开关。
           */
          Button('建组').fontSize(11).height(28)
            .onClick(() => {
              this.batchGroup();
            })
          Button('移出').fontSize(11).height(28)
            .onClick(() => {
              this.batchUngroup();
            })
          Button('删除').fontSize(11).height(28)
            .onClick(() => {
              this.batchDelete();
            })
          Text(this.dragLayerId !== 0 ? '拖动中…'
            : (this.multiIds.length > 0 ? ('多选 ' + this.multiIds.length.toString() + ' 层')
              : '单层'))
            .fontSize(10)
            .fontColor(this.multiIds.length > 0 ? '#7fb0ff' : '#6a7080')
            .layoutWeight(1)
        }
        .width('100%')

        /*
         * 混合模式 + 剪贴蒙版。
         *
         * 两者都是**已参与像素求值**的图层属性，此前鸿蒙侧完全没有入口：混合只有
         * 一个写死「乘法」的演示按钮，剪贴根本没接（引擎的 set_clipping_source
         * 早就能用）。用户遇到「画面不对但透明度都对」时无从下手。
         */
        Row({ space: 6 }) {
          Text('混合').fontSize(11).fontColor('#9aa4b2')
          Select(this.blendOptions)
            .selected(this.blendIndex)
            .value(this.blendModeName())
            .font({ size: 11 })
            .width(140)
            .onSelect((index: number) => {
              this.applyBlend(index);
            })
          Button('剪贴').fontSize(11).height(28)
            .onClick(() => {
              this.toggleClipping();
            })
          Text(this.clipState).fontSize(10).fontColor('#8a8a8a').layoutWeight(1).maxLines(1)
        }
        .width('100%')

        if (this.layerRows.length > 0) {
          List({ space: 2 }) {
            ForEach(this.displayRows, (item: LayerRowItem) => {
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
                  /*
                   * 多选勾选点。
                   *
                   * 手机上没有 Shift/Ctrl，长按又被拖行占了，所以单独给一个可点的
                   * 圆点：点它加入/移出多选，点名字才是单选。
                   */
                  Text(this.multiIds.indexOf(item.id) >= 0 ? '●' : '○')
                    .fontSize(12)
                    .fontColor(this.multiIds.indexOf(item.id) >= 0 ? '#7fb0ff' : '#5a6070')
                    .width(16)
                    .textAlign(TextAlign.Center)
                    .onClick(() => {
                      this.toggleMulti(item.id);
                    })
                  /*
                   * 组标记兼折叠开关：▾ 展开 / ▸ 收起。子层留同宽占位（不对齐会
                   * 看着像错位）。点图标才算折叠，点空白不折叠——避免误触。
                   */
                  Text(item.isGroup !== 0 ? (this.isCollapsed(item.id) ? '▸' : '▾') : '')
                    .fontSize(11)
                    .fontColor('#8a93a6')
                    .width(12)
                    .onClick(() => {
                      if (item.isGroup !== 0) {
                        this.toggleCollapse(item.id);
                      }
                    })
                  Text(item.name)
                    .fontSize(13)
                    .fontColor(item.id === this.selectedLayerId ? '#7fb0ff' : '#d8d8de')
                    .maxLines(1)
                    .layoutWeight(1)
                    .onClick(() => {
                      this.selectLayer(item.id);
                    })
                  /*
                   * 行内标记：剪 = 这层是剪贴蒙版成员，混 = 混合模式不是 Normal。
                   *
                   * 固定宽度（不是按文字伸缩）：宽度一变行的对齐就跟着抖，而这一列
                   * 的作用是「扫一眼看出哪几层特殊」，抖动的列反而更难扫。
                   */
                  Text(item.clip !== 0 ? '剪' : '')
                    .fontSize(9)
                    .fontColor('#e0a44a')
                    .width(11)
                    .textAlign(TextAlign.Center)
                  Text(item.blend !== 0 ? '混' : '')
                    .fontSize(9)
                    .fontColor('#7fb0ff')
                    .width(11)
                    .textAlign(TextAlign.Center)
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
            /*
             * key 还要带上**折叠与多选状态**：这两个变了图标和颜色就得换，
             * 只用 id 的话 ArkUI 会复用旧节点（本轮踩过：拖进组后缩进不更新）。
             */
            (item: LayerRowItem) =>
              item.id.toString() + '_' + item.lv.toString() + '_' + item.pp.toString() +
              '_' + item.name + '_c' + (this.isCollapsed(item.id) ? '1' : '0') +
              '_m' + (this.multiIds.indexOf(item.id) >= 0 ? '1' : '0') +
              /* 混合模式与剪贴标记同理：状态变了必须换节点，否则行内标记不更新。 */
              '_b' + item.blend.toString() + '_k' + item.clip.toString())
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
      /*
       * 调整区（可编辑调整层）放进 Scroll：参数可能十几个，屏幕放不下。
       *
       * 这里本来是空的（上一轮把图层面板搬出去固定显示了），正好留给它。
       */
      /*
       * 调整区（可编辑调整层）**固定在画布面板下方**，不放进 Scroll。
       *
       * 放 Scroll 里被挤成 0 高（上方画布 + 图层面板 + 四行按钮之后只剩一百多 px），
       * 「调整」两个字都看不见——等于这个功能没做。宁可让下面的调试按钮去滚，
       * 也得让调整区露出来。
       */
      Column({ space: 6 }) {
          Row({ space: 6 }) {
            Text('调整').fontSize(12).fontColor('#9aa4b2')
            Select(this.adjustKindOptions)
              .selected(this.adjustKindIndex)
              .value(this.adjustKindName())
              .font({ size: 11 })
              .width(150)
              .onSelect((index: number) => {
                this.selectAdjustKind(index);
              })
            Button(this.editingAdjustIndex >= 0 ? '更新' : '新增')
              .fontSize(11)
              .height(26)
              .onClick(() => {
                this.applyAdjustment();
              })
            Text(this.adjustState).fontSize(9).fontColor('#8a8a8a').layoutWeight(1).maxLines(1)
          }
          .width('100%')

          /*
           * 选中层已有的调整放在**参数区上面**：放下面会被参数区挡在滚动内容里，
           * 看得见点不着（实测点击落到了下方的按钮区）。
           * 点一下进入编辑，点 ✕ 删掉。
           */
          /* 选中层已有的调整：点一下进入编辑，点 ✕ 删掉。 */
          if (this.layerAdjusts.length > 0) {
            Row({ space: 6 }) {
              Text('已有').fontSize(10).fontColor('#9aa4b2')
              ForEach(this.layerAdjusts, (entry: AdjustEntry) => {
                Row({ space: 3 }) {
                  Text(entry.name)
                    .fontSize(10)
                    .fontColor(this.editingAdjustIndex === entry.index ? '#7fb0ff' : '#c8ccd6')
                    .onClick(() => {
                      this.editExistingAdjust(entry);
                    })
                  Text('✕')
                    .fontSize(10)
                    .fontColor('#c96a6a')
                    .onClick(() => {
                      this.removeAdjustmentAt(entry);
                    })
                }
                .padding({ left: 6, right: 6, top: 2, bottom: 2 })
                .backgroundColor('#22262e')
                .borderRadius(4)
              }, (entry: AdjustEntry) => entry.index.toString() + '_' + entry.kind.toString())
            }
            .width('100%')
          }
      }
      .width('100%')
      .padding({ top: 4, bottom: 4 })
      .backgroundColor('#14161a')

      /*
       * 调试按钮区：**固定高度**（不再 layoutWeight(1)）。
       *
       * 为什么必须固定：Column 的内容一旦把高度吃满，`layoutWeight(1)` 的 Scroll
       * 拿到的是 0 剩余空间——不是「变矮」，是「整个区块连一个按钮都不渲染」，
       * 而且它不报错。2026-10-27 图层面板加了「混合/剪贴」一行（+34vp），
       * 验证脚本从第六节起所有 tap 静默失败，界面上看不出任何异常。
       *
       * 固定 112vp 至少露出两行最常用的按钮，其余靠滚动。
       */
      Scroll() {
        Column({ space: 8 }) {
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

          /*
           * 参数控件由**引擎的元数据**生成：范围、缺省、类型都来自
           * comp_adjustment_param，壳不维护参数表。
           */
          Scroll() {
            Column({ space: 4 }) {
              ForEach(this.adjustParams, (param: AdjustParamItem, index: number) => {
                Row({ space: 6 }) {
                  Text(param.label)
                    .fontSize(10)
                    .fontColor('#9aa4b2')
                    .width(86)
                    .maxLines(1)
                  if (param.type === 1) {
                    Toggle({ type: ToggleType.Switch, isOn: this.adjustValues[index] > 0.5 })
                      .width(38)
                      .height(20)
                      .selectedColor('#3f7fd0')
                      .onChange((on: boolean) => {
                        this.setAdjustValue(index, on ? 1 : 0);
                      })
                  } else {
                    Slider({
                      value: this.adjustValues[index],
                      min: param.min,
                      max: param.max,
                      step: this.adjustStep(param)
                    })
                      .layoutWeight(1)
                      .onChange((value: number) => {
                        this.setAdjustValue(index, value);
                      })
                  }
                  Text(this.adjustValues[index].toFixed(2))
                    .fontSize(10)
                    .fontColor('#9aa4b2')
                    .width(46)
                }
                .width('100%')
              }, (param: AdjustParamItem, index: number) => param.path + '_' + index.toString())
            }
            .width('100%')
          }
          /* 参数多时内部滚动（外层不给它无限长：它下面还有按钮区）。 */
          .height(88)
          .scrollBar(BarState.Auto)

        }
        .width('100%')
      }
      .height(112)
      .scrollBar(BarState.Auto)

    }
    .justifyContent(FlexAlign.Center)
    .width('100%')
    .height('100%')
    .padding(14)
  }
}
