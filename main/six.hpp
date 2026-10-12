#pragma once

#include <cstdint>
#include "analog_dsp.hpp"

namespace shaker {
// Fixed-patch port of MDLR SIX: DCO/sub, HPF, four-pole VCF and dual-tap chorus.
class Six {
public:
    Six();
    void reset();
    void trigger(int midi_note,float velocity);
    float tick();
private:
    using Stage = analog::Stage;
    struct Voice {
        float phase=0, sub_phase=0, drift_phase=0, trim=0;
        float hz=0, pitch=0, increment=0, width=.43f;
        float amp_env=0, filter_env=0, velocity=0;
        float stages[4]={}, filter_g=0, filter_step=0, width_step=0, hpf=0;
        float onset=0, carry=0, last=0;
        Stage amp_stage=Stage::Idle, filter_stage=Stage::Idle;
        int gate=0;
    } voices_[6];
    float chorus_[512]={}; // Mono equivalent of SIX's identical centered L/R inputs.
    float chorus_phase_a_=0, chorus_phase_b_=.31f, delay_a_=345, delay_b_=345;
    float dc_x_=0, dc_y_=0;
    int next_voice_=0, write_=0;
    uint32_t control_counter_=0;
    float attack_, filter_attack_, decay_, filter_decay_, release_, filter_release_, hpf_alpha_;
    float read_chorus(float delay) const;
    float tick_core();
    float previous_sample_=0, current_sample_=0;
    bool odd_sample_=false;
};
} // namespace shaker
