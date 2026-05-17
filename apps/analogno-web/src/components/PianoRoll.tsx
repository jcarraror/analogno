import { useEffect, useRef } from "react";

const BTN_LABELS = ["✕","○","□","△","↑","↓","←","→"];
const BLACK_KEYS = new Set([1,3,6,8,10]);

export function PianoRoll({
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
