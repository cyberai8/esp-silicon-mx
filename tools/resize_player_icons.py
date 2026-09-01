#!/usr/bin/env python3
"""把 player_masters 里的原图高质量缩放到圆屏按钮实际显示尺寸。

用法：
  python3 tools/resize_player_icons.py
  python3 tools/resize_player_icons.py --play 44 --step 34

目录约定：
  - 原图：main/custom-assets/player_masters/
  - 生成：main/custom-assets/player_icons/
  - 安装：默认再复制到 main/xingzhi-assets/ 供编译打包
"""
from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

from PIL import Image

# ========== 只改这里即可（与 music_screen_sd 布局常量对齐）==========
PLAY_SIZE = 44  # 播放 / 暂停
STEP_SIZE = 34  # 上一首 / 下一首 / 音量加减
# ==================================================================

ROOT = Path(__file__).resolve().parent.parent
CUSTOM_ASSETS = ROOT / "main" / "custom-assets"
MASTERS_DIR = CUSTOM_ASSETS / "player_masters"
OUT_DIR = CUSTOM_ASSETS / "player_icons"
INSTALL_DIR = ROOT / "main" / "xingzhi-assets"

NAME_ALIASES: dict[str, str] = {
    "play": "play",
    "播放": "play",
    "播放器": "play",
    "pause": "pause",
    "暂停": "pause",
    "previous": "previous",
    "prev": "previous",
    "上一首": "previous",
    "上一曲": "previous",
    "next": "next",
    "下一首": "next",
    "下一曲": "next",
    "volume_up": "volume_up",
    "vol_up": "volume_up",
    "音量加": "volume_up",
    "音量+": "volume_up",
    "volume_down": "volume_down",
    "vol_down": "volume_down",
    "音量减": "volume_down",
    "音量-": "volume_down",
}

SUFFIX_TO_ASSET: dict[str, str] = {
    "play": "ic_s_player_play.png",
    "pause": "ic_s_player_pause.png",
    "previous": "ic_s_player_previous.png",
    "next": "ic_s_player_next.png",
    "volume_up": "ic_s_music_volume_up.png",
    "volume_down": "ic_s_music_volume_down.png",
}

PLAY_SUFFIXES = {"play", "pause"}


def normalize_key(stem: str) -> str:
    s = stem.strip()
    s = re.sub(r"\(\d+\)", "", s)
    s = re.sub(r"[_\-\s]+", "", s)
    return s.casefold()


def resolve_suffix(src: Path) -> str | None:
    stem = src.stem
    for prefix in ("ic_s_player_", "ic_s_music_"):
        if stem.startswith(prefix):
            rest = stem[len(prefix) :]
            if rest in SUFFIX_TO_ASSET:
                return rest
            if rest in ("volume_up", "volume_down", "play", "pause", "previous", "next"):
                return rest

    key = normalize_key(stem)
    for alias, suffix in NAME_ALIASES.items():
        if normalize_key(alias) == key:
            return suffix
    best: tuple[int, str] | None = None
    for alias, suffix in NAME_ALIASES.items():
        ak = normalize_key(alias)
        if ak and ak in key:
            if best is None or len(ak) > best[0]:
                best = (len(ak), suffix)
    return None if best is None else best[1]


def target_size_for(suffix: str, play_size: int, step_size: int) -> int:
    return play_size if suffix in PLAY_SUFFIXES else step_size


