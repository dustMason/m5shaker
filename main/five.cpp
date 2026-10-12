// Adapted from the user's MDLR engine/src/modules/{five,dsp}.rs.
// This is FIVE's core at a fixed patch, with five shake-allocated voices.
#include "five.hpp"
#include "shaker.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace shaker {
using namespace analog;

Five::Five() : attack_(coefficient(.0007f,kSampleRate)),
    filter_attack_(coefficient(.0005f,kSampleRate)), filter_decay_(coefficient(.055f,kSampleRate)),
    amp_decay_(coefficient(.10f,kSampleRate)), filter_release_(coefficient(.055f,kSampleRate)),
    amp_release_(coefficient(.12f,kSampleRate)) { reset(); }

void Five::reset() {
    constexpr float phases[]={.02f,.19f,.41f,.68f,.86f};
    constexpr float drift[]={.13f,.31f,.47f,.61f,.79f};
    constexpr float trim[]={-.58f,-.24f,.03f,.27f,.51f};
    for (int i=0;i<5;++i) {
        voices_[i]=Voice{};
        voices_[i].phase_a=phases[i];
        voices_[i].phase_b=wrap(phases[i]+.271f);
        voices_[i].drift_phase=drift[i];
        voices_[i].trim=trim[i];
    }
    next_voice_=0; control_counter_=0; charge_=0;
}
void Five::trigger(int midi_note,float velocity) {
    if (!std::isfinite(velocity) || velocity<=0) return;
    auto& v=voices_[next_voice_];
    next_voice_=(next_voice_+1)%5;
    midi_note=std::clamp(midi_note,52,76);
    v.hz=440*std::exp2((midi_note-69)/12.0f);
    v.pitch=(midi_note-48)/12.0f;
    v.inc_a=v.hz/kSampleRate;
    v.inc_b=v.inc_a*std::exp2(.07f/12);
    v.velocity=std::pow(std::clamp(velocity,0.0f,1.0f),.8f);
    v.carry=v.last; v.onset=0;
    v.filter_env=v.amp_env=0;
    v.filter_stage=v.amp_stage=Stage::Attack;
    v.gate=kSampleRate*45/1000; // Short gate and zero sustain give every strike a fresh pluck.
    control_counter_=0;
}
float Five::tick(float activity) {
    activity=std::isfinite(activity) ? std::clamp(activity,0.0f,1.0f) : 0;
    // Repeated shaking builds brightness. Stronger motion charges it faster;
    // a 1.43 s time constant closes the filter gradually when the instrument rests.
    charge_=std::clamp(charge_+(1.7f*activity-.7f*charge_)/kSampleRate,0.0f,1.0f);
    if (charge_<1e-12f) charge_=0;
    const bool control=(control_counter_++ & 31)==0;
    constexpr float resonance=.40f, drive=.20f, rev=1.0f;
    constexpr float level_sum=.82f+.54f*.64f;
    constexpr float feedback=resonance*(1.26f+resonance*(1.38f+rev*.28f));
    constexpr float input_drive=(1+drive*5.2f+level_sum*.18f)*1.04f;
    float sum=0;
    for (int i=0;i<5;++i) {
        auto& v=voices_[i];
        if (v.gate>0 && --v.gate==0) v.filter_stage=v.amp_stage=Stage::Release;
        envelope(v.filter_env,v.filter_stage,filter_attack_,filter_decay_,filter_release_,0);
        envelope(v.amp_env,v.amp_stage,attack_,amp_decay_,amp_release_,0);
        if (v.amp_stage==Stage::Idle) {
            std::fill(std::begin(v.stages),std::end(v.stages),0);
            v.velocity=v.last=0;
            continue;
        }
        const float env_curve=v.filter_env*v.filter_env*(3-2*v.filter_env);
        if (control) {
            v.drift_phase=wrap(v.drift_phase+(32.0f/kSampleRate)*(.018f+i*.0065f));
            const float drift=(v.trim*.12f+std::sin(tau*v.drift_phase)*.075f)*(.22f*.22f);
            v.inc_a=v.hz*std::exp2(drift/12)/kSampleRate;
            v.inc_b=v.inc_a*1.0040515f; // FIVE's +0.07-semitone oscillator-B fine tune.
            const float cutoff_param=.57f+.37f*charge_;
            const float octaves=cutoff_param*7.4f+env_curve*.50f*(.7f+.3f*v.velocity)*4.8f+v.pitch*.18f*2.2f;
            const float cutoff=std::clamp(24*std::exp2(octaves),18.0f,8000.0f);
            v.filter_g=std::clamp((1-std::exp(-tau*cutoff/kSampleRate))*1.10f,.00004f,.78f);
        }
        const float a=saw(v.phase_a,v.inc_a)*.82f+pulse(v.phase_a,v.inc_a)*.18f;
        const float b=saw(v.phase_b,v.inc_b)*.54f;
        v.phase_a=wrap(v.phase_a+v.inc_a);
        v.phase_b=wrap(v.phase_b+v.inc_b);
        const float raw=(a*.82f+b*.64f)/std::sqrt(level_sum);
        // FIVE's saturating feedback, four cascaded poles, and second-stage drive.
        float sample=saturate((raw-v.stages[3]*feedback)*input_drive);
        for (int stage=0;stage<4;++stage) {
            v.stages[stage]+=(sample-v.stages[stage])*v.filter_g;
            sample=stage==1 ? saturate(v.stages[stage]*(1+resonance*.11f)) : v.stages[stage];
            if (std::fabs(v.stages[stage])<1e-20f) v.stages[stage]=0;
        }
        const float filtered=v.stages[3]*(1+resonance*(.22f+rev*.14f));
        // Centered mono mix; the original engine's stereo spread is unused here.
        v.onset=std::min(1.0f,v.onset+1.0f/96); // 2 ms crossfade when stealing a ringing voice.
        v.last=v.onset*(filtered*v.amp_env*v.velocity*.38f*.70710678f)+(1-v.onset)*v.carry;
        sum+=v.last;
    }
    const float x=std::clamp(sum*.62f*(1.25f+drive),-4.0f,4.0f);
    return x/(1+std::fabs(x)*.38f); // FIVE's output soft clip; shared DC block follows.
}
} // namespace shaker
