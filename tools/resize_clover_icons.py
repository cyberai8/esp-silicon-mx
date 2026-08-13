#!/usr/bin/env python3
"""把下载的四叶瓣图标（通常 128×128）高质量缩放到固件显示尺寸。

用法（二选一）：

1) 只改下面两个数字，然后执行：
     python3 tools/resize_clover_icons.py

2) 命令行指定：
     python3 tools/resize_clover_icons.py --from 128 --to 80

目录约定（优先用 main/custom-assets）：
  - 原图：main/custom-assets/clover_masters/
      文件名任意（中文/英文均可），如「AI聊天.png」「电台.png」
  - 生成：main/custom-assets/clover_<TO_SIZE>/
      自动映射为固件名 ic_clover_<suffix>.png
  - 安装：默认再复制到 main/xingzhi-assets/ 供编译打包
  - 默认同步 home_screen.cc 圆屏 kCloverIconFrame = TO_SIZE
"""
from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

from PIL import Image

# ========== 只改这里即可 ==========
FROM_SIZE = 128  # 下载图标边长
TO_SIZE = 80  # 固件显示边长（须与 kCloverIconFrame 圆屏一致）
# ==================================

ROOT = Path(__file__).resolve().parent.parent
CUSTOM_ASSETS = ROOT / "main" / "custom-assets"
MASTERS_DIR = CUSTOM_ASSETS / "clover_masters"
INSTALL_DIR = ROOT / "main" / "xingzhi-assets"
HOME_CC = ROOT / "main" / "display" / "screen" / "home_screen" / "home_screen.cc"

# 任意文件名 → 固件 icon_suffix（匹配时忽略空格、括号序号、大小写）
# 也可直接使用 ic_clover_xxx.png / xxx.png（xxx 为 suffix）
NAME_ALIASES: dict[str, str] = {
    # 聊天
    "chat": "chat",
    "聊天": "chat",
    "ai聊天": "chat",
    "aichat": "chat",
    # 网络
    "wifi": "wifi",
    "网络": "wifi",
    "网络配置": "wifi",
    # 数字人
    "digital_people": "digital_people",
    "数字人": "digital_people",
    "ai数字人": "digital_people",
    # 电话
    "call": "call",
    "电话": "call",
    # 音乐
    "music": "music",
    "音乐": "music",
    "音乐厅": "music",
    # 日历
    "calendar": "calendar",
    "日历": "calendar",
    # 地图 / GPS
    "gps": "gps",
    "地图": "gps",
    # 水平仪
    "spirit_level": "spirit_level",
    "水平仪": "spirit_level",
    # 磁场
    "magnet": "magnet",
    "磁场": "magnet",
    # 震动
    "vibrate": "vibrate",
    "震动": "vibrate",
    # 计算器
    "calculator": "calculator",
    "计算器": "calculator",
    # 天气
    "weather": "weather",
    "天气": "weather",
    # SD
    "sd": "sd",
    "sd卡": "sd",
    # 引脚
    "pin": "pin",
    "引脚测试": "pin",
    "引脚": "pin",
    # 2048
    "2048": "2048",
    # 信息
    "info": "info",
    "信息": "info",
    # 测试
    "test": "test",
    "测试": "test",
    # 设置
    "settings": "settings",
    "设置": "settings",
    # 电台
    "radio": "radio",
    "电台": "radio",
    # 录音
    "recording": "recording",
    "录音": "recording",
    # AI 生图
    "ai_image_gen": "ai_image_gen",
    "ai生图": "ai_image_gen",
    "生图": "ai_image_gen",
    # 翻译
    "translate": "translate",
    "翻译": "translate",
    # 主题 / claw（历史资源）
    "theme": "theme",
    "主题": "theme",
    "espclaw": "espclaw",
    "chat1": "chat1",
    # 闹钟 / 时钟
    "alarm": "alarm",
    "闹钟": "alarm",
    "时钟": "alarm",
}


def out_dir_for(to_size: int) -> Path:
    return CUSTOM_ASSETS / f"clover_{to_size}"


def normalize_key(stem: str) -> str:
    s = stem.strip()
    s = re.sub(r"\(\d+\)", "", s)  # 音乐厅 (1) → 音乐厅
    s = re.sub(r"[_\-\s]+", "", s)
    return s.casefold()


def resolve_output_name(src: Path) -> tuple[str, str | None]:
    """返回 (输出文件名, suffix或None)。已是 ic_clover_* 则直接用。"""
    stem = src.stem
    if stem.startswith("ic_clover_"):
        suffix = stem[len("ic_clover_") :]
        return f"ic_clover_{suffix}.png", suffix

    key = normalize_key(stem)
    # 精确别名
    for alias, suffix in NAME_ALIASES.items():
        if normalize_key(alias) == key:
            return f"ic_clover_{suffix}.png", suffix
    # 包含匹配（较长别名优先）
    best: tuple[int, str] | None = None
    for alias, suffix in NAME_ALIASES.items():
        ak = normalize_key(alias)
        if ak and ak in key:
            if best is None or len(ak) > best[0]:
                best = (len(ak), suffix)
    if best is not None:
        return f"ic_clover_{best[1]}.png", best[1]

    # 未映射：保留原名，安装时跳过（固件认不上）
    return src.name, None


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


