#include "shaker.hpp"
#include "instrument_icons.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace shaker;
constexpr float pi = 3.14159265358979323846f;

struct Stats { double rms, mean; int peak; };
Stats stats(const std::vector<int16_t>& data) {
    double sum = 0, square = 0; int peak = 0;
    for (auto sample : data) {
        sum += sample; square += double(sample)*sample;
        peak = std::max(peak, std::abs(int(sample)));
    }
    return {std::sqrt(square/data.size()), sum/data.size(), peak};
}

std::vector<int16_t> perform(int preset, float acceleration, float seconds = 3, float active_seconds = 99) {
    Motion motion; Synth synth(42);
    std::vector<int16_t> audio;
    uint32_t last_strike = 0;
    motion.update(0, 0, 1, .005f);
    for (int n = 0; n < int(seconds*200); ++n) {
        float t = n * .005f;
        auto f = motion.update(t < active_seconds ? acceleration*std::sin(2*pi*5*t) : 0, 0, 1, .005f);
        std::array<int16_t, 240> block;
        synth.render(block.data(), block.size(), preset, f.activity,
                     f.strike_id != last_strike ? f.strike : 0,1,0,-1,f.strike_direction);
        last_strike = f.strike_id;
        audio.insert(audio.end(), block.begin(), block.end());
    }
    return audio;
}

void write_wav(const char* name, const std::vector<int16_t>& samples) {
    std::ofstream f(name, std::ios::binary);
    assert(f.good());
    auto u16 = [&f](uint16_t n) { f.put(char(n & 255)); f.put(char(n >> 8)); };
    auto u32 = [&f](uint32_t n) { for (int i=0;i<4;++i) f.put(char((n >> (8*i)) & 255)); };
    f.write("RIFF",4); u32(uint32_t(36 + samples.size()*2)); f.write("WAVEfmt ",8);
    u32(16); u16(1); u16(1); u32(kSampleRate); u32(kSampleRate*2); u16(2); u16(16);
    f.write("data",4); u32(uint32_t(samples.size()*2));
    for (int16_t sample : samples) u16(uint16_t(sample));
    assert(f.good());
}

// Estimate the audible fundamental from normalized autocorrelation, including
// fractional-lag interpolation. This measures rendered audio, not coefficients.
double measuredFrequency(const std::vector<int16_t>& audio, double expected) {
    const int first = kSampleRate/10, length = kSampleRate/8;
    const int low = int(kSampleRate/expected*.95), high = int(kSampleRate/expected*1.05)+1;
    auto correlation = [&](int lag) {
        double xy=0, xx=0, yy=0;
        for (int i=first; i<first+length; ++i) {
            const double x=audio[i], y=audio[i+lag];
            xy+=x*y; xx+=x*x; yy+=y*y;
        }
        return xy/std::sqrt(xx*yy);
    };
    int best = low;
    for (int lag=low+1; lag<=high; ++lag)
        if (correlation(lag)>correlation(best)) best=lag;
    assert(best>low && best<high && correlation(best)>.90);
    const double a=correlation(best-1), b=correlation(best), c=correlation(best+1);
    return kSampleRate/(best+.5*(a-c)/(a-2*b+c));
}

void testDorian() {
    constexpr std::array<int,15> scale = {50,52,53,55,57,59,60,62,64,65,67,69,71,72,74};
    DorianNotes notes;
    assert(notes.update(0)==62);
    for (int i=0; i<15; ++i) assert(notes.update((i-7)*12.0f/7)==scale[i]);
    for (int i=14; i>=0; --i) assert(notes.update((i-7)*12.0f/7)==scale[i]);
    assert(notes.update(-1000)==50 && notes.update(1000)==74);
    assert(notes.update(std::numeric_limits<float>::quiet_NaN())==62);
    assert(notes.update(.59f*12/7)==62);
    assert(notes.update(.63f*12/7)==64);
    for (int i=0; i<100; ++i) assert(notes.update((.50f+.09f*std::sin(i))*12/7)==64);
    assert(notes.update(.37f*12/7)==62);

    double worst_cents = 0;
    for (int p : {kDorianPluck,kDorianHalo}) {
        assert(presetHasNotes(p));
        Synth silent;
        std::vector<int16_t> audio(kSampleRate/2);
        for (int note : scale) {
            silent.render(audio.data(),audio.size(),p,1,0,1,note-62.0f,note);
            assert(stats(audio).peak==0); // Activity/rotation cannot start a note.
            Synth single(31);
            single.render(audio.data(),audio.size(),p,0,.8f,1,0,note);
            const double expected=440*std::exp2((note-69)/12.0);
            const double actual=measuredFrequency(audio,expected);
            const double cents=1200*std::log2(actual/expected);
            worst_cents=std::max(worst_cents,std::abs(cents));
            assert(std::abs(cents)<5);
        }
        // New rotation/selection never bends or retriggers an already ringing note.
        Synth held(12), rotating(12);
        std::array<int16_t,192> a{}, b{};
        for (int block=0; block<1250; ++block) {
            held.render(a.data(),a.size(),p,0,block==0 ? .7f : 0,1,0,62);
            rotating.render(b.data(),b.size(),p,1,block==0 ? .7f : 0,1,
                            12*std::sin(block*.2f),block==0 ? 62 : scale[block%15]);
            assert(a==b);
        }
        // One strike ID produces exactly one note, and uses the note at the peak
        // even if a newer sensor reading has selected a different note meanwhile.
        OutputVoice voice;
        Synth expected;
        Performance state;
        state.preset=p;
        for (int block=0; block<750; ++block) {
            const int64_t now=1000000+int64_t(block)*4000;
            state.motion_us=now;
            state.pitch_semitones=12;
            state.selected_note=74;
            if (block==1) { state.strike_id=1; state.strike=.8f; state.strike_note=53; state.strike_us=now; }
            voice.render(a.data(),a.size(),state,now,1);
            expected.render(b.data(),b.size(),p,0,block==1 ? .8f : 0,1,0,53);
            assert(a==b);
        }
        // Fast, full-strength retriggers and alternating endpoint notes stay bounded.
        Synth rapid;
        for (int block=0; block<2500; ++block) {
            rapid.render(a.data(),a.size(),p,1,block%12==0 ? 1 : 0,1,0,(block/12)%2 ? 50 : 74);
            for (int16_t sample : a) assert(std::abs(int(sample))<31129);
        }
        std::vector<int16_t> decay(8*kSampleRate);
        rapid.render(decay.data(),decay.size(),p,0,0);
        assert(std::all_of(decay.end()-kSampleRate,decay.end(),[](int16_t x){return x==0;}));
        std::vector<int16_t> soft(kSampleRate/2), hard(kSampleRate/2);
        Synth gentle(4), strong(4);
        gentle.render(soft.data(),soft.size(),p,0,.15f,1,0,62);
        strong.render(hard.data(),hard.size(),p,0,.9f,1,0,62);
        assert(stats(hard).rms>stats(soft).rms*3);
    }
    std::printf("PASS: D Dorian range/hysteresis, strike-only plucks, latched notes, velocity, rapid retriggers; tuning within %.2f cents.\n",worst_cents);
}

