#pragma once

#include <cstdint>
#include "analog_dsp.hpp"

namespace shaker {
// Fixed-patch C++ port of MDLR's FIVE oscillator/envelope/filter core.
// See THIRD_PARTY.md for source hashes and the embedded adaptations.
class Five {
public:
    Five();
    void reset();
    void trigger(int midi_note, float velocity);
    float tick(float activity);
    float openness() const { return charge_; }
private:
    using Stage = analog::Stage;
    struct Voice {
        float phase_a=0, phase_b=0, drift_phase=0, trim=0;
        float inc_a=0, inc_b=0, hz=0, pitch=0;
        float filter_env=0, amp_env=0, velocity=0;
        float onset=0, carry=0, last=0;
        Stage filter_stage=Stage::Idle, amp_stage=Stage::Idle;
        float stages[4]={}, filter_g=0;
        int gate=0;
    } voices_[5];
    int next_voice_=0;
    uint32_t control_counter_=0;
    float charge_=0;
    float attack_, filter_attack_, filter_decay_, amp_decay_, filter_release_, amp_release_;
};
} // namespace shaker
