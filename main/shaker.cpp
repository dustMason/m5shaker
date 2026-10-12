#include "shaker.hpp"
#include "analog_dsp.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace shaker {
namespace {
constexpr float pi = 3.14159265358979323846f;
float bounded(float x, float lo, float hi) {
    return std::isfinite(x) ? std::clamp(x, lo, hi) : lo;
}
struct Preset {
    const char* name;
    float hz[3], radius[3];
    float density, grain_ms, decay_ms, noise_mix, resonance_mix, level;
    enum Kind { Acoustic, Chain, Plucked, Halo, FiveSynth, SixSynth, Cuica, Guiro } kind = Acoustic;
    float wet = 0, feedback = 0;
};
constexpr Preset presets[kPresetCount] = {
    {"EGG",        {3200, 5600, 8100}, {.88f, .86f, .82f}, 1800, 1.0f,  65, .90f, .30f, 1.8f},
    {"CABASA",     {2700, 5100, 7600}, {.96f, .95f, .93f}, 3200,  .5f,  45, .65f,  .70f, 1.8f},
    {"CHAIN RIDE", {0, 0, 0}, {0, 0, 0}, 210, .55f, 240, .10f, 4.2f, 3.0f, Preset::Chain, .22f, .77f},
    {"DORIAN PLUCK", {}, {}, 0, 1, 1, 0, 0, 1.7f, Preset::Plucked, .08f, .62f},
    {"DORIAN HALO",  {}, {}, 0, 1, 1, 0, 0, 1.5f, Preset::Halo, .60f, .87f},
    {"FIVE HIRA",    {}, {}, 0, 1, 1, 0, 0, 5.0f, Preset::FiveSynth, .10f, .68f},
    {"SIX LYDIAN",     {}, {}, 0, 1, 1, 0, 0, 4.2f, Preset::SixSynth, .12f, .70f},
    {"CUICA", {520, 1450, 2600}, {.988f, .975f, .95f}, 0, 1, 72, 0, 0, 2.4f, Preset::Cuica, .06f, .52f},
    {"GUIRO", {780, 1560, 3050}, {.993f, .989f, .975f}, 0, 1, 42, 0, 0, 1.9f, Preset::Guiro, .035f, .45f},
};
constexpr int dorian[] = {50,52,53,55,57,59,60,62,64,65,67,69,71,72,74};
constexpr int lydian[] = {52,54,56,58,59,61,63,64,66,68,70,71,73,75,76};
constexpr int hirajoshi[] = {52,54,55,59,60,64,66,67,71,72,76};
constexpr const char* note_names[] = {"D3","D#3","E3","F3","F#3","G3","G#3","A3","A#3","B3",
    "C4","C#4","D4","D#4","E4","F4","F#4","G4","G#4","A4","A#4","B4","C5","C#5","D5","D#5","E5"};
constexpr float chain_hz[] = {391, 693, 1013, 1477, 2231, 3001, 3919, 5077, 6803, 8819, 11027, 14209};
constexpr float chain_radius[] = {.99991f, .99990f, .99988f, .99986f, .99983f, .99980f,
                                  .99976f, .99970f, .99962f, .99951f, .99936f, .99912f};
constexpr int room_length[] = {1153, 1429, 1559, 1867};
}