void testButtonVolume() {
    using Action = ButtonControl::Action;
    ButtonControl button;
    assert(button.preset()==0 && button.volumeStep()==0);
    assert(button.update(false,0)==Action::None);
    int64_t now=1000000;
    // Short presses select on release; pressing down must not change a sound.
    for (int i=0; i<kPresetCount; ++i) {
        assert(button.update(true,now)==Action::None);
        assert(button.preset()==i);
        assert(button.update(false,now+100000)==Action::Sound);
        assert(button.preset()==(i+1)%kPresetCount && button.volumeStep()==0);
        assert(button.update(false,now+105000)==Action::None);
        now+=200000;
    }
    // Each hold advances one level; even a very long hold never auto-repeats.
    for (int level : {1,2,0,1}) {
        assert(button.update(true,now)==Action::None);
        assert(button.update(true,now+kButtonHoldUs-1)==Action::None);
        assert(button.update(true,now+kButtonHoldUs)==Action::Volume);
        assert(button.volumeStep()==level && button.preset()==0);
        for (int i=1; i<=3000; ++i)
            assert(button.update(true,now+kButtonHoldUs+i*5000)==Action::None);
        assert(button.update(false,now+16000000)==Action::None);
        assert(button.volumeStep()==level && button.preset()==0);
        now+=17000000;
    }
    // Release exactly at the threshold remains a hold if no poll caught it.
    ButtonControl boundary;
    boundary.update(true,0);
    assert(boundary.update(false,kButtonHoldUs)==Action::Volume);
    assert(boundary.volumeStep()==1 && boundary.preset()==0);
    boundary.update(true,1000000);
    assert(boundary.update(false,1000000+kButtonHoldUs-1)==Action::Sound);
    assert(boundary.volumeStep()==1 && boundary.preset()==1);
    constexpr int percent[] = {30,60,100}, setting[] = {77,153,255};
    for (int step=0; step<kVolumeCount; ++step) {
        assert(speakerVolumePercent(step)==percent[step] && speakerVolume(step)==setting[step]);
        VolumeCue whole, split;
        std::vector<int16_t> a(kSampleRate), b(kSampleRate);
        whole.mix(a.data(),a.size());
        assert(stats(a).peak==0); // No startup beep.
        whole.start(step+1); split.start(step+1);
        whole.mix(a.data(),a.size());
        for (size_t offset=0; offset<b.size();) {
            const size_t n=std::min(size_t(137),b.size()-offset);
            split.mix(b.data()+offset,n);
            offset+=n;
        }
        assert(a==b); // Tone timing is independent of callback block boundaries.
        int pulses=0;
        bool previous=false;
        for (size_t start=0; start<a.size(); start+=240) {
            bool audible=false;
            for (size_t i=start; i<start+240; ++i) audible |= std::abs(int(a[i]))>50;
            if (audible && !previous) ++pulses;
            previous=audible;
        }
        assert(pulses==step+1 && stats(a).peak<5000);
        // A cue cannot produce overflow with full-scale instrument PCM, and
        // the normal instrument level returns exactly after the brief duck.
        VolumeCue mixed;
        std::vector<int16_t> loud(kSampleRate,32767);
        mixed.start(step+1);
        mixed.mix(loud.data(),loud.size());
        assert(stats(loud).peak<=32767);
        assert(std::all_of(loud.end()-240,loud.end(),[](int16_t x){return x==32767;}));
    }
    std::puts("PASS: short press selects on release; 0.8 s hold advances 30/60/100 once, wraps, and consumes release; 1/2/3 bounded speaker cues.");
}

void testFive() {
    constexpr int scale[]={52,54,55,59,60,64,66,67,71,72,76};
    HirajoshiNotes notes;
    assert(notes.update(0)==64);
    for (int i=0;i<11;++i) assert(notes.update((i-5)*12.0f/5)==scale[i]);
    for (int i=10;i>=0;--i) assert(notes.update((i-5)*12.0f/5)==scale[i]);
    assert(notes.update(-100)==52 && notes.update(100)==76);
    assert(notes.update(std::numeric_limits<float>::quiet_NaN())==64);
    assert(notes.update(.59f*12/5)==64);
    assert(notes.update(.63f*12/5)==66);
    for (int i=0;i<100;++i) assert(notes.update((.50f+.09f*std::sin(i))*12/5)==66);
    assert(notes.update(.37f*12/5)==64);
    Five quiet, strong;
    float previous=0;
    for (int i=0;i<2*kSampleRate;++i) {
        assert(quiet.tick(.05f)==0 && strong.tick(.85f)==0); // Cutoff alone cannot start a note.
        assert(strong.openness()>=previous && strong.openness()<=1);
        previous=strong.openness();
    }
    assert(strong.openness()>.95f && quiet.openness()<.15f);
    quiet.trigger(64,.7f); strong.trigger(64,.7f);
    double soft_power=0, hard_power=0, soft_diff=0, hard_diff=0;
    float last_a=0,last_b=0;
    for (int i=0;i<kSampleRate/3;++i) {
        const float a=quiet.tick(.05f), b=strong.tick(.85f);
        if (i>kSampleRate/20) {
            soft_power+=a*a; hard_power+=b*b;
            soft_diff+=(a-last_a)*(a-last_a); hard_diff+=(b-last_b)*(b-last_b);
        }
        last_a=a; last_b=b;
    }
    const double brightness=(hard_diff/hard_power)/(soft_diff/soft_power);
    assert(brightness>2); // Normalized spectral-brightness proxy, independent of loudness.
    previous=strong.openness();
    for (int i=0;i<8*kSampleRate;++i) {
        const float y=strong.tick(0);
        assert(std::isfinite(y) && std::abs(y)<1);
        assert(strong.openness()<=previous);
        previous=strong.openness();
        if (i>3*kSampleRate) assert(y==0);
    }
    assert(strong.openness()<.005f);
    double worst_cents=0;
    for (int note : scale) {
        Synth voice;
        std::vector<int16_t> warmup(2*kSampleRate), audio(kSampleRate/2);
        voice.render(warmup.data(),warmup.size(),kFiveHira,.8f,0);
        voice.render(audio.data(),audio.size(),kFiveHira,.8f,.8f,1,0,note);
        const double expected=440*std::exp2((note-69)/12.0);
        const double cents=1200*std::log2(measuredFrequency(audio,expected)/expected);
        worst_cents=std::max(worst_cents,std::abs(cents));
        assert(std::abs(cents)<15); // Includes FIVE's deliberate oscillator fine detune.
    }
    Synth held, rotating;
    std::array<int16_t,192> a{}, b{};
    for (int block=0;block<1000;++block) {
        held.render(a.data(),a.size(),kFiveHira,.4f,block==0 ? .8f : 0,1,0,64);
        rotating.render(b.data(),b.size(),kFiveHira,.4f,block==0 ? .8f : 0,1,
                        12*std::sin(block*.03f),block==0 ? 64 : scale[block%11]);
        assert(a==b); // A new note selection does not retune a ringing voice.
    }
    Five rapid;
    for (int i=0;i<10*kSampleRate;++i) {
        if (i%2160==0) rapid.trigger(scale[(i/2160)%11],1); // 45 ms retriggers; all five voices active.
        const float y=rapid.tick(i%10000<5000 ? 1 : 0);
        assert(std::isfinite(y) && std::abs(y)<1);
    }
    rapid.reset();
    assert(rapid.openness()==0 && rapid.tick(0)==0);
    std::printf("PASS: E Hirajoshi, progressive/releasing cutoff, strike-only FIVE voices, stable five-voice retriggers; brightness %.1fx, tuning within %.2f cents.\n",brightness,worst_cents);
}

