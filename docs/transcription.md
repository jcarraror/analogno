# Transcription and Stem Arrangement

Analogno can turn audio in a sampler bank into sequencer steps. This is used by
the `Transcribe` and `Arrange` buttons in the UI, after a song has
been split into stems.


## User Workflow

The normal stem workflow is:

1. Upload or download a full song.
2. demucs splits it into stems.
3. Pick a stem and load it into a sampler bank.
4. Press `Transcribe` or `Arrange`.

`Transcribe` writes the bank's generated pattern starting at the currently active
sequencer track.

`Arrange` treats the bank as part of a multi-stem arrangement. It gives each bank
its own block of tracks. If needed, it expands the sequencer so all generated
tracks fit, up to the sequencer track limit.

## Core Concepts

### Onset

An onset is the moment a new sound event starts.

Examples:

- The start of a kick drum hit.
- The pluck of a bass note.
- The start of a sung syllable.
- A piano hammer strike.
- A guitar pick attack.

onset, then, can be seen as a point in time where something new happens in the audio.
the definition of "something new" is flexible. It can be a sharp transient, or it can be a softer change in the signal that indicates a new note or event.

### Transient

A transient is the short burst of energy at the beginning of a sounds. It is
often sharp, loud, and brief. Drums have obvious transients. Plucked strings have
clear transients. Smooth vocals and pads can have softer, less reliable
transients.

Onset detection works by looking for transient changes in the signal.

### Pitch

Pitch is the perceived musical height of a sound, such as A4, C3, or MIDI note
60. Analogno stores pitch as a MIDI note number.

Pitch estimation is easy for stable monophonic sounds, such as a clean bass
note. It is harder for drums, noisy vocals, chords, distortion, reverb, and
mixed material.

### Velocity

Velocity is the MIDI-style strength of a note, from `1` to `127`.

Here, velocity is estimated from the peak loudness shortly after an
onset. Louder attacks become higher velocities.

## The Analysis Pipeline

The core analyzer is `transcribe_to_seq` in `src/stem_transcriber.cpp`.

It takes:

- Raw bank samples.
- Channel count.
- Bank trim start/end.
- Current sequencer BPM.
- Current sequencer step division.
- Current sequencer step count.

It returns a `TranscriptionResult` containing:

- Generated tracks.
- Detected onset frame positions.
- A suggested root note.
- A suggested loop length.

### 1. Trim and Mono Mix

The analyzer only looks at the trimmed region of the bank.

If the audio is stereo or multi-channel, each analysis frame is converted to mono
by averaging channels. This keeps the onset and pitch detectors working on one
signal.

### 2. Onset Detection

Analogno uses aubio's onset detector with the `complex` method.

At a high level, this scans the audio in small windows and looks for changes
that resemble new sound events. The code uses:

- Sample rate: `48000`
- Hop size: `256` frames
- Onset buffer: `512` frames
- Silence threshold: `-60 dB`
- Detection threshold: `0.2`
- Minimum onset spacing: about `40 ms`

The minimum spacing prevents a single hit from producing several nearby onsets.

For every accepted onset, also measures the local peak amplitude near
the onset. That peak is later converted into velocity.

### 3. Peak-Grid Fallback

Sometimes aubio finds too few onsets. This can happen with soft material,
compressed audio, or stems without strong transients.

If the number of detected onsets is too low, Analogno falls back to a simpler
grid scan:

- Divide the trimmed audio into `step_count` slices.
- Find the loudest point in each slice.
- Keep slices whose peak is high enough relative to the whole recording.


### 4. Pitch Estimation with YIN

After onsets are found, Analogno estimates pitch once per onset using aubio's
`yin` pitch detector.

YIN is a pitch detection algorithm designed to estimate the fundamental
frequency of a sound. The fundamental frequency is the main repeating frequency
we perceive as pitch. For example, A4 is `440 Hz`.

YIN works like this:

1. Take a short window of audio.
2. Compare that window against delayed copies of itself.
3. Look for the delay where the waveform best matches itself again.
4. Convert that delay into a frequency.
5. Report a confidence value for how pitch like the result is.

If a waveform repeats every `N` samples, the estimated frequency is roughly:

```text
sample_rate / N
```

For example, at `48000 Hz`, if the waveform repeats about every `109` samples,
the frequency is about:

```text
48000 / 109 ~= 440 Hz
```

That is close to A4.

YIN is useful because it is more robust than simply picking the loudest frequency
bin. It tries to find the period of the waveform, which is often closer to how
humans hear pitch.

Analogno uses YIN with:

- Pitch buffer: `2048` frames.
- Silence threshold: `-55 dB`.
- Tolerance: `0.15`.
- Confidence check: only accept the pitch if confidence is above `0.5`.
- Frequency range check: only accept values between `20 Hz` and `8000 Hz`.

Accepted frequencies are converted to MIDI notes:

```text
midi = round(69 + 12 * log2(hz / 440))
```

If pitch detection fails, the fallback MIDI note is `48`.

### Why Estimate Pitch?

### 5. Velocity Estimation

Velocity is calculated from the peak amplitude near each onset:

```text
velocity = clamp(peak * 127, 1, 127)
```

This means:

- Quiet attacks produce low velocities.
- Loud attacks produce high velocities.
- Velocity is always at least `1`, so a detected event can still trigger.
- Velocity is capped at `127`, matching MIDI convention.


### 6. Snap Events to Sequencer Steps

The analyzer converts onset times to sequencer steps using the current BPM and
step division.

Step duration is:

```text
step_duration_seconds = 60 / bpm * 4 / step_division
```

At `120 BPM` with a `16` step division:

```text
60 / 120 * 4 / 16 = 0.125 seconds
```

So each sequencer step is `125 ms`.

The first detected onset is normalized to step `0`. This means leading silence
in the stem does not shift the whole pattern later in the sequence.

Each later onset is rounded to the nearest step and wrapped into the sequencer's
step count.

### 7. Build Generated Tracks

After snapping, detections are grouped by step.

If several detections land on the same step, treats that as polyphony or
overlap. It sorts those events by velocity and spreads them across generated
tracks.

Rules:

- Maximum generated polyphony is `4` tracks.
- Louder events get priority when too many events land on the same step.
- Each generated track is sorted by step.
- Generated tracks with fewer than `2` notes are discarded.

## Known Limitations

### Long Stems May Not Transcribe

`AudioSampler::bank_snapshot` returns no samples for streamed/file-backed banks.
`load_stem_to_bank` streams stems longer than about `30` seconds instead of
loading them fully into memory.

That means long stems can be playable but unavailable to the current
transcription path.
