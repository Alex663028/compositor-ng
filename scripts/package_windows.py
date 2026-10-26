# AI生成
"""package_windows.py — 把 Windows 外壳打成可直接分发��绿色包。

产出：dist/Compositor-<版本>-win64/ 与同名 .zip

设计取舍：
- **不做安装器**。安装器要签名、要写注册表、卸载项，对当前阶段是纯负担；
  绿色包解压即用，反而方便内部传阅。
- **必须先跑 build.py 与 build_windows_shell.py**。本脚本只搬运，不编译，
  避免出现「打的包和刚验证的 exe 不是同一份」。
- **打包后自检一次**：在临时目录里跑 --selftest，确认 resources 的相对路径
  真的被找到（否则发出去才发现字体/Camera Raw 缺，属于典型漏项）。

用法：
    python scripts/package_windows.py
"""
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
DIST = os.path.join(ROOT, "dist")
VERSION = "0.1.0"
NAME = "Compositor-%s-win64" % VERSION

README = """Compositor {version} — Windows 版快速上手
=================================================

【启动】
双击 compositor_shell.exe。无需安装，但 resources\\ 目录必须与 exe 放在一起
（Camera Raw 的色彩表和中文/日韩字体在里面）。

依赖：只有 Windows 系统 DLL 与系统自带的 Universal CRT（Windows 10 及以上）。
不含 VC++ 运行库，也不硬依赖 Vulkan（GPU 后端在运行时探测，没有就回落 CPU）。
实测导入表 18 项，全部是系统组件。

【第一次上手】
1. 文件 > 新建文档…（Ctrl+Alt+N）：填宽高、分辨率、背景
2. 左侧工具栏选工具，右侧面板管图层 / 调整 / 滤镜
3. 做完：文件 > 另存为

【常用快捷键】
  Ctrl+N            立即新建（1280x720 透明）
  Ctrl+Alt+N        新建文档对话框
  Ctrl+Alt+I        图像大小
  Ctrl+Alt+C        画布大小
  Ctrl+O            打开
  Ctrl+S / Ctrl+Shift+S   保存 / 另存为
  Ctrl+Z / Ctrl+Y   撤销 / 重做
  Ctrl+A / Ctrl+D   全选 / 取消选区
  Ctrl+Shift+I      反选
  M                 矩形选框
  B / E             画笔 / 橡皮
  T                 文字
  R                 循环 Blur / Smudge / Liquify
  Alt+[ / Alt+]     调整笔刷半径

【能做什么】
- 图层：图层/组/嵌套、24 种混合模式、不透明度、图层蒙版、剪贴蒙版、
        25 种调整图层、10 种图层样式、智能对象与智能滤镜、图层复合、
        填充层、图层链接、按类型/颜色/名称过滤
- 选区：矩形/椭圆选框、套索、魔棒、色彩范围、羽化、扩展/收缩/边界/平滑、
        扩大/选取相似、存储与载入选区通道、变换选区
- 绘制：画笔（预设/流量/压感）、橡皮、仿制图章、涂抹、模糊、减淡加深海绵、
        修复画笔、污点修复、内容识别填充、液化、渐变五类型、形状
- 文字：可编辑文字图层、字体枚举与选择器、段落框与对齐、栅格化
- 调整与滤镜：25 种调整（含破坏性「图像 > 调整」20 项）、16 种滤镜、
        Camera Raw 12 行 Basic
- 文件：PNG / JPEG / PSD（自研分层读写）/ HDR / EXR 导入，
        PNG / JPEG / PSD / EXR / .comp 导出
- 视图：标尺 / 参考线 / 网格 / 对齐、HiDPI（Per-Monitor V2）、撤销历史面板

【还没做】
色彩管理（ICC / CMYK / Lab / 灰度）、打印与 PDF/X、动作与脚本、多文档标签、
ML 功能（选择主体 / 去背景 / Neural Filters）、TIFF / WebP / HEIF / SVG。

【已知限制】
- 只有 sRGB
- 单文档：新建会替换当前文档（会先询问是否保存）
- PSD 只支持常用的 8/16 位 RGB 子集；PSB、CMYK、32 位、智能对象会如实拒绝
- 超大文档受内存限制

【排查】
- 双击没反应：确认 resources\\ 与 exe 同目录
- 文字显示方框：缺 resources\\fonts\\NotoSansSC-Regular-Subset.ttf
- Camera Raw 提示「色彩表未加载」：缺 resources\\camera_raw_tables.bin
- 想确认程序本身是否正常：命令行跑
      compositor_shell.exe --selftest=report.txt
  应输出 [PASS]，全部断言 0 失败

【许可】
MIT。上游 robbietilton/Compositor（macOS 版）同许可，见 LICENSE.txt。
字体 / zlib / libpng / stb / FreeType 的许可见 THIRD-PARTY.md。
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def copy_tree(src, dst):
    if not os.path.isdir(src):
        print("[pack] 缺少目录：" + src)
        return False
    shutil.copytree(src, dst)
    return True


def main():
    exe = os.path.join(BUILD, "compositor_shell.exe")
    if not os.path.isfile(exe):
        print("[pack] 未找到 build/compositor_shell.exe，请先运行：")
        print("       python scripts/build.py && python scripts/build_windows_shell.py")
        return 2

    target = os.path.join(DIST, NAME)
    if os.path.isdir(target):
        shutil.rmtree(target)
    os.makedirs(target)

    shutil.copy2(exe, os.path.join(target, "compositor_shell.exe"))

    # 运行时资源：缺了程序仍能启动，但中文字体与 Camera Raw 会退化为不可用。
    resources = os.path.join(target, "resources")
    os.makedirs(resources)
    tables = os.path.join(ROOT, "resources", "camera_raw_tables.bin")
    if os.path.isfile(tables):
        shutil.copy2(tables, os.path.join(resources, "camera_raw_tables.bin"))
    else:
        print("[pack] 警告：缺 resources/camera_raw_tables.bin（Camera Raw 将不可用）")
    fonts = os.path.join(ROOT, "resources", "fonts")
    if os.path.isdir(fonts):
        shutil.copytree(fonts, os.path.join(resources, "fonts"))
    else:
        print("[pack] 警告：缺 resources/fonts（中文字体会退化为点阵）")

    upstream_license = os.path.join(ROOT, "..", "compositor", "LICENSE")
    if os.path.isfile(upstream_license):
        shutil.copy2(upstream_license, os.path.join(target, "LICENSE.txt"))
    third_party = os.path.join(ROOT, "docs", "THIRD-PARTY.md")
    if os.path.isfile(third_party):
        shutil.copy2(third_party, os.path.join(target, "THIRD-PARTY.md"))

    with open(os.path.join(target, "快速上手.txt"), "w", encoding="utf-8-sig") as fh:
        fh.write(README.format(version=VERSION))

    # 打包前自检：在包目录里跑一遍，确保 resources 的相对定位在当前布局下成立。
    report = os.path.join(target, "_selftest.txt")
    result = subprocess.run([os.path.join(target, "compositor_shell.exe"),
                             "--selftest=" + report],
                            cwd=target, capture_output=True, text=True)
    if os.path.isfile(report):
        with open(report, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        ok = "[PASS]" in text and "failures = 0" in text
        os.remove(report)
    else:
        ok = False
        text = ""
    if not ok:
        print("[pack] 包内自检失败，已中止打包：")
        print((result.stdout or "")[-500:])
        return 3

    archive = os.path.join(DIST, NAME + ".zip")
    if os.path.isfile(archive):
        os.remove(archive)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for base, _, files in os.walk(target):
            for name in files:
                full = os.path.join(base, name)
                zf.write(full, os.path.relpath(full, DIST))

    size_mb = os.path.getsize(archive) / (1024.0 * 1024.0)
    print("[pack] 目录：%s" % target)
    print("[pack] 压缩包：%s（%.1f MB）" % (archive, size_mb))
    print("[pack] exe  ：%.2f MB  sha256=%s"
          % (os.path.getsize(exe) / (1024.0 * 1024.0), sha256(exe)[:16]))
    # 项数从实际报告里数，不写死：写死的数字会随自检增项而变成谎话。
    # 另有几条依赖 build/ 探针夹具的断言在包目录里会 skip（包内没有 build/），
    # 单独报出来，免得「项数变少了」看着像回归。
    checked = text.count("[OK]") + text.count("[FAIL]")
    skipped = text.count("[skip]")
    print("[pack] 包内自检：PASS（%d 项断言 0 失败，%d 项跳过：缺 build/ 探针夹具）"
          % (checked, skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
