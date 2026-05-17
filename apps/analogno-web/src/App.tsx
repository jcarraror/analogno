import { type CSSProperties, type KeyboardEvent as ReactKeyboardEvent, type PointerEvent as ReactPointerEvent, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";

// General MIDI program names (0-indexed)
const GM_PROGRAMS: string[] = [
  // Piano
  "Acoustic Grand Piano","Bright Acoustic Piano","Electric Grand Piano","Honky-tonk Piano",
  "Electric Piano 1","Electric Piano 2","Harpsichord","Clavinet",
  // Chromatic Perc
  "Celesta","Glockenspiel","Music Box","Vibraphone","Marimba","Xylophone","Tubular Bells","Dulcimer",
  // Organ
  "Drawbar Organ","Percussive Organ","Rock Organ","Church Organ","Reed Organ","Accordion","Harmonica","Tango Accordion",
  // Guitar
  "Nylon Guitar","Steel Guitar","Jazz Guitar","Clean Guitar","Muted Guitar","Overdriven Guitar","Distortion Guitar","Guitar Harmonics",
  // Bass
  "Acoustic Bass","Finger Bass","Pick Bass","Fretless Bass","Slap Bass 1","Slap Bass 2","Synth Bass 1","Synth Bass 2",
  // Strings
  "Violin","Viola","Cello","Contrabass","Tremolo Strings","Pizzicato Strings","Orchestral Harp","Timpani",
  // Ensemble
  "String Ensemble 1","String Ensemble 2","Synth Strings 1","Synth Strings 2","Choir Aahs","Voice Oohs","Synth Voice","Orchestra Hit",
  // Brass
  "Trumpet","Trombone","Tuba","Muted Trumpet","French Horn","Brass Section","Synth Brass 1","Synth Brass 2",
  // Reed
  "Soprano Sax","Alto Sax","Tenor Sax","Baritone Sax","Oboe","English Horn","Bassoon","Clarinet",
  // Pipe
  "Piccolo","Flute","Recorder","Pan Flute","Blown Bottle","Shakuhachi","Whistle","Ocarina",
  // Synth Lead
  "Lead 1 (square)","Lead 2 (sawtooth)","Lead 3 (calliope)","Lead 4 (chiff)",
  "Lead 5 (charang)","Lead 6 (voice)","Lead 7 (fifths)","Lead 8 (bass+lead)",
  // Synth Pad
  "Pad 1 (new age)","Pad 2 (warm)","Pad 3 (polysynth)","Pad 4 (choir)",
  "Pad 5 (bowed)","Pad 6 (metallic)","Pad 7 (halo)","Pad 8 (sweep)",
  // Synth Effects
  "FX 1 (rain)","FX 2 (soundtrack)","FX 3 (crystal)","FX 4 (atmosphere)",
  "FX 5 (brightness)","FX 6 (goblins)","FX 7 (echoes)","FX 8 (sci-fi)",
  // Ethnic
  "Sitar","Banjo","Shamisen","Koto","Kalimba","Bagpipe","Fiddle","Shanai",
  // Percussive
  "Tinkle Bell","Agogo","Steel Drums","Woodblock","Taiko Drum","Melodic Tom","Synth Drum","Reverse Cymbal",
  // Sound effects
  "Guitar Fret Noise","Breath Noise","Seashore","Bird Tweet","Telephone Ring","Helicopter","Applause","Gunshot",
];

type Vec3 = {
  x: number;
  y: number;
  z: number;
};

type SoundfontPreset = {
  bank: number;
  program: number;
  name: string;
};

type RuntimeState = {
  type: "state";
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
    banks: Array<{
      hasData: boolean;
      frames: number;
      trimStart: number;
      trimEnd: number;
      isWavetable: boolean;
    }>;
    activeBank: number;
    touchpadSketch: number[];
    touchpadDrawing: boolean;
    touchpadRawPoints: [number, number][];
    blowMode: boolean;
    wavetableMorph: number;
    wavetableNoise: number;
    wavetableUnison: number;
    blowSensitivity: number;
    blowActive: boolean;
    blowLevel: number;  // 0..2, relative to threshold (1.0 = at gate)
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
    specSamples: number[]; // 2048 raw audio samples for frontend FFT
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
      loopLength: number;
      muted: boolean;
      steps: Array<{ active: boolean; tie: boolean; degree: number; velocity: number; midiNote: number }>;
    }>;
  };
  presets: SoundfontPreset[];
  soundfonts: string[];
  activeSoundfont: string;
  pianoRollVisible: boolean;
  spectrogramVisible: boolean;
};

type ConnectionState = "connecting" | "online" | "offline";

const websocketUrl = "ws://127.0.0.1:8765";

function formatNumber(value: number): string {
  return value.toFixed(3);
}

function midiNoteName(note: number): string {
  const names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
  const octave = Math.floor(note / 12) - 1;
  return `${names[note % 12]}${octave}`;
}

function soundfontName(path: string): string {
  const file = path.split(/[\\/]/).pop() ?? path;
  return file.replace(/\.[^.]+$/i, '') || path;
}

function soundfontFormat(path: string): string {
  const file = path.split(/[\\/]/).pop() ?? path;
  const match = file.match(/\.([^.]+)$/);
  return match?.[1]?.toUpperCase() ?? 'Soundfont';
}

function pluralize(count: number, singular: string): string {
  return `${count} ${singular}${count === 1 ? '' : 's'}`;
}

function bankLabel(bank: number): string {
  return `Bank ${bank}`;
}

function bankRole(bank: number): string {
  return bank === 128 ? 'percussion' : 'melodic';
}

function patchName(
  bank: number,
  program: number,
  presets: SoundfontPreset[],
): string {
  return presets.find(p => p.bank === bank && p.program === program)?.name
    ?? GM_PROGRAMS[program]
    ?? `Program ${program + 1}`;
}

const SCALE_SEMITONES: Record<string, number[]> = {
  minor_pentatonic: [0, 3, 5, 7, 10],
  major:            [0, 2, 4, 5, 7, 9, 11],
  natural_minor:    [0, 2, 3, 5, 7, 8, 10],
  dorian:           [0, 2, 3, 5, 7, 9, 10],
  phrygian:         [0, 1, 3, 5, 7, 8, 10],
  chromatic:        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
};

function stepNoteName(degree: number, rootMidi: number, scaleName: string): string {
  const semitones = SCALE_SEMITONES[scaleName] ?? SCALE_SEMITONES['major'];
  const idx = ((degree % semitones.length) + semitones.length) % semitones.length;
  const octaveBonus = Math.floor(degree / semitones.length);
  const midi = rootMidi + semitones[idx] + octaveBonus * 12;
  return midiNoteName(Math.max(0, Math.min(127, midi)));
}

function StatusPill({ state }: { state: ConnectionState }) {
  return <div className={`pill pill-${state}`}>{state}</div>;
}

const BTN_LABELS = ["✕","○","□","△","↑","↓","←","→"];
const BLACK_KEYS = new Set([1,3,6,8,10]);