uint8_t speakerVolume(int step) {
    constexpr uint8_t settings[kVolumeCount] = {77,153,255};
    return settings[std::clamp(step,0,kVolumeCount-1)];
}
int speakerVolumePercent(int step) {
    constexpr int percentages[kVolumeCount] = {30,60,100};
    return percentages[std::clamp(step,0,kVolumeCount-1)];
}
ButtonControl::Action ButtonControl::update(bool pressed, int64_t now_us) {
    Action action = Action::None;
    if (pressed && !down_) {
        pressed_us_ = now_us;
        consumed_ = false;
        down_ = true;
    }
    if (down_ && !consumed_) {
        // Also recognize a hold on release if polling missed the threshold.
        if (now_us-pressed_us_ >= kButtonHoldUs) {
            volume_step_ = (volume_step_+1)%kVolumeCount;
            action = Action::Volume;
            consumed_ = true;
        } else if (!pressed) {
            preset_ = (preset_+1)%kPresetCount;
            action = Action::Sound;
        }
    }
    down_ = pressed;
    return action;
}
void ButtonControl::restore(int preset, int volume_step) {
    *this = ButtonControl{};
    preset_ = std::clamp(preset,0,kPresetCount-1);
    volume_step_ = std::clamp(volume_step,0,kVolumeCount-1);
}
void IdlePower::update(int64_t now_us, bool activity, bool usb_host, bool sensor_ok) {
    if (!initialized_ || now_us<last_activity_ || now_us<last_awake_) {
        last_activity_ = last_awake_ = now_us;
        initialized_ = true;
    }
    if (activity || !sensor_ok) last_activity_ = now_us;
    if (activity || usb_host || !sensor_ok) last_awake_ = now_us;
    screen_off_ = now_us-last_activity_ >= kScreenOffUs;
    sleep_ = now_us-last_awake_ >= kSleepUs;
}
void VolumeCue::start(int beeps) {
    beeps_ = std::clamp(beeps,1,kVolumeCount);
    position_ = 0;
}
void VolumeCue::mix(int16_t* samples, size_t count) {
    constexpr int tone_frames = kSampleRate*60/1000;
    constexpr int gap_frames = kSampleRate*70/1000;
    constexpr int period = tone_frames+gap_frames;
    if (!beeps_ && duck_==1) return;
    for (size_t i=0; i<count; ++i) {
        const bool active = beeps_>0;
        duck_ += .005f*((active ? .18f : 1)-duck_);
        if (!active && duck_>.99999f) duck_=1;
        float tone = 0;
        if (active) {
            const int within = position_%period;
            if (within<tone_frames) {
                const float envelope = std::min({1.0f,within/144.0f,(tone_frames-1-within)/240.0f});
                tone = 4915*envelope*std::sin(2*pi*1200*within/kSampleRate);
            }
            if (++position_ >= beeps_*period-gap_frames) beeps_=0;
        }
        // Duck ringing tails briefly so the confirmation can be counted clearly.
        samples[i] = static_cast<int16_t>(std::clamp(samples[i]*duck_+tone,-32767.0f,32767.0f));
    }
}

