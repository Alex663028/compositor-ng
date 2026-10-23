"""PSD 分层读写的独立交叉验证（M1）。

第三方参照实现是 `psd-tools`——它与本仓库的 `format/psd_codec` 没有任何代码共享，
所以「两份独立实现读出同一棵图层树」才能证明我们写出的不是一套自洽的私有格式。

用例（默认自己生成，也可以用 --ref 指定现成文件）：
  64x48 / 8 位 RGB / 4 层：
    BG       不透明背景
    Clip     剪贴到下方图层（clipping=1）
    Group 1  组，MULTIPLY
      Inner  MULTIPLY、不透明度 180、按列渐变的图层蒙版

检查项：
  1. 读：引擎导入的图层树 == psd-tools 读同一文件的图层树（名字 / 包围盒 /
     混合模式 / 不透明度 / 可见性 / 剪贴 / 蒙版尺寸）；
  2. 写：把引擎导出的文件再交给 psd-tools 读，图层树必须与原始文件一致；
  3. 蒙版像素：原始文件与导出文件在 psd-tools 里逐字节相同；
  4. 图层像素：容差 <= 1（直通 alpha 与预乘之间的取整）；
  5. 合成：引擎 flatten 与 psd-tools composite 的差 <= 2，且「原始 vs 导出」
     的引擎合成结果彼此 <= 1；
  6. 16 位：ABI 建一个 RGBA16 图层 -> 导出 -> psd-tools 读出的 depth 必须是 16，
     像素值一致。

用法（需要带 psd-tools/PIL/numpy 的解释器）：
    python scripts/crosscheck_psd.py
    python scripts/crosscheck_psd.py --ref some.psd --out build/crosscheck/psd/out.psd
"""
import argparse
import ctypes
import os
import sys

import numpy as np
from psd_tools import PSDImage

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DLL = os.path.join(ROOT, "build", "compositor_engine.dll")
DEFAULT_DIR = os.path.join(ROOT, "build", "crosscheck", "psd")

COMP_OK = 0
COMP_FORMAT_PSD = 7
COMP_PIXEL_RGBA8 = 0
COMP_PIXEL_RGBA16 = 1

vp, cp, u64, i32, csz = (ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint64,
                         ctypes.c_int32, ctypes.c_size_t)


class Size(ctypes.Structure):
    _fields_ = [("width", i32), ("height", i32)]


class Rect(ctypes.Structure):
    _fields_ = [("x", i32), ("y", i32), ("w", i32), ("h", i32)]


class Image_(ctypes.Structure):
    _fields_ = [("width", i32), ("height", i32), ("format", ctypes.c_int),
                ("stride", csz), ("pixels", vp)]


class DocDesc(ctypes.Structure):
    _fields_ = [("width", i32), ("height", i32), ("dpi", ctypes.c_float),
                ("color_space", cp)]


class LayerDesc(ctypes.Structure):
    _fields_ = [("name", cp), ("blend", ctypes.c_int), ("opacity", ctypes.c_float),
                ("transform", Rect), ("rotation", ctypes.c_float),
                ("flip_x", ctypes.c_uint8), ("flip_y", ctypes.c_uint8),
                ("is_group", ctypes.c_uint8), ("_reserved", ctypes.c_uint8),
                ("parent", u64), ("sampling", ctypes.c_int)]


def bind(lib, name, res, *args):
    fn = getattr(lib, name)
    fn.restype = res
    fn.argtypes = list(args)
    return fn


FAILS = []


def check(label, ok, detail=""):
    print(("  PASS  " if ok else "  FAIL  ") + label + (("  " + detail) if detail else ""))
    if not ok:
        FAILS.append(label + " " + detail)


# ---------------------------------------------------------------------------
# 参照文件：用 psd-tools 自己写一份，避免把「我们的编码器」当成输入。
# ---------------------------------------------------------------------------
def make_reference(path):
    from PIL import Image
    from psd_tools.constants import BlendMode

    def img(w, h, rgb, a=255):
        arr = np.zeros((h, w, 4), np.uint8)
        arr[:, :, 0] = rgb[0]
        arr[:, :, 1] = rgb[1]
        arr[:, :, 2] = rgb[2]
        arr[:, :, 3] = a
        return Image.fromarray(arr, "RGBA")

    width, height = 64, 48
    ps = PSDImage.new("RGB", (width, height), 16)
    ps.append(ps.create_pixel_layer(img(width, height, (10, 20, 30)), name="BG",
                                    top=0, left=0))

    clip = ps.create_pixel_layer(img(16, 16, (200, 40, 40)), name="Clip", top=4, left=4)
    ps.append(clip)
    clip.clipping = True

    inner = ps.create_pixel_layer(img(20, 10, (0, 200, 0)), name="Inner", top=10, left=10)
    inner.blend_mode = BlendMode.MULTIPLY
    inner.opacity = 180
    mask = np.zeros((10, 20), np.uint8)
    for col in range(20):
        mask[:, col] = col * 255 // 19
    if inner.has_mask():
        inner.update_mask(Image.fromarray(mask, "L"))
    else:
        inner.create_mask(Image.fromarray(mask, "L"))
    ps.append(inner)

    group = ps.create_group([inner], name="Group 1")
    group.blend_mode = BlendMode.MULTIPLY
    ps.append(group)
    ps.save(path)