function PianoRoll({
  activeNotes,
  buttonMidiNotes,
  rootMidiNote,
}: {
  activeNotes: number[];
  buttonMidiNotes: number[];
  rootMidiNote: number;
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const historyRef = useRef<Array<{midi:number; start:number; end?:number}>>([]);
  const prevRef = useRef(new Set<number>());
  const animRef = useRef(0);
  // Always-current props readable from the animation loop without re-subscribing
  const propsRef = useRef({rootMidiNote, buttonMidiNotes});
  propsRef.current = {rootMidiNote, buttonMidiNotes};

  // Track note-on / note-off by diffing activeNotes
  useEffect(() => {
    const now = performance.now();
    const newSet = new Set(activeNotes);
    for (const n of newSet) {
      if (!prevRef.current.has(n))
        historyRef.current.push({midi: n, start: now});
    }
    for (const n of prevRef.current) {
      if (!newSet.has(n)) {
        for (let i = historyRef.current.length - 1; i >= 0; i--) {
          const r = historyRef.current[i];
          if (r.midi === n && r.end === undefined) { r.end = now; break; }
        }
      }
    }
    prevRef.current = newSet;
  }, [activeNotes]);

  // Single animation loop, reads refs so it never needs to restart
  useEffect(() => {
    const canvas = canvasRef.current!;
    const WINDOW_MS = 5000;
    const FADE_MS   = 500;
    const PIANO_W   = 38;
    const N         = 25; // semitones shown (2 octaves)
    const MIDI_MAX  = 127;

    // Animated view position (fractional semitones from bottom)
    const initMin = propsRef.current.buttonMidiNotes.length
      ? Math.min(...propsRef.current.buttonMidiNotes) : 36;
    let viewStart = Math.max(0, initMin - 4);

    const draw = () => {
      const ctx = canvas.getContext("2d");
      if (!ctx) { animRef.current = requestAnimationFrame(draw); return; }

      const W = canvas.width;
      const H = canvas.height;
      const now = performance.now();
      const {rootMidiNote, buttonMidiNotes} = propsRef.current;
      const activeSet = prevRef.current;

      // Purge notes well outside the visible window
      historyRef.current = historyRef.current.filter(
        n => n.end === undefined || now - n.end < WINDOW_MS + FADE_MS
      );

      // Smooth pan: anchor view so lowest button note has ~4 semitones of margin below it.
      // This ensures all 8 mapped button notes stay visible regardless of scale/octave.
      const minBtn = buttonMidiNotes.length ? Math.min(...buttonMidiNotes) : rootMidiNote;
      const targetStart = Math.max(0, Math.min(MIDI_MAX - N + 1, minBtn - 4));
      viewStart += (targetStart - viewStart) * 0.08;

      const keyH = H / N;
      const rollW = W - PIANO_W;

      const btnMap = new Map<number,string>();
      buttonMidiNotes.forEach((n, i) => { if (!btnMap.has(n)) btnMap.set(n, BTN_LABELS[i]); });

      // Background
      ctx.fillStyle = "#111";
      ctx.fillRect(0, 0, W, H);

      // Roll lanes
      for (let i = 0; i < N + 1; i++) {
        const midi = Math.floor(viewStart) + i;
        const y = H - (midi - viewStart + 1) * keyH;
        ctx.fillStyle = BLACK_KEYS.has(midi % 12) ? "#0e0e0e" : "#171717";
        ctx.fillRect(PIANO_W, y, rollW, keyH);
        if (midi % 12 === 0) {
          ctx.strokeStyle = "#252525";
          ctx.lineWidth = 1;
          ctx.beginPath(); ctx.moveTo(PIANO_W, y); ctx.lineTo(W, y); ctx.stroke();
        }
      }

      // Time grid lines (every second)
      ctx.strokeStyle = "#1e1e1e";
      ctx.lineWidth = 1;
      for (let t = 0; t <= WINDOW_MS; t += 1000) {
        const x = PIANO_W + rollW * (1 - t / WINDOW_MS);
        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
      }

      // Note bars
      for (const note of historyRef.current) {
        const rel = note.midi - viewStart;
        if (rel < -1 || rel >= N + 1) continue;
        const y = H - (rel + 1) * keyH + 1;
        const h = keyH - 2;
        const endT   = note.end ?? now;
        const x1 = PIANO_W + rollW * Math.max(0, 1 - (now - note.start) / WINDOW_MS);
        const x2 = PIANO_W + rollW * Math.max(0, 1 - (now - endT)   / WINDOW_MS);
        const bx = Math.min(x1, x2);
        const bw = Math.max(2, x2 - bx);
        const alpha = note.end ? Math.max(0, 1 - (now - note.end) / FADE_MS) : 1;
        ctx.globalAlpha = alpha;
        ctx.shadowColor = "#4a9eff";
        ctx.shadowBlur  = note.end ? 0 : 6;
        ctx.fillStyle   = note.end ? "#2060aa" : "#4a9eff";
        const r = Math.min(3, h / 2, bw / 2);
        ctx.beginPath();
        ctx.roundRect(bx, y, bw, h, r);
        ctx.fill();
        ctx.shadowBlur = 0;
        ctx.globalAlpha = 1;
      }

      // Piano key strip
      for (let i = 0; i < N + 1; i++) {
        const midi = Math.floor(viewStart) + i;
        if (midi < 0 || midi > MIDI_MAX) continue;
        const y = H - (midi - viewStart + 1) * keyH;
        const isBlack  = BLACK_KEYS.has(midi % 12);
        const isActive = activeSet.has(midi);
        const isRoot   = midi === rootMidiNote;
        const label    = btnMap.get(midi);

        ctx.fillStyle = isActive ? "#4a9eff"
          : isBlack  ? "#1c1c1c"
          : isRoot   ? "#444"
          : "#c8c8c8";
        const kw = isBlack ? PIANO_W - 10 : PIANO_W - 2;
        ctx.fillRect(1, y + 1, kw, keyH - 2);
        ctx.strokeStyle = "#111";
        ctx.lineWidth = 0.5;
        ctx.strokeRect(1, y + 1, kw, keyH - 2);

        if (label && keyH >= 9) {
          ctx.fillStyle = isActive ? "#000" : isBlack ? "#777" : "#555";
          ctx.font = `bold ${Math.min(9, keyH - 3)}px monospace`;
          ctx.textAlign = "right";
          ctx.textBaseline = "middle";
          ctx.fillText(label, PIANO_W - (isBlack ? 12 : 4), y + keyH / 2);
        }
        if (midi % 12 === 0 && keyH >= 10) {
          ctx.fillStyle = isActive ? "#000" : "#444";
          ctx.font = "7px sans-serif";
          ctx.textAlign = "left";
          ctx.textBaseline = "middle";
          ctx.fillText(`C${Math.floor(midi / 12) - 1}`, 2, y + keyH / 2);
        }
      }

      // "now" edge
      ctx.strokeStyle = "#333";
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(W - 1, 0); ctx.lineTo(W - 1, H); ctx.stroke();

      // ── Position bar (bottom strip, 10px tall) ──
      const PH = 10;
      const PY = H - PH;
      ctx.fillStyle = "#0a0a0a";
      ctx.fillRect(PIANO_W, PY, rollW, PH);

      // Full-range note dots (active notes anywhere on 0-127)
      for (const note of historyRef.current) {
        if (note.end !== undefined && now - note.end > 200) continue;
        const nx = PIANO_W + (note.midi / MIDI_MAX) * rollW;
        ctx.fillStyle = "#4a9eff";
        ctx.fillRect(nx - 1, PY + 2, 2, PH - 4);
      }

      // Button positions as small ticks
      buttonMidiNotes.forEach(n => {
        const nx = PIANO_W + (n / MIDI_MAX) * rollW;
        ctx.fillStyle = "#444";
        ctx.fillRect(nx, PY + 3, 1, PH - 6);
      });

      // Current view window
      const wx1 = PIANO_W + (viewStart / MIDI_MAX) * rollW;
      const wx2 = PIANO_W + ((viewStart + N) / MIDI_MAX) * rollW;
      ctx.strokeStyle = "#4a9eff66";
      ctx.lineWidth = 1;
      ctx.strokeRect(wx1, PY + 1, wx2 - wx1, PH - 2);

      animRef.current = requestAnimationFrame(draw);
    };

    animRef.current = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(animRef.current);
  }, []); // intentionally empty — loop reads everything from refs

  return (
    <canvas
      ref={canvasRef}
      width={900}
      height={210}
      style={{width:"100%", height:"210px", borderRadius:"6px", display:"block"}}
    />
  );
}

function Panel({
  title,
  children,
  wide,
  action,
}: {
  title: string;
  children: React.ReactNode;
  wide?: boolean;
  action?: React.ReactNode;
}) {
  return (
    <section className={`panel${wide ? " panel-wide" : ""}`}>
      <h2 style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <span>{title}</span>
        {action}
      </h2>
      {children}
    </section>
  );
}

function ConfigModal({
  open,
  onClose,
  children,
}: {
  open: boolean;
  onClose: () => void;
  children: React.ReactNode;
}) {
  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') onClose(); };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, onClose]);

  if (!open) return null;
  return (
    <div className="config-backdrop" onClick={onClose}>
      <div className="config-modal" onClick={e => e.stopPropagation()}>
        <div className="config-modal-header">
          <span className="config-modal-title">Config</span>
          <button type="button" className="config-modal-close" onClick={onClose}>✕</button>
        </div>
        <div className="config-modal-body">{children}</div>
      </div>
    </div>
  );
}

function StateLine({
  label,
  value
}: {
  label: string;
  value: React.ReactNode;
}) {
  return (
    <div className="state-line">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

function Meter({ label, value }: { label: string; value: number }) {
  const percent = Math.round(Math.max(0, Math.min(1, value)) * 100);

  return (
    <div className="meter">
      <div className="meter-head">
        <span>{label}</span>
        <strong>{formatNumber(value)}</strong>
      </div>
      <div className="meter-track">
        <div className="meter-fill" style={{ width: `${percent}%` }} />
      </div>
    </div>
  );
}

function TrimWaveform({ waveform, trimStart, trimEnd, disabled, onChange }: {
  waveform: number[];
  trimStart: number;
  trimEnd: number;
  disabled: boolean;
  onChange: (next: { start?: number; end?: number }) => void;
}) {
  const svgRef = useRef<SVGSVGElement>(null);
  const dragging = useRef<'start' | 'end' | null>(null);

  if (waveform.length === 0) return null;

  const W = 1000;
  const H = 80;
  const n = waveform.length;
  const hitZone = 20;

  function svgX(e: React.MouseEvent | MouseEvent): number {
    const svg = svgRef.current;
    if (!svg) return 0;
    const rect = svg.getBoundingClientRect();
    return Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
  }

  function onMouseDown(e: React.MouseEvent) {
    if (disabled) return;
    const svg = svgRef.current;
    if (!svg) return;
    const rect = svg.getBoundingClientRect();
    const svgPx = (e.clientX - rect.left) / rect.width * W;
    const startPx = trimStart * W;
    const endPx = trimEnd * W;
    if (Math.abs(svgPx - startPx) < hitZone) {
      dragging.current = 'start';
    } else if (Math.abs(svgPx - endPx) < hitZone) {
      dragging.current = 'end';
    } else {
      return;
    }
    e.preventDefault();
    const onMove = (me: MouseEvent) => {
      const x = svgX(me);
      if (dragging.current === 'start') onChange({ start: Math.min(x, trimEnd - 0.01) });
      else onChange({ end: Math.max(x, trimStart + 0.01) });
    };
    const onUp = () => {
      dragging.current = null;
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
  }

  const midY = H / 2;
  return (
    <svg ref={svgRef} viewBox={`0 0 ${W} ${H}`}
      className={`sample-waveform-svg${disabled ? '' : ' swv-interactive'}`}
      preserveAspectRatio="none"
      onMouseDown={onMouseDown}>
      {/* Excluded regions */}
      <rect x={0} y={0} width={trimStart * W} height={H} className="swv-excluded" />
      <rect x={trimEnd * W} y={0} width={(1 - trimEnd) * W} height={H} className="swv-excluded" />
      {/* Bars */}
      {waveform.map((v, i) => {
        const x = (i / n) * W;
        const bw = W / n;
        const h = Math.max(1, v * H);
        const pos = (i + 0.5) / n;
        return (
          <rect key={i} x={x} y={(H - h) / 2}
            width={Math.max(0.5, bw - 0.8)} height={h}
            className={(pos >= trimStart && pos <= trimEnd) ? 'swv-bar-on' : 'swv-bar-off'} />
        );
      })}
      {/* Trim lines */}
      <line x1={trimStart * W} y1={0} x2={trimStart * W} y2={H} className="swv-marker" />
      <line x1={trimEnd * W} y1={0} x2={trimEnd * W} y2={H} className="swv-marker" />
    </svg>
  );
}

function Waveform({ samples }: { samples: number[] }) {
  const points = samples.length > 1
    ? samples
        .map((sample, index) => {
          const x = (index / (samples.length - 1)) * 100;
          const clamped = Math.max(-1, Math.min(1, sample));
          const y = 50 - clamped * 45;

          return `${x.toFixed(2)},${y.toFixed(2)}`;
        })
        .join(" ")
    : "";

  return (
    <div className="waveform">
      <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
        <line className="waveform-center" x1="0" y1="50" x2="100" y2="50" />
        <polyline className="waveform-line" points={points} />
      </svg>
    </div>
  );
}

// Draws the raw finger path from the DualSense touchpad as X/Y (1:1, no normalization).
const WBW = 512, WBH = 160;

function TouchpadWhiteboard({
  points,
  drawing,
}: {
  points: [number, number][];
  drawing: boolean;
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d')!;
    ctx.clearRect(0, 0, WBW, WBH);
    // Grid lines
    ctx.strokeStyle = 'rgba(255,255,255,0.07)';
    ctx.lineWidth = 1;
    for (let i = 1; i < 4; i++) {
      ctx.beginPath(); ctx.moveTo(WBW * i / 4, 0); ctx.lineTo(WBW * i / 4, WBH); ctx.stroke();
    }
    ctx.beginPath(); ctx.moveTo(0, WBH / 2); ctx.lineTo(WBW, WBH / 2); ctx.stroke();
    if (points.length < 2) return;
    ctx.strokeStyle = drawing ? 'rgba(255,255,255,0.9)' : 'rgba(200,200,255,0.5)';
    ctx.lineWidth = 1.5;
    ctx.lineJoin = 'round';
    ctx.lineCap = 'round';
    ctx.beginPath();
    for (let i = 0; i < points.length; i++) {
      const [x, y] = points[i];
      if (i === 0) ctx.moveTo(x * WBW, y * WBH); else ctx.lineTo(x * WBW, y * WBH);
    }
    ctx.stroke();
    // Start = green dot, end = yellow (drawing) / red (committed)
    const [sx, sy] = points[0];
    ctx.fillStyle = '#4ade80';
    ctx.beginPath(); ctx.arc(sx * WBW, sy * WBH, 3, 0, Math.PI * 2); ctx.fill();
    const [ex, ey] = points[points.length - 1];
    ctx.fillStyle = drawing ? '#facc15' : '#f87171';
    ctx.beginPath(); ctx.arc(ex * WBW, ey * WBH, 3, 0, Math.PI * 2); ctx.fill();
  }, [points, drawing]);

  return (
    <canvas ref={canvasRef} width={WBW} height={WBH}
      className="touchpad-whiteboard"
      title="Raw finger path — 1:1 position, no normalization"
    />
  );
}


const EDITOR_N = 128;

type CP = { x: number; amp: number };

function cpToSamples(cps: CP[], n: number): number[] {
  if (cps.length === 0) return new Array(n).fill(0);
  const pts = [...cps].sort((a, b) => a.x - b.x);
  const out: number[] = new Array(n);
  for (let i = 0; i < n; i++) {
    const x = i / (n - 1);
    if (pts.length === 1 || x <= pts[0].x) { out[i] = pts[0].amp; continue; }
    if (x >= pts[pts.length - 1].x) { out[i] = pts[pts.length - 1].amp; continue; }
    let lo = 0;
    for (let j = 0; j < pts.length - 1; j++) {
      if (pts[j].x <= x && pts[j + 1].x > x) { lo = j; break; }
    }
    const p0 = pts[lo], p1 = pts[lo + 1];
    out[i] = Math.max(-1, Math.min(1, p0.amp + ((x - p0.x) / (p1.x - p0.x)) * (p1.amp - p0.amp)));
  }
  return out;
}

const PRESET_POINTS: Record<string, CP[]> = {
  sine:     Array.from({ length: 33 }, (_, i) => ({ x: i / 32, amp: Math.sin(2 * Math.PI * i / 32) })),
  saw:      [{ x: 0, amp: 1 }, { x: 0.998, amp: -1 }, { x: 0.999, amp: 1 }],
  triangle: [{ x: 0, amp: 0 }, { x: 0.25, amp: 1 }, { x: 0.75, amp: -1 }, { x: 1, amp: 0 }],
  square:   [{ x: 0, amp: 1 }, { x: 0.499, amp: 1 }, { x: 0.5, amp: -1 }, { x: 0.999, amp: -1 }, { x: 1, amp: 1 }],
  pulse:    [{ x: 0, amp: 1 }, { x: 0.245, amp: 1 }, { x: 0.25, amp: -1 }, { x: 0.999, amp: -1 }, { x: 1, amp: 1 }],
  organ:    Array.from({ length: 33 }, (_, i) => {
    const phase = 2 * Math.PI * i / 32;
    return { x: i / 32, amp: Math.max(-1, Math.min(1, Math.sin(phase) * 0.72 + Math.sin(phase * 2) * 0.22 + Math.sin(phase * 3) * 0.12)) };
  }),
};

const CW = 512, CH = 130, HIT_R = 7;
const GRAPH_TOP = 0;
const GRAPH_H = CH;

function cpXY(cp: CP): [number, number] {
  return [cp.x * CW, GRAPH_TOP + (0.5 - cp.amp * 0.45) * GRAPH_H];
}
function xyCP(cx: number, cy: number): CP {
  return {
    x: Math.max(0, Math.min(1, cx / CW)),
    amp: Math.max(-1, Math.min(1, (0.5 - (cy - GRAPH_TOP) / GRAPH_H) / 0.45)),
  };
}

function WaveformEditor({
  touchpadSketch,
  touchpadDrawing,
  morphAmount,
  noiseAmount,
  unisonAmount,
  onApply,
  disabled,
}: {
  touchpadSketch: number[];
  touchpadDrawing: boolean;
  morphAmount: number;
  noiseAmount: number;
  unisonAmount: number;
  onApply: (samples: number[], morphSamples: number[]) => void;
  disabled?: boolean;
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const ptsRef = useRef<CP[]>([...PRESET_POINTS.sine]);
  const morphPtsRef = useRef<CP[]>([...PRESET_POINTS.triangle]);
  const [mainName, setMainName] = useState<keyof typeof PRESET_POINTS | null>('sine');
  const [morphName, setMorphName] = useState<keyof typeof PRESET_POINTS>('triangle');
  const dragIdx = useRef<number | null>(null);
  const [ver, setVer] = useState(0);
  const sketchRef = useRef<number[]>(touchpadSketch);
  const drawingRef = useRef(touchpadDrawing);
  const macroRef = useRef({ morphAmount, noiseAmount, unisonAmount });

  function sampleLoop(samples: number[], index: number): number {
    if (samples.length === 0) return 0;
    const wrapped = ((index % samples.length) + samples.length) % samples.length;
    const lo = Math.floor(wrapped);
    const hi = (lo + 1) % samples.length;
    const t = wrapped - lo;
    return samples[lo] + (samples[hi] - samples[lo]) * t;
  }

  function drawWave(ctx: CanvasRenderingContext2D, samples: number[], color: string, width: number, yOffset = 0) {
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.beginPath();
    for (let i = 0; i < CW; i++) {
      const src = i * (samples.length - 1) / (CW - 1);
      const y = GRAPH_TOP + (0.5 - sampleLoop(samples, src) * 0.45) * GRAPH_H + yOffset;
      if (i === 0) ctx.moveTo(0, y); else ctx.lineTo(i, y);
    }
    ctx.stroke();
  }

  function redraw() {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d')!;
    ctx.clearRect(0, 0, CW, CH);

    ctx.fillStyle = '#141923';
    ctx.fillRect(0, 0, CW, CH);
    ctx.strokeStyle = 'rgba(216, 208, 189, 0.12)';
    ctx.lineWidth = 1;
    for (let x = 0; x <= CW; x += CW / 8) {
      ctx.beginPath(); ctx.moveTo(x, GRAPH_TOP); ctx.lineTo(x, GRAPH_TOP + GRAPH_H); ctx.stroke();
    }
    for (let y = GRAPH_TOP + GRAPH_H / 4; y < GRAPH_TOP + GRAPH_H; y += GRAPH_H / 4) {
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(CW, y); ctx.stroke();
    }
    ctx.strokeStyle = 'rgba(216, 208, 189, 0.28)';
    ctx.beginPath(); ctx.moveTo(0, GRAPH_TOP + GRAPH_H / 2); ctx.lineTo(CW, GRAPH_TOP + GRAPH_H / 2); ctx.stroke();

    const sk = sketchRef.current;
    if (sk.length >= 2) {
      ctx.strokeStyle = drawingRef.current ? 'rgba(214, 176, 92, 0.82)' : 'rgba(214, 176, 92, 0.36)';
      ctx.lineWidth = 1;
      ctx.beginPath();
      for (let i = 0; i < sk.length; i++) {
        const x = (i / (sk.length - 1)) * CW;
        const y = GRAPH_TOP + (0.5 - sk[i] * 0.45) * GRAPH_H;
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }
    const main = cpToSamples([...ptsRef.current].sort((a, b) => a.x - b.x), CW);
    const morph = cpToSamples(morphPtsRef.current, CW);
    const { morphAmount: morphMix } = macroRef.current;
    const result = main.map((sample, i) => sample + (morph[i] - sample) * morphMix);

    drawWave(ctx, morph, 'rgba(143, 173, 151, 0.5)', 1);
    drawWave(ctx, result, 'rgba(245, 241, 229, 0.96)', 1.8);

    // CP curve on top
    const sorted = [...ptsRef.current].sort((a, b) => a.x - b.x);
    if (sorted.length >= 2) {
      const samps = cpToSamples(sorted, CW);
      ctx.strokeStyle = 'rgba(110, 168, 254, 0.88)'; ctx.lineWidth = 1.5;
      ctx.beginPath();
      for (let i = 0; i < CW; i++) {
        const y = GRAPH_TOP + (0.5 - samps[i] * 0.45) * GRAPH_H;
        if (i === 0) ctx.moveTo(0, y); else ctx.lineTo(i, y);
      }
      ctx.stroke();
    }
    for (const cp of ptsRef.current) {
      const [cx, cy] = cpXY(cp);
      ctx.beginPath(); ctx.arc(cx, cy, HIT_R - 1, 0, Math.PI * 2);
      ctx.fillStyle = '#e8e2d3'; ctx.fill();
      ctx.strokeStyle = '#141923'; ctx.lineWidth = 1.5; ctx.stroke();
    }
  }

  // Redraws when ver bumps (structural CP changes like add/delete/preset).
  useEffect(() => { redraw(); }, [ver]); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    macroRef.current = { morphAmount, noiseAmount, unisonAmount };
    redraw();
  }, [morphAmount, noiseAmount, unisonAmount]); // eslint-disable-line react-hooks/exhaustive-deps

  // On finger lift: auto-load sketch as control points, then redraw.
  // While drawing: just update refs and redraw the amber overlay.
  useEffect(() => {
    const wasDrawing = drawingRef.current;
    sketchRef.current = touchpadSketch;
    drawingRef.current = touchpadDrawing;
    if (wasDrawing && !touchpadDrawing && touchpadSketch.length >= 2) {
      const n = 32;
      const next = Array.from({ length: n }, (_, i) => {
        const src = Math.round(i * (touchpadSketch.length - 1) / (n - 1));
        return { x: i / (n - 1), amp: touchpadSketch[src] };
      });
      ptsRef.current = next;
      setMainName(null);
      if (!disabled) {
        onApply(
          cpToSamples([...next].sort((a, b) => a.x - b.x), EDITOR_N),
          cpToSamples([...morphPtsRef.current].sort((a, b) => a.x - b.x), EDITOR_N),
        );
      }
      setVer(v => v + 1); // bump triggers redraw via the other useEffect
    } else {
      redraw();
    }
  }, [touchpadSketch, touchpadDrawing, disabled, onApply]); // eslint-disable-line react-hooks/exhaustive-deps

  function evXY(e: React.PointerEvent<HTMLCanvasElement> | React.MouseEvent<HTMLCanvasElement>): [number, number] {
    const r = canvasRef.current!.getBoundingClientRect();
    return [(e.clientX - r.left) * (CW / r.width), (e.clientY - r.top) * (CH / r.height)];
  }
  function hitTest(cx: number, cy: number): number {
    for (let i = 0; i < ptsRef.current.length; i++) {
      const [px, py] = cpXY(ptsRef.current[i]);
      if (Math.hypot(cx - px, cy - py) <= HIT_R + 3) return i;
    }
    return -1;
  }

  const onPointerDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (disabled) return;
    const [cx, cy] = evXY(e);
    const h = hitTest(cx, cy);
    (e.target as HTMLCanvasElement).setPointerCapture(e.pointerId);
    if (h >= 0) {
      dragIdx.current = h;
    } else {
      ptsRef.current = [...ptsRef.current, xyCP(cx, cy)];
      dragIdx.current = ptsRef.current.length - 1;
      setMainName(null);
      setVer(v => v + 1);
    }
  };
  const onPointerMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (dragIdx.current === null) return;
    const [cx, cy] = evXY(e);
    const pts = [...ptsRef.current];
    pts[dragIdx.current] = xyCP(cx, cy);
    ptsRef.current = pts;
    if (mainName !== null) setMainName(null);
    redraw(); // immediate feedback during drag, no re-render needed
  };
  function mainSamples(): number[] {
    return cpToSamples([...ptsRef.current].sort((a, b) => a.x - b.x), EDITOR_N);
  }

  function morphSamples(): number[] {
    return cpToSamples([...morphPtsRef.current].sort((a, b) => a.x - b.x), EDITOR_N);
  }

  function applyCurrent() {
    if (!disabled) onApply(mainSamples(), morphSamples());
  }

  const onPointerUp = () => {
    if (dragIdx.current !== null) applyCurrent();
    dragIdx.current = null;
  };
  const onDoubleClick = (e: React.MouseEvent<HTMLCanvasElement>) => {
    if (disabled || ptsRef.current.length <= 2) return;
    const [cx, cy] = evXY(e);
    const h = hitTest(cx, cy);
    if (h >= 0) {
      ptsRef.current = ptsRef.current.filter((_, i) => i !== h);
      setMainName(null);
      setVer(v => v + 1);
      applyCurrent();
    }
  };

  function setMainPreset(name: keyof typeof PRESET_POINTS) {
    ptsRef.current = [...PRESET_POINTS[name]];
    setMainName(name);
    setVer(v => v + 1);
    applyCurrent();
  }

  function setMorphPreset(name: keyof typeof PRESET_POINTS) {
    morphPtsRef.current = [...PRESET_POINTS[name]];
    setMorphName(name);
    setVer(v => v + 1);
    applyCurrent();
  }

  return (
    <div className="waveform-editor">
      <div className="waveform-editor-head">
        <span className="waveform-editor-title">OSC TABLE</span>
        <span className="waveform-editor-readout">draw / bank / morph</span>
      </div>
      <div className="waveform-preset-row">
        <span className="waveform-preset-label">A</span>
        <div className="waveform-editor-presets">
          {(Object.keys(PRESET_POINTS) as (keyof typeof PRESET_POINTS)[]).map(name => (
            <button key={name} type="button"
              className={`preset-btn${mainName === name ? ' preset-btn-active' : ''}`}
              disabled={disabled}
              onClick={() => setMainPreset(name)}>{name}</button>
          ))}
        </div>
      </div>
      <div className="waveform-preset-row">
        <span className="waveform-preset-label">B</span>
        <div className="waveform-editor-presets">
          {(Object.keys(PRESET_POINTS) as (keyof typeof PRESET_POINTS)[]).map(name => (
            <button key={name} type="button"
              className={`preset-btn${morphName === name ? ' preset-btn-active' : ''}`}
              disabled={disabled}
              onClick={() => setMorphPreset(name)}>{name}</button>
          ))}
        </div>
      </div>
      <div className="waveform-display">
        <canvas ref={canvasRef} width={CW} height={CH} className="waveform-editor-canvas"
          onPointerDown={onPointerDown} onPointerMove={onPointerMove}
          onPointerUp={onPointerUp} onDoubleClick={onDoubleClick}
          style={{ cursor: disabled ? 'default' : 'crosshair', touchAction: 'none' }}
        />
      </div>
      <p className="waveform-editor-hint">Click → add point · drag → move · double-click → delete</p>
      <button type="button" className="waveform-apply-btn" disabled={disabled}
        onClick={() => onApply(
          cpToSamples([...ptsRef.current].sort((a, b) => a.x - b.x), EDITOR_N),
          cpToSamples([...morphPtsRef.current].sort((a, b) => a.x - b.x), EDITOR_N),
        )}>
        Apply to bank
      </button>
    </div>
  );
}

function WavetableMacroSlider({
  label,
  value,
  disabled,
  tone,
  onChange,
}: {
  label: string;
  value: number;
  disabled?: boolean;
  tone: 'morph' | 'noise' | 'unison';
  onChange: (value: number) => void;
}) {
  const pct = Math.round(value * 100);
  const tag = tone === 'morph' ? 'M' : tone === 'noise' ? 'N' : 'U';
  const angle = -135 + value * 270;
  return (
    <label className={`wavetable-macro wavetable-macro-${tone}`}>
      <span className="wavetable-macro-head">
        <span className="wavetable-macro-tag">{tag}</span>
        <span>{label}</span>
        <strong>{String(pct).padStart(3, '0')}</strong>
      </span>
      <span className="wavetable-macro-dial">
        <span className="wavetable-macro-ring" />
        <span className="wavetable-macro-needle" style={{ transform: `rotate(${angle}deg)` }} />
        <input type="range" min={0} max={100}
          value={pct}
          disabled={disabled}
          onChange={(e) => onChange(Number(e.target.value) / 100)} />
      </span>
    </label>
  );
}

// --- Sequencer ---

type SeqStepEdit = { active: boolean; tie: boolean; degree: number; velocity: number; midiNote: number };
type SeqTrackEdit = { midiChannel: number; midiProgram: number; midiBank: number; loopLength: number; muted: boolean; steps: SeqStepEdit[] };
const SEQ_STEP_COUNTS = [8, 16, 32, 64] as const;
const SEQ_STEP_DIVISIONS = [8, 16, 32] as const;
const SEQ_TRACK_COLORS = ['#6ea8fe', '#86efac', '#f59e0b', '#f472b6', '#22d3ee', '#a78bfa', '#fb7185', '#c084fc'];
const SEQ_PAGE_SIZE = 16;
const emptySeqStep = (): SeqStepEdit => ({ active: false, tie: false, degree: 0, velocity: 100, midiNote: -1 });
const resizeSeqSteps = (steps: SeqStepEdit[], count: number): SeqStepEdit[] =>
  Array.from({ length: count }, (_, i) => steps[i] ? { ...steps[i] } : emptySeqStep());
const seqTracksSignature = (tracks: RuntimeState['seq']['tracks']): string =>
  tracks.map(t => [
    t.midiChannel,
    t.midiProgram,
    t.midiBank,
    t.loopLength,
    t.muted ? 1 : 0,
    t.steps.length,
    t.steps.map(s => `${s.active ? 1 : 0}${s.tie ? 1 : 0}:${s.degree}:${s.velocity}:${s.midiNote}`).join(',')
  ].join('|')).join(';');

function SequencerPanel({
  seq,
  presets,
  connection,
  sampleMode,
  sampleIsWavetable,
  onPlay,
  onStop,
  onSelectTrack,
  onSelectStep,
  onAddTrack,
  onRemoveTrack,
  onChange,
}: {
  seq: RuntimeState['seq'] | undefined;
  presets: SoundfontPreset[];
  connection: ConnectionState;
  sampleMode: boolean;
  sampleIsWavetable: boolean;
  onPlay: () => void;
  onStop: () => void;
  onSelectTrack: (track: number) => void;
  onSelectStep: (step: number) => void;
  onAddTrack: () => void;
  onRemoveTrack: (track: number) => void;
  onChange: (cfg: { bpm: number; gatePct: number; stepCount: number; stepDivision: number; tracks: SeqTrackEdit[] }) => void;
}) {
  const initialized = useRef(false);
  const tracksSignature = useRef('');
  const [bpm, setBpm] = useState(120);
  const [gate, setGate] = useState(50);
  const [stepCount, setStepCount] = useState(32);
  const [stepDivision, setStepDivision] = useState(16);
  const [stepPage, setStepPage] = useState(0);
  const [tracks, setTracks] = useState<SeqTrackEdit[]>(
    Array.from({ length: 4 }, (_, ti) => ({
      midiChannel: ti, midiProgram: 0, midiBank: 0, loopLength: 32, muted: false,
      steps: Array.from({ length: 32 }, emptySeqStep),
    }))
  );

  const activeTrack = seq?.activeTrack ?? 0;
  const selectedStep = seq?.selectedStep ?? -1;

  // Mirror server state (controller arm-writes notes in)
  useEffect(() => {
    if (!seq) return;
    if (!initialized.current) {
      initialized.current = true;
      setBpm(seq.bpm);
      setGate(seq.gatePct);
      setStepCount(seq.stepCount ?? seq.tracks[0]?.steps.length ?? 32);
      setStepDivision(seq.stepDivision ?? 16);
    }
    const nextStepCount = seq.stepCount ?? seq.tracks[0]?.steps.length ?? 32;
    const nextStepDivision = seq.stepDivision ?? 16;
    setStepCount(nextStepCount);
    setStepDivision(nextStepDivision);
    const nextSignature = seqTracksSignature(seq.tracks);
    if (nextSignature !== tracksSignature.current) {
      tracksSignature.current = nextSignature;
      setTracks(seq.tracks.map(t => ({
        midiChannel: t.midiChannel,
        midiProgram: t.midiProgram,
        midiBank: t.midiBank,
        loopLength: Math.max(1, Math.min(nextStepCount, t.loopLength ?? nextStepCount)),
        muted: t.muted,
        steps: resizeSeqSteps(t.steps.map(s => ({ ...s })), nextStepCount),
      })));
    }
  }, [seq]);

  const disabled = connection !== 'online';
  const currentStep = seq?.currentStep ?? -1;
  const playheadStep = seq?.playheadStep ?? currentStep;
  const playing = seq?.playing ?? false;
  const activeLoopLength = Math.max(1, Math.min(stepCount, tracks[activeTrack]?.loopLength ?? stepCount));
  const activeTrackCurrentStep = playing && playheadStep >= 0 ? playheadStep % activeLoopLength : -1;
  const pageCount = Math.max(1, Math.ceil(stepCount / SEQ_PAGE_SIZE));
  const visibleStart = Math.min(stepPage, pageCount - 1) * SEQ_PAGE_SIZE;
  const visibleEnd = Math.min(stepCount, visibleStart + SEQ_PAGE_SIZE);
  const visibleStepCount = visibleEnd - visibleStart;
  const visibleTracks = useMemo(() => tracks.map(track => ({
    ...track,
    visibleSteps: track.steps.slice(visibleStart, visibleEnd),
  })), [tracks, visibleStart, visibleEnd]);

  useEffect(() => {
    setStepPage(page => Math.min(page, Math.max(0, Math.ceil(stepCount / SEQ_PAGE_SIZE) - 1)));
  }, [stepCount]);

  useEffect(() => {
    if (selectedStep >= 0) {
      setStepPage(Math.floor(selectedStep / SEQ_PAGE_SIZE));
    }
  }, [selectedStep]);

  useEffect(() => {
    if (playing && activeTrackCurrentStep >= 0) {
      setStepPage(Math.floor(activeTrackCurrentStep / SEQ_PAGE_SIZE));
    }
  }, [playing, activeTrackCurrentStep]);

  function send(t: SeqTrackEdit[], b: number, g: number, count = stepCount, division = stepDivision) {
    onChange({ bpm: b, gatePct: g, stepCount: count, stepDivision: division, tracks: t });
  }

  function handleStepClick(trackIdx: number, stepIdx: number) {
    if (stepIdx >= (tracks[trackIdx]?.loopLength ?? stepCount)) return;
    if (activeTrack !== trackIdx) {
      onSelectTrack(trackIdx);
    }
    onSelectStep(activeTrack === trackIdx && selectedStep === stepIdx ? -1 : stepIdx);
  }

  function clearArmedStep() {
    if (selectedStep < 0) return;
    const next = tracks.map((t, ti) =>
      ti === activeTrack
        ? { ...t, steps: t.steps.map((s, si) => si === selectedStep ? emptySeqStep() : s) }
        : t
    );
    setTracks(next);
    send(next, bpm, gate);
    onSelectStep(-1);
  }

  function handleStepRightClick(trackIdx: number, stepIdx: number, e: React.MouseEvent) {
    e.preventDefault();
    if (!tracks[trackIdx]?.steps[stepIdx]?.active) return;
    const next = tracks.map((t, ti) =>
      ti === trackIdx
        ? { ...t, steps: t.steps.map((s, si) => si === stepIdx ? emptySeqStep() : s) }
        : t
    );
    setTracks(next);
    send(next, bpm, gate);
    if (trackIdx === activeTrack && selectedStep === stepIdx) onSelectStep(-1);
  }

  function handleBpm(v: number) {
    const c = Math.max(20, Math.min(300, v));
    setBpm(c); send(tracks, c, gate);
  }

  function handleGate(v: number) {
    const nextGate = Math.max(5, Math.min(100, Math.round(v)));
    setGate(nextGate); send(tracks, bpm, nextGate);
  }

  function handleGatePointer(e: ReactPointerEvent<HTMLSpanElement>) {
    if (disabled) return;
    const rect = e.currentTarget.getBoundingClientRect();
    const nextGate = ((e.clientX - rect.left) / rect.width) * 100;
    handleGate(nextGate);
  }

  function handleGateKey(e: ReactKeyboardEvent<HTMLSpanElement>) {
    if (disabled) return;
    if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') {
      e.preventDefault();
      handleGate(gate - 1);
    } else if (e.key === 'ArrowRight' || e.key === 'ArrowUp') {
      e.preventDefault();
      handleGate(gate + 1);
    } else if (e.key === 'PageDown') {
      e.preventDefault();
      handleGate(gate - 10);
    } else if (e.key === 'PageUp') {
      e.preventDefault();
      handleGate(gate + 10);
    } else if (e.key === 'Home') {
      e.preventDefault();
      handleGate(5);
    } else if (e.key === 'End') {
      e.preventDefault();
      handleGate(100);
    }
  }

  function handleStepCount(v: number) {
    const nextCount = SEQ_STEP_COUNTS.includes(v as typeof SEQ_STEP_COUNTS[number]) ? v : 32;
    const nextTracks = tracks.map(t => ({
      ...t,
      loopLength: Math.max(1, Math.min(nextCount, t.loopLength === stepCount ? nextCount : t.loopLength)),
      steps: resizeSeqSteps(t.steps, nextCount),
    }));
    tracksSignature.current = '';
    setStepCount(nextCount);
    setTracks(nextTracks);
    if (selectedStep >= nextCount) onSelectStep(-1);
    setStepPage(page => Math.min(page, Math.max(0, Math.ceil(nextCount / SEQ_PAGE_SIZE) - 1)));
    send(nextTracks, bpm, gate, nextCount, stepDivision);
  }

  function handleStepDivision(v: number) {
    const nextDivision = SEQ_STEP_DIVISIONS.includes(v as typeof SEQ_STEP_DIVISIONS[number]) ? v : 16;
    setStepDivision(nextDivision);
    send(tracks, bpm, gate, stepCount, nextDivision);
  }

  function handleLoopLength(trackIdx: number, length: number) {
    const nextLength = Math.max(1, Math.min(stepCount, length));
    const next = tracks.map((t, i) => i === trackIdx ? { ...t, loopLength: nextLength } : t);
    tracksSignature.current = '';
    setTracks(next);
    if (trackIdx === activeTrack && selectedStep >= nextLength) onSelectStep(-1);
    send(next, bpm, gate);
  }

  function handleActiveLoopLength(length: number) {
    handleLoopLength(activeTrack, length);
  }

  const armedStep = selectedStep >= 0 ? tracks[activeTrack]?.steps[selectedStep] : null;
  const trackPatchName = (track: SeqTrackEdit) => patchName(track.midiBank, track.midiProgram, presets);

  return (
    <div className="sequencer">
      <div className="seq-controls">
        <div className="seq-control-group">
          <span className="seq-group-label">Clock</span>
          <div className="seq-group-body">
            <button type="button" className={`seq-playstop${playing ? ' seq-playing' : ''}`}
              disabled={disabled} onClick={playing ? onStop : onPlay}>
              {playing ? '\u25a0 Stop' : '\u25b6 Play'}
            </button>
            <label className="seq-label">
              BPM
              <input type="number" className="seq-bpm-input" min={20} max={300}
                value={bpm} disabled={disabled}
                onChange={e => handleBpm(Number(e.target.value))} />
            </label>
          </div>
        </div>

        <div className="seq-control-group seq-step-group">
          <span className="seq-group-label">Step</span>
          <div className="seq-group-body">
            <div className="seq-chip-group">
              {SEQ_STEP_DIVISIONS.map(n => (
                <button key={n} type="button"
                  className={stepDivision === n ? 'seq-chip-active' : ''}
                  disabled={disabled}
                  onClick={() => handleStepDivision(n)}>
                  1/{n}
                </button>
              ))}
            </div>
            <div className="seq-gate-field">
              <div className="seq-gate-head">
                <span>Gate</span>
                <span>{gate}%</span>
              </div>
              <span className={`seq-gate-control${disabled ? ' seq-gate-disabled' : ''}`}
                role="slider"
                tabIndex={disabled ? -1 : 0}
                aria-label="Gate"
                aria-valuemin={5}
                aria-valuemax={100}
                aria-valuenow={gate}
                aria-valuetext={`${gate}%`}
                onKeyDown={handleGateKey}
                onPointerDown={e => {
                  if (disabled) return;
                  e.currentTarget.setPointerCapture(e.pointerId);
                  handleGatePointer(e);
                }}
                onPointerMove={e => {
                  if (e.currentTarget.hasPointerCapture(e.pointerId)) handleGatePointer(e);
                }}
                onPointerUp={e => {
                  if (e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId);
                }}>
                <span className="seq-gate-fill" style={{ width: `${gate}%` }} />
                <span className="seq-gate-handle" style={{ left: `${gate}%` }} />
              </span>
            </div>
          </div>
        </div>

        <div className="seq-control-group">
          <span className="seq-group-label">Loop</span>
          <div className="seq-group-body">
            <div className="seq-chip-group">
              {SEQ_STEP_COUNTS.map(n => (
                <button key={n} type="button"
                  className={stepCount === n ? 'seq-chip-active' : ''}
                  disabled={disabled}
                  onClick={() => handleStepCount(n)}>
                  {n}
                </button>
              ))}
            </div>
            <div className="seq-len-stepper">
              <span>T{activeTrack + 1}</span>
              <button type="button" disabled={disabled || activeLoopLength <= 1}
                onClick={() => handleActiveLoopLength(activeLoopLength - 1)}
                aria-label="Shorten active track loop">-</button>
              <input type="number" min={1} max={stepCount}
                value={activeLoopLength} disabled={disabled}
                onChange={e => handleActiveLoopLength(Number(e.target.value))} />
              <button type="button" disabled={disabled || activeLoopLength >= stepCount}
                onClick={() => handleActiveLoopLength(activeLoopLength + 1)}
                aria-label="Lengthen active track loop">+</button>
            </div>
          </div>
        </div>

        {selectedStep >= 0 && (
          <span className="seq-armed-info">
            T{activeTrack + 1} &middot; step {selectedStep + 1}
            {armedStep && armedStep.midiNote >= 0
              ? ` \u00b7 ${midiNoteName(armedStep.midiNote)}`
              : armedStep?.active
                ? ` \u00b7 scale deg ${armedStep.degree}`
                : ' \u00b7 empty \u2014 press a face button'}
          </span>
        )}
      </div>

      {pageCount > 1 && (
        <div className="seq-page-tabs">
          {Array.from({ length: pageCount }, (_, page) => {
            const start = page * SEQ_PAGE_SIZE;
            const end = Math.min(stepCount, start + SEQ_PAGE_SIZE);
            const containsCurrent = activeTrackCurrentStep >= start && activeTrackCurrentStep < end;
            return (
              <button key={page} type="button"
                className={[
                  stepPage === page ? 'seq-page-active' : '',
                  containsCurrent ? 'seq-page-current' : '',
                ].filter(Boolean).join(' ')}
                disabled={disabled}
                onClick={() => setStepPage(page)}>
                {start + 1}-{end}
              </button>
            );
          })}
        </div>
      )}

      <div className="seq-tracks">
        {visibleTracks.map((track, ti) => (
          <div key={ti}
            className={`seq-track-row${tracks[ti].muted ? ' seq-track-muted' : ''}${ti === activeTrack ? ' seq-row-active' : ''}`}
            style={{ '--seq-track-color': SEQ_TRACK_COLORS[ti % SEQ_TRACK_COLORS.length] } as CSSProperties}>
            <div
              className={`seq-track-header${ti === activeTrack ? ' seq-track-active' : ''}`}
              role="button"
              tabIndex={0}
              onClick={() => { if (!disabled) onSelectTrack(ti); }}
              onKeyDown={e => { if (e.key === 'Enter' && !disabled) onSelectTrack(ti); }}
              title={`Track ${ti + 1} — MIDI ch ${(track.midiChannel ?? ti) + 1} — ${sampleMode ? (sampleIsWavetable ? 'Wavetable' : 'Mic sample') : trackPatchName(track)}`}>
              <div className="seq-track-header-top">
                <span className="seq-track-id">T{ti + 1} <small>ch{(track.midiChannel ?? ti) + 1}</small></span>
                <span className="seq-track-loop">L{track.loopLength}</span>
                <button type="button" className={`seq-track-mute-btn${track.muted ? ' seq-track-muted-active' : ''}`}
                  disabled={disabled}
                  title={track.muted ? 'Unmute track' : 'Mute track'}
                  onClick={e => {
                    e.stopPropagation();
                    const next = tracks.map((t, i) => i === ti ? { ...t, muted: !t.muted } : t);
                    setTracks(next);
                    send(next, bpm, gate);
                  }}>
                  M
                </button>
                <button type="button" className="seq-track-remove-btn"
                  disabled={disabled || tracks.length <= 1}
                  title="Remove track"
                  onClick={e => { e.stopPropagation(); onRemoveTrack(ti); }}>
                  &#x2715;
                </button>
              </div>
              <span className="seq-track-name">{sampleMode ? (sampleIsWavetable ? 'Wavetable' : 'Mic sample') : trackPatchName(track)}</span>
            </div>
            <div className="seq-track-steps" style={{
              '--seq-step-cols': visibleStepCount,
              '--seq-step-mobile-cols': Math.min(visibleStepCount, 16),
            } as CSSProperties}>
              {track.visibleSteps.map((step, offset) => {
                const si = visibleStart + offset;
                const label = step.midiNote >= 0 ? midiNoteName(step.midiNote) : '';
                const isArmed = ti === activeTrack && selectedStep === si;
                const loopLength = Math.max(1, Math.min(stepCount, track.loopLength));
                const isInLoop = si < loopLength;
                const isCurrent = isInLoop && playheadStep >= 0 && playheadStep % loopLength === si && playing;
                return (
                  <button key={si} type="button"
                    className={[
                      'seq-step-btn',
                      !isInLoop ? 'seq-step-out' : '',
                      si % 16 === 0 ? 'seq-step-page' : '',
                      si % 4 === 0 ? 'seq-step-beat' : '',
                      step.active ? 'seq-step-on' : '',
                      isCurrent ? 'seq-step-current' : '',
                      isArmed ? 'seq-step-armed' : '',
                    ].filter(Boolean).join(' ')}
                    disabled={disabled || !isInLoop}
                    onClick={() => handleStepClick(ti, si)}
                    onContextMenu={(e) => { if (!disabled) handleStepRightClick(ti, si, e); }}
                    title={`T${ti + 1} step ${si + 1}${isInLoop ? '' : ' out of loop'}${step.active ? ` \u2014 ${step.tie ? 'tie' : (label || 'scale')} vel ${step.velocity}` : ''}`}>
                    <span className="seq-step-num">{si + 1}</span>
                    {step.active && (
                      <span className="seq-step-note">{step.tie ? '–' : label}</span>
                    )}
                    {step.active && (
                      <span className="seq-step-vel-dot"
                        style={{ width: `${Math.round(step.velocity / 127 * 100)}%` }} />
                    )}
                  </button>
                );
              })}
            </div>
          </div>
        ))}
      </div>

      <button type="button" className="seq-add-track-btn"
        disabled={disabled || tracks.length >= 16}
        onClick={onAddTrack}>
        + Track
      </button>
    </div>
  );
}