const char* presetName(int preset) { return presets[std::clamp(preset, 0, kPresetCount-1)].name; }
bool presetHasPitch(int preset) {
    const auto kind = presets[std::clamp(preset, 0, kPresetCount-1)].kind;
    return kind == Preset::Chain || kind == Preset::Plucked || kind == Preset::Halo ||
           kind == Preset::FiveSynth || kind == Preset::SixSynth;
}
bool presetHasNotes(int preset) {
    const auto kind = presets[std::clamp(preset, 0, kPresetCount-1)].kind;
    return kind == Preset::Plucked || kind == Preset::Halo || kind == Preset::FiveSynth || kind == Preset::SixSynth;
}
const char* presetScaleName(int preset) {
    return preset == kSixLydian ? "E LYDIAN" : preset == kFiveHira ? "E HIRA" : "D DORIAN";
}
const char* noteName(int midi_note) {
    return midi_note>=50 && midi_note<=76 ? note_names[midi_note-50] : "--";
}
int DorianNotes::update(float pitch_semitones) {
    if (!std::isfinite(pitch_semitones)) { step_ = 7; return dorian[step_]; }
    const float position = 7 + bounded(pitch_semitones,-12,12)*7/12;
    // 0.12 of a scale step adds about 1.5 degrees of boundary hysteresis.
    while (step_<14 && position>step_+.62f) ++step_;
    while (step_>0 && position<step_-.62f) --step_;
    return dorian[step_];
}
int HirajoshiNotes::update(float pitch_semitones) {
    if (!std::isfinite(pitch_semitones)) { step_=5; return hirajoshi[step_]; }
    const float position=5+bounded(pitch_semitones,-12,12)*5/12;
    while (step_<10 && position>step_+.62f) ++step_;
    while (step_>0 && position<step_-.62f) --step_;
    return hirajoshi[step_];
}
int LydianNotes::update(float pitch_semitones) {
    if (!std::isfinite(pitch_semitones)) { step_=7; return lydian[step_]; }
    const float position=7+bounded(pitch_semitones,-12,12)*7/12;
    while (step_<14 && position>step_+.62f) ++step_;
    while (step_>0 && position<step_-.62f) --step_;
    return lydian[step_];
}
void Twist::reset() { *this = Twist{}; }
float Twist::update(float ax, float ay, float az, float gx, float gy, float gz, float dt) {
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az) ||
        !std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(gz) ||
        !std::isfinite(dt) || dt <= 0 || dt > .1f) { reset(); return 0; }
    const float a[] = {ay, -ax, az};
    const float g[] = {gy, -gx, gz}; // degrees/second, in enclosure coordinates
    const float norm = std::sqrt(ax*ax + ay*ay + az*az);
    if (!initialized_) {
        if (norm < .8f || norm > 1.2f) return 0;
        for (int i=0; i<3; ++i) { gravity_[i]=a[i]/norm; previous_[i]=a[i]; }
        initialized_ = true;
        return 0;
    }
    float change = 0, speed = 0;
    for (int i=0; i<3; ++i) {
        change += (a[i]-previous_[i])*(a[i]-previous_[i]);
        speed += g[i]*g[i]; previous_[i]=a[i];
    }
    change = std::sqrt(change);
    const bool still = std::fabs(norm-1)<.05f && change<.01f && speed<9;
    still_ = still ? still_ + dt : 0;
    if (still_ > .5f) for (int i=0; i<3; ++i) bias_[i] += dt/(8+dt)*(g[i]-bias_[i]);
    const float w=q_[0], x=q_[1], y=q_[2], z=q_[3];
    const float rx=gravity_[0], ry=gravity_[1], rz=gravity_[2];
    const float predicted[] = {
        (1-2*(y*y+z*z))*rx + 2*(x*y+w*z)*ry + 2*(x*z-w*y)*rz,
        2*(x*y-w*z)*rx + (1-2*(x*x+z*z))*ry + 2*(y*z+w*x)*rz,
        2*(x*z+w*y)*rx + 2*(y*z-w*x)*ry + (1-2*(x*x+y*y))*rz
    };
    float omega[3];
    for (int i=0; i<3; ++i) omega[i] = (g[i]-bias_[i]) * pi/180;
    // Shaking corrupts gravity measurements: trust the gyro during acceleration.
    const float confidence = bounded(1-std::fabs(norm-1)/.15f,0,1) * bounded(1-change/.15f,0,1);
    if (norm > .1f && confidence > 0) {
        const float k = .75f*confidence/norm;
        omega[0] += k*(a[1]*predicted[2]-a[2]*predicted[1]);
        omega[1] += k*(a[2]*predicted[0]-a[0]*predicted[2]);
        omega[2] += k*(a[0]*predicted[1]-a[1]*predicted[0]);
    }
    const float halfdt = .5f*dt;
    q_[0] += (-x*omega[0]-y*omega[1]-z*omega[2])*halfdt;
    q_[1] += ( w*omega[0]+y*omega[2]-z*omega[1])*halfdt;
    q_[2] += ( w*omega[1]+z*omega[0]-x*omega[2])*halfdt;
    q_[3] += ( w*omega[2]+x*omega[1]-y*omega[0])*halfdt;
    const float inv = 1/std::sqrt(q_[0]*q_[0]+q_[1]*q_[1]+q_[2]*q_[2]+q_[3]*q_[3]);
    for (float& v:q_) v*=inv;
    // Use the current USB-axis twist, never an accumulated revolution count:
    // tumbling or full turns must not pin the scale at an endpoint afterward.
    // At the 180-degree swing singularity, retain the last meaningful twist.
    if (q_[0]*q_[0]+q_[2]*q_[2] > .01f) {
        float angle = std::remainder(2*std::atan2(q_[2],q_[0]),2*pi);
        // Fold the back half of a turn smoothly through neutral. This preserves
        // linear +/-90-degree playing while avoiding a jump at +/-180 degrees.
        if (angle > pi*.5f) angle = pi-angle;
        else if (angle < -pi*.5f) angle = -pi-angle;
        target_ = bounded(angle*24/pi,-12,12);
    }
    pitch_ += dt/(.04f+dt)*(target_-pitch_);
    return pitch_;
}
void Motion::reset() { *this = Motion{}; }