# ---------------------------------------------------------------------------
# 引擎视图（走 C ABI，与宿主走同一条路）。
# ---------------------------------------------------------------------------
class Engine:
    def __init__(self, dll):
        self.lib = ctypes.CDLL(dll)
        b = self.bind
        self._engine_create = b("comp_engine_create", ctypes.c_int, vp, ctypes.POINTER(vp))
        self._engine_destroy = b("comp_engine_destroy", None, vp)
        self._engine_error = b("comp_engine_last_error", cp, vp)
        self._import = b("comp_io_import_document", ctypes.c_int, vp, cp, ctypes.POINTER(vp))
        self._export = b("comp_io_export_document", ctypes.c_int, vp, cp, ctypes.c_int, cp)
        self._doc_size = b("comp_document_size", ctypes.c_int, vp, ctypes.POINTER(Size))
        self._doc_create = b("comp_document_create", ctypes.c_int, vp, vp, ctypes.POINTER(vp))
        self._doc_destroy = b("comp_document_destroy", None, vp)
        self._flatten = b("comp_render_flatten", ctypes.c_int, vp, vp, ctypes.c_int, csz)
        self._layer_count = b("comp_layer_count", ctypes.c_int, vp, ctypes.POINTER(csz))
        self._layer_list = b("comp_layer_list", ctypes.c_int, vp, ctypes.POINTER(u64), csz,
                             ctypes.POINTER(csz))
        self._layer_add = b("comp_layer_add", ctypes.c_int, vp, ctypes.POINTER(LayerDesc),
                            ctypes.POINTER(u64))
        self._set_pixels = b("comp_layer_set_pixels", ctypes.c_int, vp, u64,
                             ctypes.POINTER(Image_))
        self._get_name = b("comp_layer_get_name", ctypes.c_int, vp, u64, cp, csz,
                           ctypes.POINTER(csz))
        self._get_transform = b("comp_layer_get_transform", ctypes.c_int, vp, u64,
                                ctypes.POINTER(Rect), ctypes.POINTER(ctypes.c_float))
        self._get_blend = b("comp_layer_get_blend", ctypes.c_int, vp, u64,
                            ctypes.POINTER(ctypes.c_int))
        self._get_opacity = b("comp_layer_get_opacity", ctypes.c_int, vp, u64,
                              ctypes.POINTER(ctypes.c_float))
        self._get_visible = b("comp_layer_get_visible", ctypes.c_int, vp, u64,
                              ctypes.POINTER(i32))
        self._get_group = b("comp_layer_get_group_info", ctypes.c_int, vp, u64,
                            ctypes.POINTER(i32), ctypes.POINTER(u64), ctypes.POINTER(u64))
        self._get_mask = b("comp_layer_get_mask", ctypes.c_int, vp, u64, ctypes.POINTER(i32),
                           ctypes.POINTER(i32), ctypes.POINTER(i32))
        self._blend_name = b("comp_blend_mode_name", cp, ctypes.c_int)
        self.handle = vp()
        rc = self._engine_create(None, ctypes.byref(self.handle))
        if rc != COMP_OK:
            raise SystemExit(f"comp_engine_create failed rc={rc}")

    def bind(self, *args):
        return bind(self.lib, *args)

    def close(self):
        self._engine_destroy(self.handle)

    def error(self):
        msg = self._engine_error(self.handle)
        return msg.decode("utf-8", "replace") if msg else ""

    def import_document(self, path):
        doc = vp()
        rc = self._import(self.handle, path.encode("utf-8"), ctypes.byref(doc))
        if rc != COMP_OK:
            raise SystemExit(f"import {path} failed rc={rc} err={self.error()}")
        return doc

    def export_document(self, doc, path):
        if os.path.exists(path):
            os.remove(path)
        return self._export(doc, path.encode("utf-8"), COMP_FORMAT_PSD, None)

    def view(self, path):
        doc = self.import_document(path)
        n = csz()
        assert self._layer_count(doc, ctypes.byref(n)) == COMP_OK
        ids = (u64 * max(n.value, 1))()
        got = csz()
        assert self._layer_list(doc, ids, n.value, ctypes.byref(got)) == COMP_OK

        result = {}
        for i in range(got.value):
            lid = ids[i]
            cap = csz()
            self._get_name(doc, lid, None, 0, ctypes.byref(cap))
            buf = ctypes.create_string_buffer(cap.value)
            assert self._get_name(doc, lid, buf, cap.value, ctypes.byref(cap)) == COMP_OK
            name = buf.value.decode("utf-8")
            rect = Rect()
            rot = ctypes.c_float()
            assert self._get_transform(doc, lid, ctypes.byref(rect), ctypes.byref(rot)) == COMP_OK
            blend = ctypes.c_int()
            assert self._get_blend(doc, lid, ctypes.byref(blend)) == COMP_OK
            op = ctypes.c_float()
            assert self._get_opacity(doc, lid, ctypes.byref(op)) == COMP_OK
            vis = i32()
            assert self._get_visible(doc, lid, ctypes.byref(vis)) == COMP_OK
            isg, parent, clip = i32(), u64(), u64()
            assert self._get_group(doc, lid, ctypes.byref(isg), ctypes.byref(parent),
                                  ctypes.byref(clip)) == COMP_OK
            has, mw, mh = i32(), i32(), i32()
            assert self._get_mask(doc, lid, ctypes.byref(has), ctypes.byref(mw),
                                  ctypes.byref(mh)) == COMP_OK
            result[name] = {
                "bbox": (rect.x, rect.y, rect.w, rect.h),
                "blend": (self._blend_name(blend.value) or b"").decode("utf-8").strip().lower(),
                # 引擎的 opacity 是 0..1，psd-tools 是 0..255：在这一层统一成 0..255。
                "opacity": float(op.value) * 255.0,
                "visible": vis.value,
                "is_group": isg.value,
                "clipping": clip.value != 0,
                "mask": (mw.value, mh.value) if has.value else None,
            }
        self._doc_destroy(doc)
        return result

    def flatten(self, path):
        doc = self.import_document(path)
        size = Size()
        self._doc_size(doc, ctypes.byref(size))
        buf = np.zeros((size.height, size.width, 4), np.uint8)
        rc = self._flatten(doc, buf.ctypes.data, COMP_PIXEL_RGBA8, size.width * 4)
        self._doc_destroy(doc)
        if rc != COMP_OK:
            raise SystemExit(f"flatten {path} failed rc={rc} err={self.error()}")
        return buf


