# Dependencies and local changes

The project uses these pinned registry dependencies, with hashes in `dependencies.lock`:

- M5Stack M5Unified 0.2.25 (MIT): https://github.com/m5stack/M5Unified
- M5Stack M5GFX 0.2.32 (MIT and notices in its bundled sources): https://github.com/m5stack/M5GFX
- Espressif TinyUSB 0.19.0~3 (MIT): https://components.espressif.com/components/espressif/tinyusb/versions/0.19.0~3
- Espressif cmake_utilities 1.1.1 (Apache-2.0): https://components.espressif.com/components/espressif/cmake_utilities/versions/1.1.1
- ESP-IDF 5.4.x (Apache-2.0 plus component notices): https://github.com/espressif/esp-idf

`components/usb_device_uac` is vendored from Espressif's esp-iot-solution commit **58c2f852125cb1606c2137fc9b697efc3ce1c3d0** (component version 1.3.2). Original copyright and license notices are retained.

Source: https://github.com/espressif/esp-iot-solution/tree/58c2f852125cb1606c2137fc9b697efc3ce1c3d0/components/usb/usb_device_uac

Local changes for the mono instrument:

1. `tusb_uac/uac_config.h`: advertise mute/volume on the master channel only. Upstream's state arrays are sized for speaker channels and thus contain only the master entry in microphone-only mode.
2. `usb_device_uac.c`: reject control requests outside those state arrays, decode volume as little-endian signed data, and clamp it to the advertised -50..0 dB range.
3. `usb_device_uac.c` / its public header: expose active-host-bus state and disconnect for inactivity power management; no stream timing change.
4. `tusb/usb_descriptors.c`: assert that the compiled configuration descriptor matches its advertised byte length, use the product name for the audio control/input interface strings (macOS displays the input interface name), and increment the device revision to 1.01.

This copy includes upstream's microphone FIFO startup prefill and buffer sizing changes. It is kept local so those transport fixes and the mono control correction remain reproducible.

Original application code is provided under the MIT license in `LICENSE`. The MDLR-derived FIVE/SIX/helper files described below are an exception: their source provenance is preserved rather than assigning a new license to them. Third-party sources keep their own licenses.

## MDLR FIVE and SIX ports

`main/five.*`, `main/six.*`, and `main/analog_dsp.*` adapt the FIVE and SIX engines from MDLR (`engine/src/modules/five.rs`, `six.rs`, and `dsp.rs`). The source engine manifest declares `license = "UNLICENSED"`; no top-level license was supplied. These files and `main/five_tanh.inc` are excluded from the original application MIT grant. Publication here does not assign them a new open-source license. The upstream project is not included.

Source SHA-256 digests, captured October 11, 2026:

- `five.rs`: `1cdf98fc5e8221b1dc037c907eda33287b9944b180831e39fc2429579845baf1`
- `six.rs`: `f71a295072211adb195f4f6b6ba5c804aa4f54670b7bf1d24a89e06eb7e4de66`
- `dsp.rs`: `d61bc7370222d479db85c7e9f04ac1777d5ea07681cd1a5abfcba5ec7f441b6e`

FIVE retained core: five voices; original initial phase/drift/trim values; PolyBLEP saw and pulse equations; dual-oscillator mix and normalization; oscillator-B fine tune; attack/decay/sustain/release equations; Rev 3 envelope shaping; four filter poles with saturating feedback and second-stage saturation; output soft-clip equation.

Embedded adaptations:

- Rust is translated to a fixed-patch C++ class with no allocation or rack graph. Each output owns an independent instance.
- Shake peaks allocate voices round-robin with 45 ms gates for FIVE / 65 ms for SIX and per-note velocity (input velocity to the power 0.8). Idle voices skip DSP. Retriggers retain oscillator/filter state, restart envelopes, and crossfade the previous voice output over 2 ms for FIVE / 3 ms for SIX.
- The FIVE patch uses Rev 3, resonance 0.40, drive 0.20, vintage 0.22, key tracking 0.18, and velocity-scaled filter envelope amount 0.50. Amp attack/decay/sustain/release time constants are 0.0007 s / 0.10 s / 0 / 0.12 s; filter attack/decay/release are 0.0005 s / 0.055 s / 0.055 s, with zero sustain. Wave levels follow the original default patch.
- Shake activity accumulates a bounded filter-opening control, with a 1.43 s decay constant. The FIVE cutoff parameter spans 0.57..0.94, with a final 8 kHz cutoff ceiling.
- FIVE pitch/drift and filter coefficients update every 32 samples at 48 kHz. Oscillators, envelopes, and filter states still update every sample. Audio-rate Poly-Mod, sync, LFO routing, glide, unison, and external CV routing are not enabled by this patch.
- A 513-entry, linearly interpolated tanh table replaces per-sample transcendental calls. It covers 0..8 with odd symmetry and saturates outside that range. `five_tanh.inc` is generated from the mathematical function.
- Voices are centered and summed to mono; stereo spread is unused. The instrument's common DC blocker, output limiter, and light reverb follow both engines. A final 7.6 Hz DC blocker removes offset introduced by nonlinear limiting of the asymmetric synth waves.

SIX retained core: six voices with original initial phase/drift/trim values; normalized DCO saw/pulse/sub mixing; partial high-pass equation; separate envelopes; four filter poles with saturating feedback and third-stage saturation; mode-I chorus rates/depths/mixing; soft clipping.

SIX patch/adaptations:

- Saw 0.62, pulse 0.76, sub 0.20; no noise. Pulse width 0.43 plus filter-envelope PWM amount 0.28. The pulse's analytical DC component is removed before mixing.
- HPF 0.12, cutoff parameter 0.61, resonance 0.30, velocity-scaled filter envelope amount 0.52, key follow 0.30, analog drift 0.12, drive 0.16, VCA level 0.82.
- Amp attack/decay/sustain/release time constants: 0.001 s / 0.14 s / 0 / 0.17 s. Filter: 0.0008 s / 0.085 s / 0 / 0.10 s. Filter cutoff capped at 4 kHz.
- The SIX core runs at 24 kHz with linear reconstruction to 48 kHz, keeping all six voices. Envelopes, oscillators, filter coefficients, and delay times use the core rate. Its internal DC blocker coefficient is squared to preserve its time constant. Coefficients update every 16 core samples, with interpolated filter/PWM controls; other core state advances every core sample.
- Mode-I chorus uses a 345-core-sample nominal delay, rates 0.36/0.47 Hz, depths 4/3.5 core samples, mix 0.24. These preserve the original 48 kHz delay/modulation times. One 512-sample buffer replaces two 4,096-sample buffers because centered mono inputs are identical; both fractional read taps are retained and averaged.
- The fixed patch omits the parameter/CV graph, stereo spread, noise, and unused LFO/pitch routing. Notes latch on shake peaks. It does not accumulate shake activity into cutoff; per-strike velocity and envelope shape the pluck.

These are fixed-patch embedded ports, not byte-identical builds of the complete MDLR engine.

## Power-management register references

`main/power_control.cpp` uses device register addresses and power-state values from:

- [Espressif ESP-ADF v2.7 ES8311 suspend sequence](https://github.com/espressif/esp-adf/blob/v2.7/components/audio_hal/driver/es8311/es8311.c#L250). Only the register/value sequence is used; the driver implementation is not bundled.
- [Bosch BMI270 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf), power modes and auxiliary manual I2C; auxiliary setup also follows the pinned M5Unified BMI270 driver.
- [Bosch BMM150 sensor API](https://github.com/boschsensortec/BMM150_SensorAPI), register 0x4B power-control semantics.
- [TI LP5562 datasheet](https://www.ti.com/lit/ds/symlink/lp5562.pdf), CHIP_EN=0 standby.
- [M5Stack Echo Base schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/903/Sch_ECHO_Base_v1.0.pdf), IO-expander-controlled amplifier and directly powered microphone.

The power-control helper is application code. These register facts do not imply a measured whole-system standby current.

## Cuica and Guiro

The new pressure-envelope, body-resonance and ridge-contact models in `main/shaker.cpp`, along with their pixel art, are original application work. No recorded samples or third-party instrument model code are bundled. Cuica calls the existing shared PolyBLEP pulse helper described in the MDLR section above. The Smithsonian references in the README informed the instruments' playing behavior; they are not source-code dependencies.