// Meter that shows a threshold line at 50% (1.0 normalised to 0..2 range)
// ─── FFT utilities ────────────────────────────────────────────────────────────


function hannCoeff(i: number, N: number): number {
  return 0.5 * (1 - Math.cos((2 * Math.PI * i) / (N - 1)));
}

/** In-place Cooley-Tukey radix-2 DIT FFT. N must be a power of two. */
function fftInPlace(re: Float32Array, im: Float32Array): void {
  const N = re.length;
  for (let i = 1, j = 0; i < N; i++) {
    let bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      let t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (let len = 2; len <= N; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    const wRe = Math.cos(ang), wIm = Math.sin(ang);
    for (let i = 0; i < N; i += len) {
      let tRe = 1, tIm = 0;
      for (let j = 0; j < (len >> 1); j++) {
        const uRe = re[i + j], uIm = im[i + j];
        const h   = i + j + (len >> 1);
        const vRe = re[h] * tRe - im[h] * tIm;
        const vIm = re[h] * tIm + im[h] * tRe;
        re[i + j] = uRe + vRe; im[i + j] = uIm + vIm;
        re[h]     = uRe - vRe; im[h]     = uIm - vIm;
        const nTRe = tRe * wRe - tIm * wIm;
        tIm        = tRe * wIm + tIm * wRe;
        tRe        = nTRe;
      }
    }
  }
}

/** Windowed FFT → magnitude spectrum in dBFS, bins 0..N/2. */
function spectrumDB(samples: number[]): Float32Array {
  const N  = samples.length;
  const re = new Float32Array(N);
  const im = new Float32Array(N);
  for (let i = 0; i < N; i++) re[i] = samples[i] * hannCoeff(i, N);
  fftInPlace(re, im);
  const half = (N >> 1) + 1;
  const out  = new Float32Array(half);
  for (let i = 0; i < half; i++) {
    const mag = Math.sqrt(re[i] * re[i] + im[i] * im[i]) / (N >> 1);
    out[i]    = 20 * Math.log10(Math.max(mag, 1e-10));
  }
  return out;
}

function dbToRgb(dB: number): [number, number, number] {
  const t = Math.max(0, Math.min(1, (dB - (-90)) / ((-10) - (-90))));
  if (t < 0.25) {
    const u = t * 4;
    return [0, 0, Math.round(u * 200)];
  } else if (t < 0.5) {
    const u = (t - 0.25) * 4;
    return [0, Math.round(u * 200), 200];
  } else if (t < 0.75) {
    const u = (t - 0.5) * 4;
    return [Math.round(u * 255), Math.round(200 + u * 55), Math.round(200 * (1 - u))];
  } else {
    const u = (t - 0.75) * 4;
    return [255, 255, Math.round(u * 255)];
  }
}

// ─── Spectrogram component ────────────────────────────────────────────────────

const SPEC_CANVAS_W  = 800;
const SPEC_CANVAS_H  = 220;
const STRIP_H        = 54;    // 3 lanes × 18 px each
const SPEC_SAMPLE_RATE = 48_000;
const SPEC_F_MIN     = 80;    // Hz at canvas bottom
const SPEC_F_MAX     = 8_000; // Hz at canvas top
const SPEC_LOG_RNG   = Math.log2(SPEC_F_MAX / SPEC_F_MIN);

/** Y pixel (0 = top = high freq) for a frequency on the log-scale canvas */
function specFreqToRow(f: number): number {
  if (f <= SPEC_F_MIN) return SPEC_CANVAS_H - 1;
  if (f >= SPEC_F_MAX) return 0;
  return Math.round((1 - Math.log2(f / SPEC_F_MIN) / SPEC_LOG_RNG) * (SPEC_CANVAS_H - 1));
}

/** Frequency at a given Y pixel on the log-scale canvas */
function specRowToFreq(y: number): number {
  return SPEC_F_MIN * Math.pow(2, (1 - y / (SPEC_CANVAS_H - 1)) * SPEC_LOG_RNG);
}

/** MIDI note → Hz */
function specMidiFreq(n: number): number {
  return 440 * Math.pow(2, (n - 69) / 12);
}

function Spectrogram({
  specSamples,
  activeNotes,
  blowActive,
  blowLevel,
  expression,
  filterCutoff,
}: {
  specSamples: number[];
  activeNotes: number[];
  blowActive: boolean;
  blowLevel: number;
  expression: number;
  filterCutoff: number;
}) {
  const canvasRef    = useRef<HTMLCanvasElement>(null);
  const stripRef     = useRef<HTMLCanvasElement>(null);
  const histRef      = useRef<ImageData | null>(null);
  const stripHistRef = useRef<ImageData | null>(null);
  const prevNotes    = useRef<Set<number>>(new Set());

  useEffect(() => {
    const canvas = canvasRef.current;
    const strip  = stripRef.current;
    if (!canvas || !strip || specSamples.length === 0) return;
    const ctx  = canvas.getContext('2d');
    const sctx = strip.getContext('2d');
    if (!ctx || !sctx) return;

    const N    = specSamples.length;
    const mags = spectrumDB(specSamples);

    // ── Waterfall ──────────────────────────────────────────────────────────
    if (!histRef.current) histRef.current = ctx.createImageData(SPEC_CANVAS_W, SPEC_CANVAS_H);
    const hist = histRef.current;
    const d    = hist.data;

    for (let y = 0; y < SPEC_CANVAS_H; y++) {
      const row = y * SPEC_CANVAS_W * 4;
      d.copyWithin(row, row + 4, row + SPEC_CANVAS_W * 4);
    }
    
    const currNotes  = new Set(activeNotes);
    const hasNewNote = [...currNotes].some(n => !prevNotes.current.has(n));
    prevNotes.current = currNotes;

    const noteRows = new Set<number>();
    for (const n of activeNotes) {
      const nf  = specMidiFreq(n);
      const yHi = specFreqToRow(nf * Math.pow(2,  1 / 24));
      const yLo = specFreqToRow(nf * Math.pow(2, -1 / 24));
      for (let y = yHi; y <= yLo; y++) noteRows.add(y);
    }

    for (let y = 0; y < SPEC_CANVAS_H; y++) {
      const freq = specRowToFreq(y);
      const bin  = Math.round(freq / (SPEC_SAMPLE_RATE / N));
      const db   = mags[Math.min(bin, mags.length - 1)];
      let [r, g, b] = dbToRgb(db);
      if (hasNewNote) {
        r = 255; g = 255; b = Math.min(255, b + 80);
      } else if (noteRows.has(y)) {
        r = Math.min(255, r + 20);
        g = Math.min(255, g + 160);
        b = Math.min(255, b + 20);
      }
      const px = (y * SPEC_CANVAS_W + (SPEC_CANVAS_W - 1)) * 4;
      d[px] = r; d[px + 1] = g; d[px + 2] = b; d[px + 3] = 255;
    }

    ctx.putImageData(hist, 0, 0);

    // Frequency grid lines + Hz labels (log scale)
    ctx.font = '9px monospace'; ctx.textAlign = 'right';
    [100, 200, 440, 880, 2000, 4000].forEach(f => {
      const y = specFreqToRow(f);
      ctx.fillStyle = 'rgba(255,255,255,0.07)';
      ctx.fillRect(0, y, SPEC_CANVAS_W, 1);
      ctx.fillStyle = 'rgba(0,0,0,0.6)';
      ctx.fillRect(0, y - 5, 40, 12);
      ctx.fillStyle = '#777';
      ctx.fillText(f >= 1000 ? `${f / 1000}k` : `${f}`, 39, y + 4);
    });

    // Active note labels at right edge
    ctx.textAlign = 'left'; ctx.font = 'bold 9px monospace';
    for (const n of activeNotes) {
      const y = specFreqToRow(specMidiFreq(n));
      ctx.fillStyle = 'rgba(0,0,0,0.65)';
      ctx.fillRect(SPEC_CANVAS_W - 30, y - 6, 30, 10);
      ctx.fillStyle = '#4ade80';
      ctx.fillText(midiNoteName(n), SPEC_CANVAS_W - 29, y + 3);
    }

    // Blow glow
    if (blowActive) {
      ctx.fillStyle = 'rgba(100,200,255,0.35)';
      ctx.fillRect(SPEC_CANVAS_W - 3, 0, 3, SPEC_CANVAS_H);
    }

    // ── Controller strip ──────────────────────────────────────────────────
    if (!stripHistRef.current) stripHistRef.current = sctx.createImageData(SPEC_CANVAS_W, STRIP_H);
    const sh = stripHistRef.current;
    const sd = sh.data;

    for (let y = 0; y < STRIP_H; y++) {
      const row = y * SPEC_CANVAS_W * 4;
      sd.copyWithin(row, row + 4, row + SPEC_CANVAS_W * 4);
    }

    const LANE_H = Math.floor(STRIP_H / 3);
    const lanes = [
      { value: expression,               max: 1, r: 80,  g: 210, b: 80  }, // expr — green
      { value: filterCutoff,             max: 1, r: 80,  g: 140, b: 220 }, // cut  — blue
      { value: Math.min(blowLevel, 2),   max: 2, r: blowActive ? 255 : 80, g: 220, b: blowActive ? 80 : 220 }, // blow
    ];

    for (let lane = 0; lane < lanes.length; lane++) {
      const { value, max, r, g, b } = lanes[lane];
      const top = lane * LANE_H;
      // dark bg for this lane's rightmost column
      for (let y = top; y < top + LANE_H; y++) {
        const px = (y * SPEC_CANVAS_W + (SPEC_CANVAS_W - 1)) * 4;
        sd[px] = 15; sd[px + 1] = 15; sd[px + 2] = 20; sd[px + 3] = 255;
      }
      // value dot (3 px tall)
      const dotY = Math.round(top + (LANE_H - 1) * (1 - Math.min(1, value / max)));
      for (let dy = -1; dy <= 1; dy++) {
        const y = Math.max(top, Math.min(top + LANE_H - 1, dotY + dy));
        const px = (y * SPEC_CANVAS_W + (SPEC_CANVAS_W - 1)) * 4;
        sd[px] = r; sd[px + 1] = g; sd[px + 2] = b; sd[px + 3] = 255;
      }
    }

    sctx.putImageData(sh, 0, 0);

    // Lane labels
    sctx.font = '9px monospace'; sctx.textAlign = 'right';
    (['expr', 'cut', 'blow'] as const).forEach((label, i) => {
      sctx.fillStyle = 'rgba(0,0,0,0.6)';
      sctx.fillRect(0, i * LANE_H, 34, LANE_H - 1);
      sctx.fillStyle = i === 0 ? '#4ade80' : i === 1 ? '#60a5fa' : '#22d3ee';
      sctx.fillText(label, 33, i * LANE_H + LANE_H - 4);
    });

  }, [specSamples, activeNotes, blowActive, blowLevel, expression, filterCutoff]);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '2px' }}>
      <canvas
        ref={canvasRef}
        width={SPEC_CANVAS_W}
        height={SPEC_CANVAS_H}
        style={{ width: '100%', height: `${SPEC_CANVAS_H}px`, display: 'block', background: '#000', borderRadius: '4px 4px 0 0' }}
      />
      <canvas
        ref={stripRef}
        width={SPEC_CANVAS_W}
        height={STRIP_H}
        style={{ width: '100%', height: `${STRIP_H}px`, display: 'block', background: '#0f0f14', borderRadius: '0 0 4px 4px' }}
      />
    </div>
  );
}

