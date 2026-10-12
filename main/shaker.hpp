#pragma once

#include <cstddef>
#include <cstdint>
#include "five.hpp"
#include "six.hpp"

namespace shaker {
constexpr int kSampleRate = 48000;
// Append new sounds so stored preset IDs keep their meaning across updates.
enum PresetId { kEgg, kCabasa, kChainRide, kDorianPluck, kDorianHalo, kFiveHira, kSixLydian,
                kCuica, kGuiro, kPresetCount };
constexpr float kSensitivity = 1.0f;  // Increase for a lighter playing touch.
constexpr int kVolumeCount = 3;
constexpr int64_t kButtonHoldUs = 800000;
uint8_t speakerVolume(int step); // 30%, 60%, 100% of the 0..255 control range.
int speakerVolumePercent(int step);

// Input is already debounced by M5Unified. A hold consumes the entire gesture:
// no auto-repeat and no sound change when that hold is released.
class ButtonControl {
public:
    enum class Action { None, Sound, Volume };
    Action update(bool pressed, int64_t now_us);
    void restore(int preset, int volume_step);
    int preset() const { return preset_; }
    int volumeStep() const { return volume_step_; }
private:
    int preset_ = 0, volume_step_ = 0;
    bool down_ = false, consumed_ = false;
    int64_t pressed_us_ = 0;
};

// Physical activity controls the screen; an awake USB host also prevents sleep.
// A charger/power bank does not count as a USB host. No timer wakes from sleep.
class IdlePower {
public:
    static constexpr int64_t kScreenOffUs = 15000000;
    static constexpr int64_t kSleepUs = 120000000;
    void update(int64_t now_us, bool activity, bool usb_host, bool sensor_ok);
    bool screenOff() const { return screen_off_; }
    bool shouldSleep() const { return sleep_; }
private:
    int64_t last_activity_ = 0, last_awake_ = 0;
    bool initialized_ = false, screen_off_ = false, sleep_ = false;
};

// Brief count-coded confirmation, mixed only into local speaker PCM.
class VolumeCue {
public:
    void start(int beeps);
    void mix(int16_t* samples, size_t count);
private:
    int beeps_ = 0, position_ = 0;
    float duck_ = 1;
};
const char* presetName(int preset);
bool presetHasPitch(int preset);
bool presetHasNotes(int preset);
const char* presetScaleName(int preset);
const char* noteName(int midi_note);

// Fifteen evenly spaced positions: D3..D5, with D4 at the power-on pose.
// Hysteresis prevents small sensor fluctuations from changing the selected note.
class DorianNotes {
public:
    int update(float pitch_semitones);
private:
    int step_ = 7;
};
// E F# G B C, across E3..E5, with E4 at neutral.
class HirajoshiNotes {
public:
    int update(float pitch_semitones);
private:
    int step_ = 5;
};

// E F# G# A# B C# D#, E3..E5; fifteen positions with E4 at neutral.
class LydianNotes {
public:
    int update(float pitch_semitones);
private:
    int step_ = 7;
};

// Quaternion attitude relative to the power-on pose; inputs are raw BMI270 axes.
// AtomS3R board axes = (raw Y, -raw X, raw Z); the USB plug points along board Y.
class Twist {
public:
    float update(float ax, float ay, float az, float gx, float gy, float gz, float dt);
    void reset();
private:
    float q_[4] = {1, 0, 0, 0};
    float gravity_[3] = {0, 0, 1}, previous_[3] = {};
    float bias_[3] = {};
    float still_ = 0, target_ = 0, pitch_ = 0;
    bool initialized_ = false;
};

struct MotionFrame {
    float activity = 0;     // Acceleration-derived intensity, 0..1.
    float strike = 0;       // Velocity of latest detected acceleration peak.
    uint32_t strike_id = 0;
    int strike_direction = 1; // +1 push / -1 pull, latched with the accepted peak.
};

class Motion {
public:
    MotionFrame update(float ax, float ay, float az, float dt);
    void reset();
private:
    float gravity_[3] = {};
    float direction_filter_[3] = {}, previous_direction_[3] = {}, stroke_axis_[3] = {};
    bool stroke_axis_ready_ = false;
    float smooth_ = 0;
    float previous_ = 0;
    float elapsed_ = 0;
    float last_strike_ = -1;
    bool rising_ = false;
    bool initialized_ = false;
    MotionFrame frame_;
};

// One owner per audio output. No allocation, I2C, or display work while rendering.
class Synth {
public:
    explicit Synth(uint32_t seed = 0x12345678);
    void reset();
    void render(int16_t* out, size_t count, int preset, float activity,
                float strike, float gain = 1.0f, float pitch_semitones = 0,
                int strike_note = -1, int strike_direction = 1);
private:
    struct Resonator {
        float a1 = 0, a2 = 0, b = 0, y1 = 0, y2 = 0;
        float tick(float input);
    } modes_[12];
    struct String {
        float delay[512] = {};
        int length = 0, position = 0;
        float a = 0, feedback = 0, input = 0, output = 0, previous = 0;
        float tick();
    };
    struct Pluck {
        String strings[2];
        int remaining = 0;
        float attack = 0, carry = 0, last = 0;
    } plucks_[4];
    int next_pluck_ = 0;
    struct Friction {
        float phase = 0, pressure = 0, grain = 0, noise = 0;
        float fast = 0, slow = 0, hz = 210, jitter = 0, ridge = 1;
        float pull = 0, pull_target = 0;
    } friction_;
    float cuica(float activity, float decay);
    float guiro(float activity, float decay);
    Five five_;
    Six six_;
    void excite(String& string, float hz, float velocity, bool halo);
    void pluck(int note, float velocity);
    float strings();
    float room(float input);
    void select(int preset);
    void tune(float ratio);
    float uniform();
    uint32_t rng_;
    int preset_ = -1;
    int mode_count_ = 3;
    float energy_ = 0, grain_ = 0, low_ = 0;
    float dc_x_ = 0, dc_y_ = 0, gain_ = 0;
    float final_dc_x_ = 0, final_dc_y_ = 0;
    float fade_ = 0;
    float pitch_ratio_ = 1;
    uint32_t mod_counter_ = 0;
    // Four unequal delay lines form a damped, energy-preserving feedback matrix.
    // Keep Synth in persistent storage on the ESP32, never its small task stack.
    float room_delay_[4][2048] = {};
    float room_damp_[4] = {};
    int room_pos_[4] = {};
};

struct Performance {
    float activity = 0, strike = 0;
    float pitch_semitones = 0;
    int selected_note = 62, strike_note = 62; // D4; note is latched at the shake peak.
    int strike_direction = 1; // Shared by both audio outputs; never a local counter.
    uint32_t strike_id = 0;
    int preset = 0;
    int64_t motion_us = 0, strike_us = 0;
};

// Independent state per output: I2S must keep playing while USB is stopped.
// Sharing a Synth between the USB and speaker tasks would race its filters/RNG.
class OutputVoice {
public:
    void render(int16_t* out, size_t count, const Performance& state, int64_t now_us, float gain);
    bool idle() const { return idle_; }
private:
    Synth synth_;
    int64_t last_call_us_ = 0;
    uint32_t last_strike_ = 0;
    size_t quiet_frames_ = 0;
    int last_preset_ = -1;
    float last_gain_ = 0;
    bool idle_ = false;
};
} // namespace shaker
