"""AI生成

comp_crosscheck 交叉验证驱动：用 PIL（独立 PNG 编解码器）与引擎互解，避免「自己验自己」。

流程：
  1) 编译 tools/comp_crosscheck.cpp（只走 C ABI），链接 build/engine_*.o；
  2) write 模式：引擎建 16x16 文档 + 确定性图案图层 → 保存 .comp → 导出写入的原始像素；
  3) 方向 A（引擎 → PIL）：PIL 解码 .comp 内 PNG，与引擎像素做三种口径比对；
  4) 方向 B（PIL → 引擎）：PIL 重写该 PNG → 引擎 load + thumbnail 取回 → 与原像素比对；
  5) 校验 manifest.json 的上游关键键名与取值。

用法：python scripts/crosscheck_comp.py
"""

from __future__ import annotations

import glob
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
OUT = BUILD / "crosscheck"
EXE = OUT / "comp_crosscheck.exe"

SIZE = 16
TOL = 1  # LSB


def log(message: str) -> None:
    print(message, flush=True)


def zig_env() -> dict:
    env = dict(os.environ)
    env.setdefault("LOCALAPPDATA", "<local-workspace>/tmp/localappdata")
    env.setdefault("HOME", "<local-workspace>/tmp/home")
    env.setdefault("XDG_CACHE_HOME", "<local-workspace>/tmp/xdgcache")
    return env


def build_host() -> bool:
    objects = sorted(glob.glob(str(BUILD / "engine_*.o")))
    if not objects:
        log("[build] 未找到 build/engine_*.o，请先运行 scripts/build.py")
        return False
    cmd = [
        sys.executable, "-m", "ziglang", "c++",
        str(ROOT / "tools" / "comp_crosscheck.cpp"), *objects,
        "-o", str(EXE), "-std=c++17", "-O2",
        "-I", str(ROOT / "engine"), "-I", str(ROOT / "engine" / "abi" / "include"),
        "-pthread",
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, env=zig_env())
    if result.returncode != 0:
        log("[build] 编译失败：")
        log((result.stdout or "")[-2000:])
        log((result.stderr or "")[-2000:])
        return False
    log(f"[build] OK  {EXE.name} ({EXE.stat().st_size:,} B, {len(objects)} objects)")
    return True


def run_mode(mode: str, comp_dir: Path, raw_out: Path) -> bool:
    result = subprocess.run([str(EXE), mode, str(comp_dir), str(raw_out)],
                            capture_output=True, text=True, env=zig_env())
    log((result.stdout or "").strip())
    if result.returncode != 0:
        log((result.stderr or "").strip())
    return result.returncode == 0


def channel_diff(a: np.ndarray, b: np.ndarray) -> tuple[int, int]:
    left = a.astype(np.int16)
    right = b.astype(np.int16)
    if left.shape != right.shape:
        return -1, -1
    delta = np.abs(left - right)
    return int((delta > TOL).sum()), int(delta.max())


def to_straight_rgb(arr: np.ndarray) -> np.ndarray:
    """预乘 RGBA8 -> 直通 RGB（0..255，整数四舍五入）。"""
    alpha = arr[..., 3:4].astype(np.float64) / 255.0
    rgb = arr[..., :3].astype(np.float64) / 255.0
    straight = np.divide(rgb, np.maximum(alpha, 1e-6))
    return np.clip(np.round(straight * 255.0), 0, 255).astype(np.uint8)