// Musical response tests: a single shake after silence must have a clear early
// attack, then lose most of its energy quickly rather than blooming into a pad.
void testPluckySynths() {
    const std::vector<int> hira_scale={52,54,55,59,60,64,66,67,71,72,76};
    const std::vector<int> lydian_scale={52,54,56,58,59,61,63,64,66,68,70,71,73,75,76};
    LydianNotes selector;
    assert(selector.update(0)==64);
    for (int i=0;i<15;++i) assert(selector.update((i-7)*12.0f/7)==lydian_scale[i]);
    for (int i=14;i>=0;--i) assert(selector.update((i-7)*12.0f/7)==lydian_scale[i]);
    assert(selector.update(-100)==52 && selector.update(100)==76);
    assert(selector.update(std::numeric_limits<float>::quiet_NaN())==64);
    assert(selector.update(.59f*12/7)==64 && selector.update(.63f*12/7)==66);
    for (int i=0;i<100;++i) assert(selector.update((.5f+.09f*std::sin(i))*12/7)==66);
    assert(selector.update(.37f*12/7)==64);
    assert(std::string(presetScaleName(kSixLydian))=="E LYDIAN");
    assert(std::string(noteName(68))=="G#4" && std::string(noteName(70))=="A#4");
    for (int p : {kFiveHira,kSixLydian}) {
        const auto& scale=p==kSixLydian ? lydian_scale : hira_scale;
        assert(presetHasNotes(p));
        double minimum_attack=1e9, maximum_tail=0, worst_cents=0;
        int latest_peak=0;
        for (int note : scale) {
            double soft_attack=0;
            for (float velocity : {.2f,.8f}) {
                Synth synth;
                std::vector<int16_t> pcm(kSampleRate);
                synth.render(pcm.data(),pcm.size(),p,1,0,1,12,note);
                assert(stats(pcm).peak==0); // Motion/rotation alone never trigger.
                // Let FIVE's accumulated cutoff close completely before the strike.
                for (int i=0;i<12;++i) synth.render(pcm.data(),pcm.size(),p,0,0);
                synth.render(pcm.data(),pcm.size(),p,0,velocity,1,0,note);
                const double attack=stats(std::vector<int16_t>(pcm.begin(),pcm.begin()+1200)).rms;
                const double tail=stats(std::vector<int16_t>(pcm.begin()+9600,pcm.begin()+14400)).rms;
                minimum_attack=std::min(minimum_attack,attack);
                maximum_tail=std::max(maximum_tail,tail/attack);
                assert(attack>(velocity<.5f ? 1000 : 3000));
                assert(tail<attack*.4); // By 200 ms it is a decaying pluck.
                if (velocity<.5f) soft_attack=attack;
                else assert(attack>soft_attack*2);
                double peak=0; int peak_ms=0;
                for (int ms=0;ms<150;++ms) {
                    const double rms=stats(std::vector<int16_t>(pcm.begin()+48*ms,pcm.begin()+48*(ms+5))).rms;
                    if (rms>peak) { peak=rms; peak_ms=ms; }
                }
                latest_peak=std::max(latest_peak,peak_ms);
                // Chorus can reinforce a later peak; the direct attack must
                // already have substantial energy in the first 10 ms.
                const double immediate=stats(std::vector<int16_t>(pcm.begin(),pcm.begin()+480)).rms;
                assert(immediate>peak*.5);
                assert(peak_ms<30);
                if (p==kSixLydian && velocity>.5f) {
                    // SIX includes a sub oscillator one octave below the selected note.
                    const double expected=220*std::exp2((note-69)/12.0);
                    const double cents=1200*std::log2(measuredFrequency(pcm,expected)/expected);
                    worst_cents=std::max(worst_cents,std::abs(cents));
                    assert(std::abs(cents)<5);
                }
            }
        }
        Synth held,rotating;
        OutputVoice output;
        Synth expected;
        Performance state; state.preset=p; state.activity=.4f;
        std::array<int16_t,192> a{},b{};
        for (int block=0;block<750;++block) {
            held.render(a.data(),a.size(),p,.4f,block==0 ? .8f : 0,1,0,64);
            rotating.render(b.data(),b.size(),p,.4f,block==0 ? .8f : 0,1,
                            12*std::sin(block*.03f),block==0 ? 64 : scale[block%scale.size()]);
            assert(a==b);
            const int64_t now=1000000+int64_t(block)*4000;
            state.motion_us=now; state.selected_note=76; state.pitch_semitones=12;
            if (block==1) { state.strike_id=1; state.strike=.8f; state.strike_note=55; state.strike_us=now; }
            output.render(a.data(),a.size(),state,now,1);
            expected.render(b.data(),b.size(),p,.4f,block==1 ? .8f : 0,1,0,55);
            assert(a==b); // Deliver once, using the note at the shake peak.
        }
        std::printf("PASS: %s cold plucks, velocity, latched notes; attack >=%.0f RMS, peak window <=%d ms, 200 ms tail <=%.0f%% of attack",presetName(p),minimum_attack,latest_peak,maximum_tail*100);
        if (p==kSixLydian) std::printf("; tuning within %.2f cents",worst_cents);
        std::puts(".");
    }
    Six rapid;
    for (int i=0;i<10*kSampleRate;++i) {
        if (i%2160==0) rapid.trigger(lydian_scale[(i/2160)%15],1);
        const float y=rapid.tick();
        assert(std::isfinite(y) && std::abs(y)<1);
    }
    for (int i=0;i<3*kSampleRate;++i) rapid.tick();
    assert(std::abs(rapid.tick())<1e-15f);
    rapid.trigger(64,std::numeric_limits<float>::quiet_NaN());
    assert(std::abs(rapid.tick())<1e-15f);
    rapid.reset();
    assert(rapid.tick()==0);
    std::puts("PASS: SIX six-voice retriggers, release to silence, invalid velocity, reset.");
    Synth whole, split;
    std::vector<int16_t> reference(48000), chunked(48000);
    whole.render(reference.data(),reference.size(),kSixLydian,0,.8f,1,0,70);
    for (size_t offset=0;offset<chunked.size();) {
        const size_t count=std::min(size_t(127),chunked.size()-offset);
        split.render(chunked.data()+offset,count,kSixLydian,0,offset==0 ? .8f : 0,1,0,70);
        offset+=count;
    }
    assert(reference==chunked); // Half-rate reconstruction must survive odd buffer lengths.
    Synth retriggers;
    std::array<int16_t,240> block{};
    int last=0, largest_step=0, zero_run=0;
    for (int tick=0;tick<2000;++tick) {
        retriggers.render(block.data(),block.size(),kSixLydian,1,tick%9==0 ? 1 : 0,1,0,lydian_scale[(tick/9)%15]);
        for (int16_t sample : block) {
            largest_step=std::max(largest_step,std::abs(int(sample)-last)); last=sample;
            if (tick>20) {
                zero_run=sample==0 ? zero_run+1 : 0;
                assert(zero_run<24); // No half-millisecond zero dropouts during dense playing.
            }
        }
    }
    // Report waveform edge size; a bright periodic edge alone is not proof of a click.
    std::printf("PASS: E Lydian mapping/hysteresis, odd block continuity, dense-six-voice output; max sample step %d.\n",largest_step);

}

