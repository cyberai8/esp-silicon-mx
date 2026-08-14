// 主界面的仿真占位。真实 home_screen.cc 会把 27 个子屏、网络、电量、
// 电源键策略全链进来，仿真里暂时不需要，只要「返回键有地方去」。
#include "home_screen/home_screen.h"

lv_obj_t* HomeScreen::Create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_t* label = lv_label_create(scr);
    lv_label_set_text(label, "HOME (sim)");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    return scr;
}

void HomeScreen::RefreshStatusBar() {}
void HomeScreen::ResetToFirstPage() {}
void HomeScreen::WarmStatusCaches() {}
void HomeScreen::ShowPowerOptionsDialog() {}
void HomeScreen::RequestSystemShutdown(const char* /*reason*/) {}

int HomeScreen::GetIdleShutdownMinutes() {
    return 0;
}
void HomeScreen::SetIdleShutdownMinutes(int /*minutes*/) {}
int HomeScreen::GetIdleStandbyMinutes() {
    return 0;
}
void HomeScreen::SetIdleStandbyMinutes(int /*minutes*/) {}
