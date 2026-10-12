#pragma once
// Shared fixed-patch helpers adapted from MDLR FIVE/SIX and dsp.rs.
#include <algorithm>
#include <cmath>

namespace shaker::analog {
constexpr float tau=6.2831853071795864769f;
extern const float tanh_table[513];
inline float saturate(float x) {
    const float position=std::min(std::fabs(x)*64,512.0f);
    const int index=std::min(int(position),511);
    const float y=tanh_table[index]+(tanh_table[index+1]-tanh_table[index])*(position-index);
    return std::copysign(y,x);
}
inline float wrap(float phase) { return phase>=1 ? phase-1 : phase<0 ? phase+1 : phase; }
inline float blep(float phase,float increment) {
    if (phase<increment) {
        const float t=phase/increment;
        return t+t-t*t-1;
    }
    if (phase>1-increment) {
        const float t=(phase-1)/increment;
        return t*t+t+t+1;
    }
    return 0;
}
inline float saw(float phase,float increment) { return phase*2-1-blep(phase,increment); }
inline float pulse(float phase,float increment,float width=.5f) {
    return (phase<width ? 1 : -1)+blep(phase,increment)-blep(wrap(phase-width),increment);
}
inline float coefficient(float seconds,int sample_rate) { return std::exp(-1/(seconds*sample_rate)); }
enum class Stage { Idle, Attack, Decay, Sustain, Release };
inline void envelope(float& value,Stage& stage,float attack,float decay,float release,float sustain) {
    switch (stage) {
    case Stage::Idle: value=0; break;
    case Stage::Attack:
        value=1+(value-1)*attack;
        if (value>=.995f) { value=1; stage=Stage::Decay; }
        break;
    case Stage::Decay:
        value=sustain+(value-sustain)*decay;
        if (std::fabs(value-sustain)<.002f) { value=sustain; stage=Stage::Sustain; }
        break;
    case Stage::Sustain: value=sustain; break;
    case Stage::Release:
        value*=release;
        if (value<=.0005f) { value=0; stage=Stage::Idle; }
        break;
    }
}
} // namespace shaker::analog