void testTwistRecovery() {
    // A complete turn returns to the same physical pose. It must not leave a
    // hidden revolution to unwind before the next note can be selected.
    for (int turns : {-3,-1,1,3}) for (int samples_per_turn : {40,400}) {
        Twist twist;
        twist.update(0,0,1,0,0,0,.005f);
        const int direction = turns>0 ? 1 : -1;
        float pitch=0, previous=0;
        for (int i=1;i<=std::abs(turns)*samples_per_turn;++i) {
            const float angle=direction*2*pi*i/samples_per_turn;
            pitch=twist.update(0,-std::sin(angle),std::cos(angle),
                               -direction*72000.0f/samples_per_turn,0,0,.005f);
            assert(std::isfinite(pitch) && std::abs(pitch)<=12);
            if (samples_per_turn==400) assert(std::abs(pitch-previous)<.3f);
            previous=pitch;
        }
        for (int i=0;i<600;++i) pitch=twist.update(0,0,1,0,0,0,.005f);
        assert(std::abs(pitch)<.25f);
        DorianNotes dorian; HirajoshiNotes hira; LydianNotes lydian;
        assert(dorian.update(pitch)==62 && hira.update(pitch)==64 && lydian.update(pitch)==64);
        // Confirm useful note control immediately after the recovery, both ways.
        float angle=0;
        for (float destination : {pi*.25f, -pi*.25f, 0.0f}) {
            const float start=angle, speed=(destination-start)/pi*180;
            for (int i=1;i<=200;++i) {
                angle=start+(destination-start)*i/200;
                pitch=twist.update(0,-std::sin(angle),std::cos(angle),-speed,0,0,.005f);
            }
            for (int i=0;i<100;++i)
                pitch=twist.update(0,-std::sin(angle),std::cos(angle),0,0,0,.005f);
            assert(std::abs(pitch-destination*24/pi)<.25f);
        }
    }
    // Violent linear acceleration must not permanently disturb note selection.
    Twist shaken;
    shaken.update(0,0,1,0,0,0,.005f);
    float pitch=0;
    for (int i=0;i<400;++i)
        pitch=shaken.update(i%2 ? 7 : -7,4,-3,0,0,0,.005f);
    for (int i=0;i<400;++i) pitch=shaken.update(0,0,1,0,0,0,.005f);
    assert(std::abs(pitch)<.01f);
    // Model a tumble through the swing/twist singularity, then its exact reverse.
    // Exact quaternion increments generate physical sensor readings independently
    // of the attitude filter's Euler integration and musical pitch mapping.
    for (bool accelerating : {false,true}) {
        Twist tumble;
        tumble.update(0,0,1,0,0,0,.005f);
        std::array<float,4> q={1,0,0,0};
        auto turn = [&](int axis, float degrees) {
            const float half=degrees*pi/180/200;
            const float c=std::cos(half), s=std::sin(half);
            for (int i=0;i<100;++i) {
                std::array<float,3> v={0,0,0}; v[axis]=s;
                const auto old=q;
                q={old[0]*c-old[1]*v[0]-old[2]*v[1]-old[3]*v[2],
                   old[1]*c+old[0]*v[0]+old[2]*v[2]-old[3]*v[1],
                   old[2]*c+old[0]*v[1]+old[3]*v[0]-old[1]*v[2],
                   old[3]*c+old[0]*v[2]+old[1]*v[1]-old[2]*v[0]};
                const float board_x=2*(q[1]*q[3]-q[0]*q[2]);
                const float board_y=2*(q[2]*q[3]+q[0]*q[1]);
                const float board_z=1-2*(q[1]*q[1]+q[2]*q[2]);
                const float shake=accelerating ? (i%2 ? 1 : -1) : 0;
                const float speed=degrees*2;
                pitch=tumble.update(-board_y+6*shake,board_x-5*shake,board_z+4*shake,
                                    axis==1 ? -speed : 0,axis==0 ? speed : 0,
                                    axis==2 ? speed : 0,.005f);
                assert(std::isfinite(pitch) && std::abs(pitch)<=12);
            }
        };
        constexpr int axes[]={0,1,2,0};
        constexpr float degrees[]={180,270,-180,-270};
        for (int i=0;i<4;++i) turn(axes[i],degrees[i]);
        for (int i=3;i>=0;--i) turn(axes[i],-degrees[i]);
        for (int i=0;i<600;++i) pitch=tumble.update(0,0,1,0,0,0,.005f);
        assert(std::abs(pitch)<.25f);
        turn(1,45);
        for (int i=0;i<200;++i)
            pitch=tumble.update(0,-std::sin(pi*.25f),std::cos(pi*.25f),0,0,0,.005f);
        assert(std::abs(pitch-6)<.25f);
    }
    std::puts("PASS: repeated full-turn recovery at 180/1800 deg/s, wrap continuity, neutral scale notes, post-turn control, violent acceleration and tumble recovery.");
}