function BlowMeter({ label, level, active }: { label: string; level: number; active: boolean }) {
  const pct = Math.min(100, Math.round(level * 50)); // level 2.0 → 100%
  return (
    <div className="meter">
      <div className="meter-head">
        <span>{label}</span>
        <strong style={{ color: active ? '#4ade80' : '#eee' }}>{active ? '● BLOWING' : `${pct}%`}</strong>
      </div>
      <div className="meter-track" style={{ position: 'relative' }}>
        <div className="meter-fill" style={{ width: `${pct}%`, background: active ? '#4ade80' : pct >= 50 ? '#facc15' : undefined }} />
        {/* threshold line at 50% */}
        <div style={{
          position: 'absolute', top: 0, bottom: 0, left: '50%',
          width: '2px', background: '#f87171', opacity: 0.8,
          transform: 'translateX(-50%)',
        }} title="trigger threshold" />
      </div>
      <div style={{ fontSize: '10px', color: '#555', marginTop: '2px' }}>
        red line = trigger threshold · fill must pass it to start a note
      </div>
    </div>
  );
}

function BipolarMeter({ label, value }: { label: string; value: number }) {
  const clamped = Math.max(-1, Math.min(1, value));
  const left = clamped < 0 ? 50 + clamped * 50 : 50;
  const width = Math.abs(clamped) * 50;

  return (
    <div className="meter">
      <div className="meter-head">
        <span>{label}</span>
        <strong>{formatNumber(value)}</strong>
      </div>
      <div className="meter-track meter-bipolar">
        <div className="meter-center" />
        <div
          className="meter-fill"
          style={{
            left: `${left}%`,
            width: `${width}%`
          }}
        />
      </div>
    </div>
  );
}