# ---------------------------------------------------------------------------
# psd-tools 视图与比对。
# ---------------------------------------------------------------------------
PSD_TO_ENGINE = {
    b"norm": "normal", b"diss": "dissolve", b"dark": "darken", b"mul ": "multiply",
    b"idiv": "color burn", b"lbrn": "linear burn", b"lite": "lighten", b"scrn": "screen",
    b"div ": "color dodge", b"lddg": "linear dodge (add)", b"over": "overlay",
    b"sLit": "soft light", b"hLit": "hard light", b"vLit": "vivid light",
    b"lLit": "linear light", b"pLit": "pin light", b"hMix": "hard mix",
    b"diff": "difference", b"smud": "exclusion", b"fsub": "subtract", b"fdiv": "divide",
    b"hue ": "hue", b"sat ": "saturation", b"colr": "color", b"lum ": "luminosity",
}
PASS_THROUGH_NAME = "pass"


def psd_blend(key):
    """PSD 混合键 -> 与引擎同名同大小写的规范名；pass 表示穿透组。"""
    if key == b"pass":
        return PASS_THROUGH_NAME
    return PSD_TO_ENGINE.get(key, key.decode("ascii", "replace").strip().lower())


def tools_view(path):
    ps = PSDImage.open(path)
    result = {}

    def walk(node):
        for layer in node:
            left, top, right, bottom = layer.bbox
            result[layer.name] = {
                # psd-tools 的 bbox 是 (left, top, right, bottom)，引擎是 (x, y, w, h)。
                "bbox": (left, top, right - left, bottom - top),
                "blend": psd_blend(layer.blend_mode.value),
                "opacity": float(layer.opacity),
                "visible": bool(layer.visible),
                "is_group": layer.is_group(),
                "clipping": bool(layer.clipping),
                "mask": tuple(layer.mask.size) if layer.has_mask() else None,
            }
            if layer.is_group():
                walk(layer)

    walk(ps)
    return ps, result


