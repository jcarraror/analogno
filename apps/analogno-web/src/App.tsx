import { useEffect, useMemo, useRef, useState } from "react";

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
  };
  seq: {
    playing: boolean;
    activeTrack: number;
    selectedStep: number;
    bpm: number;
    currentStep: number;
    gatePct: number;
    tracks: Array<{
      midiChannel: number;
      midiProgram: number;
      midiBank: number;
      muted: boolean;
      steps: Array<{ active: boolean; degree: number; velocity: number; midiNote: number }>;
    }>;
  };
  presets: Array<{ bank: number; program: number; name: string }>;
  soundfonts: string[];
  activeSoundfont: string;
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

function Panel({
  title,
  children,
  wide,
}: {
  title: string;
  children: React.ReactNode;
  wide?: boolean;
}) {
  return (
    <section className={`panel${wide ? " panel-wide" : ""}`}>
      <h2>{title}</h2>
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
};

const CW = 512, CH = 130, HIT_R = 7;

function cpXY(cp: CP): [number, number] {
  return [cp.x * CW, (0.5 - cp.amp * 0.45) * CH];
}
function xyCP(cx: number, cy: number): CP {
  return { x: Math.max(0, Math.min(1, cx / CW)), amp: Math.max(-1, Math.min(1, (0.5 - cy / CH) / 0.45)) };
}

function WaveformEditor({
  touchpadSketch,
  touchpadDrawing,
  onApply,
  disabled,
}: {
  touchpadSketch: number[];
  touchpadDrawing: boolean;
  onApply: (samples: number[]) => void;
  disabled?: boolean;
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const ptsRef = useRef<CP[]>([...PRESET_POINTS.sine]);
  const dragIdx = useRef<number | null>(null);
  const [ver, setVer] = useState(0);
  const sketchRef = useRef<number[]>(touchpadSketch);
  const drawingRef = useRef(touchpadDrawing);

  function redraw() {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d')!;
    ctx.clearRect(0, 0, CW, CH);
    ctx.strokeStyle = 'rgba(255,255,255,0.15)';
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(0, CH / 2); ctx.lineTo(CW, CH / 2); ctx.stroke();

    const sk = sketchRef.current;
    if (sk.length >= 2) {
      ctx.strokeStyle = drawingRef.current ? 'rgba(255,190,50,0.75)' : 'rgba(255,190,50,0.28)';
      ctx.lineWidth = 1;
      ctx.beginPath();
      for (let i = 0; i < sk.length; i++) {
        const x = (i / (sk.length - 1)) * CW;
        const y = (0.5 - sk[i] * 0.45) * CH;
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }
    // CP curve on top
    const sorted = [...ptsRef.current].sort((a, b) => a.x - b.x);
    if (sorted.length >= 2) {
      const samps = cpToSamples(sorted, CW);
      ctx.strokeStyle = '#6ea8fe'; ctx.lineWidth = 1.5;
      ctx.beginPath();
      for (let i = 0; i < CW; i++) {
        const y = (0.5 - samps[i] * 0.45) * CH;
        if (i === 0) ctx.moveTo(0, y); else ctx.lineTo(i, y);
      }
      ctx.stroke();
    }
    for (const cp of ptsRef.current) {
      const [cx, cy] = cpXY(cp);
      ctx.beginPath(); ctx.arc(cx, cy, HIT_R - 1, 0, Math.PI * 2);
      ctx.fillStyle = '#93bbff'; ctx.fill();
      ctx.strokeStyle = '#1e3a6e'; ctx.lineWidth = 1.5; ctx.stroke();
    }
  }

  // Redraws when ver bumps (structural CP changes like add/delete/preset).
  useEffect(() => { redraw(); }, [ver]); // eslint-disable-line react-hooks/exhaustive-deps

  // On finger lift: auto-load sketch as control points, then redraw.
  // While drawing: just update refs and redraw the amber overlay.
  useEffect(() => {
    const wasDrawing = drawingRef.current;
    sketchRef.current = touchpadSketch;
    drawingRef.current = touchpadDrawing;
    if (wasDrawing && !touchpadDrawing && touchpadSketch.length >= 2) {
      const n = 32;
      ptsRef.current = Array.from({ length: n }, (_, i) => {
        const src = Math.round(i * (touchpadSketch.length - 1) / (n - 1));
        return { x: i / (n - 1), amp: touchpadSketch[src] };
      });
      setVer(v => v + 1); // bump triggers redraw via the other useEffect
    } else {
      redraw();
    }
  }, [touchpadSketch, touchpadDrawing]); // eslint-disable-line react-hooks/exhaustive-deps

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
      setVer(v => v + 1);
    }
  };
  const onPointerMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (dragIdx.current === null) return;
    const [cx, cy] = evXY(e);
    const pts = [...ptsRef.current];
    pts[dragIdx.current] = xyCP(cx, cy);
    ptsRef.current = pts;
    redraw(); // immediate feedback during drag, no re-render needed
  };
  const onPointerUp = () => { dragIdx.current = null; };
  const onDoubleClick = (e: React.MouseEvent<HTMLCanvasElement>) => {
    if (disabled || ptsRef.current.length <= 2) return;
    const [cx, cy] = evXY(e);
    const h = hitTest(cx, cy);
    if (h >= 0) { ptsRef.current = ptsRef.current.filter((_, i) => i !== h); setVer(v => v + 1); }
  };

  function bump(pts: CP[]) { ptsRef.current = pts; setVer(v => v + 1); }

  return (
    <div className="waveform-editor">
      <div className="waveform-editor-presets">
        {(Object.keys(PRESET_POINTS) as (keyof typeof PRESET_POINTS)[]).map(name => (
          <button key={name} type="button" className="preset-btn" disabled={disabled}
            onClick={() => bump([...PRESET_POINTS[name]])}>{name}</button>
        ))}

      </div>
      <canvas ref={canvasRef} width={CW} height={CH} className="waveform-editor-canvas"
        onPointerDown={onPointerDown} onPointerMove={onPointerMove}
        onPointerUp={onPointerUp} onDoubleClick={onDoubleClick}
        style={{ cursor: disabled ? 'default' : 'crosshair', touchAction: 'none' }}
      />
      <p className="waveform-editor-hint">Click → add point · drag → move · double-click → delete</p>
      <button type="button" className="waveform-apply-btn" disabled={disabled}
        onClick={() => onApply(cpToSamples([...ptsRef.current].sort((a, b) => a.x - b.x), EDITOR_N))}>
        Apply to bank
      </button>
    </div>
  );
}