void testPowerSaving() {
    IdlePower power;
    constexpr int64_t second=1000000;
    power.update(second,false,false,true);
    power.update(16*second-1,false,false,true);
    assert(!power.screenOff() && !power.shouldSleep());
    power.update(16*second,false,false,true);
    assert(power.screenOff() && !power.shouldSleep());
    power.update(121*second-1,false,false,true);
    assert(!power.shouldSleep());
    power.update(121*second,false,false,true);
    assert(power.shouldSleep());
    power.update(122*second,true,false,true);
    assert(!power.screenOff() && !power.shouldSleep());
    // A host keeps it awake without needing to open the audio input. The screen
    // may still go dark. On USB removal/suspend, grant a full inactivity timeout.
    for (int i=123;i<400;++i) power.update(i*second,false,true,true);
    assert(power.screenOff() && !power.shouldSleep());
    power.update(519*second-1,false,false,true);
    assert(!power.shouldSleep());
    power.update(519*second,false,false,true);
    assert(power.shouldSleep());
    power.update(520*second,false,false,false);
    assert(!power.shouldSleep() && !power.screenOff()); // Sensor fault remains visible.
    power.update(second,false,false,true); // Clock restart cannot cause immediate sleep.
    assert(!power.shouldSleep() && !power.screenOff());
    ButtonControl button;
    button.restore(kPresetCount-1,2);
    assert(button.preset()==kPresetCount-1 && button.volumeStep()==2);
    assert(button.update(false,second)==ButtonControl::Action::None);
    button.update(true,second+1);
    assert(button.update(false,second+100001)==ButtonControl::Action::Sound);
    assert(button.preset()==kEgg && button.volumeStep()==2);
    button.restore(-1,99);
    assert(button.preset()==kEgg && button.volumeStep()==2);
    for (int preset=0;preset<kPresetCount;++preset) {
        OutputVoice output;
        Synth reference;
        Performance state;
        state.preset=preset;
        state.strike_note=preset==kFiveHira || preset==kSixLydian ? 64 : 62;
        std::array<int16_t,192> block{}, expected{};
        bool reached_idle=false;
        int64_t now=second;
        for (int i=0;i<5000;++i,now+=4000) {
            state.motion_us=now;
            const bool strike=i==10;
            if (strike) { ++state.strike_id; state.strike=1; state.strike_us=now; }
            output.render(block.data(),block.size(),state,now,1);
            reference.render(expected.data(),expected.size(),preset,0,strike ? 1 : 0,1,0,state.strike_note);
            if (!reached_idle) assert(block==expected); // Preserve the complete audible tail.
            if (output.idle()) {
                assert(std::all_of(block.begin(),block.end(),[](int16_t x){return x==0;}));
                reached_idle=true;
                break;
            }
        }
        assert(reached_idle);
        ++state.strike_id;
        state.strike=1;
        int peak=0;
        for (int i=0;i<10;++i) {
            now+=4000; state.motion_us=now;
            if (i==0) state.strike_us=now;
            output.render(block.data(),block.size(),state,now,1);
            assert(!output.idle());
            for (auto sample:block) peak=std::max(peak,std::abs(int(sample)));
        }
        assert(peak>1000); // The first new strike must survive the idle-to-playing transition.
    }
    std::puts("PASS: 15 s display/120 s sleep policy, USB protection/removal, sensor faults, restored controls; all preset tails preserved and first post-idle notes audible.");
}

void testFrictionPercussion() {
    // Appending sounds must not change any previously stored preset ID.
    static_assert(kSixLydian==6 && kCuica==7 && kGuiro==8);
    ButtonControl button;
    button.restore(kSixLydian,1);
    for (int expected : {kCuica,kGuiro,kVibroslap,kEgg}) {
        button.update(true,1000000);
        assert(button.update(false,1100000)==ButtonControl::Action::Sound);
        assert(button.preset()==expected && button.volumeStep()==1);
    }
    std::vector<int16_t> cuica;
    for (int p : {kCuica,kGuiro}) {
        assert(!presetHasNotes(p) && !presetHasPitch(p));
        Synth single(123);
        std::vector<int16_t> hit(kSampleRate);
        single.render(hit.data(),hit.size(),p,0,.9f);
        const auto window = [&](float t) {
            return stats(std::vector<int16_t>(hit.begin()+int(t*kSampleRate),
                                             hit.begin()+int((t+.02f)*kSampleRate))).rms;
        };
        assert(window(0)>400); // A single short shake must speak within 20 ms.
        assert(window(.4f)<window(.02f)*.05); // Distinct strokes rather than a stuck tone.
        assert(window(.8f)<1);
        if (p==kCuica) cuica=hit;
        // Sensor history arrives in 5 ms chunks, audio may request any block size.
        Synth whole(21), split(21);
        for (int n=0;n<400;++n) {
            std::array<int16_t,240> a{},b{};
            const float activity=n<200 ? .8f*std::abs(std::sin(n*.13f)) : 0;
            const float strike=n<200 && n%10==0 ? .9f : 0;
            whole.render(a.data(),a.size(),p,activity,strike);
            split.render(b.data(),127,p,activity,strike);
            split.render(b.data()+127,113,p,activity,0);
            assert(a==b);
        }
    }
    // Measure the voiced squeak in rendered PCM: a coherent fundamental with a
    // falling end to its pressure scoop, unlike a generic filtered-noise shaker.
    const auto pitch = [&](float start) {
        const int first=int(start*kSampleRate), count=1440;
        double best=-1; int best_lag=0;
        for (int lag=96;lag<350;++lag) {
            double xy=0,xx=0,yy=0;
            for (int i=0;i<count-lag;++i) {
                const double x=cuica[first+i],y=cuica[first+i+lag];
                xy+=x*y;xx+=x*x;yy+=y*y;
            }
            const double correlation=xy/std::sqrt(xx*yy);
            if (correlation>best) {best=correlation;best_lag=lag;}
        }
        assert(best>.85);
        return double(kSampleRate)/best_lag;
    };
    const double high=pitch(.025f),low=pitch(.13f);
    assert(high>low*1.2 && high<500 && low>150);
    // Count distinct bursts in a 1 ms power envelope over a settled scrape.
    // Harder motion must pack the audible ridges closer together.
    const auto bursts = [](float activity) {
        Synth synth(456);
        std::vector<int16_t> pcm(2*kSampleRate);
        synth.render(pcm.data(),pcm.size(),kGuiro,activity,0);
        std::vector<double> envelope(kSampleRate);
        double power=0,mean=0;
        for(int i=0;i<kSampleRate+47;++i) {
            const double x=pcm[kSampleRate/2+i];power+=x*x;
            if(i>=48) {const double old=pcm[kSampleRate/2+i-48];power-=old*old;}
            if(i>=47) {envelope[i-47]=power/48;mean+=power/48;}
        }
        mean/=envelope.size();
        int count=0,previous=-1000;
        for(size_t i=1;i+1<envelope.size();++i) {
            if(envelope[i]>mean*1.5 && envelope[i]>=envelope[i-1] &&
               envelope[i]>envelope[i+1] && int(i)-previous>160) {
                ++count;previous=int(i);
            }
        }
        return count;
    };
    const int soft=bursts(.15f),hard=bursts(.8f);
    assert(soft>40 && soft<110 && hard>soft*1.4);
    std::printf("PASS: CUICA/GUIRO order, fast attack, short decay, 127/113-frame continuity; cuica scoop %.1f -> %.1f Hz; guiro audible bursts %d -> %d/s.\n",high,low,soft,hard);
}

