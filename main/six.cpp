// Adapted from the user's MDLR engine/src/modules/{six,dsp}.rs.
#include "six.hpp"
#include "shaker.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace shaker {
using namespace analog;
namespace {
// The complete six-voice engine runs at 24 kHz. Linear reconstruction supplies
// the 48 kHz outputs, retaining polyphony while halving audio-rate DSP work.
constexpr int core_rate=kSampleRate/2;
constexpr int control_interval=16;
}

Six::Six() : attack_(coefficient(.0010f,core_rate)), filter_attack_(coefficient(.0008f,core_rate)),
    decay_(coefficient(.14f,core_rate)), filter_decay_(coefficient(.085f,core_rate)),
    release_(coefficient(.17f,core_rate)), filter_release_(coefficient(.10f,core_rate)),
    hpf_alpha_(1-std::exp(-tau*(18+.12f*.12f*1900)/core_rate)) { reset(); }

void Six::reset() {
    constexpr float phases[]={.01f,.17f,.34f,.52f,.71f,.88f};
    constexpr float drift[]={.09f,.23f,.39f,.57f,.73f,.91f};
    constexpr float trim[]={-.58f,-.34f,-.12f,.14f,.32f,.53f};
    for (int i=0;i<6;++i) {
        voices_[i]=Voice{};
        voices_[i].phase=phases[i];
        voices_[i].sub_phase=wrap(phases[i]+.5f);
        voices_[i].drift_phase=drift[i];
        voices_[i].trim=trim[i];
    }
    std::fill(std::begin(chorus_),std::end(chorus_),0);
    chorus_phase_a_=0; chorus_phase_b_=.31f; delay_a_=delay_b_=345;
    next_voice_=write_=0; control_counter_=0; dc_x_=dc_y_=0;
    previous_sample_=current_sample_=0; odd_sample_=false;
}

void Six::trigger(int midi_note,float velocity) {
    if (!std::isfinite(velocity) || velocity<=0) return;
    auto& v=voices_[next_voice_];
    next_voice_=(next_voice_+1)%6;
    midi_note=std::clamp(midi_note,52,76);
    v.hz=440*std::exp2((midi_note-69)/12.0f);
    v.pitch=(midi_note-48)/12.0f;
    v.increment=v.hz/core_rate;
    v.velocity=std::pow(std::clamp(velocity,0.0f,1.0f),.8f);
    v.carry=v.last; v.onset=0;
    v.amp_env=v.filter_env=0;
    v.amp_stage=v.filter_stage=Stage::Attack;
    v.gate=core_rate*65/1000;
    control_counter_=0;
}

float Six::read_chorus(float delay) const {
    float position=write_-delay;
    if (position<0) position+=512;
    const int index=int(position);
    const float fraction=position-index;
    return chorus_[index]*(1-fraction)+chorus_[(index+1)&511]*fraction;
}

float Six::tick() {
    if (!odd_sample_) {
        previous_sample_=current_sample_;
        current_sample_=tick_core();
    }
    const float out=odd_sample_ ? current_sample_ : .5f*(previous_sample_+current_sample_);
    odd_sample_=!odd_sample_;
    return out;
}

float Six::tick_core() {
    const bool control=(control_counter_++ & (control_interval-1))==0;
    constexpr float resonance=.30f, drive=.16f;
    constexpr float feedback=resonance*(1.14f+resonance*1.44f);
    constexpr float normalization=1.0f/1.2569805f; // sqrt(.62 saw + .76 pulse + .20 sub)
    if (control) {
        chorus_phase_a_=wrap(chorus_phase_a_+control_interval*.36f/core_rate);
        chorus_phase_b_=wrap(chorus_phase_b_+control_interval*.47f/core_rate);
        delay_a_=345+std::sin(tau*chorus_phase_a_)*4;
        delay_b_=345+std::sin(tau*wrap(chorus_phase_b_+.5f))*3.5f;
    }
    float sum=0;
    for (int i=0;i<6;++i) {
        auto& v=voices_[i];
        if (v.gate>0 && --v.gate==0) v.amp_stage=v.filter_stage=Stage::Release;
        envelope(v.amp_env,v.amp_stage,attack_,decay_,release_,0);
        envelope(v.filter_env,v.filter_stage,filter_attack_,filter_decay_,filter_release_,0);
        if (v.amp_stage==Stage::Idle) {
            std::fill(std::begin(v.stages),std::end(v.stages),0);
            v.hpf=v.last=0;
            continue;
        }
        if (control) {
            v.drift_phase=wrap(v.drift_phase+control_interval*(.019f+i*.0055f)/core_rate);
            const float drift=(v.trim*.07f+std::sin(tau*v.drift_phase)*.045f)*(.12f*.12f);
            v.increment=v.hz*std::exp2(drift/12)/core_rate;
            // SIX's filter-envelope PWM mode, with a modest pluck-shaped sweep.
            v.width_step=(.43f+v.filter_env*.28f*.36f-v.width)/control_interval;
            const float cutoff_octaves=.61f*7.5f+v.filter_env*.52f*(.65f+.35f*v.velocity)*4.9f+v.pitch*.30f*1.8f;
            const float cutoff=std::clamp(22*std::exp2(cutoff_octaves),18.0f,4000.0f);
            const float target_g=std::clamp(1-std::exp(-tau*cutoff/core_rate),.00004f,.78f);
            v.filter_step=(target_g-v.filter_g)/control_interval;
        }
        v.width+=v.width_step;
        v.filter_g+=v.filter_step;
        const float oscillator=(saw(v.phase,v.increment)*.62f+
            (pulse(v.phase,v.increment,v.width)-(2*v.width-1))*.76f+
            pulse(v.sub_phase,v.increment*.5f)*.20f*.82f)*normalization;
        v.phase=wrap(v.phase+v.increment);
        v.sub_phase=wrap(v.sub_phase+v.increment*.5f);
        v.hpf+=(oscillator-v.hpf)*hpf_alpha_;
        const float highpassed=oscillator-v.hpf*.12f;
        float sample=saturate((highpassed-v.stages[3]*feedback)*(1+drive*4.8f));
        for (int stage=0;stage<4;++stage) {
            v.stages[stage]+=(sample-v.stages[stage])*v.filter_g;
            sample=stage==2 ? saturate(v.stages[stage]*(1+resonance*.08f)) : v.stages[stage];
            if (std::fabs(v.stages[stage])<1e-20f) v.stages[stage]=0;
        }
        const float filtered=v.stages[3]*(1+resonance*.28f);
        v.onset=std::min(1.0f,v.onset+1.0f/72);
        v.last=v.onset*(filtered*v.amp_env*v.velocity*.33f*.70710678f)+(1-v.onset)*v.carry;
        sum+=v.last;
    }
    const float input=sum*.82f*(1.32f+drive);
    const float dry=input-dc_x_+.990025f*dc_y_;
    dc_x_=input; dc_y_=std::fabs(dry)<1e-18f ? 0 : dry;
    chorus_[write_]=dc_y_;
    write_=(write_+1)&511;
    // SIX mode-I chorus, averaged to mono. Both taps remain; no feedback.
    constexpr float mix=.24f;
    const float x=std::clamp(dc_y_*(1-mix*.48f)+.5f*(read_chorus(delay_a_)+read_chorus(delay_b_))*mix*.72f,-4.0f,4.0f);
    return x/(1+std::fabs(x)*.38f);
}
} // namespace shaker