MotionFrame Motion::update(float ax, float ay, float az, float dt) {
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az) ||
        !std::isfinite(dt) || dt <= 0 || dt > .1f) {
        reset();
        return frame_;
    }
    const float a[] = {ax, ay, az};
    if (!initialized_) {
        std::copy(a, a+3, gravity_);
        initialized_ = true;
        return frame_;
    }
    elapsed_ += dt;
    float norm = 0;
    const float alpha = dt / (.28f + dt);
    for (int i = 0; i < 3; ++i) {
        gravity_[i] += alpha * (a[i] - gravity_[i]);
        const float dynamic = a[i] - gravity_[i];
        direction_filter_[i] += dt/(.008f+dt)*(dynamic-direction_filter_[i]);
        norm += dynamic * dynamic;
    }
    // Remove gravity and a 0.12 g dead zone. Deliberately do not integrate
    // acceleration into speed: an unaided IMU would accumulate drift.
    const float level = bounded((std::sqrt(norm) - .12f) * kSensitivity / 2.8f, 0, 1);
    smooth_ += dt / (.008f + dt) * (level - smooth_);
    if (smooth_ > previous_ + .002f) rising_ = true;
    if (rising_ && smooth_ < previous_ - .002f) {
        if (previous_ > .035f && elapsed_ - last_strike_ > .045f) {
            // Learn the stroke axis from signed, gravity-removed acceleration at
            // the peak. Opposite strokes get opposite sounds; a repeated peak in
            // the same direction retains its sound even if another event was missed.
            float magnitude = 0;
            for (float v : previous_direction_) magnitude += v*v;
            if (magnitude > .0001f && std::isfinite(magnitude)) {
                const float inverse = 1/std::sqrt(magnitude);
                float unit[3], alignment = 0;
                for (int i=0;i<3;++i) {
                    unit[i] = previous_direction_[i]*inverse;
                    alignment += unit[i]*stroke_axis_[i];
                }
                if (!stroke_axis_ready_) frame_.strike_direction = 1;
                else if (std::fabs(alignment)>.35f) frame_.strike_direction = alignment>0 ? 1 : -1;
                // A substantially changed playing axis keeps the current stroke
                // identity, then tracks the new axis. Noise near perpendicular
                // cannot make the sound chatter, and the next reversal still flips.
                const float follow = !stroke_axis_ready_ || std::fabs(alignment)<=.35f ? 1 : .25f;
                float length = 0;
                for (int i=0;i<3;++i) {
                    stroke_axis_[i] += follow*(unit[i]*frame_.strike_direction-stroke_axis_[i]);
                    length += stroke_axis_[i]*stroke_axis_[i];
                }
                const float scale = 1/std::sqrt(length);
                for (float& v : stroke_axis_) v *= scale;
                stroke_axis_ready_ = true;
            }
            frame_.strike = previous_;
            ++frame_.strike_id;
            last_strike_ = elapsed_;
        }
        rising_ = false;
    }
    previous_ = smooth_;
    std::copy(std::begin(direction_filter_),std::end(direction_filter_),previous_direction_);
    frame_.activity = smooth_ < .005f ? 0 : smooth_;
    return frame_;
}

