#include "shaker.hpp"
#include "power_control.hpp"
#include "instrument_icons.hpp"
#include <M5Unified.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "usb_device_uac.h"

static_assert(CONFIG_UAC_SAMPLE_RATE == shaker::kSampleRate);
static_assert(CONFIG_UAC_MIC_CHANNEL_NUM == 1 && CONFIG_UAC_SPEAKER_CHANNEL_NUM == 0);
static_assert(CONFIG_UAC_BIT_RESOLUTION == 16);
static_assert(configTICK_RATE_HZ == 1000, "USB producer requires a 1 ms RTOS tick");

namespace {
struct Shared : shaker::Performance {
    float volume = 1;
    bool mute = false;
    bool speaker_ready = false, speaker_error = false;
    int speaker_volume_step = 0;
    uint32_t volume_change_id = 0;
    int64_t audio_us = 0;
} shared;
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
shaker::OutputVoice usb_voice;
shaker::OutputVoice speaker_voice;
constexpr size_t audio_frames = 192; // 4 ms at 48 kHz.
alignas(4) int16_t speaker_pcm[2][audio_frames] = {};
std::atomic<bool> speaker_busy[2] = {{false}, {false}};
TaskHandle_t speaker_producer = nullptr;
std::atomic<bool> speaker_stop{false}, speaker_parked{false}, shutting_down{false};
bool echo_present = false;
power_control::Resume resume_state;
static_assert(instrument_icons::kCount == shaker::kPresetCount);
// One opaque replacement per icon change; no erase frame or playing animation.
alignas(4) uint8_t icon_pixels[instrument_icons::kFrameBytes] = {};
constexpr m5gfx::bgr888_t icon_palette[16] = {{0,0,0}, {255,255,255}, {255,0,0}};
int displayed_icon = -1;

Shared snapshot() {
    portENTER_CRITICAL(&state_lock);
    Shared result = shared;
    portEXIT_CRITICAL(&state_lock);
    return result;
}

esp_err_t audio_input(uint8_t* buffer, size_t bytes, size_t* bytes_read, void*) {
    if (bytes % sizeof(int16_t)) { *bytes_read = 0; return ESP_ERR_INVALID_SIZE; }
    if (shutting_down.load()) {
        std::memset(buffer,0,bytes);
        *bytes_read=bytes;
        return ESP_OK;
    }
    const int64_t now = esp_timer_get_time();
    auto state = snapshot();
    // Use aligned storage; the UAC driver supplies a byte buffer.
    int16_t samples[192];
    size_t remaining = bytes / sizeof(int16_t), offset = 0;
    while (remaining) {
        const size_t n = std::min(remaining, size_t(192));
        usb_voice.render(samples, n, state, now, state.mute ? 0 : state.volume);
        std::memcpy(buffer + offset, samples, n * sizeof(int16_t));
        remaining -= n;
        offset += n * sizeof(int16_t);
    }
    *bytes_read = bytes;
    portENTER_CRITICAL(&state_lock);
    shared.audio_us = now;
    portEXIT_CRITICAL(&state_lock);
    // usb_device_uac owns the 4 ms cadence. No delay in this callback.
    return ESP_OK;
}

void speaker_released(void*, const void* data, uint8_t) {
    // M5Unified calls this only after it has finished reading the PCM buffer.
    for (int i = 0; i < 2; ++i) {
        if (data == speaker_pcm[i]) {
            speaker_busy[i].store(false, std::memory_order_release);
            // M5Unified invokes this from its playback task, not an interrupt.
            // Wake the renderer immediately instead of losing up to a 1 ms tick.
            if (speaker_producer) xTaskNotifyGive(speaker_producer);
        }
    }
}

void speaker_task(void*) {
    int index = 0;
    uint32_t volume_change_id = 0;
    shaker::VolumeCue volume_cue;
    for (;;) {
        while (speaker_stop.load()) {
            speaker_parked.store(true);
            ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        }
        speaker_parked.store(false);
        // Backpressure comes from I2S consumption, not USB or a second timer.
        // The buffers stay alive until the playback task releases them.
        while (speaker_busy[index].load(std::memory_order_acquire) && !speaker_stop.load())
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (speaker_stop.load()) continue;
        auto state = snapshot();
        if (state.volume_change_id != volume_change_id) {
            volume_change_id = state.volume_change_id;
            M5.Speaker.setVolume(shaker::speakerVolume(state.speaker_volume_step));
            volume_cue.start(state.speaker_volume_step+1);
        }
        speaker_voice.render(speaker_pcm[index], audio_frames, state, esp_timer_get_time(), 1.0f);
        volume_cue.mix(speaker_pcm[index], audio_frames);
        speaker_busy[index].store(true, std::memory_order_release);
        if (!M5.Speaker.playRaw(speaker_pcm[index], audio_frames, shaker::kSampleRate, false, 1, 0, false)) {
            speaker_busy[index].store(false, std::memory_order_release);
            portENTER_CRITICAL(&state_lock);
            shared.speaker_error = true;
            shared.speaker_ready = false;
            portEXIT_CRITICAL(&state_lock);
            // Keep USB and motion running if speaker playback fails.
            speaker_producer = nullptr;
            vTaskDelete(nullptr);
        }
        index ^= 1;
    }
}

bool echo_base_present() {
    // Probe before starting any motion/audio task. M5Stack's Echo driver also
    // temporarily routes I2C1 to these bottom pins, then restores the IMU bus.
    m5gfx::i2c::i2c_temporary_switcher_t routing(1, GPIO_NUM_38, GPIO_NUM_39);
    uint8_t reset_register = 0;
    const bool found = M5.In_I2C.readRegister(0x18, 0x00, &reset_register, 1, 100000);
    routing.restore();
    return found;
}

void start_echo_speaker() {
    echo_present=echo_base_present();
    if (!echo_present) return;
    if (!power_control::restore_codec(resume_state)) {
        shared.speaker_error=true;
        return;
    }
    auto cfg = M5.Speaker.config();
    cfg.sample_rate = shaker::kSampleRate;
    cfg.dma_buf_len = audio_frames;
    cfg.dma_buf_count = 2;
    cfg.task_priority = 7;
    cfg.task_pinned_core = 1;
    M5.Speaker.config(cfg);
    M5.Speaker.setVolume(shaker::speakerVolume(resume_state.volume));
    M5.Speaker.setBufferReleaseCallback(nullptr, speaker_released);
    if (!M5.Speaker.begin()) {
        M5.Speaker.end();
        shared.speaker_error = true;
        return;
    }
    shared.speaker_ready = true;
    if (xTaskCreatePinnedToCore(speaker_task, "speaker_pcm", 4096, nullptr, 6, &speaker_producer, 1) != pdPASS) {
        shared.speaker_ready = false;
        shared.speaker_error = true;
        M5.Speaker.end();
    }
}

void audio_mute(uint32_t mute, void*) {
    portENTER_CRITICAL(&state_lock);
    shared.mute = mute != 0;
    portEXIT_CRITICAL(&state_lock);
}

void audio_volume(uint32_t percent, void*) {
    // The pinned UAC component converts host dB controls to 0..100 percent.
    const float db = float(std::min(percent, uint32_t(100))) * .5f - 50;
    float gain = std::pow(10.0f, db / 20.0f);
    portENTER_CRITICAL(&state_lock);
    shared.volume = gain;
    portEXIT_CRITICAL(&state_lock);
}

void screen_icon(int icon) {
    if (icon == displayed_icon) return;
    M5.Display.waitDMA();
    instrument_icons::compose(icon,icon_pixels);
    M5.Display.pushImage(0,0,128,128,icon_pixels,m5gfx::color_depth_t::palette_4bit,icon_palette);
    M5.Display.waitDMA(); // Keep the packed frame intact until the transfer finishes.
    displayed_icon = icon;
}

void fatal_screen(const char* message) {
    ESP_LOGE("shaker", "%s", message);
    screen_icon(instrument_icons::kFault);
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

bool enter_idle_sleep(int preset, int volume) {
    if (uac_device_host_active()) return false;
    if (!power_control::save(preset,volume,echo_present)) {
        ESP_LOGW("shaker_power","Cannot save resume state; deferring sleep");
        return false;
    }
    // Cooperatively park the producer. Never delete/suspend it while it owns a
    // speaker lock, and never free a buffer that I2S may still be reading.
    const auto producer=speaker_producer;
    if (producer) {
        speaker_stop.store(true);
        xTaskNotifyGive(producer);
        const int64_t deadline=esp_timer_get_time()+200000;
        while (!speaker_parked.load() && esp_timer_get_time()<deadline)
            vTaskDelay(pdMS_TO_TICKS(1));
        if (!speaker_parked.load() || uac_device_host_active()) {
            speaker_stop.store(false);
            xTaskNotifyGive(producer);
            return false;
        }
    }
    // All notes have long since decayed at the two-minute inactivity boundary.
    shutting_down.store(true);
    if (echo_present && !power_control::suspend_codec())
        ESP_LOGW("shaker_power","Echo power-down I2C write failed");
    if (M5.Speaker.isRunning()) M5.Speaker.end();
    M5.Display.setBrightness(0);
    M5.Display.sleep();
    M5.Display.waitDisplay();
    if (!power_control::suspend_sensors_and_backlight())
        ESP_LOGW("shaker_power","Sensor/backlight power-down I2C write failed");
    power_control::hold_sleep_pins(echo_present);
    uac_device_disconnect();
    // No timer, motion or front-button wake: GPIO41 cannot wake deep sleep.
    // A brief physical side-reset tap boots normally and reloads the NVS snapshot.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,ESP_PD_OPTION_OFF);
#if SOC_PM_SUPPORT_RTC_SLOW_MEM_PD
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM,ESP_PD_OPTION_OFF);
#endif
#if SOC_PM_SUPPORT_RTC_FAST_MEM_PD
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM,ESP_PD_OPTION_OFF);
#endif
    esp_deep_sleep_start();
    return true;
}

