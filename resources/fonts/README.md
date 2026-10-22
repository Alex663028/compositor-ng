# 随仓字体资源（P5-4）

| 文件 | 作用 |
|------|------|
| `NotoSansSC-Regular-Subset.ttf` | 文字工具的可加载字体（TrueType，`glyf`）；测试与外壳都用它 |
| `OFL.txt` | SIL Open Font License 1.1 原文（OFL 要求与字体一同分发） |

## 来源与再生成

源字体是 Noto Sans SC 可变字体（`wght` 100..900，默认实例 Thin），由
`scripts/make_font_subset.py` 固定到 **Regular（wght=400）** 并按固定字符集裁剪：

```bash
python scripts/make_font_subset.py --source <NotoSansSC-VF.ttf>
```

脚本只在**重新生成资源**时需要 fontTools；构建与单测直接读入仓的 `.ttf`，
所以字体字节是确定的，不随机器上装了什么字体而变。

| 项 | 值 |
|----|----|
| 源文件 | `NotoSansSC-VF.ttf`（Windows 字体目录同名文件，OFL） |
| 源 SHA256 | `763146584cf0710223441356b4395e279021b0806c196614377a7a0174ae074a` |
| 产物 | `NotoSansSC-Regular-Subset.ttf` |
| 产物 SHA256 | `81feff40b78f352e3765689262fbd59c7a1f2c06745d69dc85349f0294410db4` |
| 产物大小 | 1,235,112 字节 |
| 字形数 | 4491（原始 31036） |
| unitsPerEm | 1000 |

字符集（写死在脚本里，改它就必须重新生成并更新上表）：

- ASCII `0x20..0x7E`、Latin-1 补充 `0x00A0..0x00FF`；
- 常用标点 `U+2000..U+206F`、CJK 标点 `U+3000..U+303F`、全角形式 `U+FF01..U+FF5E`；
- **GB2312 一级汉字 3755 字**（0xB0A1..0xD7F9）。二级生僻字不收；
  缺字形时引擎如实报告（note 里点名缺字形，不画假字），宿主据此换字体。

## 许可

字体按 **SIL Open Font License 1.1** 分发，原文见同目录 `OFL.txt`。
版权声明保留在字体 `name` 表（nameID 0/13/14）里，裁剪脚本刻意不删名字表。

OFL 的要点与对本仓的约束：

- 可以随软件捆绑、嵌入、再分发，也可以修改；**不得单独售卖字体本身**；
- 修改版（本仓的 subset 就属于修改版）必须继续使用 OFL，且不得使用保留字体名
  （源字体的保留名是 `Source`，与本仓产物 `Noto Sans SC Subset` 无关）；
- 为了让改动一目了然，产物字体族名标为 `Noto Sans SC Subset`，不冒充完整字体。

完整组件与许可证索引见 `docs/THIRD-PARTY.md`。
