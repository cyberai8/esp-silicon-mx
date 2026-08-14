#pragma once

// 挂 'A' 盘：设备上它是 mmap 资源分区（.spng/.sjpg），仿真里直接读
// main/xingzhi-assets 下的原始 .png/.jpg，屏幕代码里的路径不用改。
namespace SimAssets {

void Mount(const char* dir);

}  // namespace SimAssets
