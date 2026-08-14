#include "sim_input.h"

namespace SimInput {
namespace {

lv_indev_t* s_indev = nullptr;
int32_t s_x = 0;
int32_t s_y = 0;
bool s_pressed = false;

void ReadCb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->point.x = s_x;
    data->point.y = s_y;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

void Init() {
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, ReadCb);
}

void MoveTo(int32_t x, int32_t y) {
    s_x = x;
    s_y = y;
}

void SetPressed(bool pressed) {
    s_pressed = pressed;
}

}  // namespace SimInput
