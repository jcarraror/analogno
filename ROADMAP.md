# Analogno Roadmap

> A DualSense MIDI controller — translates DS5 motion, touch, and triggers into
> expressive MIDI, with a built-in step sequencer for live looping.
>
> Stack: DS5 → Analogno (SDL3/C++) → ALSA MIDI → FluidSynth + SF2 → audio out  
> Config surface: browser at `localhost:8765`

---

## Done

- Gyro / accelerometer → pitch bend, filter cutoff, modulation
- Mic input → velocity and gate (dynamics from breath/noise)
- Touchpad drawing → wavetable synthesis
- Step sequencer with live arm-recording from controller
- Touchpad swipe navigation (left/right = steps, up/down = tracks while staying armed)
- Touchpad click = delete armed step; right-click in UI = delete step
- Start button = seq play/stop
- SF2 soundfont parser — real preset browser with bank tabs (incl. Drums/bank 128)
- Multi-SF2 selector — scans `/usr/share/sounds/sf2` and shows dropdown
- GM percussion routing (bank 128 → MIDI ch 9 automatically)
- LED lightbar microflash queue — one colour per note-on, keyed to program family
- Config modal (Controller, Audio input, Motion, Music panels)
- Sampler: record from mic, trim, multi-bank, wavetable mode

---

## Near term


### 2. Adaptive trigger resistance
SDL3 exposes `SDL_SetGamepadSensorEnabled` and trigger effect APIs.
Map L2 stiffness to filter closedness, or give tactile zones at musical intervals.
Makes the controller feel like an instrument rather than a gamepad.

### 3. Pattern switching (A/B/C)
Store multiple 16-step pattern snapshots per track. Switch patterns while the
sequencer runs (queued at the next bar boundary). Essential for live improv.

### 4. Sensor remapping UI
Right now gyro X → pitch bend is hardcoded in `music_mapper.cpp`. Add a
per-axis assignment table in the web Config modal so players can tune the layout
to their playing style without recompiling.

### 5. Loop record mode
Hold L1 + Start → enter loop-record: every note played is written into the
current track in real time, quantised to the nearest step. Complement to the
current arm-and-press workflow.

---

## Medium term

### 6. Song/arrangement mode
Chain patterns in order (e.g. A A B A) and let the sequencer walk through them.
A simple list in the web UI is enough — drag to reorder, click to insert/delete.

### 7. Right-stick second mode
R-stick currently drives filter cutoff (Y) and resonance (X). When the sequencer
is playing, optionally remap it to arpeggio speed (Y) and octave transpose (X),
switchable with a hold combo.

### 8. Per-note velocity from trigger timing
Model note velocity from how fast L2 / face buttons are pressed rather than
(or in addition to) mic envelope. Gives finger-dynamic expression without a mic.

### 9. BLE / USB-C wireless on laptop
Verify DS5 Bluetooth pairing works reliably on the target laptop. Document the
`bluetoothctl` pairing steps in the setup guide.

---

## Polish / portability

- Single-command launch: `./scripts/dev.sh` already does most of this; add a
  friendly error if `fluidsynth` or `fluid-soundfont-gm` is not installed
- Systemd user service option for FluidSynth so it survives session restarts
- README with setup steps, dependency list (`apt install fluidsynth fluid-soundfont-gm`)
  and a diagram of the signal chain
- License file