def patch_home_frame(to_size: int) -> bool:
    if not HOME_CC.is_file():
        print(f"warn: 找不到 {HOME_CC}，跳过同步显示框", file=sys.stderr)
        return False
    text = HOME_CC.read_text(encoding="utf-8")
    pat = re.compile(
        r"(constexpr int kCloverIconFrame = kLayoutRoundSmall \? )\d+( : \d+;)"
    )
    new_text, n = pat.subn(rf"\g<1>{to_size}\2", text, count=1)
    if n == 0:
        print("warn: 未匹配到 kCloverIconFrame，请手动改 home_screen.cc", file=sys.stderr)
        return False
    if new_text == text:
        print(f"kCloverIconFrame 圆屏已是 {to_size}，无需修改")
        return True
    HOME_CC.write_text(new_text, encoding="utf-8")
    print(f"已同步 home_screen.cc：圆屏 kCloverIconFrame = {to_size}")
    return True


def collect_sources(masters: Path, pattern: str) -> list[Path]:
    return [p for p in sorted(masters.glob(pattern)) if p.is_file()]


def ensure_masters_readme(masters: Path) -> None:
    readme = masters / "README.txt"
    readme.write_text(
        "把任意文件名的原尺寸 PNG 放此目录（中文名也可），例如：\n"
        "  AI聊天.png / 电台.png / 音乐厅.png / ic_clover_wifi.png\n"
        "然后运行：\n"
        "  python3 tools/resize_clover_icons.py\n"
        "生成：main/custom-assets/clover_<目标边长>/ic_clover_*.png\n"
        "并默认复制到 main/xingzhi-assets/ 供编译。\n",
        encoding="utf-8",
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="高质量缩放四叶瓣图标：文件名任意，自动映射为 ic_clover_*.png"
    )
    ap.add_argument("--from", dest="from_size", type=int, default=FROM_SIZE)
    ap.add_argument("--to", dest="to_size", type=int, default=TO_SIZE)
    ap.add_argument("--masters", type=Path, default=MASTERS_DIR)
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument(
        "--pattern",
        default="*.png",
        help="匹配原图的 glob（默认 *.png，文件名任意）",
    )
    ap.add_argument("--no-patch-frame", action="store_true")
    ap.add_argument(
        "--no-install",
        action="store_true",
        help="不复制到 main/xingzhi-assets",
    )
    args = ap.parse_args()

    from_size = args.from_size
    to_size = args.to_size
    if from_size <= 0 or to_size <= 0:
        print("error: --from / --to 必须为正整数", file=sys.stderr)
        return 1
    if to_size > from_size:
        print(
            f"warn: 目标 {to_size} > 原图 {from_size}，会放大，清晰度可能下降",
            file=sys.stderr,
        )

    masters: Path = args.masters
    out_dir: Path = args.out if args.out is not None else out_dir_for(to_size)

    CUSTOM_ASSETS.mkdir(parents=True, exist_ok=True)
    if not masters.is_dir():
        masters.mkdir(parents=True, exist_ok=True)
        ensure_masters_readme(masters)
        print(
            f"已创建原图目录：{masters}\n"
            f"请把 {from_size}×{from_size} 的 PNG（文件名任意）放进去后再运行。"
        )
        return 1

    ensure_masters_readme(masters)
    srcs = collect_sources(masters, args.pattern)
    if not srcs:
        print(
            f"error: 在 {masters} 下未找到 {args.pattern}\n"
            f"请放入任意文件名的 {from_size}×{from_size} PNG 后再运行。",
            file=sys.stderr,
        )
        return 1

    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"缩放 {from_size} → {to_size}")
    print(f"  原图：{masters}")
    print(f"  生成：{out_dir}")
    print(f"  共 {len(srcs)} 张")

    written: list[Path] = []
    unmatched: list[str] = []
    for src in srcs:
        out_name, suffix = resolve_output_name(src)
        im = Image.open(src)
        out = resize_rgba(im, from_size, to_size)
        dst = out_dir / out_name
        out.save(dst, format="PNG", optimize=True)
        if suffix is None:
            unmatched.append(src.name)
            print(
                f"  {src.name} → {out_name}  "
                f"{im.size[0]}x{im.size[1]}→{to_size}x{to_size}  "
                f"[未映射，不会安装到固件]"
            )
        else:
            written.append(dst)
            print(
                f"  {src.name} → {out_name}  "
                f"{im.size[0]}x{im.size[1]}→{to_size}x{to_size}"
            )

    if unmatched:
        print(
            "提示：未映射的文件名可在脚本 NAME_ALIASES 里补一条，"
            "或改名为 ic_clover_<suffix>.png",
            file=sys.stderr,
        )

    if not args.no_install and written:
        INSTALL_DIR.mkdir(parents=True, exist_ok=True)
        for dst in written:
            shutil.copy2(dst, INSTALL_DIR / dst.name)
        print(f"已安装 {len(written)} 张到：{INSTALL_DIR}")
    elif not args.no_install and not written:
        print("warn: 没有可安装的映射图标", file=sys.stderr)

    if not args.no_patch_frame:
        patch_home_frame(to_size)

    print("完成。接下来编译烧录即可。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