def to_premultiplied_rgba(rgb: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    """直通 RGB + A -> 预乘 RGBA8（整数四舍五入）。"""
    out = np.zeros((*rgb.shape[:2], 4), dtype=np.uint8)
    factor = alpha.astype(np.float64) / 255.0
    out[..., :3] = np.clip(np.round(rgb.astype(np.float64) * factor[..., None]), 0, 255).astype(np.uint8)
    out[..., 3] = alpha
    return out


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    if not build_host():
        return 1

    comp_dir = OUT / "sample.comp"
    pngs = glob.glob(str(comp_dir / "**" / "*.png"), recursive=True)
    for stale in pngs:
        os.remove(stale)

    raw_written = OUT / "written.raw"
    raw_decoded = OUT / "decoded.raw"
    for stale in (comp_dir, raw_written, raw_decoded):
        if stale.is_file():
            stale.unlink()

    # ---- 1) 引擎写 .comp ----
    if not run_mode("write", comp_dir, raw_written):
        log("[FAIL] 引擎写 .comp 失败")
        return 1

    written = np.frombuffer(raw_written.read_bytes(), np.uint8).reshape(SIZE, SIZE, 4)
    pngs = sorted(glob.glob(str(comp_dir / "**" / "*.png"), recursive=True))
    if not pngs:
        log("[FAIL] .comp 内无图层 PNG")
        return 1

    # 新建文档自带背景层，因此 .comp 内可能有多张 PNG；按「与引擎写入像素的贴合度」
    # 选中本次写入的那张（背景层为空白，必然不匹配）。
    candidates: list[tuple[Path, dict, tuple[int, int]]] = []
    for path_text in pngs:
        path = Path(path_text)
        try:
            arr = np.array(Image.open(path).convert("RGBA"), dtype=np.uint8)
        except Exception as exc:  # noqa: BLE001
            candidates.append((path, {"decode_error": str(exc)}, (10**9, 10**9)))
            continue
        if arr.shape[:2] != (SIZE, SIZE):
            candidates.append((path, {"shape": list(arr.shape)}, (10**9, 10**9)))
            continue
        detail = {
            "direct": channel_diff(written, arr),
            "straight": channel_diff(to_straight_rgb(written), arr[..., :3]),
            "premul": channel_diff(written, to_premultiplied_rgba(arr[..., :3], arr[..., 3])),
        }
        best_local = min(detail.values(), key=lambda item: (item[0], item[1]))
        candidates.append((path, detail, best_local))

    candidates.sort(key=lambda item: (item[2][0], item[2][1]))
    png_path, per_hypothesis, chosen = candidates[0]
    log(f"[info] .comp 内 PNG 数 = {len(pngs)}，选中 {png_path.name} ({png_path.stat().st_size:,} B)")
    for path, detail, score in candidates:
        log(f"        - {path.name}: 最优超容差={score[0]} 最大偏差={score[1]} ({detail})")
    if chosen[0] != 0:
        log("[FAIL] 没有任何图层 PNG 与引擎写入像素在容差内一致")
        return 1

    # ---- 2) 方向 A：引擎 -> PIL ----
    labels = {
        "direct": "PNG 存预乘 RGBA（与引擎内部像素同口径）",
        "straight": "PNG 存直通 RGBA（引擎写出时已解预乘）",
        "premul": "PNG 存直通 RGBA，PIL 侧再预乘后与引擎像素一致",
    }
    hypotheses = {labels[key]: per_hypothesis[key] for key in ("direct", "straight", "premul")}
    best = min(hypotheses.items(), key=lambda item: (item[1][0], item[1][1]))
    log("[方向A] 引擎像素 -> PNG -> PIL 解码，三种口径超容差通道数：")
    for name, (bad, worst) in hypotheses.items():
        log(f"        - {name}: 超容差={bad} 最大偏差={worst} LSB")
    log(f"[方向A] 判定口径 = {best[0]}（超容差 {best[1][0]}，最大偏差 {best[1][1]} LSB）")

    # ---- 3) 方向 B：PIL -> 引擎 ----
    alt = np.zeros((SIZE, SIZE, 4), dtype=np.uint8)
    for y in range(SIZE):
        for x in range(SIZE):
            a = 32 + ((x * 5 + y * 9) % 200)
            alt[y, x, 0] = (x * 11 + 5) % (a + 1)
            alt[y, x, 1] = (y * 19 + 7) % (a + 1)
            alt[y, x, 2] = (x + y * 3) % (a + 1)
            alt[y, x, 3] = a
    # 方向 A 已判定：.comp 的 PNG 存「直通（non-premultiplied）RGBA」。
    # 外部编码器写入时必须先解预乘，否则引擎读回时会再乘一次 alpha。
    straight_alt = np.dstack([to_straight_rgb(alt), alt[..., 3:4]])
    Image.fromarray(straight_alt, "RGBA").save(png_path)
    log(f"[info] PIL 重写图层 PNG（直通口径，{png_path.stat().st_size:,} B)")

    if not run_mode("read", comp_dir, raw_decoded):
        log("[FAIL] 引擎 re-load 失败")
        return 1
    engine_decoded = np.frombuffer(raw_decoded.read_bytes(), np.uint8).reshape(SIZE, SIZE, 4)
    round_bad, round_max = channel_diff(alt, engine_decoded)
    log(f"[方向B] PIL 写 PNG -> 引擎解码：超容差={round_bad} 最大偏差={round_max} LSB")

    # ---- 4) manifest 键名校验 ----
    manifest_path = comp_dir / "manifest.json"
    manifest_ok = False
    manifest_keys: list[str] = []
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest_keys = sorted(manifest.keys())
        required = ["format", "version", "documentID", "width", "height", "layers"]
        missing = [key for key in required if key not in manifest]
        manifest_ok = not missing
        log(f"[info] manifest 键 = {manifest_keys}")
        log(f"[info] format={manifest.get('format')!r} version={manifest.get('version')!r} "
            f"size={manifest.get('width')}x{manifest.get('height')} documentID={manifest.get('documentID')!r}")
        if missing:
            log(f"[FAIL] manifest 缺上游关键键: {missing}")
    else:
        log("[FAIL] 未找到 manifest.json")

    # ---- 5) 结论 ----
    verdict = (best[1][0] == 0 and round_bad == 0 and manifest_ok)
    report = {
        "host": str(EXE.relative_to(ROOT)),
        "png": str(png_path.relative_to(ROOT)),
        "direction_a": {name: {"over_tol_channels": bad, "max_delta_lsb": worst}
                        for name, (bad, worst) in hypotheses.items()},
        "direction_a_verdict": best[0],
        "direction_b": {"over_tol_channels": round_bad, "max_delta_lsb": round_max},
        "manifest": {"keys": manifest_keys, "required_present": manifest_ok},
        "tolerance_lsb": TOL,
        "verdict": "PASS" if verdict else "FAIL",
    }
    (OUT / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    log(f"[summary] 方向A PASS={best[1][0] == 0} 方向B PASS={round_bad == 0} manifest PASS={manifest_ok}")
    log(f"[{'PASS' if verdict else 'FAIL'}] .comp PNG 与外部编解码器（PIL）双向一致；报告 {OUT.relative_to(ROOT) / 'report.json'}")
    return 0 if verdict else 1


if __name__ == "__main__":
    raise SystemExit(main())