void testCuicaDirections() {
    // Signed peaks alternate along any playing axis, at gentle/firm intensities.
    for (const auto axis : {std::array<float,3>{1,0,0}, {0,1,0}, {0,0,1}, {.6f,0,.8f}}) {
        for(float hz : {2.0f,5.0f,7.0f}) for(float strength : {.7f,3.2f}) {
            Motion motion; motion.update(0,0,1,.005f);
            uint32_t last=0; int direction=0,events=0;
            for(int n=1;n<=800;++n) {
                const float a=strength*std::sin(2*pi*hz*n*.005f);
                const auto frame=motion.update(axis[0]*a,axis[1]*a,1+axis[2]*a,.005f);
                if(frame.strike_id!=last) {
                    assert(frame.strike_direction==1 || frame.strike_direction==-1);
                    if(events) assert(frame.strike_direction==-direction);
                    direction=frame.strike_direction;last=frame.strike_id;++events;
                }
            }
            assert(events>=int(8*hz)-2);
        }
    }
    // Separate kicks in one direction retain their sound. This fails a simple
    // event-count toggle; inserting a pause or missing a peak cannot swap sides.
    Motion kicks; kicks.update(0,0,1,.005f);
    uint32_t last=0; int events=0;
    for(int direction : {1,1,-1,-1,1}) {
        int count=0;
        for(int n=0;n<100;++n) {
            const auto frame=kicks.update(n<4 ? direction*2.0f : 0,0,1,.005f);
            if(frame.strike_id!=last) {
                assert(frame.strike_direction==direction);
                last=frame.strike_id;++events;++count;
            }
        }
        assert(count==1);
    }
    assert(events==5);
    // A new perpendicular playing axis is learned; its return stroke reverses.
    for(int direction : {1,-1,1,-1}) {
        int count=0;
        for(int n=0;n<100;++n) {
            const auto frame=kicks.update(0,n<4 ? direction*2.0f : 0,1,.005f);
            if(frame.strike_id!=last) {
                assert(frame.strike_direction==direction);last=frame.strike_id;++count;
            }
        }
        assert(count==1);
    }
    std::array<std::vector<int16_t>,2> strokes;
    for(int i=0;i<2;++i) {
        Synth voice(123);
        strokes[i].resize(kSampleRate);
        voice.render(strokes[i].data(),strokes[i].size(),kCuica,0,.9f,1,0,-1,i ? -1 : 1);
    }
    const auto frequency = [](const std::vector<int16_t>& pcm) {
        const int first=1200,count=1440;
        int best_lag=0;double best=-1;
        for(int lag=96;lag<400;++lag) {
            double xy=0,xx=0,yy=0;
            for(int i=0;i<count-lag;++i) {
                const double x=pcm[first+i],y=pcm[first+i+lag];xy+=x*y;xx+=x*x;yy+=y*y;
            }
            const double correlation=xy/std::sqrt(xx*yy);
            if(correlation>best) {best=correlation;best_lag=lag;}
        }
        assert(best>.80);
        return double(kSampleRate)/best_lag;
    };
    const double push=frequency(strokes[0]),pull=frequency(strokes[1]);
    assert(push>pull*1.35); // Audible push/pull contrast, not just a polarity inversion.
    for(int direction : {1,-1}) {
        Synth gentle(7),hard(7);
        std::vector<int16_t> a(kSampleRate/2),b(kSampleRate/2);
        gentle.render(a.data(),a.size(),kCuica,0,.15f,1,0,-1,direction);
        hard.render(b.data(),b.size(),kCuica,0,.9f,1,0,-1,direction);
        assert(stats(b).rms>stats(a).rms*2.5);
    }
    // Both outputs consume the latched direction with the same strike ID. A
    // skipped ID, repeated same-direction peak or non-event cannot cause drift.
    OutputVoice speaker,usb; Synth reference;
    Performance state;state.preset=kCuica;
    for(int block=0;block<200;++block) {
        const int64_t now=1000000+block*4000;
        state.motion_us=now;
        const bool hit=block==1 || block==30 || block==60 || block==90;
        if(hit) {
            state.strike_id+=block==30 ? 2 : 1;
            state.strike=.8f;state.strike_us=now;
            state.strike_direction=block==1 || block==90 ? 1 : -1;
        }
        std::array<int16_t,192> a{},b{},c{};
        speaker.render(a.data(),a.size(),state,now,1);
        usb.render(b.data(),b.size(),state,now,1);
        reference.render(c.data(),c.size(),kCuica,0,hit ? .8f : 0,1,0,-1,state.strike_direction);
        assert(a==b && b==c);
    }
    // Rapid alternating strokes retain exact output over odd transport splits.
    Synth whole(31),split(31);
    for(int n=0;n<600;++n) {
        std::array<int16_t,240> a{},b{};
        const float strike=n%10==0 ? .9f : 0;
        const int direction=(n/10)%2 ? -1 : 1;
        whole.render(a.data(),a.size(),kCuica,.4f,strike,1,0,-1,direction);
        split.render(b.data(),127,kCuica,.4f,strike,1,0,-1,direction);
        split.render(b.data()+127,113,kCuica,.4f,0,1,0,-1,-direction);
        assert(a==b); // A direction without a new strike is ignored.
        for(auto sample:a) assert(std::abs(int(sample))<31129);
    }
    std::printf("PASS: cuica push/pull directions on four axes at 2/5/7 Hz, same-direction kicks, axis changes, sink agreement and rapid split-block strokes; %.1f / %.1f Hz.\n",push,pull);
}