def resize_to_square(im: Image.Image, to_size: int) -> Image.Image:
    """按原图比例缩放到 to_size 正方形画布（透明底，居中）。"""
    im = im.convert("RGBA")
    w, h = im.size
    if w == to_size and h == to_size:
        return im

    scale = min(to_size / w, to_size / h)
    nw = max(1, int(round(w * scale)))
    nh = max(1, int(round(h * scale)))
    fitted = im.resize((nw, nh), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (to_size, to_size), (0, 0, 0, 0))
    canvas.paste(fitted, ((to_size - nw) // 2, (to_size - nh) // 2), fitted)
    return canvas


def collect_sources(masters: Path, pattern: str) -> list[Path]:
    # 「暂停.png」优先于「暂停 (1).png」
    files = [
        p
        for p in masters.glob(pattern)
        if p.is_file() and p.suffix.lower() != ".txt"
    ]
    return sorted(
        files,
        key=lambda p: (1 if re.search(r"\(\d+\)", p.stem) else 0, p.name),
    )


def ensure_masters_readme(masters: Path, play_size: int, step_size: int) -> None:
    readme = masters / "README.txt"
    readme.write_text(
        "把原图 PNG 放此目录（中文名也可，尺寸随意），例如：\n"
        "  播放.png / 暂停.png / 上一首.png / 下一首.png\n"
        "  音量加.png / 音量减.png\n"
        "然后运行：\n"
        "  python3 tools/resize_player_icons.py\n"
        f"生成：main/custom-assets/player_icons/\n"
        f"  播放/暂停 → {play_size}×{play_size}\n"
        f"  切歌/音量 → {step_size}×{step_size}\n"
        "并默认复制到 main/xingzhi-assets/ 供编译。\n",
        encoding="utf-8",
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="高质量缩放播放控件图标到目标显示尺寸")
    ap.add_argument("--play", dest="play_size", type=int, default=PLAY_SIZE)
    ap.add_argument("--step", dest="step_size", type=int, default=STEP_SIZE)
    ap.add_argument("--masters", type=Path, default=MASTERS_DIR)
    ap.add_argument("--out", type=Path, default=OUT_DIR)
    ap.add_argument("--pattern", default="*.png")
    ap.add_argument(
        "--no-install",
        action="store_true",
        help="不复制到 main/xingzhi-assets",
    )
    args = ap.parse_args()

    play_size = args.play_size
    step_size = args.step_size
    if play_size <= 0 or step_size <= 0:
        print("error: --play / --step 必须为正整数", file=sys.stderr)
        return 1

    masters: Path = args.masters
    out_dir: Path = args.out

    CUSTOM_ASSETS.mkdir(parents=True, exist_ok=True)
    if not masters.is_dir():
        masters.mkdir(parents=True, exist_ok=True)
        ensure_masters_readme(masters, play_size, step_size)
        print(
            f"已创建原图目录：{masters}\n"
            "请把 PNG（播放/暂停/上一首/下一首）放进去后再运行。"
        )
        return 1

    ensure_masters_readme(masters, play_size, step_size)
    srcs = collect_sources(masters, args.pattern)
    if not srcs:
        print(f"error: 在 {masters} 下未找到 {args.pattern}", file=sys.stderr)
        return 1

    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"缩放 → 播放/暂停 {play_size}，切歌/音量 {step_size}")
    print(f"  原图：{masters}")
    print(f"  生成：{out_dir}")
    print(f"  共 {len(srcs)} 张")

    written: list[Path] = []
    unmatched: list[str] = []
    seen_suffix: set[str] = set()
    for src in srcs:
        suffix = resolve_suffix(src)
        if suffix is None:
            unmatched.append(src.name)
            print(f"  {src.name}  [未映射，跳过]")
            continue
        # 同名映射只保留第一次（优先无「暂停.png」而不是「暂停 (1).png」）
        if suffix in seen_suffix:
            print(f"  {src.name}  [重复映射 {suffix}，跳过]")
            continue
        seen_suffix.add(suffix)
        to_size = target_size_for(suffix, play_size, step_size)
        out_name = SUFFIX_TO_ASSET[suffix]
        im = Image.open(src)
        out = resize_to_square(im, to_size)
        dst = out_dir / out_name
        out.save(dst, format="PNG", optimize=True)
        written.append(dst)
        print(
            f"  {src.name} → {out_name}  "
            f"{im.size[0]}x{im.size[1]}→{to_size}x{to_size}"
        )

    if unmatched:
        print(
            "提示：未映射的文件名可在脚本 NAME_ALIASES 里补一条，"
            "或改名为 ic_s_player_play.png 这类固件名。",
            file=sys.stderr,
        )

    if not args.no_install and written:
        INSTALL_DIR.mkdir(parents=True, exist_ok=True)
        for dst in written:
            shutil.copy2(dst, INSTALL_DIR / dst.name)
        print(f"已安装 {len(written)} 张到：{INSTALL_DIR}")
    elif not args.no_install and not written:
        print("warn: 没有可安装的映射图标", file=sys.stderr)

    print("完成。接下来编译烧录即可。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
