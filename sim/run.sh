#!/usr/bin/env bash
# 一条命令：编译 + 跑一个屏幕 + 出截图。
#
#   ./run.sh                          # 相册，跑默认场景
#   ./run.sh album                    # 指定屏幕
#   ./run.sh album scenarios/album.txt
#   ./run.sh --list                   # 看有哪些屏幕
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build -j"$(nproc)" | tail -1

if [[ "${1:-}" == "--list" ]]; then
    exec ./build/sim --list
fi

screen="${1:-album}"
script="${2:-scenarios/$screen.txt}"

mkdir -p shots
if [[ -f "$script" ]]; then
    ./build/sim --screen "$screen" --script "$script" --out shots
else
    echo "（没有场景脚本 $script，就静态截一张）"
    ./build/sim --screen "$screen" --out shots --hold 3000
fi

echo
echo "截图在 $(pwd)/shots/："
ls -1 shots/