Synth::Synth(uint32_t seed) : rng_(seed ? seed : 1) { select(0); }
void Synth::reset() {
    energy_ = grain_ = low_ = dc_x_ = dc_y_ = fade_ = 0;
    final_dc_x_ = final_dc_y_ = 0;
    mod_counter_ = 0;
    friction_ = Friction{};
    five_.reset();
    six_.reset();
    std::fill(std::begin(room_damp_), std::end(room_damp_), 0);
    std::fill(std::begin(room_pos_), std::end(room_pos_), 0);
    for (auto& line : room_delay_) std::fill(std::begin(line), std::end(line), 0);
    for (auto &m : modes_) m.y1 = m.y2 = 0;
    for (auto& voice : plucks_) {
        voice.remaining = 0;
        voice.attack = voice.carry = voice.last = 0;
    }
    next_pluck_ = 0;
}
float Synth::uniform() {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return float(rng_ >> 8) * (1.0f / 16777216.0f);
}
float Synth::Resonator::tick(float input) {
    float y = b * input + a1 * y1 - a2 * y2;
    y2 = y1; y1 = std::fabs(y) < 1e-18f ? 0 : y;
    return y1;
}
float Synth::String::tick() {
    const float delayed = delay[position];
    // First-order allpass supplies the fractional part of the string length.
    const float interpolated = a*(delayed-output) + input;
    input = delayed; output = interpolated;
    const float damped = .5f*(interpolated+previous);
    previous = interpolated;
    delay[position] = feedback*damped;
    if (++position == length) position = 0;
    return damped;
}
void Synth::excite(String& s, float hz, float velocity, bool halo) {
    const float period = kSampleRate/hz;
    // The two-point damping filter contributes half a sample of phase delay.
    // Keep the allpass delay in [0.5,1.5), away from its poorly damped extremes.
    s.length = std::clamp(int(std::floor(period-1)), 2, 512);
    const float fraction = period-.5f-s.length;
    s.a = (1-fraction)/(1+fraction);
    s.feedback = std::pow(.001f, period/((halo ? 2.8f : 1.6f)*kSampleRate));
    s.position = 0;
    s.input = s.output = s.previous = 0;
    float mean = 0;
    const float amplitude = .70f*velocity; // Loudness follows the individual shake.
    for (int i=0; i<s.length; ++i) {
        const float phase = float(i)/s.length;
        const float triangle = phase<.22f ? phase/.22f : (1-phase)/.78f;
        // A shaped displacement gives a reliable fundamental; noise is the
        // Karplus–Strong excitation and becomes brighter with harder shakes.
        s.delay[i] = amplitude*(triangle*2-1 + (.12f+.38f*velocity)*(uniform()*2-1));
        mean += s.delay[i];
    }
    mean /= s.length;
    for (int i=0; i<s.length; ++i) s.delay[i] -= mean;
}
void Synth::pluck(int note, float velocity) {
    auto& voice = plucks_[next_pluck_];
    next_pluck_ = (next_pluck_+1)%4;
    voice.carry = voice.last; // Smooth a stolen voice into the next pluck.
    voice.attack = 0;
    voice.remaining = 4*kSampleRate;
    const float hz = 440*std::exp2((std::clamp(note,50,74)-69)/12.0f);
    const bool halo = presets[preset_].kind == Preset::Halo;
    excite(voice.strings[0], hz, velocity, halo);
    if (halo) excite(voice.strings[1], hz*2, velocity, true);
}
float Synth::strings() {
    const bool halo = presets[preset_].kind == Preset::Halo;
    float sum = 0;
    for (auto& voice : plucks_) {
        if (!voice.remaining) continue;
        float x = voice.strings[0].tick();
        if (halo) x = .8f*x + .35f*voice.strings[1].tick();
        voice.attack = std::min(1.0f,voice.attack+1.0f/96); // 2 ms click suppression.
        x = voice.attack*x + (1-voice.attack)*voice.carry;
        x *= std::min(1.0f,voice.remaining/240.0f);
        --voice.remaining;
        voice.last = voice.remaining ? x : 0;
        sum += x;
    }
    return sum;
}
void Synth::select(int preset) {
    preset_ = std::clamp(preset, 0, kPresetCount-1);
    reset();
    const auto &p = presets[preset_];
    mode_count_ = p.kind == Preset::Chain ? 12 : 3;
    pitch_ratio_ = 1;
    tune(1);
}
void Synth::tune(float ratio) {
    const auto& p = presets[preset_];
    for (int i = 0; i < mode_count_; ++i) {
        const float r = p.kind == Preset::Chain ? chain_radius[i] : p.radius[i];
        const float hz = p.kind == Preset::Chain ? chain_hz[i] : p.hz[i];
        const float omega = 2 * pi * std::min(hz*ratio,18000.0f) / kSampleRate;
        modes_[i].a1 = 2 * r * std::cos(omega);
        modes_[i].a2 = r*r;
        modes_[i].b = p.kind == Preset::Cuica ? 2*(1-r)*std::sin(omega) : std::sqrt(1-r*r);
    }
}

