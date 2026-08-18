#!/usr/bin/env python3
"""把下载的播放控件图标（通常 32×32）高质量缩放到圆屏按钮里的实际显示尺寸。

用法（二选一）：

1) 只改下面数字，然后执行：
     python3 tools/resize_player_icons.py

2) 命令行指定：
     python3 tools/resize_player_icons.py --from 32 --play 24 --step 18

目录约定：
  - 原图：main/custom-assets/player_masters/
      文件名任意（中文/英文均可），如「播放.png」「暂停.png」「上一首.png」
  - 生成：main/custom-assets/player_icons/
      自动映射为固件名 ic_s_player_*.png / ic_s_music_volume_*.png
  - 安装：默认再复制到 main/xingzhi-assets/ 供编译打包

圆屏按钮里图标区 = 按钮直径 × 11/20：
  播放/暂停 最大 24×24（电台 44 按钮）
  上一首/下一首/音量 最大 18×18（34 按钮）
"""
from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

from PIL import Image

# ========== 只改这里即可 ==========
FROM_SIZE = 32  # 下载图标边长
PLAY_SIZE = 24  # 播放 / 暂停
STEP_SIZE = 18  # 上一首 / 下一首 / 音量加减
# ==================================

ROOT = Path(__file__).resolve().parent.parent
CUSTOM_ASSETS = ROOT / "main" / "custom-assets"
MASTERS_DIR = CUSTOM_ASSETS / "player_masters"
OUT_DIR = CUSTOM_ASSETS / "player_icons"
INSTALL_DIR = ROOT / "main" / "xingzhi-assets"

# 任意文件名 → (固件文件名不含扩展名, 目标边长键)
# 目标：play=PLAY_SIZE，其余=STEP_SIZE
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


def resize_rgba(im: Image.Image, from_size: int, to_size: int) -> Image.Image:
    """高质量缩放：先规范到 from_size 画布，再 LANCZOS 到 to_size。"""
    im = im.convert("RGBA")
    w, h = im.size

    if w == to_size and h == to_size:
        return im

    if w != from_size or h != from_size:
        canvas = Image.new("RGBA", (from_size, from_size), (0, 0, 0, 0))
        scale = min(from_size / w, from_size / h)
        nw = max(1, int(round(w * scale)))
        nh = max(1, int(round(h * scale)))
        fitted = im.resize((nw, nh), Image.Resampling.LANCZOS)
        canvas.paste(fitted, ((from_size - nw) // 2, (from_size - nh) // 2), fitted)
        im = canvas

    if from_size == to_size:
        return im

    return im.resize((to_size, to_size), Image.Resampling.LANCZOS)


def collect_sources(masters: Path, pattern: str) -> list[Path]:
    return [p for p in sorted(masters.glob(pattern)) if p.is_file() and p.suffix.lower() != ".txt"]


def ensure_masters_readme(masters: Path) -> None:
    readme = masters / "README.txt"
    readme.write_text(
        "把 32×32 的 PNG 放此目录（中文名也可），例如：\n"
        "  播放.png / 暂停.png / 上一首.png / 下一首.png\n"
        "  音量加.png / 音量减.png\n"
        "  或 ic_s_player_play.png\n"
        "然后运行：\n"
        "  python3 tools/resize_player_icons.py\n"
        "生成：main/custom-assets/player_icons/\n"
        "  播放/暂停 → 24×24（ic_s_player_play / pause）\n"
        "  切歌/音量 → 18×18（previous / next / volume_up / volume_down）\n"
        "并默认复制到 main/xingzhi-assets/ 供编译。\n",
        encoding="utf-8",
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="高质量缩放播放控件图标：32×32 → 播放24 / 切歌18"
    )
    ap.add_argument("--from", dest="from_size", type=int, default=FROM_SIZE)
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

    from_size = args.from_size
    play_size = args.play_size
    step_size = args.step_size
    if from_size <= 0 or play_size <= 0 or step_size <= 0:
        print("error: --from / --play / --step 必须为正整数", file=sys.stderr)
        return 1
    for label, size in (("播放", play_size), ("切歌", step_size)):
        if size > from_size:
            print(
                f"warn: {label} 目标 {size} > 原图 {from_size}，会放大，清晰度可能下降",
                file=sys.stderr,
            )

    masters: Path = args.masters
    out_dir: Path = args.out

    CUSTOM_ASSETS.mkdir(parents=True, exist_ok=True)
    if not masters.is_dir():
        masters.mkdir(parents=True, exist_ok=True)
        ensure_masters_readme(masters)
        print(
            f"已创建原图目录：{masters}\n"
            f"请把 {from_size}×{from_size} 的 PNG（播放/暂停/上一首/下一首）放进去后再运行。"
        )
        return 1

    ensure_masters_readme(masters)
    srcs = collect_sources(masters, args.pattern)
    if not srcs:
        print(
            f"error: 在 {masters} 下未找到 {args.pattern}\n"
            f"请放入 {from_size}×{from_size} PNG 后再运行。",
            file=sys.stderr,
        )
        return 1

    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"缩放 {from_size} → 播放/暂停 {play_size}，切歌/音量 {step_size}")
    print(f"  原图：{masters}")
    print(f"  生成：{out_dir}")
    print(f"  共 {len(srcs)} 张")

    written: list[Path] = []
    unmatched: list[str] = []
    for src in srcs:
        suffix = resolve_suffix(src)
        if suffix is None:
            unmatched.append(src.name)
            print(f"  {src.name}  [未映射，跳过]")
            continue
        to_size = target_size_for(suffix, play_size, step_size)
        out_name = SUFFIX_TO_ASSET[suffix]
        im = Image.open(src)
        out = resize_rgba(im, from_size, to_size)
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
