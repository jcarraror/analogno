import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { PianoRoll } from "./components/PianoRoll";
import { SequencerPanel, type SeqTrackEdit, type VoiceSeqState } from "./components/SequencerPanel";
import { Spectrogram } from "./components/Spectrogram";
import { StemSplitter } from "./components/StemSplitter";
import { useAnalognoSocket } from "./hooks/useAnalognoSocket";
import { GM_PROGRAMS, bankLabel, bankRole, formatNumber, midiNoteName, patchName, pluralize, soundfontFormat, soundfontName } from "./lib/music";
import type { ConnectionState, Vec3 } from "./types/runtime";

function StatusPill({ state }: { state: ConnectionState }) {
  return <div className={`pill pill-${state}`}>{state}</div>;
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

function TrimWaveform({ waveform, trimStart, trimEnd, totalFrames, disabled, onChange }: {
  waveform: number[];
  trimStart: number;
  trimEnd: number;
  totalFrames: number;
  disabled: boolean;
  onChange: (next: { start?: number; end?: number }) => void;
}) {
  const svgRef = useRef<SVGSVGElement>(null);
  const dragging = useRef<'start' | 'end' | null>(null);

  const W = 1000;
  const H = 100;
  const n = waveform.length;
  const hitZone = 22;
  const totalSecs = totalFrames / 48000;

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
      if (dragging.current === 'start') onChange({ start: Math.min(x, trimEnd - 0.001) });
      else onChange({ end: Math.max(x, trimStart + 0.001) });
    };
    const onUp = () => {
      dragging.current = null;
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
  }

  const fmtSec = (frac: number) => (frac * totalSecs).toFixed(2) + 's';

  return (
    <div className="trim-waveform-wrap">
      {waveform.length > 0 ? (
        <svg ref={svgRef} viewBox={`0 0 ${W} ${H}`}
          className={`sample-waveform-svg${disabled ? '' : ' swv-interactive'}`}
          preserveAspectRatio="none"
          onMouseDown={onMouseDown}>
          <rect x={0} y={0} width={trimStart * W} height={H} className="swv-excluded" />
          <rect x={trimEnd * W} y={0} width={(1 - trimEnd) * W} height={H} className="swv-excluded" />
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
          <line x1={trimStart * W} y1={0} x2={trimStart * W} y2={H} className="swv-marker" />
          <line x1={trimEnd * W} y1={0} x2={trimEnd * W} y2={H} className="swv-marker" />
        </svg>
      ) : (
        <div className="swv-loading">computing waveform…</div>
      )}
      <div className="trim-controls">
        <label className="trim-field">
          <span>Start</span>
          <input type="number" min={0} max={99.9} step={0.1} disabled={disabled}
            value={(trimStart * 100).toFixed(1)}
            onChange={e => {
              const v = Math.max(0, Math.min(99.9, Number(e.target.value))) / 100;
              onChange({ start: Math.min(v, trimEnd - 0.001) });
            }} />
          <span className="trim-secs">{fmtSec(trimStart)}</span>
        </label>
        <label className="trim-field">
          <span>End</span>
          <input type="number" min={0.1} max={100} step={0.1} disabled={disabled}
            value={(trimEnd * 100).toFixed(1)}
            onChange={e => {
              const v = Math.max(0.1, Math.min(100, Number(e.target.value))) / 100;
              onChange({ end: Math.max(v, trimStart + 0.001) });
            }} />
          <span className="trim-secs">{fmtSec(trimEnd)}</span>
        </label>
        <span className="trim-dur">
          {((trimEnd - trimStart) * totalSecs).toFixed(2)}s selected
        </span>
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
  activeBank,
  bankCycle,
  bankIsWavetable,
}: {
  touchpadSketch: number[];
  touchpadDrawing: boolean;
  morphAmount: number;
  noiseAmount: number;
  unisonAmount: number;
  onApply: (samples: number[], morphSamples: number[]) => void;
  disabled?: boolean;
  activeBank: number;
  bankCycle: number[];
  bankIsWavetable: boolean;
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

  useEffect(() => {
    if (!bankIsWavetable || bankCycle.length < 2) return;
    const n = Math.min(bankCycle.length, 64);
    ptsRef.current = Array.from({ length: n }, (_, i) => ({
      x: i / (n - 1),
      amp: bankCycle[Math.round(i * (bankCycle.length - 1) / (n - 1))],
    }));
    setMainName(null);
    setVer(v => v + 1);
  }, [activeBank]); // eslint-disable-line react-hooks/exhaustive-deps

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
  tone: 'morph' | 'noise' | 'unison' | 'volume';
  onChange: (value: number) => void;
}) {
  const pct = Math.round(value * 100);
  const tag = tone === 'morph' ? 'M' : tone === 'noise' ? 'N' : tone === 'unison' ? 'U' : 'V';
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
  const { connection, socket, runtime, runtimeRef, library, bankWaveforms, stemWaveforms } = useAnalognoSocket();

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

  function setBankRootNote(bank: number, note: number) {
    socket?.send(JSON.stringify({ type: "setBankRootNote", bank, note }));
  }

  function setBankSliceCount(bank: number, count: number) {
    socket?.send(JSON.stringify({ type: "setBankSliceCount", bank, count }));
  }

  function transcribeBankToSeq(bank: number) {
    socket?.send(JSON.stringify({ type: "transcribeBankToSeq", bank }));
  }

  function arrangeBankToSeq(bank: number) {
    socket?.send(JSON.stringify({ type: "arrangeBankToSeq", bank }));
  }

  function transcribeMicToSeq() {
    socket?.send(JSON.stringify({ type: "transcribeMicToSeq" }));
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

  function seqTrackVolume(track: number, volume: number) {
    socket?.send(JSON.stringify({ type: "setTrackVolume", track, volume }));
  }

  function seqTrackPan(track: number, pan: number) {
    socket?.send(JSON.stringify({ type: "setTrackPan", track, pan }));
  }

  function seqTrackVelocityScale(track: number, scale: number) {
    socket?.send(JSON.stringify({ type: "setTrackVelocityScale", track, scale }));
  }

  function seqTrackSolo(track: number, solo: boolean) {
    socket?.send(JSON.stringify({ type: "setTrackSolo", track, solo }));
  }

  function seqClearTrack(track: number) {
    socket?.send(JSON.stringify({ type: "clearSeqTrack", track }));
  }

  function seqClearAll() {
    socket?.send(JSON.stringify({ type: "clearAllSeqTracks" }));
  }

  function seqRemoveAll() {
    socket?.send(JSON.stringify({ type: "removeAllSeqTracks" }));
  }

  function seqSetTrackName(track: number, name: string) {
    socket?.send(JSON.stringify({ type: "setSeqTrackName", track, name }));
  }

  function seqSetSwing(swing: number) {
    socket?.send(JSON.stringify({ type: "setSeqSwing", swing }));
  }

  function seqSetStepProbability(track: number, step: number, probability: number) {
    socket?.send(JSON.stringify({ type: "setStepProbability", track, step, probability }));
  }

  function seqCopyTrack(track: number) {
    socket?.send(JSON.stringify({ type: "copySeqTrack", track }));
  }

  function seqPasteTrack(track: number) {
    socket?.send(JSON.stringify({ type: "pasteSeqTrack", track }));
  }

  function setSignalsVolume(volume: number) {
    socket?.send(JSON.stringify({ type: "setSignalsVolume", volume: Math.round(volume * 100) }));
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
  const sampleMode = audio?.sampleReady ?? false;
  const activePatch = music?.midiProgram ?? 0;
  const activeBank = music?.midiBank ?? 0;
  const activeSeqTrack = runtime?.seq.tracks[runtime?.seq.activeTrack ?? 0];
  const activeSeqTrackSampleBank = activeSeqTrack?.sampleBank ?? -1;
  const activeBankData = audio?.banks[audio?.activeBank ?? 0];
  const activeBankHasSample = !!(activeBankData?.hasData && !activeBankData?.isWavetable);
  const activeBankHasMicSample = activeBankHasSample && !(activeBankData?.isStream ?? false);
  const activeStemIdx = audio?.stems?.findIndex(s => s.isActive) ?? -1;
  const activeStem = (audio?.stems ?? [])[activeStemIdx] ?? (audio?.stems ?? [])[0];
  const showStemWaveform = !activeBankHasSample && !(activeBankData?.isWavetable ?? false) && (audio?.stems ?? []).length > 0 && audio?.stemSplitState !== "done";

  const activeBankWaveform = bankWaveforms.current[audio?.activeBank ?? 0] ?? [];
  const activeStemWaveform = stemWaveforms.current[activeStemIdx >= 0 ? activeStemIdx : 0] ?? [];

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
          {audio?.micHasSample && (() => {
            const ts = audio?.transcribeState ?? "idle";
            return (
              <button
                type="button"
                className={`transcribe-btn${ts === "running" ? " transcribe-btn--running" : ""}`}
                disabled={connection !== "online" || ts === "running"}
                title="Analyze the last mic recording and write detected onsets as sequencer steps"
                onClick={transcribeMicToSeq}
              >
                {ts === "running" ? (
                  <><span className="transcribe-spinner" /> Analyzing…</>
                ) : "Transcribe mic → seq"}
              </button>
            );
          })()}
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
            const presets = library?.presets ?? [];
            const hasSf2 = presets.length > 0;
            const soundfonts = library?.soundfonts ?? [];
            const activeSoundfont = library?.activeSoundfont ?? '';

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
                          disabled={connection !== 'online'}
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
                        disabled={connection !== 'online'}
                        onChange={(e) => {
                          const v = Math.max(0, Math.min(127, Number(e.target.value)));
                          if (!Number.isNaN(v)) setPatch(v, activePatch);
                        }} />
                    </label>
                  )}
                  <p className="patch-active-name">
                    {activeSeqTrackSampleBank >= 0
                      ? `Active    sampler bank B${activeSeqTrackSampleBank + 1}`
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
                        program === activePatch && visibleBank === activeBank && activeSeqTrackSampleBank < 0 ? ' patch-active' : ''
                      }`}
                      disabled={connection !== 'online'}
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
              <div className="bank-grid">
                {Array.from({ length: 8 }, (_, i) => {
                  const bank = audio?.banks[i];
                  const isActive = (audio?.activeBank ?? 0) === i;
                  const hasData = bank?.hasData ?? false;
                  const isStream = bank?.isStream ?? false;
                  const frames = bank?.frames ?? 0;
                  const totalSecs = frames / 48000;
                  const trimmedSecs = hasData && frames > 0
                    ? (((bank?.trimEnd ?? 1) - (bank?.trimStart ?? 0)) * totalSecs).toFixed(1)
                    : null;
                  return (
                    <div
                      key={i}
                      className={`bank-slot${isActive ? " bank-active" : ""}${hasData ? " bank-filled" : ""}${isStream ? " bank-stream" : ""}`}
                      role="button"
                      tabIndex={0}
                      onClick={() => { if (connection === "online") setActiveBank(i); }}
                      onKeyDown={(e) => { if (e.key === "Enter" && connection === "online") setActiveBank(i); }}
                    >
                      <span className="bank-num">B{i + 1}</span>
                      {isStream && <span className="bank-type-tag">STR</span>}
                      <span className="bank-dur">{trimmedSecs != null ? `${trimmedSecs}s` : "—"}</span>
                      {hasData && !isStream && (
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
                value={(() => {
                  if (!activeBankData?.hasData) return "empty";
                  const frames = activeBankData.frames;
                  if (frames === 0) return "loading…";
                  const totalSecs = frames / 48000;
                  const trimmed = ((activeBankData.trimEnd - activeBankData.trimStart) * totalSecs).toFixed(2);
                  const total = totalSecs.toFixed(2);
                  const tag = activeBankData.isStream ? " [stream]" : activeBankData.isWavetable ? " [osc]" : " [sample]";
                  return `${trimmed}s / ${total}s${tag}`;
                })()}
              />
              {activeBankHasSample && (
                <div className="bank-params">
                  <div className="bank-param-row">
                    <span className="bank-param-label">Root note</span>
                    <select
                      className="bank-root-select"
                      value={activeBankData?.rootNote ?? 48}
                      disabled={connection !== "online"}
                      onChange={(e) => setBankRootNote(audio?.activeBank ?? 0, Number(e.target.value))}
                    >
                      {Array.from({ length: 128 }, (_, i) => (
                        <option key={i} value={i}>{midiNoteName(i)} ({i})</option>
                      ))}
                    </select>
                  </div>
                  <div className="bank-param-row">
                    <span className="bank-param-label">Slices</span>
                    <div className="slice-chips">
                      {[0, 4, 8, 16, 32].map((n) => (
                        <button
                          key={n}
                          type="button"
                          className={`slice-chip${(activeBankData?.sliceCount ?? 0) === n ? " slice-chip-active" : ""}`}
                          disabled={connection !== "online"}
                          onClick={() => setBankSliceCount(audio?.activeBank ?? 0, n)}
                        >
                          {n === 0 ? "off" : n}
                        </button>
                      ))}
                    </div>
                  </div>
                  <div className="bank-param-row">
                    {(() => {
                      const ts = audio?.transcribeState ?? "idle";
                      const activeBank = audio?.activeBank ?? 0;
                      const cached = activeBankData?.transcribeCached ?? false;
                      const trackCount = activeBankData?.generatedTrackCount ?? 0;
                      const trackBadge = trackCount > 0 ? ` · ${trackCount}ch` : "";
                      return (
                        <>
                          <button
                            type="button"
                            className={`transcribe-btn${ts === "running" ? " transcribe-btn--running" : ""}`}
                            disabled={connection !== "online" || ts === "running"}
                            title={cached
                              ? "Re-apply the cached transcription to the active track (capped to existing tracks)"
                              : "Analyse this bank and write the result to the active track"}
                            onClick={() => transcribeBankToSeq(activeBank)}
                          >
                            {ts === "running"
                              ? <><span className="transcribe-spinner" /> Analyzing…</>
                              : `Transcribe${cached ? trackBadge : ""}`}
                          </button>
                          <button
                            type="button"
                            className={`transcribe-btn${ts === "running" ? " transcribe-btn--running" : ""}`}
                            disabled={connection !== "online" || ts === "running"}
                            title="Add this bank to the arrangement — each bank gets its own dedicated track block so all stems play simultaneously"
                            onClick={() => arrangeBankToSeq(activeBank)}
                          >
                            {ts === "running"
                              ? <><span className="transcribe-spinner" /> Analyzing…</>
                              : `Arrange${trackCount > 0 ? trackBadge : ""}`}
                          </button>
                        </>
                      );
                    })()}
                  </div>
                </div>
              )}
              <div className="wavetable-macros">
                {!activeBankHasSample && (
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
                <WavetableMacroSlider label="Volume" tone="volume"
                  value={audio?.signalsVolume ?? 1}
                  disabled={connection !== "online"}
                  onChange={(value) => setSignalsVolume(value)} />
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
              {activeBankHasSample ? (
                <div className="trim">
                  <TrimWaveform
                    waveform={activeBankWaveform}
                    trimStart={audio?.sampleTrimStart ?? 0}
                    trimEnd={audio?.sampleTrimEnd ?? 1}
                    totalFrames={activeBankData?.frames ?? 0}
                    disabled={connection !== 'online'}
                    onChange={setSampleTrim}
                  />
                </div>
              ) : showStemWaveform ? (
                <div className="trim">
                  <TrimWaveform
                    waveform={activeStemWaveform}
                    trimStart={0}
                    trimEnd={1}
                    totalFrames={activeStem?.frames ?? 0}
                    disabled={true}
                    onChange={() => {}}
                  />
                  <div className="stem-waveform-label">{activeStem?.name ?? ""}</div>
                </div>
              ) : (
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
                    activeBank={audio?.activeBank ?? 0}
                    bankCycle={audio?.wavetableCycle ?? []}
                    bankIsWavetable={activeBankData?.isWavetable ?? false}
                  />
                </div>
              )}
            </div>
          </div>
          <div style={{ marginTop: 14 }}>
            <div className="stems-section-head">
              <span>Split track into stems</span>
              <button
                type="button"
                className="stems-folder-btn"
                disabled={connection !== "online"}
                onClick={() => socket?.send(JSON.stringify({ type: "openStemFolderDialog" }))}
                title={audio?.stemsFolder ?? ""}
              >
                Stems folder
              </button>
            </div>
            <StemSplitter
              stemSplitState={audio?.stemSplitState ?? "idle"}
              stemSplitError={audio?.stemSplitError ?? ""}
              stemSplitProgress={audio?.stemSplitProgress ?? 0}
              stemSplitDetail={audio?.stemSplitDetail ?? ""}
              stemSplitLog={audio?.stemSplitLog ?? []}
              sampleWaveform={audio?.sampleWaveform ?? []}
              sampleTrimStart={audio?.sampleTrimStart ?? 0}
              sampleTrimEnd={audio?.sampleTrimEnd ?? 1}
              stems={(audio?.stems ?? []).map((stem, i) => ({
                ...stem,
                waveform: stemWaveforms.current[i]?.length > 0 ? stemWaveforms.current[i] : stem.waveform,
              }))}
              activeBank={audio?.activeBank ?? 0}
              send={msg => socket?.send(JSON.stringify(msg))}
            />
          </div>
        </Panel>

        <Panel title="Sequencer" wide>
          <SequencerPanel
            seq={runtime?.seq}
            presets={library?.presets ?? []}
            connection={connection}
            sampleMode={sampleMode}
            sampleIsWavetable={sampleMode && !activeBankHasMicSample}
            sampleBanks={audio?.banks ?? []}
            onPlay={seqPlay}
            onStop={seqStop}
            onSelectTrack={seqSelectTrack}
            onSelectStep={seqSelectStep}
            onAddTrack={seqAddTrack}
            onRemoveTrack={seqRemoveTrack}
            onChange={seqChange}
            onTrackVolume={seqTrackVolume}
            onTrackPan={seqTrackPan}
            onTrackVelocityScale={seqTrackVelocityScale}
            onTrackSolo={seqTrackSolo}
            onClearTrack={seqClearTrack}
            onClearAll={seqClearAll}
            onRemoveAll={seqRemoveAll}
            onSetTrackName={seqSetTrackName}
            onSetSwing={seqSetSwing}
            onSetStepProbability={seqSetStepProbability}
            onCopyTrack={seqCopyTrack}
            onPasteTrack={seqPasteTrack}
            voiceSeq={{
              available: audio?.voiceSeqAvailable ?? false,
              enabled: audio?.voiceSeqEnabled ?? false,
              recording: audio?.voiceSeqRecording ?? false,
              mode: audio?.voiceSeqMode ?? "percussion",
              snap: audio?.voiceSeqSnap ?? true,
              sensitivity: audio?.voiceSeqSensitivity ?? 0.65,
              timingOffsetMs: audio?.voiceSeqTimingOffsetMs ?? 0,
              lastNote: audio?.voiceSeqLastNote ?? -1,
              lastVelocity: audio?.voiceSeqLastVelocity ?? 0,
              compiled: audio?.voiceSeqCompiled ?? false,
              acceptedNotes: audio?.voiceSeqAcceptedNotes ?? 0,
              rejectedNotes: audio?.voiceSeqRejectedNotes ?? 0,
              recordedSegments: audio?.voiceSeqRecordedSegments ?? 0,
            } satisfies VoiceSeqState}
            onVoiceSeq={setVoiceSeq}
          />
        </Panel>

        {(runtime?.spectrogramVisible ?? true) && (
        <Panel
          title="Spectrogram"
          wide
        >
          <Spectrogram
            specSamples={audio?.specSamples ?? []}
            activeNotes={music?.activeNotes ?? []}
            expression={music?.expression ?? 0}
            filterCutoff={music?.filterCutoff ?? 0}
          />
        </Panel>
        )}
      </div>
    </main>
  );
}
