#include "power_control.hpp"
#include "shaker.hpp"
#include <M5Unified.h>
#include "nvs.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace power_control {
namespace {
constexpr uint8_t codec_registers[] = {0x32,0x17,0x0E,0x12,0x14,0x0D,0x15,0x45};
constexpr gpio_num_t lcd_high[]={GPIO_NUM_14,GPIO_NUM_48}; // CS and RESET.
constexpr gpio_num_t lcd_low[]={GPIO_NUM_15,GPIO_NUM_21,GPIO_NUM_42,GPIO_NUM_47}; // SPI and IR.
constexpr gpio_num_t echo_low[]={GPIO_NUM_5,GPIO_NUM_6,GPIO_NUM_8}; // Data, LRCK, BCLK.
bool write(uint8_t address, uint8_t reg, uint8_t value) {
    for (int retry=0;retry<3;++retry) {
        if (M5.In_I2C.writeRegister8(address,reg,value,100000)) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}
bool read(uint8_t address, uint8_t reg, uint8_t& value) {
    return M5.In_I2C.readRegister(address,reg,&value,1,100000);
}
}
Resume load() {
    Resume state;
    nvs_handle_t handle;
    if (nvs_open("shaker_power",NVS_READONLY,&handle)!=ESP_OK) return state;
    size_t size=sizeof(state);
    const auto result=nvs_get_blob(handle,"resume",&state,&size);
    nvs_close(handle);
    if (result!=ESP_OK || size!=sizeof(state) || state.version!=1 ||
        state.preset>=shaker::kPresetCount || state.volume>=shaker::kVolumeCount || state.codec_valid>1)
        return Resume{};
    return state;
}
bool save(int preset, int volume, bool echo_present) {
    Resume state;
    state.preset=preset;
    state.volume=volume;
    if (echo_present) {
        m5gfx::i2c::i2c_temporary_switcher_t routing(1,GPIO_NUM_38,GPIO_NUM_39);
        bool ok=true;
        for (unsigned i=0;i<sizeof(codec_registers);++i)
            ok=read(0x18,codec_registers[i],state.codec[i]) && ok;
        routing.restore();
        if (!ok) return false; // Don't power down a codec we cannot restore.
        state.codec_valid=1;
    }
    nvs_handle_t handle;
    if (nvs_open("shaker_power",NVS_READWRITE,&handle)!=ESP_OK) return false;
    auto result=nvs_set_blob(handle,"resume",&state,sizeof(state));
    if (result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
    return result==ESP_OK;
}
bool restore_codec(const Resume& state) {
    if (!state.codec_valid) return true;
    m5gfx::i2c::i2c_temporary_switcher_t routing(1,GPIO_NUM_38,GPIO_NUM_39);
    // Keep the amp off while restoring exactly the pre-sleep register values.
    // M5Unified's short begin sequence does not undo all ES8311 suspend registers.
    bool ok=write(0x43,0x05,0x00);
    for (unsigned i=0;i<sizeof(codec_registers);++i)
        ok=write(0x18,codec_registers[i],state.codec[i]) && ok;
    routing.restore();
    return ok;
}
bool suspend_compass() {
    // BMI270 auxiliary manual I2C, as configured by M5Unified. BMM150's 0x4B
    // power-control bit must be cleared before disabling its auxiliary bus.
    bool ok=write(0x68,0x7C,0x00);
    vTaskDelay(pdMS_TO_TICKS(1));
    ok=write(0x68,0x7D,0x06) && ok;
    ok=write(0x68,0x6B,0x20) && ok;
    ok=write(0x68,0x4C,0x80) && ok;
    ok=write(0x68,0x4B,0x20) && ok;
    ok=write(0x68,0x4F,0x00) && ok;
    ok=write(0x68,0x4E,0x4B) && ok;
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t status=0;
    ok=read(0x68,0x03,status) && !(status&0x04) && ok;
    // Leave the unused auxiliary interface disabled during normal playing too.
    ok=write(0x68,0x6B,0x00) && ok;
    return ok;
}
bool suspend_codec() {
    m5gfx::i2c::i2c_temporary_switcher_t routing(1,GPIO_NUM_38,GPIO_NUM_39);
    bool ok=write(0x43,0x05,0x00); // Echo Base IO expander: NS4150B CTRL low.
    // ES8311 suspend register sequence documented in Espressif ESP-ADF v2.7.
    constexpr uint8_t values[]={0x00,0x00,0xFF,0x02,0x00,0xFA,0x00,0x01};
    for (unsigned i=0;i<sizeof(codec_registers);++i)
        ok=write(0x18,codec_registers[i],values[i]) && ok;
    routing.restore();
    return ok;
}
bool suspend_sensors_and_backlight() {
    bool ok=suspend_compass();
    ok=write(0x68,0x7D,0x00) && ok; // Accelerometer, gyro, aux and temperature off.
    ok=write(0x68,0x7C,0x01) && ok; // BMI270 advanced power save / suspend.
    // LP5562 CHIP_EN=0: standby. Brightness=0 alone leaves its oscillator on.
    ok=write(0x30,0x00,0x00) && ok;
    vTaskDelay(pdMS_TO_TICKS(2));
    return ok;
}
void hold_sleep_pins(bool echo_present) {
    auto hold=[](gpio_num_t pin, int level) {
        gpio_set_level(pin,level);
        gpio_set_direction(pin,GPIO_MODE_OUTPUT);
        gpio_hold_en(pin);
    };
    for (auto pin:lcd_high) hold(pin,1);
    for (auto pin:lcd_low) hold(pin,0);
    // GPIO8 is the A151 voltage divider without Echo; do not drive it then.
    if (echo_present) for (auto pin:echo_low) hold(pin,0);
    gpio_deep_sleep_hold_en();
}
void release_sleep_pins() {
    gpio_deep_sleep_hold_dis();
    for (auto pin:lcd_high) gpio_hold_dis(pin);
    for (auto pin:lcd_low) gpio_hold_dis(pin);
    for (auto pin:echo_low) gpio_hold_dis(pin);
}
}