function Vec3Readout({ label, value }: { label: string; value: Vec3 }) {
  return (
    <div className="vec3">
      <span>{label}</span>
      <code>
        x {formatNumber(value.x)} / y {formatNumber(value.y)} / z{" "}
        {formatNumber(value.z)}
      </code>
    </div>
  );
}

export function App() {
  const [connection, setConnection] = useState<ConnectionState>("connecting");
  const [socket, setSocket] = useState<WebSocket | null>(null);
  const [runtime, setRuntime] = useState<RuntimeState | null>(null);
  const runtimeRef = useRef<RuntimeState | null>(null);

  useEffect(() => {
    let ws: WebSocket;
    let retryDelay = 1000;
    let stopped = false;
    let retryTimer: ReturnType<typeof setTimeout> | null = null;

    function connect() {
      if (stopped) return;
      setConnection("connecting");
      ws = new WebSocket(websocketUrl);

      ws.addEventListener("open", () => {
        retryDelay = 1000;
        setConnection("online");
        setSocket(ws);
      });

      ws.addEventListener("close", () => {
        setConnection("offline");
        setSocket(null);
        if (!stopped) {
          retryTimer = setTimeout(() => { retryDelay = Math.min(retryDelay * 1.5, 8000); connect(); }, retryDelay);
        }
      });

      ws.addEventListener("error", () => {
        setConnection("offline");
        setSocket(null);
        // close event fires after error, reconnect handled there
      });

      ws.addEventListener("message", (event: MessageEvent<string>) => {
        const parsed = JSON.parse(event.data) as RuntimeState;
        if (parsed.type === "state") {
          runtimeRef.current = parsed;
          setRuntime(parsed);
        }
      });
    }

    connect();

    return () => {
      stopped = true;
      if (retryTimer !== null) clearTimeout(retryTimer);
      ws?.close();
    };
  }, []);

  const activeNoteText = useMemo(() => {
    const notes = runtime?.music.activeNotes ?? [];

    if (notes.length === 0) {
      return "none";
    }

    return notes.map((note) => `${midiNoteName(note)} / ${note}`).join(", ");
  }, [runtime]);

  function panic() {
    socket?.send(JSON.stringify({ type: "panic" }));
  }

  function selectCaptureDevice(value: string) {
    socket?.send(JSON.stringify({
      type: "setCaptureDevice",
      deviceIndex: value === "default" ? null : Number(value)
    }));
  }

  function setSampleTrim(next: { start?: number; end?: number }) {
    const start = next.start ?? audio?.sampleTrimStart ?? 0;
    const end = next.end ?? audio?.sampleTrimEnd ?? 1;

    socket?.send(JSON.stringify({
      type: "setSampleTrim",
      start,
      end
    }));
  }

  function setActiveBank(bank: number) {
    socket?.send(JSON.stringify({ type: "setActiveBank", bank }));
  }

  function saveSample(bank: number) {
    socket?.send(JSON.stringify({ type: "saveSample", bank }));
  }

  function setPatch(bank: number, program: number) {
    socket?.send(JSON.stringify({ type: "setPatch", bank, program }));
  }

  function setSoundfont(path: string) {
    socket?.send(JSON.stringify({ type: "setSoundfont", path }));
  }

  const sendWavetable = useCallback((samples: number[], morphSamples: number[]) => {
    socket?.send(JSON.stringify({ type: "setWavetable", data: samples, morphData: morphSamples }));
  }, [socket]);

  const setWavetableControls = useCallback((next: { morph?: number; noise?: number; unison?: number }) => {
    const current = runtimeRef.current?.audio;
    socket?.send(JSON.stringify({
      type: "setWavetableControls",
      morph: next.morph ?? current?.wavetableMorph ?? 0,
      noise: next.noise ?? current?.wavetableNoise ?? 0,
      unison: next.unison ?? current?.wavetableUnison ?? 0,
    }));
  }, [socket]);

  function seqPlay() { socket?.send(JSON.stringify({ type: "seqPlay" })); }
  function seqStop() { socket?.send(JSON.stringify({ type: "seqStop" })); }
  function seqSelectStep(step: number) { socket?.send(JSON.stringify({ type: "selectSeqStep", step })); }
  function seqSelectTrack(track: number) { socket?.send(JSON.stringify({ type: "selectSeqTrack", track })); }
  function seqAddTrack() { socket?.send(JSON.stringify({ type: "seqAddTrack" })); }
  function seqRemoveTrack(track: number) { socket?.send(JSON.stringify({ type: "seqRemoveTrack", track })); }
  function seqChange(cfg: { bpm: number; gatePct: number; stepCount: number; stepDivision: number; tracks: SeqTrackEdit[] }) {
    socket?.send(JSON.stringify({ type: "setSeq", ...cfg }));
  }

  function setBlowMode(enabled: boolean) {
    socket?.send(JSON.stringify({ type: "setBlowMode", enabled }));
  }

  function setBlowSensitivity(sensitivity: number) {
    socket?.send(JSON.stringify({ type: "setBlowSensitivity", sensitivity: Math.round(sensitivity * 100) }));
  }

  function setVoiceSeq(next: {
    enabled?: boolean;
    recording?: boolean;
    mode?: "percussion" | "harmonic" | "hybrid";
    snapToScale?: boolean;
    sensitivity?: number;
    timingOffsetMs?: number;
  }) {
    socket?.send(JSON.stringify({
      type: "setVoiceSeq",
      enabled: next.enabled ?? audio?.voiceSeqEnabled ?? false,
      recording: next.recording ?? audio?.voiceSeqRecording ?? false,
      mode: next.mode ?? audio?.voiceSeqMode ?? "percussion",
      snapToScale: next.snapToScale ?? audio?.voiceSeqSnap ?? true,
      sensitivity: Math.round((next.sensitivity ?? audio?.voiceSeqSensitivity ?? 0.65) * 100),
      timingOffsetMs: next.timingOffsetMs ?? audio?.voiceSeqTimingOffsetMs ?? 0,
    }));
  }

  const [configOpen, setConfigOpen] = useState(false);

  const controller = runtime?.controller;
  const music = runtime?.music;
  const audio = runtime?.audio;
  const backendOnline = connection === "online" && runtime != null;
  const sampleMode = audio?.sampleReady ?? false;
  const activePatch = music?.midiProgram ?? 0;
  const activeBank = music?.midiBank ?? 0;
  const activeBankHasMicSample = (() => { const b = audio?.banks[audio?.activeBank ?? 0]; return !!(b?.hasData && !b?.isWavetable); })();

  return (
    <main className="app">
      <header className="hero">
        <div>
          <p className="eyebrow">DualSense MIDI workstation</p>
          <h1>Analogno</h1>
        </div>

        <div className="hero-actions">
          <StatusPill state={connection} />
          <button className="config-button" onClick={() => setConfigOpen(true)} type="button">
            Config
          </button>
          <button className="panic-button" onClick={panic} type="button">
            Panic
          </button>
        </div>
      </header>

      <ConfigModal open={configOpen} onClose={() => setConfigOpen(false)}>
        <section className="config-section">
          <h3>Controller</h3>
          <BipolarMeter label="Left X / pitch bend" value={controller?.leftX ?? 0} />
          <BipolarMeter label="Left Y" value={controller?.leftY ?? 0} />
          <BipolarMeter label="Right X / resonance" value={controller?.rightX ?? 0} />
          <BipolarMeter label="Right Y / cutoff" value={controller?.rightY ?? 0} />
          <Meter label={activeBankHasMicSample ? "L2 / sample gain" : "L2 / expression"} value={controller?.l2 ?? 0} />
          <Meter label="R2 / vibrato depth" value={controller?.r2 ?? 0} />
        </section>
        <section className="config-section">
          <h3>Audio input</h3>
          <StateLine label="Capture" value={audio?.captureRunning ? "running" : "stopped"} />
          <StateLine label="Gate" value={audio?.gateOpen ? "open" : "closed"} />
          <StateLine label="Velocity" value={audio?.velocity ?? 0} />
          <label className="field">
            <span>Input device</span>
            <select
              value={audio?.selectedDeviceIndex == null ? "default" : String(audio.selectedDeviceIndex)}
              onChange={(event) => selectCaptureDevice(event.target.value)}
              disabled={connection !== "online"}
            >
              <option value="default">System default</option>
              {(audio?.devices ?? []).map((device) => (
                <option key={device.index} value={device.index}>
                  {device.name}{device.isDefault ? " (default)" : ""}
                </option>
              ))}
            </select>
          </label>
          <Meter label="Mic level" value={audio?.micLevel ?? 0} />
          <Meter label="Envelope" value={(audio?.envelope ?? 0) * 8} />
          <Waveform samples={audio?.waveform ?? []} />
        </section>
        <section className="config-section">
          <h3>Motion</h3>
          <StateLine label="Gyro" value={controller?.hasGyro ? "available" : "missing"} />
          <StateLine label="Accelerometer" value={controller?.hasAccel ? "available" : "missing"} />
          <Vec3Readout label="gyro" value={controller?.gyro ?? { x: 0, y: 0, z: 0 }} />
          <Vec3Readout label="accel" value={controller?.accel ?? { x: 0, y: 0, z: 0 }} />
        </section>
        <section className="config-section">
          <h3>Music</h3>
          <StateLine label="Root" value={music ? `${midiNoteName(music.rootMidiNote)} / ${music.rootMidiNote}` : "—"} />
          <StateLine label="Scale" value={music?.scale ?? "—"} />
          <StateLine label="Octave offset" value={music?.octaveOffset ?? "—"} />
          <StateLine label="Active notes" value={activeNoteText} />
          <BipolarMeter label="Pitch bend" value={music?.pitchBend ?? 0} />
          <Meter label={sampleMode ? "Sample gain" : "Expression"} value={music?.expression ?? 0} />
          <Meter label="Filter cutoff" value={music?.filterCutoff ?? 0} />
          <Meter label="Resonance" value={music?.filterResonance ?? 0} />
          <Meter label="Vibrato" value={music?.vibrato ?? 0} />
        </section>
      </ConfigModal>

      <div className="layout">

        {(runtime?.pianoRollVisible ?? true) && (
        <Panel title="Notes" wide>
          <PianoRoll
            activeNotes={music?.activeNotes ?? []}
            buttonMidiNotes={music?.buttonMidiNotes ?? []}
            rootMidiNote={music?.rootMidiNote ?? 48}
          />
          <div style={{display:"flex",gap:"16px",marginTop:"6px",fontSize:"11px",color:"#aaa",flexWrap:"wrap"}}>
            <span>Root: <strong style={{color:"#eee"}}>{music ? midiNoteName(music.rootMidiNote) : "—"}</strong></span>
            <span>Scale: <strong style={{color:"#eee"}}>{music?.scale ?? "—"}</strong></span>
            <span>Oct: <strong style={{color:"#eee"}}>{music?.octaveOffset ?? 0}</strong></span>
            {(music?.activeNotes?.length ?? 0) > 0 && (
              <span>Playing: <strong style={{color:"#4a9eff"}}>{(music?.activeNotes ?? []).map(n => midiNoteName(n)).join(", ")}</strong></span>
            )}
          </div>
        </Panel>
        )}

        <Panel title="Synth patches" wide>
          {(() => {
            const presets = runtime?.presets ?? [];
            const hasSf2 = presets.length > 0;
            const soundfonts = runtime?.soundfonts ?? [];
            const activeSoundfont = runtime?.activeSoundfont ?? '';

            // Group presets by bank, sorted by bank number.
            const bankMap = new Map<number, Array<{ program: number; name: string }>>();
            for (const p of presets) {
              const list = bankMap.get(p.bank) ?? [];
              list.push({ program: p.program, name: p.name });
              bankMap.set(p.bank, list);
            }
            const banks = Array.from(bankMap.entries()).sort((a, b) => a[0] - b[0]);
            for (const [, list] of bankMap) list.sort((a, b) => a.program - b.program);

            // Which bank tab is being browsed — sync once with active bank.
            const visibleBank = hasSf2
              ? (bankMap.has(activeBank) ? activeBank : (banks[0]?.[0] ?? 0))
              : 0;
            const visiblePresets = hasSf2
              ? (bankMap.get(visibleBank) ?? [])
              : GM_PROGRAMS.map((name, i) => ({ program: i, name }));

            return (
              <>
                {soundfonts.length > 0 && (
                  <div className="sf2-selector">
                    <label htmlFor="sf2-select">Soundfont</label>
                    <select
                      id="sf2-select"
                      value={activeSoundfont}
                      disabled={connection !== 'online'}
                      onChange={(e) => setSoundfont(e.target.value)}>
                      {soundfonts.map(path => (
                        <option key={path} value={path}>
                          {soundfontName(path)} · {soundfontFormat(path)}
                        </option>
                      ))}
                    </select>
                    {activeSoundfont && (
                      <span className="sf2-summary">
                        <span className="sf2-summary-format">{soundfontFormat(activeSoundfont)}</span>
                        {hasSf2 && <span className="sf2-summary-count">{pluralize(presets.length, 'preset')}</span>}
                      </span>
                    )}
                  </div>
                )}
                <div className="patch-header">
                  {hasSf2 ? (
                    <div className="patch-bank-tabs">
                      {banks.map(([bank, bankPresets]) => (
                        <button key={bank} type="button"
                          className={`patch-bank-tab${bank === visibleBank ? ' patch-bank-tab-active' : ''}`}
                          disabled={connection !== 'online' || sampleMode}
                          onClick={() => setPatch(bank, activePatch)}>
                          <span className="patch-bank-tab-top">
                            <span className="patch-bank-id">{bankLabel(bank)}</span>
                            <span className="patch-bank-count">{bankPresets.length}</span>
                          </span>
                          <span className="patch-bank-meta">{bankRole(bank)} presets</span>
                        </button>
                      ))}
                    </div>
                  ) : (
                    <label className="field patch-bank-field">
                      <span>MIDI bank <em className="patch-bank-hint">(synth-specific, 0 = GM)</em></span>
                      <input type="number" className="patch-bank-input"
                        min={0} max={127} value={activeBank}
                        disabled={connection !== 'online' || sampleMode}
                        onChange={(e) => {
                          const v = Math.max(0, Math.min(127, Number(e.target.value)));
                          if (!Number.isNaN(v)) setPatch(v, activePatch);
                        }} />
                    </label>
                  )}
                  <p className="patch-active-name">
                    {sampleMode
                      ? 'Sampler mode — MIDI synth inactive'
                      : (() => {
                          const hit = presets.find(p => p.bank === activeBank && p.program === activePatch);
                          return hit
                            ? `${bankLabel(hit.bank)} · ${hit.program + 1}. ${hit.name}`
                            : `${activePatch + 1}. ${patchName(activeBank, activePatch, presets)}`;
                        })()}
                  </p>
                </div>
                <div className="patch-grid">
                  {visiblePresets.map(({ program, name }) => (
                    <button key={program} type="button"
                      className={`patch-btn${
                        program === activePatch && visibleBank === activeBank && !sampleMode ? ' patch-active' : ''
                      }`}
                      disabled={connection !== 'online' || sampleMode}
                      onClick={() => setPatch(visibleBank, program)}
                      title={name}>
                      <span className="patch-num">{program + 1}</span>
                      <span className="patch-name">{name}</span>
                    </button>
                  ))}
                </div>
              </>
            );
          })()}
        </Panel>

        <Panel title="Sampler" wide>
          <div className="sampler-layout">
            <div className="sampler-banks">
              <p className="sampler-hint">Touchpad → next bank · Options → prev bank · L1+Touchpad → record · Guide → clear · Drag (no click) → draw waveform</p>
              <div className="bank-grid">
                {Array.from({ length: 8 }, (_, i) => {
                  const bank = audio?.banks[i];
                  const isActive = (audio?.activeBank ?? 0) === i;
                  const hasData = bank?.hasData ?? false;
                  const trimmedSecs = hasData
                    ? (((bank?.trimEnd ?? 1) - (bank?.trimStart ?? 0)) * (bank?.frames ?? 0) / 48000).toFixed(2)
                    : null;
                  return (
                    <div
                      key={i}
                      className={`bank-slot${isActive ? " bank-active" : ""}${hasData ? " bank-filled" : ""}`}
                      role="button"
                      tabIndex={0}
                      onClick={() => { if (connection === "online") setActiveBank(i); }}
                      onKeyDown={(e) => { if (e.key === "Enter" && connection === "online") setActiveBank(i); }}
                    >
                      <span className="bank-num">B{i + 1}</span>
                      <span className="bank-dur">{trimmedSecs != null ? `${trimmedSecs}s` : "—"}</span>
                      {hasData && (
                        <button
                          className="bank-save-btn"
                          type="button"
                          onClick={(e) => { e.stopPropagation(); saveSample(i); }}
                          disabled={connection !== "online"}
                        >
                          Save
                        </button>
                      )}
                    </div>
                  );
                })}
              </div>
            </div>

            <div className="sampler-trim">
              <StateLine label="Mode" value={sampleMode ? "sampler" : "MIDI"} />
              <StateLine label="Recording" value={audio?.sampleRecording ? "armed" : "idle"} />
              <StateLine
                label="Active bank"
                value={audio?.sampleReady
                  ? `${((audio.sampleTrimEnd - audio.sampleTrimStart) * audio.sampleFrames / 48000).toFixed(2)}s`
                  : "empty"}
              />
              <div className="wavetable-macros">
                {!activeBankHasMicSample && (
                  <WavetableMacroSlider label="Morph" tone="morph"
                    value={audio?.wavetableMorph ?? 0}
                    disabled={connection !== "online"}
                    onChange={(value) => setWavetableControls({ morph: value })} />
                )}
                <WavetableMacroSlider label="Noise" tone="noise"
                  value={audio?.wavetableNoise ?? 0}
                  disabled={connection !== "online"}
                  onChange={(value) => setWavetableControls({ noise: value })} />
                <WavetableMacroSlider label="Unison" tone="unison"
                  value={audio?.wavetableUnison ?? 0}
                  disabled={connection !== "online"}
                  onChange={(value) => setWavetableControls({ unison: value })} />
              </div>
              {audio?.touchpadDrawing && (
                <div className="touchpad-sketch">
                  <span className="touchpad-sketch-label">Drawing…</span>
                  <TouchpadWhiteboard
                    points={audio.touchpadRawPoints ?? []}
                    drawing={audio.touchpadDrawing}
                  />
                </div>
              )}
              {!activeBankHasMicSample && (
                <div className="touchpad-sketch">
                  <span className="touchpad-sketch-label">
                    Waveform editor · touchpad draws · presets below
                  </span>
                  <WaveformEditor
                    touchpadSketch={audio?.touchpadSketch ?? []}
                    touchpadDrawing={audio?.touchpadDrawing ?? false}
                    morphAmount={audio?.wavetableMorph ?? 0}
                    noiseAmount={audio?.wavetableNoise ?? 0}
                    unisonAmount={audio?.wavetableUnison ?? 0}
                    onApply={sendWavetable}
                    disabled={connection !== "online"}
                  />
                </div>
              )}
              <div className="trim">
                {audio?.sampleReady && activeBankHasMicSample && (
                  <TrimWaveform
                    waveform={audio.sampleWaveform ?? []}
                    trimStart={audio.sampleTrimStart ?? 0}
                    trimEnd={audio.sampleTrimEnd ?? 1}
                    disabled={connection !== 'online'}
                    onChange={setSampleTrim}
                  />
                )}
              </div>
            </div>
          </div>
        </Panel>

        <Panel title="Sequencer" wide>
          <SequencerPanel
            seq={runtime?.seq}
            presets={runtime?.presets ?? []}
            connection={connection}
            sampleMode={sampleMode}
            sampleIsWavetable={sampleMode && !activeBankHasMicSample}
            onPlay={seqPlay}
            onStop={seqStop}
            onSelectTrack={seqSelectTrack}
            onSelectStep={seqSelectStep}
            onAddTrack={seqAddTrack}
            onRemoveTrack={seqRemoveTrack}
            onChange={seqChange}
          />
        </Panel>

        <Panel title="Voice to sequencer">
          <div className="voice-seq">
            <div className="voice-seq-actions">
              <button
                type="button"
                className={`voice-seq-primary${(audio?.voiceSeqEnabled ?? false) ? " voice-seq-enabled" : ""}`}
                disabled={!backendOnline || !(audio?.voiceSeqAvailable ?? false)}
                onClick={() => {
                  const enabled = !(audio?.voiceSeqEnabled ?? false);
                  setVoiceSeq({ enabled, recording: enabled ? audio?.voiceSeqRecording : false });
                }}
              >
                {(audio?.voiceSeqEnabled ?? false) ? "Voice ON" : "Voice OFF"}
              </button>
              <button
                type="button"
                className={`voice-seq-primary${(audio?.voiceSeqRecording ?? false) ? " voice-seq-recording" : ""}`}
                disabled={!backendOnline || !(audio?.voiceSeqAvailable ?? false)}
                onClick={() => setVoiceSeq({
                  enabled: true,
                  recording: !(audio?.voiceSeqRecording ?? false),
                })}
              >
                {(audio?.voiceSeqRecording ?? false) ? "Stop record" : "Record 1 bar"}
              </button>
              <span className="voice-seq-status">
                {(audio?.voiceSeqAvailable ?? false)
                  ? (audio?.voiceSeqLastNote ?? -1) >= 0
                    ? `${midiNoteName(audio!.voiceSeqLastNote)} vel ${audio?.voiceSeqLastVelocity ?? 0}`
                    : "ready"
                  : backendOnline
                    ? (audio?.voiceSeqCompiled ?? false)
                      ? "aubio init failed"
                      : "backend built without aubio"
                    : "backend offline"}
              </span>
            </div>

            <div className="voice-seq-grid">
              <label className="voice-seq-field">
                <span>Input type</span>
                <select
                  value={audio?.voiceSeqMode ?? "percussion"}
                  disabled={!backendOnline || !(audio?.voiceSeqAvailable ?? false)}
                  onChange={e => setVoiceSeq({ mode: e.target.value as "percussion" | "harmonic" | "hybrid" })}
                >
                  <option value="percussion">Percussion hits</option>
                  <option value="harmonic">Harmonic notes</option>
                  <option value="hybrid">Hybrid</option>
                </select>
              </label>

              <label className="voice-seq-field">
                <span>Pitch snap</span>
                <select
                  value={(audio?.voiceSeqSnap ?? true) ? "scale" : "chromatic"}
                  disabled={!backendOnline || !(audio?.voiceSeqAvailable ?? false)}
                  onChange={e => setVoiceSeq({ snapToScale: e.target.value === "scale" })}
                >
                  <option value="scale">Current scale</option>
                  <option value="chromatic">Chromatic</option>
                </select>
              </label>
            </div>

            <div className="voice-seq-slider">
              <div className="voice-seq-slider-head">
                <span>Sensitivity</span>
                <strong>{Math.round((audio?.voiceSeqSensitivity ?? 0.65) * 100)}%</strong>
              </div>
              <input
                type="range" min={0} max={100}
                value={Math.round((audio?.voiceSeqSensitivity ?? 0.65) * 100)}
                disabled={!backendOnline || !(audio?.voiceSeqAvailable ?? false)}
                onChange={e => setVoiceSeq({ sensitivity: Number(e.target.value) / 100 })}
                style={{ width: "100%" }}
              />
            </div>

            <div className="voice-seq-slider">
              <div className="voice-seq-slider-head">
                <span>Timing offset</span>
                <strong>{Math.round(audio?.voiceSeqTimingOffsetMs ?? 0)} ms</strong>
              </div>
              <input
                type="range" min={-120} max={120}
                value={Math.round(audio?.voiceSeqTimingOffsetMs ?? 0)}
                disabled={!backendOnline || !(audio?.voiceSeqAvailable ?? false)}
                onChange={e => setVoiceSeq({ timingOffsetMs: Number(e.target.value) })}
                style={{ width: "100%" }}
              />
            </div>

            <div className="voice-seq-stats">
              <span>accepted {audio?.voiceSeqAcceptedNotes ?? 0}</span>
              <span>rejected {audio?.voiceSeqRejectedNotes ?? 0}</span>
              <span>segments {audio?.voiceSeqRecordedSegments ?? 0}</span>
              {(audio?.voiceSeqRecording ?? false) && (
                <span>{Math.round((audio?.voiceSeqRecordProgress ?? 0) * 100)}%</span>
              )}
              <span>{(audio?.voiceSeqCompiled ?? false) ? "compiled aubio" : "no aubio in binary"}</span>
              <span>armed step {((runtime?.seq.selectedStep ?? -1) >= 0) ? (runtime!.seq.selectedStep + 1) : "none"}</span>
            </div>
          </div>
        </Panel>

        <Panel title="Breath / wind controller">
          <div style={{ display: "flex", flexDirection: "column", gap: "12px" }}>
            {/* <p style={{ margin: 0, fontSize: "12px", color: "#888", lineHeight: 1.5 }}>
              <strong style={{ color: '#4a9eff' }}>Tip for DualSense:</strong> select the
              <em> DualSense Wireless Controller</em> mic in <strong>Config → Audio input → Input device</strong>
              and blow directly into the small hole on the front face of the controller — that's the mic.
              Or use any room mic and point it close to your mouth.
            </p> */}
            <div style={{ display: "flex", alignItems: "center", gap: "12px", flexWrap: 'wrap' }}>
              <button
                type="button"
                className={`seq-playstop${(audio?.blowMode ?? false) ? " seq-playing" : ""}`}
                disabled={connection !== "online"}
                onClick={() => setBlowMode(!(audio?.blowMode ?? false))}
              >
                {(audio?.blowMode ?? false) ? "\uD83D\uDCA8 Blow ON" : "\uD83D\uDCA8 Blow OFF"}
              </button>
              {(audio?.blowMode ?? false) && (
                <span style={{ fontSize: "12px", color: '#aaa' }}>
                  {(audio?.blowActive) ? '🎵 note held' : 'waiting for blow…'}
                </span>
              )}
              {sampleMode && (
                <span style={{ fontSize: "11px", color: "#e74c3c" }}>⚠ sampler active — disable it to use blow</span>
              )}
            </div>
            <BlowMeter
              label="Breath level (fill must cross red line to trigger)"
              level={audio?.blowLevel ?? 0}
              active={audio?.blowActive ?? false}
            />
            <div style={{ display: "flex", flexDirection: "column", gap: "4px" }}>
              <div style={{ display: "flex", justifyContent: "space-between", fontSize: "12px", color: "#aaa" }}>
                <span>Sensitivity <em style={{color:'#555', fontSize:'10px'}}>(100% = easiest to trigger)</em></span>
                <span style={{ color: "#eee" }}>{Math.round((audio?.blowSensitivity ?? 0.5) * 100)}%</span>
              </div>
              <input
                type="range" min={0} max={100}
                value={Math.round((audio?.blowSensitivity ?? 0.5) * 100)}
                disabled={connection !== "online"}
                onChange={e => setBlowSensitivity(Number(e.target.value) / 100)}
                style={{ width: "100%" }}
              />
              <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: '10px', color: '#444' }}>
                <span>0% — hard blow only</span>
                <span>100% — very soft breath</span>
              </div>
            </div>
          </div>
        </Panel>

        {(runtime?.spectrogramVisible ?? true) && (
        <Panel
          title="Spectrogram"
          wide
        >
          <Spectrogram
            specSamples={audio?.specSamples ?? []}
            activeNotes={music?.activeNotes ?? []}
            blowActive={audio?.blowActive ?? false}
            blowLevel={audio?.blowLevel ?? 0}
            expression={music?.expression ?? 0}
            filterCutoff={music?.filterCutoff ?? 0}
          />
        </Panel>
        )}
      </div>
    </main>
  );
}