void testVibroslap() {
    static_assert(kCuica==7 && kGuiro==8 && kVibroslap==9);
    static_assert(instrument_icons::kCount==kPresetCount);
    assert(!presetHasNotes(kVibroslap) && !presetHasPitch(kVibroslap));
    // Below-threshold movement cannot excite this instrument, even at maximum
    // continuous activity. The threshold is inclusive and applies to peaks.
    Synth silent;
    std::vector<int16_t> block(kSampleRate);
    for (float strike : {0.0f, kVibroslapThreshold-.001f,
                        std::numeric_limits<float>::quiet_NaN()}) {
        silent.render(block.data(),block.size(),kVibroslap,1,strike);
        assert(stats(block).peak==0);
    }
    std::array<double,3> lengths{};
    int index=0;
    for (float strike : {kVibroslapThreshold,.7f,1.0f}) {
        Synth single(123);
        std::vector<int16_t> hit(3*kSampleRate);
        single.render(hit.data(),hit.size(),kVibroslap,0,strike);
        assert(stats({hit.begin(),hit.begin()+960}).rms>250);
        // Measure the audible duration from PCM, rather than inspecting a timer.
        for (int start=0;start<int(hit.size());start+=480) {
            if (stats({hit.begin()+start,hit.begin()+start+480}).rms>20)
                lengths[index]=double(start+480)/kSampleRate;
        }
        assert(stats({hit.end()-kSampleRate/2,hit.end()}).peak==0);
        ++index;
    }
    assert(lengths[0]>.15 && lengths[0]<.4);
    assert(lengths[1]>lengths[0]+.4 && lengths[2]>lengths[1]+.4);
    assert(lengths[2]>1.4 && lengths[2]<2);

    // Follow-through cannot extend the latched tail; an immediate second large
    // peak is ignored, but a new deliberate impact can retrigger after 160 ms.
    Synth still(7), moving(7), retriggered(7);
    bool changed=false;
    for(int n=0;n<600;++n) {
        std::array<int16_t,240> a{},b{},c{};
        const float hit=n==0 ? .7f : 0;
        still.render(a.data(),a.size(),kVibroslap,0,hit);
        moving.render(b.data(),b.size(),kVibroslap,1,n==10 ? 1 : n==0 ? hit : .3f);
        retriggered.render(c.data(),c.size(),kVibroslap,0,n==40 ? 1 : hit);
        assert(a==b);
        if(n<40) assert(a==c);
        else changed=changed || a!=c;
    }
    assert(changed);

    // Threshold gating is reachable with realistic IMU peaks on any axis.
    for(const auto axis : {std::array<float,3>{1,0,0},{0,1,0},{0,0,1},{.6f,0,.8f}}) {
        for(float force : {1.0f,3.2f}) {
            Motion motion; motion.update(0,0,1,.005f);
            Synth voice; uint32_t last=0; int peak=0,accepted=0;
            for(int n=0;n<300;++n) {
                const float a=n<12 ? force*std::sin(pi*n/12) : 0;
                const auto f=motion.update(axis[0]*a,axis[1]*a,1+axis[2]*a,.005f);
                const float strike=f.strike_id!=last ? f.strike : 0;
                last=f.strike_id;
                accepted+=strike>=kVibroslapThreshold;
                std::array<int16_t,240> pcm{};
                voice.render(pcm.data(),pcm.size(),kVibroslap,f.activity,strike);
                for(auto x:pcm) peak=std::max(peak,std::abs(int(x)));
            }
            assert(force<2 ? accepted==0 && peak==0 : accepted==1 && peak>1000);
        }
    }
    // Both sinks receive one event each; duplicate IDs cannot restart the sound.
    OutputVoice speaker,usb;
    Synth reference;
    Performance state; state.preset=kVibroslap;
    for(int n=0;n<800;++n) {
        const int64_t now=1000000+int64_t(n)*4000;
        state.motion_us=now; state.activity=1;
        if(n==1) {state.strike_id=1;state.strike=1;state.strike_us=now;}
        std::array<int16_t,192> a{},b{},c{};
        speaker.render(a.data(),a.size(),state,now,1);
        usb.render(b.data(),b.size(),state,now,1);
        reference.render(c.data(),c.size(),kVibroslap,0,n==1 ? 1 : 0);
        assert(a==b && a==c);
    }
    assert(speaker.idle() && usb.idle()); // Weak motion does not keep silent DSP busy.
    Synth whole(17),split(17);
    for(int n=0;n<600;++n) {
        const float hit=n%47==0 ? .43f+.57f*(n%3)/2 : 0;
        std::array<int16_t,240> a{},b{};
        whole.render(a.data(),a.size(),kVibroslap,1,hit);
        split.render(b.data(),127,kVibroslap,1,hit);
        split.render(b.data()+127,113,kVibroslap,1,0);
        assert(a==b);
    }
    std::printf("PASS: VIBROSLAP threshold, four-axis impact gating, one-shot/cooldown/retrigger, sink agreement, idle and split blocks; audible lengths %.2f / %.2f / %.2f s.\n",lengths[0],lengths[1],lengths[2]);
}

