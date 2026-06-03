export type Vec3 = {
  x: number;
  y: number;
  z: number;
};

export type SoundfontPreset = {
  bank: number;
  program: number;
  name: string;
};

export type RuntimeState = {
  type: "runtime";
  controller: {
    leftX: number;
    leftY: number;
    rightX: number;
    rightY: number;
    l2: number;
    r2: number;
    hasGyro: boolean;
    hasAccel: boolean;
    gyro: Vec3;
    accel: Vec3;
  };
  music: {
    rootMidiNote: number;
    octaveOffset: number;
    scale: string;
    pitchBend: number;
    expression: number;
    filterCutoff: number;
    filterResonance: number;
    modulation: number;
    vibrato: number;
    activeNotes: number[];
    midiProgram: number;
    midiBank: number;
    buttonMidiNotes: number[];
  };
  audio: {
    devices: Array<{
      index: number;
      name: string;
      isDefault: boolean;
    }>;
    selectedDeviceIndex: number | null;
    captureRunning: boolean;
    sampleRecording: boolean;
    captureDevice: string;
    micLevel: number;
    envelope: number;
    gateOpen: boolean;
    onset: boolean;
    velocity: number;
    waveform: number[];
    sampleReady: boolean;
    sampleFrames: number;
    sampleTrimStart: number;
    sampleTrimEnd: number;
    sampleWaveform: number[];
    wavetableCycle: number[];
    banks: Array<{
      hasData: boolean;
      frames: number;
      trimStart: number;
      trimEnd: number;
      isWavetable: boolean;
      isStream: boolean;
      waveformVersion: number;
      waveform: number[];
      rootNote: number;
      sliceCount: number;
      transcribeCached: boolean;
      seqStored: boolean;
      generatedTrackCount: number;
    }>;
    activeBank: number;
    touchpadSketch: number[];
    touchpadDrawing: boolean;
    touchpadRawPoints: [number, number][];
    wavetableMorph: number;
    wavetableNoise: number;
    wavetableUnison: number;
    voiceSeqAvailable: boolean;
    voiceSeqCompiled: boolean;
    voiceSeqEnabled: boolean;
    voiceSeqRecording: boolean;
    voiceSeqMode: "percussion" | "harmonic" | "hybrid";
    voiceSeqSnap: boolean;
    voiceSeqSensitivity: number;
    voiceSeqTimingOffsetMs: number;
    voiceSeqLastNote: number;
    voiceSeqLastVelocity: number;
    voiceSeqAcceptedNotes: number;
    voiceSeqRejectedNotes: number;
    voiceSeqRecordedSegments: number;
    voiceSeqRecordProgress: number;
    specSamples: number[];
    signalsVolume: number;
    micHasSample: boolean;
    transcribeState: "idle" | "running";
    stemSplitState: "idle" | "running" | "done" | "error";
    stemSplitError: string;
    stemSplitProgress: number;
    stemSplitDetail: string;
    stemSplitLog: string[];
    stems: Array<{ name: string; frames: number; isActive: boolean; waveformVersion: number; waveform: number[] }>;
    stemsFolder: string;
  };
  seq: {
    playing: boolean;
    activeTrack: number;
    selectedStep: number;
    bpm: number;
    playheadStep: number;
    currentStep: number;
    gatePct: number;
    stepCount: number;
    stepDivision: number;
    tracks: Array<{
      midiChannel: number;
      midiProgram: number;
      midiBank: number;
      sampleBank: number;
      loopLength: number;
      volume: number;
      pan: number;
      velocityScale: number;
      muted: boolean;
      solo: boolean;
      steps: Array<{
        active: boolean;
        tie: boolean;
        degree: number;
        velocity: number;
        midiNote: number;
      }>;
    }>;
  };
  pianoRollVisible: boolean;
  spectrogramVisible: boolean;
};

export type LibraryState = {
  type: "library";
  presets: SoundfontPreset[];
  soundfonts: string[];
  activeSoundfont: string;
};

export type ConnectionState = "connecting" | "online" | "offline";
