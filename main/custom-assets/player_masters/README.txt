把原图 PNG 放此目录（中文名也可，尺寸随意），例如：
  播放.png / 暂停.png / 上一首.png / 下一首.png
  音量加.png / 音量减.png
然后运行：
  python3 tools/resize_player_icons.py
生成：main/custom-assets/player_icons/
  播放/暂停 → 44×44
  切歌/音量 → 34×34
并默认复制到 main/xingzhi-assets/ 供编译。