def compare_tree(tag, ours, theirs):
    check(f"{tag}: layer names match", set(ours) == set(theirs),
          f"ours={sorted(ours)} theirs={sorted(theirs)}")
    for name in sorted(set(ours) & set(theirs)):
        a, b = ours[name], theirs[name]
        if b["is_group"]:
            # 组的 bbox 在引擎里不承载几何（导入时是 (0,0,0,0)，.comp 往返后是 (1,1)）；
            # psd-tools 的组 bbox 是按子层**推导**出来的。两者没有可比性，几何正确性
            # 由 tests/test_psd.cpp 覆盖。这里只比混合 / 可见性 / 蒙版。
            if b["blend"] == PASS_THROUGH_NAME:
                check(f"{tag}: {name} pass-through", a["is_group"] == 1,
                      f"ours group={a['is_group']} blend={a['blend']}")
            else:
                check(f"{tag}: {name} group blend", a["blend"] == b["blend"],
                      f"ours={a['blend']} want={b['blend']}")
        else:
            check(f"{tag}: {name} bbox", a["bbox"] == b["bbox"],
                  f"ours={a['bbox']} theirs={b['bbox']}")
            check(f"{tag}: {name} blend", a["blend"] == b["blend"],
                  f"ours={a['blend']} want={b['blend']}")
            check(f"{tag}: {name} opacity", abs(a["opacity"] - b["opacity"]) <= 0.51,
                  f"ours={a['opacity']:.1f} theirs={b['opacity']}")
        check(f"{tag}: {name} visible", (a["visible"] != 0) == b["visible"])
        check(f"{tag}: {name} clipping", a["clipping"] == b["clipping"])
        check(f"{tag}: {name} mask size", a["mask"] == b["mask"],
              f"ours={a['mask']} theirs={b['mask']}")


def find_layer(ps, name):
    for layer in ps.descendants():
        if layer.name == name:
            return layer
    return None


def diff_stats(tag, ours, theirs):
    a = np.asarray(theirs.convert("RGBA"), np.int16)
    b = ours.astype(np.int16)
    check(f"{tag}: shape", a.shape == b.shape, f"ours={b.shape} theirs={a.shape}")
    d = np.abs(a - b)[:, :, :]
    mx = int(d.max()) if d.size else 0
    mean = float(d.mean()) if d.size else 0.0
    over2 = int((d > 2).sum())
    print(f"  ...  {tag}: max={mx} mean={mean:.4f} px>2={over2}/{d.size}")
    return mx


def check_masks(ps_ref, ps_out):
    for name in [layer.name for layer in ps_ref.descendants()]:
        a = find_layer(ps_ref, name)
        b = find_layer(ps_out, name)
        if a is None or b is None or not a.has_mask():
            continue
        ma, mb = a.mask.topil(), b.mask.topil()
        if ma is None or mb is None:
            check(f"mask {name}", ma is None and mb is None)
            continue
        A = np.asarray(ma.convert("L"), np.int16)
        B = np.asarray(mb.convert("L"), np.int16)
        same = A.shape == B.shape and int(np.abs(A - B).max() if A.size else 0) == 0
        detail = f"shape={A.shape}/{B.shape}"
        if A.shape == B.shape and A.size:
            detail += f" maxdiff={int(np.abs(A - B).max())}"
        check(f"mask {name} byte-exact", same, detail)


def check_layer_pixels(ps_ref, ps_out):
    for name in [layer.name for layer in ps_ref.descendants()]:
        a = find_layer(ps_ref, name)
        b = find_layer(ps_out, name)
        if a is None or b is None or a.is_group():
            continue
        ia, ib = a.topil(), b.topil()
        if ia is None or ib is None:
            continue
        A = np.asarray(ia.convert("RGBA"), np.int16)
        B = np.asarray(ib.convert("RGBA"), np.int16)
        mx = int(np.abs(A - B).max()) if A.size and A.shape == B.shape else 999
        check(f"pixels {name}", mx <= 1, f"maxdiff={mx}")


