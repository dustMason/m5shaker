#pragma once
#include <cstdint>

namespace power_control {
// NVS survives the external reset used to wake this board. RTC memory alone
// cannot be relied upon across a side-button reset.
struct Resume {
    uint32_t version = 1;
    uint8_t preset = 0, volume = 0, codec_valid = 0, reserved = 0;
    uint8_t codec[8] = {};
};
Resume load();
bool save(int preset, int volume, bool echo_present);
bool restore_codec(const Resume& state);
bool suspend_compass();
bool suspend_codec();
bool suspend_sensors_and_backlight();
void hold_sleep_pins(bool echo_present);
void release_sleep_pins();
}
