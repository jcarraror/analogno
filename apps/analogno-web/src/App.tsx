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
    banks: Array<{
      hasData: boolean;
      frames: number;
      trimStart: number;
      trimEnd: number;
    }>;
    activeBank: number;
    touchpadSketch: number[];
    touchpadDrawing: boolean;
  };
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

  function sendWavetable(samples: number[]) {
    socket?.send(JSON.stringify({ type: "setWavetable", data: samples }));
  }

  const controller = runtime?.controller;
  const music = runtime?.music;
  const audio = runtime?.audio;
  const sampleMode = audio?.sampleReady ?? false;
  const activePatch = music?.midiProgram ?? 0;
  const activeBank = music?.midiBank ?? 0;

  return (
    <main className="app">
      <header className="hero">
        <div>
          <p className="eyebrow">DualSense MIDI workstation</p>
          <h1>Analogno</h1>
          <p className="subtitle">
            C++ owns controller input, MIDI, timing, and future audio. This UI
            observes and edits runtime state.
          </p>
        </div>

        <div className="hero-actions">
          <StatusPill state={connection} />
          <button className="panic-button" onClick={panic} type="button">
            Panic
          </button>
        </div>
      </header>

      <div className="layout">
        <Panel title="Controller">
          <BipolarMeter label="Left X / pitch bend" value={controller?.leftX ?? 0} />
          <BipolarMeter label="Left Y" value={controller?.leftY ?? 0} />
          <BipolarMeter label="Right X / resonance" value={controller?.rightX ?? 0} />
          <BipolarMeter label="Right Y / cutoff" value={controller?.rightY ?? 0} />
          <Meter
            label={sampleMode ? "L2 / sample gain" : "L2 / expression"}
            value={controller?.l2 ?? 0}
          />
          <Meter label="R2 / vibrato depth" value={controller?.r2 ?? 0} />
        </Panel>

        <Panel title="Music">
          <StateLine label="Root" value={music ? `${midiNoteName(music.rootMidiNote)} / ${music.rootMidiNote}` : "—"} />
          <StateLine label="Scale" value={music?.scale ?? "—"} />
          <StateLine label="Octave offset" value={music?.octaveOffset ?? "—"} />
          <StateLine label="Active notes" value={activeNoteText} />
          <BipolarMeter label="Pitch bend" value={music?.pitchBend ?? 0} />
          <Meter
            label={sampleMode ? "Sample gain" : "Expression"}
            value={music?.expression ?? 0}
          />
          <Meter label="Filter cutoff" value={music?.filterCutoff ?? 0} />
          <Meter label="Resonance" value={music?.filterResonance ?? 0} />
          <Meter label="Vibrato" value={music?.vibrato ?? 0} />
        </Panel>

        <Panel title="Synth patches" wide>
          <div className="patch-header">
            <label className="field patch-bank-field">
              <span>MIDI bank</span>
              <select
                value={activeBank}
                disabled={connection !== "online" || sampleMode}
                onChange={(e) => setPatch(Number(e.target.value), activePatch)}
              >
                {Array.from({ length: 128 }, (_, i) => (
                  <option key={i} value={i}>Bank {i}</option>
                ))}
              </select>
            </label>
            <p className="patch-active-name">
              {sampleMode ? "Sampler mode — MIDI synth inactive" : `${activePatch + 1}. ${GM_PROGRAMS[activePatch] ?? "Program " + (activePatch + 1)}`}
            </p>
          </div>
          <div className="patch-grid">
            {GM_PROGRAMS.map((name, i) => (
              <button
                key={i}
                type="button"
                className={`patch-btn${
                  i === activePatch && !sampleMode ? " patch-active" : ""
                }`}
                disabled={connection !== "online" || sampleMode}
                onClick={() => setPatch(activeBank, i)}
                title={name}
              >
                <span className="patch-num">{i + 1}</span>
                <span className="patch-name">{name}</span>
              </button>
            ))}
          </div>
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
              <div className="touchpad-sketch">
                <span className="touchpad-sketch-label">
                  Waveform editor · touchpad draws · presets below
                </span>
                <WaveformEditor
                    touchpadSketch={audio?.touchpadSketch ?? []}                    touchpadDrawing={audio?.touchpadDrawing ?? false}                  onApply={sendWavetable}
                  disabled={connection !== "online"}
                />
              </div>
              <div className="trim">
                <label>
                  <span>Trim start</span>
                  <input
                    min="0"
                    max="1"
                    step="0.001"
                    type="range"
                    value={audio?.sampleTrimStart ?? 0}
                    disabled={!audio?.sampleReady || connection !== "online"}
                    onChange={(event) => setSampleTrim({ start: Number(event.target.value) })}
                  />
                </label>
                <label>
                  <span>Trim end</span>
                  <input
                    min="0"
                    max="1"
                    step="0.001"
                    type="range"
                    value={audio?.sampleTrimEnd ?? 1}
                    disabled={!audio?.sampleReady || connection !== "online"}
                    onChange={(event) => setSampleTrim({ end: Number(event.target.value) })}
                  />
                </label>
              </div>
            </div>
          </div>
        </Panel>

        <Panel title="Audio input">
          <StateLine label="Capture" value={audio?.captureRunning ? "running" : "stopped"} />
          <StateLine label="Gate" value={audio?.gateOpen ? "open" : "closed"} />
          <StateLine label="Velocity" value={audio?.velocity ?? 0} />
          <label className="field">
            <span>Input device</span>
            <select
              value={audio?.selectedDeviceIndex == null
                ? "default"
                : String(audio.selectedDeviceIndex)}
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
        </Panel>

        <Panel title="Motion">
          <StateLine label="Gyro" value={controller?.hasGyro ? "available" : "missing"} />
          <StateLine label="Accelerometer" value={controller?.hasAccel ? "available" : "missing"} />
          <Vec3Readout
            label="gyro"
            value={controller?.gyro ?? { x: 0, y: 0, z: 0 }}
          />
          <Vec3Readout
            label="accel"
            value={controller?.accel ?? { x: 0, y: 0, z: 0 }}
          />
        </Panel>
      </div>
    </main>
  );
}