float Synth::cuica(float activity, float decay) {
    auto& f = friction_;
    energy_ = std::max(energy_*decay,activity*.72f);
    if (energy_ < .0002f) energy_ = 0;
    f.pressure += .003f*(energy_-f.pressure); // Smooth rubbing pressure, including retriggers.
    if (f.pressure < 1e-8f) f.pressure = 0;
    // Two decays create an up-and-down membrane-tension scoop on each stroke.
    f.fast *= .998959f; // 20 ms
    f.slow *= .999792f; // 100 ms
    if (f.slow < 1e-8f) f.fast = f.slow = 0;
    const float bend = 1.85f*(f.slow-f.fast);
    // Smoothly morph at a direction change; do not reset oscillator/filter state.
    // Push rises into a bright squeak; pull dips into a lower, rounder response.
    f.pull += .004f*(f.pull_target-f.pull);
    const float pitch = 190+40*f.pull+(130-215*f.pull)*bend+(105-25*f.pull)*f.pressure;
    f.hz += .002f*(pitch-f.hz);
    if ((mod_counter_++ & 63)==0) tune(.85f+.35f*bend-f.pull*(.18f+.18f*bend));
    const float increment = f.hz*(1+.008f*f.jitter)/kSampleRate;
    f.phase += increment;
    if (f.phase >= 1) { f.phase -= 1; f.jitter = uniform()*2-1; }
    f.noise += .09f*((uniform()*2-1)-f.noise);
    // A band-limited asymmetric slip pulse provides the friction harmonics;
    // membrane/body resonances and a softer fundamental make the hollow squeak.
    const float width = .18f+.12f*f.pull;
    const float slip = analog::pulse(f.phase,increment,width)+(1-2*width);
    const float rub = slip + (.22f-.08f*f.pull)*f.noise;
    const float body = .75f*modes_[0].tick(rub)+.35f*modes_[1].tick(rub)+.12f*modes_[2].tick(rub);
    const float fundamental = std::sin(2*pi*f.phase);
    return f.pressure*(.72f+.28f*f.pressure)*((.34f+.10f*f.pull)*fundamental+body+.025f*f.noise);
}

