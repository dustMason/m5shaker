# m5shaker

Firmware for the **M5Stack AtomS3R with its built-in screen and BMI270**. The board synthesizes percussion and streams it to a Mac as a USB Audio Class 2 input named **AtomS3R Shaker**. With an **Atomic Echo Base (A149)** attached, it also plays through the base's speaker. USB works with or without the base.

**Status:** nine synthesized sounds, direction-sensitive cuíca push/pull, pixel-art display, USB audio, and Echo Base speaker output. Portable sanitizer tests and ESP-IDF builds pass. The latest sound, display, and power-management changes still need complete hardware verification. This repository contains source and dependency manifests; it does not include firmware binaries or generated previews.

## Playing

- Shake in any direction. Gentle motion gives quieter, softer sounds; stronger motion increases loudness, brightness, and the density of simulated collisions.
- **Tap and release the front screen button** to cycle through the **9 sounds** listed below, then return to Egg. The change happens on release.
- **Hold the front button for 0.8 seconds** to advance speaker volume: **30% → 60% → 100% → 30%**. One hold changes volume once, even if you keep holding. Release before making another change; releasing a hold does not change the sound.
- The speaker gives **one quick beep for 30%, two for 60%, and three for 100%**. The separate side reset button is used for programming.
- The screen shows only the current instrument's pixel-art icon: no text, meters, note readout, or status overlays. Volume holds retain the instrument icon; use the one/two/three speaker beeps to identify the volume setting.
- Resting the board allows the sound to decay to silence. Button motion is suppressed while pressed and for 100 ms afterward.
- The first boot starts with Egg and 30% speaker volume. Automatic sleep saves the selected preset and volume; later boots restore that most recent sleep snapshot. Changes made after that snapshot are not saved until the next automatic sleep.

### Battery operation and automatic sleep

- After **15 seconds without motion or button activity**, the backlight turns off and screen drawing stops. Motion or a front-button press restores it. The normal tap/hold action still applies to that press.
- After **two minutes without activity and without an active USB host**, the device enters deep sleep. It shuts down audio playback, the ES8311 codec, speaker amplifier, BMI270 sensors, BMM150 compass, LCD and LP5562 backlight driver. Relevant display/audio outputs are held at defined levels.
- **Briefly tap the side reset button to wake.** Do not hold it: a long hold enters download mode. The front button and shaking do not wake deep sleep. No periodic timer or motion wakeup is enabled.
- Hold the Atom in your playing pose during wake; boot establishes a fresh neutral orientation. The most recent pre-sleep sound and speaker volume are restored from NVS, including across an external reset or power cycle. Settings are saved once on entry to sleep, not repeatedly while playing.
- An awake Mac USB bus prevents automatic deep sleep even when no recording app is open. The display can still go dark. USB disconnection or host suspend starts a fresh two-minute inactivity interval. A USB charger or power bank does not count as a host. After sleeping during host suspend, a side-reset tap is needed when the Mac wakes.
- After one second of completely zero PCM without new excitation, each output bypasses synthesis until the next activity, strike, preset or gain change. This happens after the audible tail, not on a fixed note cutoff. I2S remains paced normally until deep sleep so the next shake does not have to restart the codec. CPU speed and playing-time audio scheduling are unchanged.
- The unused BMM150 compass is suspended during playing too. Wi-Fi and Bluetooth remain unused. If motion data fails, automatic sleep is inhibited so the sensor error stays visible.