// --- Sequencer ---

type SeqStepEdit = { active: boolean; degree: number; velocity: number; midiNote: number };
type SeqTrackEdit = { midiChannel: number; midiProgram: number; midiBank: number; muted: boolean; steps: SeqStepEdit[] };

function SequencerPanel({
  seq,
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
  connection: ConnectionState;
  sampleMode: boolean;
  sampleIsWavetable: boolean;
  onPlay: () => void;
  onStop: () => void;
  onSelectTrack: (track: number) => void;
  onSelectStep: (step: number) => void;
  onAddTrack: () => void;
  onRemoveTrack: (track: number) => void;
  onChange: (cfg: { bpm: number; gatePct: number; tracks: SeqTrackEdit[] }) => void;
}) {
  const initialized = useRef(false);
  const [bpm, setBpm] = useState(120);
  const [gate, setGate] = useState(50);
  const [tracks, setTracks] = useState<SeqTrackEdit[]>(
    Array.from({ length: 4 }, (_, ti) => ({
      midiChannel: ti, midiProgram: 0, midiBank: 0, muted: false,
      steps: Array.from({ length: 16 }, () => ({ active: false, degree: 0, velocity: 100, midiNote: -1 })),
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
    }
    setTracks(seq.tracks.map(t => ({
      midiChannel: t.midiChannel,
      midiProgram: t.midiProgram,
      midiBank: t.midiBank,
      muted: t.muted,
      steps: t.steps.map(s => ({ ...s })),
    })));
  }, [seq]);

  const disabled = connection !== 'online';
  const currentStep = seq?.currentStep ?? -1;
  const playing = seq?.playing ?? false;

  function send(t: SeqTrackEdit[], b: number, g: number) {
    onChange({ bpm: b, gatePct: g, tracks: t });
  }

  function handleStepClick(trackIdx: number, stepIdx: number) {
    if (activeTrack !== trackIdx) {
      onSelectTrack(trackIdx);
    }
    onSelectStep(activeTrack === trackIdx && selectedStep === stepIdx ? -1 : stepIdx);
  }

  function clearArmedStep() {
    if (selectedStep < 0) return;
    const next = tracks.map((t, ti) =>
      ti === activeTrack
        ? { ...t, steps: t.steps.map((s, si) => si === selectedStep ? { active: false, degree: 0, velocity: 100, midiNote: -1 } : s) }
        : t
    );
    setTracks(next);
    send(next, bpm, gate);
    onSelectStep(-1);
  }

  function handleBpm(v: number) {
    const c = Math.max(20, Math.min(300, v));
    setBpm(c); send(tracks, c, gate);
  }

  function handleGate(v: number) {
    setGate(v); send(tracks, bpm, v);
  }

  const armedStep = selectedStep >= 0 ? tracks[activeTrack]?.steps[selectedStep] : null;

  return (
    <div className="sequencer">
      <div className="seq-controls">
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
        <label className="seq-label">
          Gate&nbsp;{gate}%
          <input type="range" className="seq-gate-range" min={5} max={100}
            value={gate} disabled={disabled}
            onChange={e => handleGate(Number(e.target.value))} />
        </label>
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
        {selectedStep >= 0 && armedStep?.active && (
          <button type="button" className="seq-picker-clear" disabled={disabled}
            onClick={clearArmedStep}>
            &#x2715; Clear
          </button>
        )}
      </div>

      <div className="seq-tracks">
        {tracks.map((track, ti) => (
          <div key={ti} className={`seq-track-row${tracks[ti].muted ? ' seq-track-muted' : ''}`}>
            <div
              className={`seq-track-header${ti === activeTrack ? ' seq-track-active' : ''}`}
              role="button"
              tabIndex={0}
              onClick={() => { if (!disabled) onSelectTrack(ti); }}
              onKeyDown={e => { if (e.key === 'Enter' && !disabled) onSelectTrack(ti); }}
              title={`Track ${ti + 1} — MIDI ch ${(track.midiChannel ?? ti) + 1} — ${sampleMode ? (sampleIsWavetable ? 'Wavetable' : 'Mic sample') : (GM_PROGRAMS[track.midiProgram] ?? 'Prog ' + track.midiProgram)}`}>
              <div className="seq-track-header-top">
                <span className="seq-track-id">T{ti + 1} <small>ch{(track.midiChannel ?? ti) + 1}</small></span>
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
              <span className="seq-track-name">{sampleMode ? (sampleIsWavetable ? 'Wavetable' : 'Mic sample') : (GM_PROGRAMS[track.midiProgram] ?? `Prog ${track.midiProgram}`)}</span>
            </div>
            <div className="seq-track-steps">
              {track.steps.map((step, si) => {
                const label = step.midiNote >= 0 ? midiNoteName(step.midiNote) : '';
                const isArmed = ti === activeTrack && selectedStep === si;
                const isCurrent = currentStep === si && playing;
                return (
                  <button key={si} type="button"
                    className={[
                      'seq-step-btn',
                      step.active ? 'seq-step-on' : '',
                      isCurrent ? 'seq-step-current' : '',
                      isArmed ? 'seq-step-armed' : '',
                    ].filter(Boolean).join(' ')}
                    disabled={disabled}
                    onClick={() => handleStepClick(ti, si)}
                    title={`T${ti + 1} step ${si + 1}${step.active ? ` \u2014 ${label || 'scale'} vel ${step.velocity}` : ''}`}>
                    <span className="seq-step-num">{si + 1}</span>
                    {step.active && label && <span className="seq-step-note">{label}</span>}
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

  useEffect(() => {
    const ws = new WebSocket(websocketUrl);

    ws.addEventListener("open", () => {
      setConnection("online");
      setSocket(ws);
    });

    ws.addEventListener("close", () => {
      setConnection("offline");
      setSocket(null);
    });

    ws.addEventListener("error", () => {
      setConnection("offline");
      setSocket(null);
    });

    ws.addEventListener("message", (event: MessageEvent<string>) => {
      const parsed = JSON.parse(event.data) as RuntimeState;

      if (parsed.type === "state") {
        setRuntime(parsed);
      }
    });

    return () => {
      ws.close();
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

  function sendWavetable(samples: number[]) {
    socket?.send(JSON.stringify({ type: "setWavetable", data: samples }));
  }

  function seqPlay() { socket?.send(JSON.stringify({ type: "seqPlay" })); }
  function seqStop() { socket?.send(JSON.stringify({ type: "seqStop" })); }
  function seqSelectStep(step: number) { socket?.send(JSON.stringify({ type: "selectSeqStep", step })); }
  function seqSelectTrack(track: number) { socket?.send(JSON.stringify({ type: "selectSeqTrack", track })); }
  function seqAddTrack() { socket?.send(JSON.stringify({ type: "seqAddTrack" })); }
  function seqRemoveTrack(track: number) { socket?.send(JSON.stringify({ type: "seqRemoveTrack", track })); }
  function seqChange(cfg: { bpm: number; gatePct: number; tracks: SeqTrackEdit[] }) {
    socket?.send(JSON.stringify({ type: "setSeq", ...cfg }));
  }

  const [configOpen, setConfigOpen] = useState(false);

  const controller = runtime?.controller;
  const music = runtime?.music;
  const audio = runtime?.audio;
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
          <Meter label={sampleMode ? "L2 / sample gain" : "L2 / expression"} value={controller?.l2 ?? 0} />
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

        <Panel title="Synth patches" wide>
          {(() => {
            const presets = runtime?.presets ?? [];
            const hasSf2 = presets.length > 0;

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
                {(runtime?.soundfonts ?? []).length > 0 && (
                  <div className="sf2-selector">
                    <label htmlFor="sf2-select">Soundfont</label>
                    <select
                      id="sf2-select"
                      value={runtime?.activeSoundfont ?? ''}
                      disabled={connection !== 'online'}
                      onChange={(e) => setSoundfont(e.target.value)}>
                      {(runtime?.soundfonts ?? []).map(path => (
                        <option key={path} value={path}>
                          {path.split('/').pop()}
                        </option>
                      ))}
                    </select>
                  </div>
                )}
                <div className="patch-header">
                  {hasSf2 ? (
                    <div className="patch-bank-tabs">
                      {banks.map(([bank]) => (
                        <button key={bank} type="button"
                          className={`patch-bank-tab${bank === visibleBank ? ' patch-bank-tab-active' : ''}`}
                          disabled={connection !== 'online' || sampleMode}
                          onClick={() => setPatch(bank, activePatch)}>
                          {bank === 128 ? 'Drums' : `Bank ${bank}`}
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
                            ? `${hit.program + 1}. ${hit.name}`
                            : `${activePatch + 1}. ${GM_PROGRAMS[activePatch] ?? 'Program ' + (activePatch + 1)}`;
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
              {audio?.touchpadDrawing && (
                <div className="touchpad-sketch">
                  <span className="touchpad-sketch-label">Drawing…</span>
                  <Waveform samples={audio.touchpadSketch} />
                </div>
              )}
              {(audio?.touchpadRawPoints?.length ?? 0) > 0 && (
                <div className="touchpad-sketch">
                  <span className="touchpad-sketch-label">
                    {audio!.touchpadDrawing ? 'Drawing… (raw path)' : 'Last draw — raw path'}
                  </span>
                  <TouchpadWhiteboard
                    points={audio!.touchpadRawPoints}
                    drawing={audio!.touchpadDrawing}
                  />
                </div>
              )}
              <div className="touchpad-sketch">
                <span className="touchpad-sketch-label">
                  Waveform editor · touchpad draws · presets below
                </span>
                <WaveformEditor
                    touchpadSketch={audio?.touchpadSketch ?? []}                    touchpadDrawing={audio?.touchpadDrawing ?? false}                  onApply={sendWavetable}
                  disabled={connection !== "online" || activeBankHasMicSample}
                />
              </div>
              <div className="trim">
                {audio?.sampleReady && (
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
      </div>
    </main>
  );
}