void control_task(void*) {
    shaker::Motion motion;
    shaker::Twist twist;
    shaker::DorianNotes notes;
    shaker::HirajoshiNotes hira;
    shaker::LydianNotes lydian;
    shaker::ButtonControl button;
    button.restore(resume_state.preset,resume_state.volume);
    shaker::IdlePower idle_power;
    int64_t sample_us = 0, last_accel = esp_timer_get_time();
    int64_t suppress_until = 0;
    uint32_t previous_strike = 0;
    bool stale = false;
    bool screen_off = false;
    screen_icon(button.preset());
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        M5.update();
        const int64_t now = esp_timer_get_time();
        const bool pressed = M5.BtnA.isPressed();
        const auto action = button.update(pressed,now);
        const int preset = button.preset();
        bool activity=pressed || action!=shaker::ButtonControl::Action::None;
        if (action != shaker::ButtonControl::Action::None) {
            portENTER_CRITICAL(&state_lock);
            shared.preset = preset;
            shared.activity = shared.strike = 0;
            shared.speaker_volume_step = button.volumeStep();
            if (action == shaker::ButtonControl::Action::Volume) ++shared.volume_change_id;
            portEXIT_CRITICAL(&state_lock);
        }
        // The mechanical button can shake the enclosure. Gate its impact.
        if (pressed || action != shaker::ButtonControl::Action::None) suppress_until = now + 100000;
        uint8_t status = 0, raw[12] = {};
        // Read fresh accelerometer and gyro samples together, in raw BMI270 axes.
        if (M5.In_I2C.readRegister(0x68, 0x03, &status, 1, 400000) && (status & 0xC0) == 0xC0 &&
            M5.In_I2C.readRegister(0x68, 0x0C, raw, sizeof(raw), 400000)) {
            const auto axis = [&raw](int i, float range) {
                const int16_t value = static_cast<int16_t>(uint16_t(raw[i]) | (uint16_t(raw[i+1]) << 8));
                return value * (range / 32768.0f);
            };
            float dt = sample_us ? (now - sample_us) * 1e-6f : .005f;
            sample_us = last_accel = now;
            const auto frame = motion.update(axis(0,8), axis(2,8), axis(4,8), dt);
            const float pitch = twist.update(axis(0,8), axis(2,8), axis(4,8),
                                             axis(6,2000), axis(8,2000), axis(10,2000), dt);
            const float gx=axis(6,2000), gy=axis(8,2000), gz=axis(10,2000);
            // Gravity alone and normal sensor noise do not keep the device awake.
            // Rotation also counts, even before it produces an audible shake.
            activity=activity || frame.activity>.005f || gx*gx+gy*gy+gz*gz>36;
            const bool allowed = now >= suppress_until;
            const int dorian_note = notes.update(pitch);
            const int hira_note = hira.update(pitch);
            const int lydian_note = lydian.update(pitch);
            const int selected_note = preset == shaker::kSixLydian ? lydian_note :
                preset == shaker::kFiveHira ? hira_note : dorian_note;
            portENTER_CRITICAL(&state_lock);
            shared.activity = allowed ? frame.activity : 0;
            shared.pitch_semitones = pitch;
            shared.selected_note = selected_note;
            if (frame.strike_id != previous_strike && allowed) {
                shared.strike = frame.strike;
                shared.strike_note = selected_note;
                shared.strike_direction = frame.strike_direction;
                ++shared.strike_id;
                shared.strike_us = now;
            }
            shared.motion_us = now;
            portEXIT_CRITICAL(&state_lock);
            previous_strike = frame.strike_id;
        }
        stale = now - last_accel > 100000;
        idle_power.update(now,activity,uac_device_host_active(),!stale);
        // Replace the hidden frame before restoring its backlight. Volume holds,
        // motion and pitch changes leave the instrument image completely still.
        if (!idle_power.screenOff()) {
            const auto state = snapshot();
            screen_icon(stale || state.speaker_error ? instrument_icons::kFault : preset);
        }
        if (idle_power.screenOff()!=screen_off) {
            screen_off=idle_power.screenOff();
            M5.Display.setBrightness(screen_off ? 0 : 70);
        }
        if (idle_power.shouldSleep()) {
            enter_idle_sleep(preset,button.volumeStep());
            // If a host appeared or saving/parking failed, retry after fresh idle time.
            idle_power.update(esp_timer_get_time(),true,false,true);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
    }
}
} // namespace