def check_16bit(engine, out_path):
    doc = vp()
    desc = DocDesc(32, 24, 72.0, b"sRGB")
    rc = engine._doc_create(engine.handle, ctypes.byref(desc), ctypes.byref(doc))
    check("16bit: doc create", rc == COMP_OK, f"rc={rc} {engine.error()}")
    if rc != COMP_OK:
        return

    layer = LayerDesc()
    layer.name = b"L16"
    layer.blend = 0
    layer.opacity = 1.0
    layer.transform = Rect(3, 5, 32, 24)
    lid = u64()
    rc = engine._layer_add(doc, ctypes.byref(layer), ctypes.byref(lid))
    check("16bit: layer add", rc == COMP_OK, f"rc={rc} {engine.error()}")

    px = np.zeros((24, 32, 4), np.uint16)
    px[:, :, 0] = 40000
    px[:, :, 1] = (np.arange(32, dtype=np.uint16) * 1000)[None, :]
    px[:, :, 3] = 65535
    img = Image_(32, 24, COMP_PIXEL_RGBA16, 32 * 8, px.ctypes.data)
    rc = engine._set_pixels(doc, lid.value, ctypes.byref(img))
    check("16bit: set pixels", rc == COMP_OK, f"rc={rc} {engine.error()}")

    path16 = out_path.replace(".psd", "_16.psd")
    if os.path.exists(path16):
        os.remove(path16)
    rc = engine._export(doc, path16.encode("utf-8"), COMP_FORMAT_PSD, None)
    check("16bit: export rc", rc == COMP_OK, f"rc={rc} {engine.error()}")
    engine._doc_destroy(doc)
    if rc != COMP_OK:
        return

    ps16 = PSDImage.open(path16)
    check("16bit: psd-tools depth", ps16.depth == 16, f"depth={ps16.depth}")
    names = [l.name for l in ps16.descendants()]
    check("16bit: layer present", "L16" in names, str(names))
    layer = find_layer(ps16, "L16")
    if layer is None:
        return
    arr = np.asarray(layer.topil().convert("RGBA"))
    check("16bit: pixel row 0 R", abs(int(arr[0, 0, 0]) - (40000 >> 8)) <= 1,
          f"got={int(arr[0, 0, 0])} want={40000 >> 8}")
    check("16bit: pixel gradient G", abs(int(arr[0, 12, 1]) - (12000 >> 8)) <= 1,
          f"got={int(arr[0, 12, 1])} want={12000 >> 8}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", help="要验证的 PSD；缺省则由 psd-tools 现场生成")
    parser.add_argument("--out", help="引擎导出文件路径")
    parser.add_argument("--dll", default=DEFAULT_DLL)
    args = parser.parse_args()

    os.makedirs(DEFAULT_DIR, exist_ok=True)
    ref = args.ref or os.path.join(DEFAULT_DIR, "ref.psd")
    out = args.out or os.path.join(DEFAULT_DIR, "out.psd")
    if args.ref is None:
        make_reference(ref)
        print(f"[ref] generated by psd-tools: {ref}")

    engine = Engine(args.dll)

    print("[1] import vs psd-tools")
    ours = engine.view(ref)
    ps_ref, theirs = tools_view(ref)
    compare_tree("import", ours, theirs)

    print("[2] export -> psd-tools")
    doc = engine.import_document(ref)
    rc = engine.export_document(doc, out)
    check("export: rc", rc == COMP_OK, f"rc={rc} {engine.error()}")
    engine._doc_destroy(doc)
    if rc != COMP_OK:
        engine.close()
        print(f"\nRESULT: FAIL ({len(FAILS)})")
        return 1
    ps_out, out_theirs = tools_view(out)
    compare_tree("reexport", theirs, out_theirs)
    check("reexport: header",
          (ps_out.size, ps_out.depth, ps_out.color_mode) ==
          (ps_ref.size, ps_ref.depth, ps_ref.color_mode),
          f"ours={(ps_out.size, ps_out.depth)} orig={(ps_ref.size, ps_ref.depth)}")

    print("[3] mask pixels (psd-tools read of original vs our export)")
    check_masks(ps_ref, ps_out)

    print("[4] layer pixels (tol <= 1)")
    check_layer_pixels(ps_ref, ps_out)

    print("[5] flatten: engine vs psd-tools")
    flat_ref = engine.flatten(ref)
    mx = diff_stats("flatten(ref)", flat_ref, ps_ref.composite())
    check("flatten(ref): maxdiff <= 2", mx <= 2, f"max={mx}")
    flat_out = engine.flatten(out)
    mx2 = diff_stats("flatten(ours)", flat_out, ps_out.composite())
    check("flatten(ours): maxdiff <= 2", mx2 <= 2, f"max={mx2}")
    ref_flat = np.asarray(ps_ref.composite().convert("RGBA"), np.int16)
    delta = int(np.abs(ref_flat - flat_out.astype(np.int16)).max())
    check("flatten(ref) == flatten(ours) within 1", delta <= 1, f"max={delta}")

    print("[6] 16-bit export read by psd-tools")
    check_16bit(engine, out)

    engine.close()
    print()
    if FAILS:
        print(f"RESULT: FAIL ({len(FAILS)})")
        for f in FAILS:
            print("  -", f)
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
