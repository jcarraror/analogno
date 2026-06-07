import { useEffect, useRef } from "react";
import { useTickState } from "../App";
import { midiNoteName } from "../lib/music";

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

export function Spectrogram() {
  const tick = useTickState();
  const specSamples = tick?.audio.specSamples ?? [];
  const activeNotes = tick?.music.activeNotes ?? [];
  const expression  = tick?.music.expression ?? 0;
  const filterCutoff = tick?.music.filterCutoff ?? 0;
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

    // ── Controller strip ──────────────────────────────────────────────────
    if (!stripHistRef.current) stripHistRef.current = sctx.createImageData(SPEC_CANVAS_W, STRIP_H);
    const sh = stripHistRef.current;
    const sd = sh.data;

    for (let y = 0; y < STRIP_H; y++) {
      const row = y * SPEC_CANVAS_W * 4;
      sd.copyWithin(row, row + 4, row + SPEC_CANVAS_W * 4);
    }

    const LANE_H = Math.floor(STRIP_H / 2);
    const lanes = [
      { value: expression,   max: 1, r: 80, g: 210, b: 80  }, // expr — green
      { value: filterCutoff, max: 1, r: 80, g: 140, b: 220 }, // cut  — blue
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
    (['expr', 'cut'] as const).forEach((label, i) => {
      sctx.fillStyle = 'rgba(0,0,0,0.6)';
      sctx.fillRect(0, i * LANE_H, 34, LANE_H - 1);
      sctx.fillStyle = i === 0 ? '#4ade80' : '#60a5fa';
      sctx.fillText(label, 33, i * LANE_H + LANE_H - 4);
    });

  }, [specSamples, activeNotes, expression, filterCutoff]);

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