The [Atomic Battery Base A151, 200 mAh](https://shop.m5stack.com/products/atomic-battery-base-200mah) uses a manual boost/charge switch. Its specified **2.55 µA standby applies with that physical switch OFF**, not to the complete powered stack in deep sleep. Firmware cannot switch off its boost converter or battery indicator circuit. The Echo Base microphone is also wired directly to its supply. Actual whole-stack sleep current must be measured; this firmware makes no microamp or battery-life guarantee. Use the base's switch for storage when the lowest drain is needed.

**Echo Base stacking:** M5Stack requires a pin modification when combining this battery base with the Echo/Voice Base. Follow the illustrated [manufacturer compatibility instructions](https://docs.m5stack.com/en/atom/Atomic_Battery_Base#additional-notes) before stacking them. Battery-voltage sensing is unavailable in that arrangement; the firmware deliberately does not read GPIO8 as a battery ADC because Echo uses it for its audio clock. The base must be manually switched OFF for charging through the Atom USB port.

The sleep implementation uses an NVS snapshot of the codec registers as well as musical settings: the side-reset button resets the ESP32, but does not remove power from the Echo Base. On boot, the snapshot restores registers that M5Unified's short speaker-start sequence does not initialize. Shutdown cooperatively parks the audio producer before ending playback; it never deletes a task while it owns a speaker lock. GPIO holds are released before hardware initialization on boot. NVS/save or producer-park failure defers sleep for another two minutes; peripheral shutdown writes are retried and failures are logged, but deep sleep still proceeds.

### Twist to change pitch

Twist the Atom around the axis running straight through the USB plug, like turning a screwdriver. **Chain Ride** bends smoothly over +/-12 semitones. The **Dorian strings**, **FIVE HIRA**, and **SIX LYDIAN** select scale notes for the next shake. A quarter-turn in either direction reaches the low or high end of each pitch range. Egg, Cabasa, Cuica and Guiro do not use twist-to-pitch control. Cuica's pitch contour comes from the push/pull stroke and simulated rubbing pressure.

### Dorian Pluck and Dorian Halo

These two sounds use Karplus–Strong string synthesis. Twist around the same USB axis to choose from **D E F G A B C**, spanning **D3 to D5**. The neutral pose selects **D4**; a quarter-turn down or up reaches the ends of the two-octave range. The 15 note positions are evenly spaced across the rotation range, with a little hysteresis to keep the selection steady near a boundary.

Each detected shake peak plucks the selected note. **Harder shakes play louder and brighter.** The selected note is captured at that peak and shared by both outputs. Later rotation chooses the next note without bending an already ringing string. Sustained motion intensity alone does not retrigger the strings, though a quick rotation can itself produce a detectable acceleration peak.

Dorian Pluck is a focused string sound with a small room tail. Dorian Halo adds a string one octave above the selected note, longer string decay, and a larger reverb. Both allow four overlapping plucks; a fifth smoothly replaces the oldest. These are audio synth sounds, not MIDI output.

### FIVE HIRA: shake open the filter

This sound uses a fixed-patch C++ port of MDLR's **FIVE** engine: five independent voices, dual PolyBLEP oscillators per voice, separate amplitude/filter envelopes, and its nonlinear four-pole resonant low-pass filter. The patch blends saw and pulse on oscillator A with a slightly detuned saw on B. Each shake peak triggers a fresh, velocity-sensitive pluck with a fast attack, zero sustain, a 45 ms gate, and a short release; up to five notes overlap. Rotation chooses the next note without bending a ringing one.

The updated patch opens the filter far enough for the **first shake from rest** to speak clearly. A strong filter-envelope sweep gives the attack its bite, followed by a quick decay. Gentle shakes remain audible and harder ones become louder and brighter. The amplitude attack time constant is 0.7 ms (about 3.7 ms to the envelope peak), with a 2 ms crossfade for stolen voices; these are synthesis timings after the trigger, not measured end-to-end latency.

The scale is **E Hirajōshi: E, F♯, G, B, C**, with 11 evenly spaced positions from **E3 to E5** and **E4 at neutral**. Selection has boundary hysteresis, as with the Dorian strings.

**Keep shaking to progressively raise the cutoff.** Harder motion builds the filter opening faster. When you stop, it closes gradually (about a 1.43-second control decay constant) as the notes fade away. Cutoff is bounded at 8 kHz and also shaped by FIVE's filter envelope and key tracking. Changing presets resets the accumulated filter opening. A light room effect follows the synth.

The implementation retains FIVE's oscillator and filter equations at a fixed patch, with embedded-specific changes documented in `THIRD_PARTY.md`. It does not expose the MDLR rack or FIVE's complete parameter/modulation interface.

### SIX LYDIAN: chorus plucks

**SIX LYDIAN** is a fixed-patch C++ port of MDLR's SIX engine. It keeps SIX's six DCO voices, PolyBLEP saw/pulse/sub oscillators, partial high-pass stage, nonlinear four-pole low-pass filter, separate amplitude/filter envelopes, and mode-I dual-tap chorus averaged to mono.

Each shake triggers a crisp pluck: a pulse-forward saw blend, a quiet sub oscillator one octave below, a short filter-envelope/PWM sweep, and light chorus/reverb. Up to six notes overlap. Its amplitude attack time constant is 1 ms (about 5.3 ms to the envelope peak), with zero sustain, a 65 ms gate, and a 170 ms release time constant. A 3 ms crossfade softens voice stealing. Cutoff and PWM changes are interpolated, and the cutoff ceiling is 4 kHz for a rounder attack.

SIX uses **E Lydian: E, F♯, G♯, A♯, B, C♯, D♯**, with 15 evenly spaced rotation positions from E3 to E5 and E4 at neutral. The raised fourth is A♯. FIVE retains its E Hirajōshi scale. Rotation selects the next note and each shake captures it. Harder shakes increase loudness and the filter sweep. SIX's pluck brightness responds to each strike; FIVE retains the accumulating shake-to-cutoff interaction.

### Cuíca and güiro

**CUICA** alternates between a higher, sharper push squeak and a lower, rounder pull. Shake back and forth: the motion detector learns your playing axis from gravity-removed acceleration and latches a direction with each accepted shake peak. Opposite strokes select opposite sounds; repeated peaks in the same direction retain the same sound. The pitch contour, pulse shape, and body resonance change together, with smoothing across a reversal. Both strokes remain sensitive to shake strength.

Direction is a relative playing gesture inferred from signed acceleration, not a measurement of absolute travel or hand velocity. The first accepted stroke establishes the reference; a changed playing axis is learned without toggling on ambiguous sideways motion. Both USB and speaker receive the same latched direction, so missed events or starting another output do not independently advance a push/pull counter. There is no scale quantization, and continued motion sustains the friction texture. The icon remains still.

**GUIRO** makes a dry, ridged gourd scrape. Motion moves a virtual scraper across slightly uneven ridges; every ridge excites a brief rough contact and three fixed body resonances. Stronger motion increases both scrape speed and loudness. A short shake makes a compact rasp, while repeated motion keeps the scrape going. It decays quickly when you stop.

These are original modeled sounds, not recordings or sample playback. The cuíca behavior is informed by [friction-drum playing](https://www.si.edu/object/cuica%3Aacm_2003.0032.0388), and the güiro by a [ridged gourd and scraper](https://www.si.edu/object/musical-gourd-rasp-scratcher%3Anmnhanthropology_8423930). Acoustic realism and levels on the small Echo speaker still require listening on the hardware.

### Pixel-art display

Nine original 32 × 32 icons are displayed at an exact 4× scale on the 128 × 128 screen. All pixels are black, white, or red; there is no smoothing, animation, text, or level meter. The icon changes only when the selected sound changes. One persistent 8 KiB packed frame is composed off-screen and transferred as an opaque image, with no blank erase frame and no allocations during playing. Motion, changing notes, and volume holds do not cause redraws. Screen-off/wake retains the image and restores the correct selection before the backlight returns.

| Sound | Icon |
|---|---|
| Egg | White egg with a stepped red band. |
| Cabasa | White handled shaker with red beads. |
| Chain Ride | White cymbal with dangling red chain links. |
| Dorian Pluck | White angular lyre with red strings. |
| Dorian Halo | Matching lyre beneath a red halo. |
| FIVE HIRA | White synth with a red saw waveform. |
| SIX LYDIAN | White synth with a red pulse waveform. |
| Cuica | White friction drum with a red rubbing rod. |
| Guiro | White ridged gourd with a red diagonal scraper. |

An actual sensor/speaker failure replaces the instrument with a static red warning symbol. Boot errors use the same symbol; technical details go to the firmware log. A live sensor fault still inhibits automatic sleep. This diagnostic screen also uses only the three-color palette.

### Neutral pose and sensor tracking

Hold the Atom in your normal playing pose when you reset it: that pose becomes neutral. Twisting back returns toward the original pitch. Reset establishes a new neutral pose. Pitch depends on the current twist, with no accumulated full-turn count. From neutral, +90 degrees reaches the top, +180 returns to the middle, +270 reaches the bottom, and +360 returns to the middle; turning the other way reverses that sequence. This folded mapping avoids an abrupt wrap and prevents extra turns from pinning the pitch. The middle +/-90-degree region retains its original linear response. The gyroscope works even when the USB axis points vertically; gravity correction reduces drift in other orientations. With the axis vertical, some slow drift can remain. No magnetic heading is used, avoiding interference from the speaker base.

The firmware reads both BMI270 sensors at 200 Hz: accelerometer +/-8 g and gyro +/-2000 degrees/second. M5Stack's enclosure axes map from raw BMI270 axes as (Y, -X, Z); the USB direction is enclosure Y. A quaternion attitude filter combines gyro motion with gravity correction, downweighting acceleration during shaking. A swing/twist decomposition extracts USB-axis rotation. Stationary readings slowly correct gyro bias. Chain Ride uses continuous pitch changes. The two string presets, FIVE, and SIX quantize rotation to scale notes and latch the pitch per strike. Reverb delay lengths stay fixed.

“Velocity” is musical playing intensity derived from acceleration, not an absolute speed measurement. The firmware removes estimated gravity, applies a dead zone, and measures acceleration magnitude and peaks. It does not integrate acceleration into drifting speed estimates. A fast rotation can also produce sound.

## Sounds

| Number | Sound | Character |
|---|---|---|
| 1 | Egg | Fine, dry shaker noise. |
| 2 | Cabasa | Dense, short scraping grains. |
| 3 | Chain Ride | Twelve inharmonic cymbal resonances, metallic sizzle, and a room tail. |
| 4 | Dorian Pluck | Velocity-sensitive Karplus–Strong plucks in D Dorian. |
| 5 | Dorian Halo | Octave-paired strings and a spacious reverb in D Dorian. |
| 6 | FIVE HIRA | Bright five-voice MDLR plucks in E Hirajōshi, with a cutoff that builds as you shake. |
| 7 | SIX LYDIAN | Round pulse/saw/sub MDLR plucks in E Lydian, with light chorus and six voices. |
| 8 | Cuica | Direction-selected push/pull squeaks, with contrasting pitch contours and tone. |
| 9 | Guiro | Wooden ridge scrapes; stronger motion increases speed and loudness. |

Maraca, Ion Swish, Phase Wash, Laser Sand, Glass Dust, and Deep Space have been removed. Sleigh and Tambo remain removed.

All sounds are synthesized on the Atom. Chain Ride is a physical-model-inspired sound, not a recorded cymbal sample. Changing presets clears the previous sound's resonances and reverb. A volume hold keeps the same preset and briefly ducks the speaker's ringing tails under the confirmation beeps.

## Play through the Atomic Echo Base

Attach the base before powering on, or press reset after attaching it. The firmware probes the ES8311 codec at startup and starts speaker playback when it is present. Base detection is performed at boot; hot-plug detection is not implemented.

With the base detected, simply shake to play. The speaker works with USB power from a Mac, charger, or power bank; no recording app needs to be open. You can record the USB audio at the same time. Both outputs follow the selected preset and your shake intensity.

Speaker volume defaults to **30% (77 out of 255)**. Each 0.8-second front-button hold advances to **60% (153)**, then **100% (255)**, then back to **30% (77)**. These are volume-control settings, not calibrated acoustic loudness percentages. The beeps are 60 ms each with 70 ms gaps; low, medium, and high give one, two, and three beeps respectively. There is no startup beep. After an automatic sleep, power-up restores the most recently saved volume.

The physical volume control and beeps apply **only to the Echo Base speaker**; the USB audio feed retains its independent Mac input volume/mute controls and contains no confirmation beeps. Without the base, there is no volume confirmation; the setting still advances for the speaker. Mac input volume and mute affect **USB only**, leaving the speaker audible. To hear just the base while recording, leave your Mac track's input monitoring off. Without the base, use Mac input monitoring as described below.

The Echo Base microphone is not started. The library handles its ES8311 codec and amplifier; the AtomS3R wiring is I2C SDA **G38**, SCL **G39**, I2S BCLK **G8**, LRCK **G6**, and speaker data **G5**. M5Stack now documents A149 under the name **Atomic Voice Base**.

## Build and flash

Use ESP-IDF **5.4.x**, target `esp32s3`, a USB data cable, and an AtomS3R. This is a native ESP-IDF project. Install the [Espressif toolchain](https://docs.espressif.com/projects/esp-idf/en/release-v5.4/esp32s3/get-started/index.html) and activate its environment first. The component manager downloads the pinned dependencies in `dependencies.lock`.

```sh
git clone https://github.com/dustMason/m5shaker.git
cd m5shaker
idf.py set-target esp32s3
idf.py build
```

To flash, hold the AtomS3R's **side reset button** for about two seconds until the internal green LED lights, then release. Find its programming port and replace the example port below:

```sh
idf.py -p /dev/cu.usbmodemYOUR_PORT flash
```

Flashing replaces the device's firmware. Briefly tap reset afterward if it stays in download mode. The programming serial port is replaced by the USB audio device during normal operation, so return to download mode before flashing again. Linux and Windows use their platform's serial-port names.

Generated firmware stays in the ignored `build/` directory. The repository does not contain prebuilt images, archives, downloaded components, credentials, or local device records. The USB descriptor serial `SHAKER001` in `sdkconfig.defaults` is a generic example, not a hardware serial or credential; use a different value for each unit if using several together.

## Hear and record it on macOS

This is an **audio input to the Mac**: the synthesized sound travels from the instrument into the computer. macOS does not automatically play an audio input through its speakers.

1. Open **Audio MIDI Setup** and check for **AtomS3R Shaker**, with **1 input channel, 48,000 Hz, 16-bit**.
2. In GarageBand or Logic's audio settings, choose **AtomS3R Shaker** as the **input device** and your headphones, speakers, or audio interface as the **output device**.
3. Create a **mono audio track**, choose **Input 1**, and enable **input monitoring**. Use an audio track; the firmware sends actual sound rather than MIDI notes.
4. Allow microphone/audio-input access if macOS requests it, even though these are synthesized sounds.
5. Shake to play and record the track. In software with a buffer-size control, 64 or 128 samples is a useful starting point. Wired output avoids Bluetooth monitoring delay.

The screen keeps the instrument icon while USB or speaker audio plays. Check the Mac's input settings for USB activity and mute state. A static red warning symbol indicates a sensor or speaker fault; USB remains available after a speaker failure. If sensor reads stop, excitation is gated off after 50 ms so stale movement does not sustain indefinitely.

## Firmware configuration

Important configuration is in `sdkconfig.defaults`: 48 kHz, mono, 16-bit, microphone-only USB, macOS compatibility enabled, 4 ms audio blocks, and a 1 kHz RTOS tick. USB runs on core 0; motion/UI and speaker rendering/playback run on core 1. The code uses internal RAM; PSRAM is not required. M5GFX 0.2.32 detects both the older GC9107 and newer ST7735 screen revisions.

## Adjust the instrument

| File | What to change |
|---|---|
| `main/shaker.hpp` | `kSensitivity`: increase above 1.0 for a lighter playing touch. `kButtonHoldUs`: hold duration, initially 800,000 microseconds. |
| `main/shaker.cpp` | `presets[]`: collision/resonance settings and levels. `excite()` tunes the strings; `dorian[]` / `hirajoshi[]` define the two scale maps. |
| `main/shaker.cpp` | `Motion::update()`: 0.12 g dead zone, smoothing, and peak detection. `speakerVolume()` / `speakerVolumePercent()`: the three local volume levels. `VolumeCue`: confirmation beeps. |
| `main/main.cpp` | Button, screen, motion polling, Echo Base detection/playback, and host audio controls. |
| `main/instrument_icons.hpp` | The nine 32 × 32 icons, diagnostic symbol, and packed frame composer. |
| `main/power_control.cpp` | NVS resume snapshot and hardware sleep/wake register handling. |
| `main/five.cpp` / `main/six.cpp` | Synth patches, pluck envelopes, voice allocation, filter limits, and SIX chorus. |
| `main/analog_dsp.*` | Shared MDLR oscillator/envelope helpers and tanh approximation. |
| `sdkconfig.defaults` | USB product/serial names and transport settings. Give each unit a different serial if using several. |

After editing `sdkconfig.defaults` in an already-configured checkout, update the corresponding values in `idf.py menuconfig` or regenerate `sdkconfig`; defaults do not override existing settings. Keep the audio format at 48 kHz/mono/16-bit unless you also adapt the synth and callback; compile-time checks enforce that agreement.

The synth combines random micro-collisions with three resonances for most presets and twelve for Chain Ride, velocity-dependent noise filtering, natural decay, DC blocking, and soft limiting. A four-delay feedback network supplies damped reverb. The string sounds use noise plus a shaped pluck displacement, a feedback delay with a two-point damping filter, and a first-order allpass fractional delay for accurate tuning. Each output has four pluck voices with two 512-sample string buffers each. Buffers are initialized only when a note is triggered, with no allocation. USB rendering allocates no heap memory and does no sensor or screen I/O. Both motion sensors are polled at 200 Hz; their latched data-ready flags guard fresh reads. Chain Ride pitch is smoothed and refreshed every 64 samples. FIVE runs at 48 kHz with control updates every 32 samples. SIX runs the full six-voice oscillator/envelope/filter/chorus core at 24 kHz, with 16-sample control updates and linear interpolation of cutoff/PWM controls. A persistent half-sample reconstruction state produces 48 kHz output across arbitrary block sizes. This halves SIX’s audio-rate core work while keeping all six voices; coefficient updates retain their previous cadence. Its chorus delay time and modulation rates are preserved. A final 7.6 Hz DC blocker follows the nonlinear output limiter for the two analog synth patches. Preset changes reset the resonances/reverb and start a short onset ramp.

Each output owns its synth and reverb state in persistent storage, so USB and I2S can run on independent clocks without racing filters or random generators. The two reverbs use 64 KiB total and the string delay buffers add 32 KiB total; they are not placed on task stacks. SIX adds one 512-sample mono chorus buffer per output (4 KiB total); two read taps preserve its chorus movement. Both outputs follow the same preset and motion controls; their random micro-collisions are not sample-synchronized. Speaker playback uses two persistent PCM buffers and M5Unified's release callback: a buffer is refilled only after playback has finished reading it. I2S consumption paces the speaker producer. The release callback sends a FreeRTOS task notification, waking the producer immediately; it rechecks the atomic buffer-ownership flag before refilling. This removes the former up-to-1-ms polling wait from each refill. The callback runs in M5Unified’s playback task, not an ISR. There is no speaker I/O or blocking speaker queue operation in the USB callback. Button events are classified in the motion/UI task and sent through the existing locked state snapshot. The speaker task applies volume changes and mixes the short confirmation sequence into its normal PCM buffers, with smoothed tone edges and tail ducking. No blocking tone calls, extra playback channels, or delays are used; motion sensing and USB rendering continue throughout.

## Verification and sound preview

Run the portable tests with the Mac's C++ compiler:

```sh
sh tests/run.sh
```

Button tests cover short-press release, the exact 0.8-second boundary, holds lasting more than 15 seconds without repeats, release after a hold, and volume wraparound. Audio tests count the one/two/three beep sequences, check identical timing across buffer sizes, bounded mixing, and complete restoration of the instrument level.

The tests use AddressSanitizer and UndefinedBehaviorSanitizer, and check silence in different orientations, sensor noise rejection, peak cadence, soft/hard dynamics for all presets, bounded output, DC, decay, mute, and reset. They also check that speaker rendering continues without USB, USB start/stop/mute does not alter speaker state, and old sensor data cannot sustain the sound. Reverb tests check clearing on preset changes, identical rendering across different block sizes, and audible ringing tails. To generate a 54-second WAV using the same synth and simulated shakes:

```sh
sh tests/run.sh /tmp/shaker-preview.wav
```

The preview plays all 9 presets in button order, six seconds each: 1.2 seconds of gentle shaking with a decay gap, then 1.2 seconds of strong shaking with a longer decay gap. Tails are rendered naturally. It is a software render, not a recording from the hardware.

Cuica/Guiro tests verify preserved preset IDs, fast cold attacks, short tails, arbitrary 127/113-frame splits, a voiced Cuica pitch scoop, and increasing audible ridge density for stronger Guiro motion. Both are also covered by the full preset dynamics, rotation independence, mute/reset, sensor-staleness, and power-idle tests. In the standard repeated-shake gesture, hard/soft RMS ratios are 3.5× for Cuica and 6.0× for Guiro.

Cuica direction tests cover four shake axes at 2/5/7 Hz and two strengths, repeated kicks in the same direction, a changed playing axis, missed strike IDs, shared speaker/USB direction, separate push/pull dynamics, and rapid alternating strokes across odd audio blocks. In the isolated 0.9-strength test, the early voiced pitch measures about 366 Hz on push and 203 Hz on pull. These are software checks; natural hand motions and hardware audio still need an audition.

Power tests exercise exact 15-second and 120-second boundaries, activity cancellation, USB host protection even without recording, a fresh timeout after host removal, sensor-fault handling, restored button behavior, complete audible tails for all nine presets, and the first new strike after silent DSP suspension. These are host software tests, not measurements of board sleep current.

Additional tests exercise USB-axis rotation in both directions, return to neutral, rejection of the other two pure rotation axes, vertical-axis tracking, invalid gyro data, rapid Chain Ride pitch sweeps, and identical traditional-sound output regardless of pitch input. Rotation-recovery regressions include one and three complete turns in both directions at 180 and 1800 degrees/second, smooth wrap transitions, neutral scale-note recovery, subsequent +/-45-degree control, strong linear acceleration, and multi-axis tumbling through the swing/twist singularity. The full-turn recovery test fails on the preceding firmware and passes with the fix. The main preview uses neutral pitch (D4 for the strings, E4 for FIVE/SIX). Additional tests measure actual rendered string tuning using autocorrelation, check every scale note, boundary hysteresis, silence without a strike, unchanged ringing notes during rotation, note capture at shake peaks, single-trigger delivery, and rapid retrigger stability. Soft/hard repeated shakes measured a 4.3x/4.4x RMS difference for Pluck/Halo in the test gesture.

Supply a second WAV path for an 18-second melodic preview: Dorian Pluck followed by Dorian Halo, each ascending D3..D5 with increasing velocity and a decay gap.

```sh
sh tests/run.sh /tmp/shaker-preview.wav /tmp/dorian-preview.wav
```

An optional third WAV path renders a 12-second FIVE preview with scale-note changes and increasing shake activity, followed by a decay gap:

```sh
sh tests/run.sh /tmp/shaker-preview.wav /tmp/dorian-preview.wav /tmp/five-preview.wav
```

FIVE tests cover all 11 E Hirajōshi notes, boundary stability, progressively increasing and releasing cutoff, silence without note triggers, unchanged note pitch during rotation, rapid five-voice retriggers, and a normalized audio-brightness comparison at the same note/velocity with low and high filter opening. The measured FIVE fundamental stays within 2.36 cents in the tested patch, which includes deliberate oscillator-B fine detuning.

An optional fourth WAV path adds a 12-second SIX preview:

```sh
sh tests/run.sh /tmp/shaker-preview.wav /tmp/dorian-preview.wav /tmp/five-preview.wav /tmp/six-preview.wav
```

Additional cold-pluck tests cover every FIVE Hirajōshi and SIX Lydian note at two velocities after silence. Both patches must have substantial energy within the first 10 ms, a strongest 5 ms window beginning within 30 ms, and the 200–300 ms tail below 40% of the initial 25 ms RMS. Chorus can reinforce a later peak after the direct attack has already sounded. Tests also cover captured note delivery, no retrigger from rotation alone, six-voice retrigger stability, decay to silence, SIX tuning including its sub oscillator and chorus, continuity across odd-sized buffers, and absence of half-millisecond zero runs during dense six-voice playing. These software checks do not prove hardware underruns are resolved.

All previews are software renders, not hardware recordings. Host audio tests do not measure ESP32 processor load.

After an ESP-IDF build, this checks the descriptors embedded in the compiled firmware:

```sh
python tests/check_usb_descriptor.py build/atoms3r_shaker.elf
```

Remaining hardware checks: audition Cuica push/pull with real back-and-forth motion and Guiro through speaker and USB, compare levels/character, check rapid retriggers and the two new icons; verify 15-second backlight-off and two-minute deep sleep on a charger/battery; confirm no motion/front-button wake, side-reset recovery of audio/display/IMU and settings, repeated sleep/wake cycles, USB suspend/unplug protection and whole-stack current with the A151; confirm that rough shaking/full turns do not leave note selection stuck, confirm icon colors/selection, flicker-free changes, and correct display wake, confirm SIX plays cleanly through USB, confirm cutoff progression on FIVE, note selection, and clean simultaneous speaker/USB output; boot without the Echo Base; confirm clean local playback using power-bank power; check independent host mute; verify each button press selects once; compare light/hard shakes in each orientation; verify longer recording and repeated stop/start behavior; and measure end-to-end latency. The configured 4 ms blocks are not an end-to-end latency claim.

## Dependencies and references

- [E Hirajōshi notes and musical examples](https://www.guitarworld.com/lessons/investigating-hirajoshi-scale)
- [Julius O. Smith: first-order allpass interpolation for physical audio synthesis](https://www.dsprelated.com/freebooks/pasp/First_Order_Allpass_Interpolation.html)
- [M5Stack AtomS3R specifications, pinout, screen revisions, and download procedure](https://docs.m5stack.com/en/core/AtomS3R)
- [M5Unified IMU axis remapping](https://github.com/m5stack/M5Unified/blob/master/src/utility/IMU_Class.inl)
- [Bosch BMI270 register definitions](https://github.com/boschsensortec/BMI270_SensorAPI/blob/master/bmi2_defs.h)
- [Atomic Echo Base A149 / Atomic Voice Base specifications](https://docs.m5stack.com/en/atom/Atomic_Voice_Base)
- [M5Stack's Atomic Echo Base library and hardware reference](https://github.com/m5stack/M5Atomic-EchoBase)
- [Bosch BMI270 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf)
- [Espressif USB audio component documentation](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_device/usb_device_uac.html)
- [Apple GarageBand user guide](https://support.apple.com/guide/garageband/welcome/mac)
- [Espressif flashing documentation](https://docs.espressif.com/projects/esptool/en/release-v4/esp32s3/esptool/basic-commands.html)

See `THIRD_PARTY.md` for exact dependencies and the small USB control fix. The supplied USB VID/PID are Espressif's development defaults; obtain an appropriate VID/PID before distributing a commercial device.

## License

Original application code is covered by the MIT terms in [LICENSE](LICENSE), with explicit exceptions for MDLR-derived files and third-party components. The MDLR source engine is marked `UNLICENSED`; this repository does not grant a new open-source license to those ports. See [THIRD_PARTY.md](THIRD_PARTY.md) for file scope, provenance and dependency notices.