int main(int argc, char** argv) {
    testVibroslap();
    testCuicaDirections();
    testFrictionPercussion();
    testPowerSaving();
    testTwistRecovery();
    testButtonVolume();
    testDorian();
    testFive();
    testPluckySynths();
    // Real enclosure Y (USB direction) maps to negative raw BMI270 X gyro.
    for (int direction : {-1,1}) {
        Twist twist;
        twist.update(0,0,1,0,0,0,.005f);
        float pitch=0;
        for (int i=1;i<=200;++i) {
            float angle=direction*pi*.5f*i/200;
            pitch=twist.update(0,-std::sin(angle),std::cos(angle),-direction*90,0,0,.005f);
        }
        for (int i=0;i<100;++i) pitch=twist.update(0,-float(direction),0,0,0,0,.005f);
        assert(std::abs(pitch-direction*12)<.25f);
        for (int i=1;i<=200;++i) {
            float angle=direction*pi*.5f*(1-i/200.0f);
            pitch=twist.update(0,-std::sin(angle),std::cos(angle),direction*90,0,0,.005f);
        }
        for (int i=0;i<200;++i) pitch=twist.update(0,0,1,0,0,0,.005f);
        assert(std::abs(pitch)<.25f);
    }
    for (int other_axis : {0,2}) {
        Twist twist;
        twist.update(0,0,1,0,0,0,.005f);
        float pitch=0;
        for (int i=1;i<=200;++i) {
            float angle=pi*.5f*i/200;
            pitch=other_axis==0 ? twist.update(-std::sin(angle),0,std::cos(angle),0,90,0,.005f)
                                : twist.update(0,0,1,0,0,90,.005f);
        }
        assert(std::abs(pitch)<.05f);
    }
    Twist vertical;
    vertical.update(-1,0,0,0,0,0,.005f);
    float vertical_pitch=0;
    for(int i=0;i<200;++i) vertical_pitch=vertical.update(-1,0,0,-90,0,0,.005f);
    assert(vertical_pitch>11 && vertical_pitch<=12);
    assert(vertical.update(0,0,1,0,0,std::numeric_limits<float>::quiet_NaN(),.005f)==0);
    std::puts("PASS: USB-axis pitch in both directions, return to neutral, other-axis rejection and vertical gyro tracking.");
    // Local audio runs without USB callbacks. Starting/muting/stopping USB
    // must not advance or mute the independently owned speaker synth.
    OutputVoice speaker, speaker_reference, usb;
    Performance performance;
    std::array<int16_t,192> local{}, reference{}, host{};
    int local_peak = 0;
    for (int block=0; block<750; ++block) {
        const int64_t now = 1000000 + int64_t(block)*4000;
        performance.preset = (block / 50) % kPresetCount;
        performance.pitch_semitones = 12*std::sin(block*.03f);
        performance.activity = .6f;
        performance.motion_us = now;
        if (block % 25 == 0) {
            performance.strike = .8f;
            ++performance.strike_id;
            performance.strike_us = now;
        }
        speaker.render(local.data(),local.size(),performance,now,1);
        speaker_reference.render(reference.data(),reference.size(),performance,now,1);
        assert(local == reference);
        for (auto sample : local) local_peak = std::max(local_peak,std::abs(int(sample)));
        // No host for the first second; second second USB is active but muted;
        // third second USB closes again. Speaker audio keeps running throughout.
        if (block >= 250 && block < 500) {
            usb.render(host.data(),host.size(),performance,now,0);
            assert(std::all_of(host.begin(),host.end(),[](int16_t x){return x==0;}));
        }
    }
    assert(local_peak > 1000);
    // Old motion must not restart either output after a transport pause.
    performance.activity = 0;
    usb.render(host.data(),host.size(),performance,10000000,1);
    assert(std::all_of(host.begin(),host.end(),[](int16_t x){return x==0;}));
    // A lost sensor source must eventually decay, even if the last level was high.
    performance.activity = 1;
    performance.motion_us = 10000000;
    for (int block=0; block<2000; ++block)
        speaker.render(local.data(),local.size(),performance,10000000+int64_t(block)*4000,1);
    assert(std::all_of(local.begin(),local.end(),[](int16_t x){return x==0;}));
    std::puts("PASS: standalone speaker rendering, independent USB mute/start/stop, stale motion gating.");
    // Gravity in any orientation and small sensor noise must not sound.
    for (const auto gravity : {std::array<float,3>{0,0,1}, {1,0,0}, {0,-1,0}, {.6f,0,.8f}}) {
        Motion m;
        for (int i=0; i<1000; ++i) {
            auto f = m.update(gravity[0] + .003f*std::sin(i), gravity[1], gravity[2], .005f);
            assert(f.activity == 0 && f.strike_id == 0);
        }
    }
    Motion m;
    m.update(0,0,1,.005f);
    uint32_t strikes = 0;
    float max_soft = 0;
    for (int i=0; i<400; ++i) {
        auto f = m.update(.7f*std::sin(2*pi*5*i*.005f),0,1,.005f);
        max_soft = std::max(max_soft, f.activity); strikes = f.strike_id;
    }
    assert(strikes >= 17 && strikes <= 21); // Two reversals per 5 Hz shake.
    assert(max_soft > .1f && max_soft < .3f);
    auto f = m.update(std::numeric_limits<float>::quiet_NaN(),0,1,.005f);
    assert(f.activity == 0 && f.strike_id == 0);
    f = m.update(0,0,1,.3f);
    assert(f.activity == 0);
    std::vector<int16_t> demo;
    for (int p=0; p<kPresetCount; ++p) {
        Synth s;
        std::vector<int16_t> buf(kSampleRate);
        s.render(buf.data(), buf.size(), p, 0, 0);
        assert(stats(buf).peak == 0);
        const auto soft = perform(p, .7f);
        const auto hard = perform(p, 3.2f);
        const auto a = stats(soft), b = stats(hard);
        std::printf("%-11s soft RMS %7.1f, hard RMS %7.1f, gain %4.1fx, peak %d\n",
                    presetName(p), a.rms, b.rms, a.rms>0 ? b.rms/a.rms : 0, b.peak);
        if(p==kVibroslap) assert(a.peak==0 && b.rms>200);
        else assert(a.rms > 20 && b.rms > a.rms*2);
        assert(b.peak < 31129 && std::abs(b.mean) < b.rms*.02);
        s.render(buf.data(), buf.size(), p, 1, 1);
        for (int i=0;i<8;++i) s.render(buf.data(),buf.size(),p,0,0);
        assert(stats(buf).peak == 0);  // All resonances eventually become silent.
        s.render(buf.data(),buf.size(),p,1,0,0);
        s.render(buf.data(),buf.size(),p,1,0,0);
        assert(stats(buf).peak == 0);  // Mute settles to digital silence.
        s.render(buf.data(),buf.size(),p,std::numeric_limits<float>::infinity(),0);
        s.reset();
        s.render(buf.data(),buf.size(),p,0,0);
        assert(stats(buf).peak == 0);
        // A preset change must clear the previous sound's delay lines and modes.
        s.render(buf.data(),buf.size(),p,1,1);
        s.render(buf.data(),buf.size(),(p+1)%kPresetCount,0,0);
        assert(stats(buf).peak == 0);
        // Transport block boundaries must not change the synthesis or reverb.
        Synth whole(17), split(17);
        std::vector<int16_t> a_block(4096), b_block(4096);
        whole.render(a_block.data(),a_block.size(),p,.4f,.8f,1,7);
        for (size_t offset=0;offset<b_block.size();offset+=64)
            split.render(b_block.data()+offset,64,p,.4f,offset==0 ? .8f : 0,1,7);
        assert(a_block == b_block);
        Synth flat(19), bent(19);
        std::vector<int16_t> normal(kSampleRate), shifted(kSampleRate);
        flat.render(normal.data(),normal.size(),p,.4f,.8f,1,0);
        bent.render(shifted.data(),shifted.size(),p,.4f,.8f,1,12);
        if (!presetHasPitch(p)) assert(normal==shifted); // Traditional sounds ignore rotation exactly.
        else {
            assert(presetHasPitch(p) && normal!=shifted);
            std::array<int16_t,192> bend{};
            for (int block=0;block<750;++block)
                bent.render(bend.data(),bend.size(),p,.6f,0,1,12*std::sin(block*.05f));
            for (int second=0;second<8;++second) bent.render(buf.data(),buf.size(),p,0,0,1,0);
            assert(stats(buf).peak==0); // Rapid pitch sweeps must not destabilize resonances.
        }
        if (argc > 1) {
            auto quiet = perform(p,p==kVibroslap ? 1.8f : .7f,2.5f,1.2f), loud = perform(p,3.2f,3.5f,1.2f);
            demo.insert(demo.end(), quiet.begin(),quiet.end());
            demo.insert(demo.end(), loud.begin(),loud.end());
        }
    }
    std::puts("PASS: synthetic pitch modulation and fixed traditional pitch.");
    for (int p : {kChainRide}) {
        const auto ring = perform(p,3.2f,4,1);
        std::vector<int16_t> tail(ring.begin()+2*kSampleRate,ring.begin()+3*kSampleRate);
        const auto t = stats(tail);
        std::printf("%-11s tail RMS one second after stop: %.1f\n",presetName(p),t.rms);
        assert(t.rms > 3); // Chain Ride must actually ring.
    }
    if (argc > 1) write_wav(argv[1], demo);
    if (argc > 2) {
        std::vector<int16_t> melodic;
        for (int p : {kDorianPluck,kDorianHalo}) {
            Synth pluck;
            DorianNotes selector;
            for (int step=0; step<15; ++step) {
                const int note=selector.update((step-7)*12.0f/7);
                std::vector<int16_t> phrase(int(.4f*kSampleRate));
                pluck.render(phrase.data(),phrase.size(),p,0,.25f+.65f*step/14,1,0,note);
                melodic.insert(melodic.end(),phrase.begin(),phrase.end());
            }
            std::vector<int16_t> tail(3*kSampleRate);
            pluck.render(tail.data(),tail.size(),p,0,0);
            melodic.insert(melodic.end(),tail.begin(),tail.end());
        }
        write_wav(argv[2],melodic);
    }
    for (int p : {kFiveHira,kSixLydian}) {
        const int argument=p==kFiveHira ? 3 : 4;
        if (argc<=argument) continue;
        Synth five;
        HirajoshiNotes selector;
        LydianNotes lydian_selector;
        std::vector<int16_t> demo_five;
        for (int block=0;block<3000;++block) {
            const float t=block*.004f;
            const float activity=t<8 ? std::min(.95f,.06f+t*.115f) : 0;
            const int step=std::min(p==kSixLydian ? 14 : 10,int(t/(p==kSixLydian ? .5f : .7f)));
            const int note=p==kSixLydian ? lydian_selector.update((step-7)*12.0f/7) : selector.update((step-5)*12.0f/5);
            std::array<int16_t,192> pcm{};
            five.render(pcm.data(),pcm.size(),p,activity,
                        t<8 && block%75==0 ? .65f : 0,1,0,note);
            demo_five.insert(demo_five.end(),pcm.begin(),pcm.end());
        }
        write_wav(argv[argument],demo_five);
    }
    std::puts("PASS: motion, dynamics, DC, level limits, decay, mute, reset, preset clearing, block continuity and ringing tails.");
}
