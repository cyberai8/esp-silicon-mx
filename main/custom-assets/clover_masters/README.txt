把任意文件名的原尺寸 PNG 放此目录（中文名也可），例如：
  AI聊天.png / 电台.png / 音乐厅.png / ic_clover_wifi.png
然后运行：
  python3 tools/resize_clover_icons.py
生成：main/custom-assets/clover_<目标边长>/ic_clover_*.png
并默认复制到 main/xingzhi-assets/ 供编译。
