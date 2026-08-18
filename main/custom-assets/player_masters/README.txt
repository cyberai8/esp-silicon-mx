把 32×32 的 PNG 放此目录（中文名也可），例如：
  播放.png / 暂停.png / 上一首.png / 下一首.png
  音量加.png / 音量减.png
  或 ic_s_player_play.png
然后运行：
  python3 tools/resize_player_icons.py
生成：main/custom-assets/player_icons/
  播放/暂停 → 24×24（ic_s_player_play / pause）
  切歌/音量 → 18×18（previous / next / volume_up / volume_down）
并默认复制到 main/xingzhi-assets/ 供编译。