extern "C" void app_main() {
    power_control::release_sleep_pins();
    // M5GFX/IMU use NVS for board detection/calibration. No preset writes while playing.
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    resume_state=power_control::load();
    shared.preset=resume_state.preset;
    shared.speaker_volume_step=resume_state.volume;
    auto cfg = M5.config();
    cfg.fallback_board = m5::board_t::board_M5AtomS3R;
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    cfg.external_spk = 0;
    cfg.external_speaker.atomic_echo = true;
    cfg.external_imu = false;
    cfg.internal_imu = true;
    M5.begin(cfg);
    M5.Display.setBrightness(70);
    M5.BtnA.setDebounceThresh(25);
    if (M5.Imu.getType() != m5::imu_t::imu_bmi270) fatal_screen("BMI270 NOT FOUND");
    M5.Imu.setCalibration(0, 0, 0);
    M5.Imu.clearOffsetData();
    if (!power_control::suspend_compass())
        ESP_LOGW("shaker_power","Unused compass could not be suspended");
    // BMI270 ACC_CONF: performance mode, normal bandwidth, 200 Hz ODR.
    // ACC_RANGE=2 is +/-8 g. Gyro: 200 Hz, normal bandwidth, +/-2000 deg/s.
    if (!M5.In_I2C.writeRegister8(0x68, 0x40, 0xA9, 400000) ||
        !M5.In_I2C.writeRegister8(0x68, 0x41, 0x02, 400000) ||
        !M5.In_I2C.writeRegister8(0x68, 0x42, 0xE9, 400000) ||
        !M5.In_I2C.writeRegister8(0x68, 0x43, 0x00, 400000) ||
        !M5.In_I2C.writeRegister8(0x68, 0x7D, 0x06, 400000)) {
        fatal_screen("IMU CONFIG FAILED");
    }
    vTaskDelay(pdMS_TO_TICKS(80));
    uint8_t imu_status = 0;
    if (!M5.In_I2C.readRegister(0x68, 0x21, &imu_status, 1, 400000) || (imu_status & 0x0F) != 1)
        fatal_screen("IMU INIT FAILED");
    start_echo_speaker();
    uac_device_config_t usb = {};
    usb.input_cb = audio_input;
    usb.set_mute_cb = audio_mute;
    usb.set_volume_cb = audio_volume;
    ESP_ERROR_CHECK(uac_device_init(&usb));
    if (xTaskCreatePinnedToCore(control_task, "motion_ui", 6144, nullptr, 5, nullptr, 1) != pdPASS)
        fatal_screen("TASK START FAILED");
}