float Synth::guiro(float activity, float decay) {
    auto& f = friction_;
    energy_ = std::max(energy_*decay,activity);
    if (energy_ < .0002f) energy_ = 0;
    f.pressure += .004f*(energy_-f.pressure);
    if (f.pressure < 1e-8f) f.pressure = 0;
    // Travel across successive ridges, with slightly irregular spacing. Faster
    // motion packs the wooden ticks closer together instead of adding random hits.
    f.phase += (32+245*f.pressure)/kSampleRate;
    float impact = 0;
    if (f.phase >= f.ridge) {
        f.phase -= f.ridge;
        f.ridge = .90f+.20f*uniform();
        if (f.pressure > .0002f) {
            const float force = f.pressure*(.65f+.35f*uniform());
            impact = force*(uniform() < .5f ? -1 : 1);
            f.grain = force;
        }
    }
    f.grain *= .984f; // ~1.3 ms rough contact at each ridge, not a noise wash.
    if (f.grain < 1e-10f) f.grain = 0;
    const float scratch = (uniform()*2-1)*f.grain;
    f.noise += .32f*(scratch-f.noise);
    const float excitation = impact+.65f*(scratch-.65f*f.noise);
    const float body = modes_[0].tick(excitation)+.72f*modes_[1].tick(excitation)+.45f*modes_[2].tick(excitation);
    return .75f*body+.22f*scratch;
}
float Synth::room(float input) {
    const auto& p = presets[preset_];
    float sum = 0;
    for (int j = 0; j < 4; ++j) {
        room_damp_[j] += .42f * (room_delay_[j][room_pos_[j]] - room_damp_[j]);
        if (std::fabs(room_damp_[j]) < 1e-18f) room_damp_[j] = 0;
        sum += room_damp_[j];
    }
    // Householder matrix: feedback below unity makes the tail decay naturally.
    for (int j = 0; j < 4; ++j) {
        room_delay_[j][room_pos_[j]] = .24f * input + p.feedback * (.5f * sum - room_damp_[j]);
        if (++room_pos_[j] == room_length[j]) room_pos_[j] = 0;
    }
    return .5f * (room_damp_[0] + room_damp_[1] - room_damp_[2] - room_damp_[3]);
}
void Synth::render(int16_t* out, size_t count, int preset, float activity, float strike, float gain, float pitch_semitones, int strike_note, int strike_direction) {
    preset = std::clamp(preset, 0, kPresetCount-1);
    if (preset != preset_) select(preset);
    const auto &p = presets[preset_];
    activity = bounded(activity, 0, 1);
    strike = bounded(strike, 0, 1);
    gain = bounded(gain, 0, 1);
    const bool notes = presetHasNotes(preset_);
    if (p.kind == Preset::Cuica) {
        if (strike>0) friction_.fast = friction_.slow = 1;
        if (strike>0 || mod_counter_==0) friction_.pull_target = strike_direction<0 ? 1 : 0;
        if (mod_counter_==0) friction_.pull = friction_.pull_target;
    }
    if (notes && strike>0) {
        DorianNotes selector;
        HirajoshiNotes hira;
        LydianNotes lydian;
        if (p.kind == Preset::FiveSynth)
            five_.trigger(strike_note<0 ? hira.update(pitch_semitones) : strike_note,strike);
        else if (p.kind == Preset::SixSynth)
            six_.trigger(strike_note<0 ? lydian.update(pitch_semitones) : strike_note,strike);
        else pluck(strike_note<0 ? selector.update(pitch_semitones) : strike_note, strike);
    }
    const float target_ratio = presetHasPitch(preset_) ? std::exp2(bounded(pitch_semitones,-12,12)/12) : 1;
    energy_ = std::max(energy_, strike);
    const float decay = std::exp(-1.0f / (p.decay_ms * .001f * kSampleRate));
    const float grain_decay = std::exp(-1.0f / (p.grain_ms * .001f * kSampleRate));
    for (size_t i = 0; i < count; ++i) {
        float x;
        if (p.kind == Preset::FiveSynth) {
            x = p.level*five_.tick(activity);
        } else if (p.kind == Preset::SixSynth) {
            x = p.level*six_.tick();
        } else if (notes) {
            x = p.level*strings(); // Only a strike excites the strings, never activity.
        } else if (p.kind == Preset::Cuica) {
            x = p.level*cuica(activity,decay);
        } else if (p.kind == Preset::Guiro) {
            x = p.level*guiro(activity,decay);
        } else {
            const bool modulation_tick = (mod_counter_++ & 63) == 0;
            if (modulation_tick && p.kind != Preset::Acoustic && std::fabs(pitch_ratio_-target_ratio)>.00001f) {
                pitch_ratio_ += .04f*(target_ratio-pitch_ratio_);
                tune(pitch_ratio_);
            }
            energy_ = std::max(energy_ * decay, activity);
            if (energy_ < .0002f) energy_ = 0;
            // More vigorous motion raises both collision density and amplitude.
            float impulse = 0;
            if (uniform() < p.density * std::sqrt(energy_) / kSampleRate) {
                grain_ = std::min(1.5f, grain_ + energy_ * (.3f + .7f * uniform()));
                impulse = energy_ * (uniform() * 2 - 1);
            }
            grain_ *= grain_decay;
            if (grain_ < 1e-12f) grain_ = 0;
            float noise = (uniform() * 2 - 1) * grain_;
            // Stronger shakes preserve more high-frequency noise.
            low_ += (.10f + .80f * energy_) * (noise - low_);
            float excitation = low_ + impulse * .25f;
            if (p.kind == Preset::Chain) excitation = noise - .85f * low_ + .7f * impulse;
            float resonant = 0;
            for (int m = 0; m < mode_count_; ++m) resonant += modes_[m].tick(excitation);
            float texture = low_;
            x = p.level * (p.noise_mix * texture + p.resonance_mix * resonant / mode_count_);
        }
        if (p.wet > 0) x = (1 - .35f * p.wet) * x + p.wet * room(x);
        float dc = x - dc_x_ + .995f * dc_y_;
        dc_x_ = x; dc_y_ = std::fabs(dc) < 1e-18f ? 0 : dc;
        gain_ += .002f * (gain - gain_);
        fade_ = std::min(1.0f, fade_ + 1.0f / 240); // 5 ms onset ramp.
        float y = .90f * dc / (1 + std::fabs(dc)) * gain_ * fade_;
        if (p.kind == Preset::FiveSynth || p.kind == Preset::SixSynth || p.kind == Preset::Cuica) {
            // Asymmetric synth waves can acquire DC in the nonlinear limiter.
            // A final 7.6 Hz blocker removes that offset without dulling the pluck.
            const float corrected = y - final_dc_x_ + .999f * final_dc_y_;
            final_dc_x_ = y;
            final_dc_y_ = std::fabs(corrected) < 1e-18f ? 0 : corrected;
            y = final_dc_y_;
        }
        out[i] = static_cast<int16_t>(bounded(y, -.95f, .95f) * 32767);
    }
}
void OutputVoice::render(int16_t* out, size_t count, const Performance& state, int64_t now_us, float gain) {
    if (!last_call_us_ || now_us - last_call_us_ > 100000) {
        synth_.reset();
        last_strike_ = state.strike_id; // Don't replay motion from before this output started.
        quiet_frames_ = 0;
        idle_ = false;
    }
    last_call_us_ = now_us;
    const bool fresh = state.motion_us && now_us - state.motion_us < 50000;
    const float activity = fresh ? state.activity : 0;
    const float strike = fresh && state.strike_id != last_strike_ && now_us - state.strike_us < 50000
        ? state.strike : 0;
    last_strike_ = state.strike_id;
    const bool excitation = activity>0 || strike>0 || state.preset!=last_preset_ || gain!=last_gain_;
    last_preset_ = state.preset;
    last_gain_ = gain;
    if (excitation) { quiet_frames_ = 0; idle_ = false; }
    if (idle_) {
        std::fill(out,out+count,0);
        return;
    }
    synth_.render(out, count, state.preset, activity, strike, gain, state.pitch_semitones,
                  state.strike_note,state.strike_direction);
    // Wait for a full second of actual zero PCM, not a muted or quiet note.
    // This preserves all audible reverb tails and avoids per-sample DSP at rest.
    if (!excitation && gain>0 && std::all_of(out,out+count,[](int16_t s){return s==0;})) {
        quiet_frames_ += count;
        if (quiet_frames_>=kSampleRate) {
            synth_.reset();
            idle_ = true;
        }
    } else quiet_frames_ = 0;
}
} // namespace shaker
