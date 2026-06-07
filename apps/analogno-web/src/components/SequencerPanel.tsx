import React, { type CSSProperties, type KeyboardEvent as ReactKeyboardEvent, useCallback, useContext, useEffect, useMemo, useRef, useState } from "react";
import { TickContext } from "../App";

function EraserIcon() {
  return (
    <svg width="11" height="9" viewBox="0 0 11 9" fill="none" aria-hidden="true">
      <rect x="0.5" y="1.5" width="10" height="6" rx="1.5" fill="currentColor" opacity="0.22"/>
      <rect x="0.5" y="1.5" width="4" height="6" rx="1.5" fill="currentColor" opacity="0.75"/>
      <line x1="4.5" y1="7.5" x2="10.5" y2="7.5" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"/>
    </svg>
  );
}

function ChanceIcon() {
  return (
    <svg width="10" height="10" viewBox="0 0 10 10" fill="none" aria-hidden="true">
      <rect x="0.5" y="0.5" width="9" height="9" rx="1.5" stroke="currentColor" strokeWidth="1"/>
      <circle cx="3" cy="7" r="1" fill="currentColor"/>
      <circle cx="5" cy="5" r="1" fill="currentColor"/>
      <circle cx="7" cy="3" r="1" fill="currentColor"/>
    </svg>
  );
}
import type { ConnectionState, RuntimeState, SoundfontPreset } from "../types/runtime";
import { midiNoteName, patchName } from "../lib/music";

export type VoiceSeqState = {
  available: boolean;
  enabled: boolean;
  recording: boolean;
  mode: "percussion" | "harmonic" | "hybrid";
  snap: boolean;
  sensitivity: number;
  timingOffsetMs: number;
  lastNote: number;
  lastVelocity: number;
  compiled: boolean;
  acceptedNotes: number;
  rejectedNotes: number;
  recordedSegments: number;
};

// --- Sequencer ---

export type SeqStepEdit = { active: boolean; tie: boolean; degree: number; velocity: number; midiNote: number; probability: number };
export type SeqTrackEdit = { midiChannel: number; midiProgram: number; midiBank: number; sampleBank: number; loopLength: number; volume: number; pan: number; velocityScale: number; muted: boolean; solo: boolean; name: string; steps: SeqStepEdit[] };
const SEQ_STEP_COUNTS = [8, 16, 32, 64] as const;
const SEQ_STEP_DIVISIONS = [8, 16, 32] as const;
const SEQ_TRACK_COLORS = ['#6ea8fe', '#86efac', '#f59e0b', '#f472b6', '#22d3ee', '#a78bfa', '#fb7185', '#c084fc'];
const SEQ_PAGE_SIZE = 16;
const emptySeqStep = (): SeqStepEdit => ({ active: false, tie: false, degree: 0, velocity: 100, midiNote: -1, probability: 100 });
const resizeSeqSteps = (steps: SeqStepEdit[], count: number): SeqStepEdit[] =>
  Array.from({ length: count }, (_, i) => steps[i] ? { ...steps[i] } : emptySeqStep());
const seqTracksSignature = (tracks: RuntimeState['seq']['tracks']): string =>
  tracks.map(t => [
    t.midiChannel, t.midiProgram, t.midiBank, t.sampleBank, t.loopLength,
    t.volume ?? 100, t.pan ?? 64, t.velocityScale ?? 100,
    t.muted ? 1 : 0, t.solo ? 1 : 0,
    t.steps.length,
    t.steps.map(s => `${s.active ? 1 : 0}${s.tie ? 1 : 0}:${s.degree}:${s.velocity}:${s.midiNote}:${s.probability ?? 100}`).join(',')
  ].join('|')).join(';');

type SeqStepCellProps = {
  si: number; ti: number;
  step: SeqStepEdit;
  label: string;
  isArmed: boolean; isSelected: boolean; isInLoop: boolean; isCurrent: boolean;
  disabled: boolean;
  onClick: (ti: number, si: number, e: React.MouseEvent) => void;
  onContextMenu: (ti: number, si: number, e: React.MouseEvent) => void;
  onMouseDown: (ti: number, si: number) => void;
  onMouseEnter: (ti: number, si: number, e: React.MouseEvent) => void;
};

const SeqStepCell = React.memo(function SeqStepCell({
  si, ti, step, label, isArmed, isSelected, isInLoop, isCurrent, disabled,
  onClick, onContextMenu, onMouseDown, onMouseEnter,
}: SeqStepCellProps) {
  return (
    <button
      type="button"
      className={[
        'seq-step-btn',
        !isInLoop ? 'seq-step-out' : '',
        si % 16 === 0 ? 'seq-step-page' : '',
        si % 4 === 0 ? 'seq-step-beat' : '',
        step.active ? 'seq-step-on' : '',
        isCurrent ? 'seq-step-current' : '',
        isArmed ? 'seq-step-armed' : '',
        isSelected ? 'seq-step-selected' : '',
      ].filter(Boolean).join(' ')}
      disabled={disabled || !isInLoop}
      style={step.active && (step.probability ?? 100) < 100
        ? { opacity: 0.35 + 0.65 * ((step.probability ?? 100) / 100) }
        : undefined}
      onClick={(e) => onClick(ti, si, e)}
      onContextMenu={(e) => { if (!disabled) onContextMenu(ti, si, e); }}
      onMouseDown={() => onMouseDown(ti, si)}
      onMouseEnter={(e) => onMouseEnter(ti, si, e)}
      title={`T${ti + 1} step ${si + 1}${isInLoop ? '' : ' out of loop'}${step.active ? ` — ${step.tie ? 'tie' : (label || 'scale')} vel ${step.velocity}${(step.probability ?? 100) < 100 ? ` prob ${step.probability}%` : ''}` : ''}`}>
      <span className="seq-step-num">{si + 1}</span>
      {step.active && <span className="seq-step-note">{step.tie ? '–' : label}</span>}
      {step.active && <span className="seq-step-vel-dot" style={{ width: `${Math.round(step.velocity / 127 * 100)}%` }} />}
      {step.active && (step.probability ?? 100) < 100 && (
        <span className="seq-step-prob">{step.probability}%</span>
      )}
    </button>
  );
});

function BpmField({ value, disabled, onChange }: { value: number; disabled: boolean; onChange: (v: number) => void }) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState('');
  const inputRef = useRef<HTMLInputElement>(null);
  const dragRef = useRef<{ startY: number; startVal: number } | null>(null);

  const clamp = useCallback((v: number) => Math.max(20, Math.min(300, Math.round(v))), []);

  function commit(raw: string) {
    const n = parseInt(raw, 10);
    if (!isNaN(n)) onChange(clamp(n));
    setEditing(false);
  }

  function startEdit() {
    if (disabled) return;
    setDraft(String(value));
    setEditing(true);
    requestAnimationFrame(() => inputRef.current?.select());
  }

  function onPointerDown(e: React.PointerEvent<HTMLSpanElement>) {
    if (disabled || editing) return;
    e.preventDefault();
    e.currentTarget.setPointerCapture(e.pointerId);
    dragRef.current = { startY: e.clientY, startVal: value };
  }

  function onPointerMove(e: React.PointerEvent<HTMLSpanElement>) {
    if (!dragRef.current) return;
    const delta = dragRef.current.startY - e.clientY;
    if (Math.abs(delta) > 2) {
      onChange(clamp(dragRef.current.startVal + Math.round(delta * 0.5)));
    }
  }

  function onPointerUp(e: React.PointerEvent<HTMLSpanElement>) {
    const wasDrag = dragRef.current && Math.abs(dragRef.current.startY - e.clientY) > 4;
    dragRef.current = null;
    if (!wasDrag) startEdit();
  }

  function onWheel(e: React.WheelEvent<HTMLSpanElement>) {
    if (disabled) return;
    e.preventDefault();
    onChange(clamp(value + (e.deltaY < 0 ? 1 : -1)));
  }

  function onKeyDown(e: React.KeyboardEvent<HTMLSpanElement>) {
    if (disabled) return;
    if (e.key === 'ArrowUp') { e.preventDefault(); onChange(clamp(value + 1)); }
    else if (e.key === 'ArrowDown') { e.preventDefault(); onChange(clamp(value - 1)); }
    else if (e.key === 'Enter' || e.key === ' ') startEdit();
  }

  if (editing) {
    return (
      <input
        ref={inputRef}
        className="seq-bpm-input seq-bpm-editing"
        type="text"
        inputMode="numeric"
        value={draft}
        onChange={e => setDraft(e.target.value)}
        onBlur={() => commit(draft)}
        onKeyDown={e => {
          if (e.key === 'Enter') commit(draft);
          else if (e.key === 'Escape') setEditing(false);
        }}
      />
    );
  }

  return (
    <span
      className={`seq-bpm-input seq-bpm-drag${disabled ? ' seq-bpm-disabled' : ''}`}
      role="spinbutton"
      tabIndex={disabled ? -1 : 0}
      aria-label="BPM"
      aria-valuenow={value}
      aria-valuemin={20}
      aria-valuemax={300}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onWheel={onWheel}
      onKeyDown={onKeyDown}
    >
      {value}
    </span>
  );
}

export function SequencerPanel({
  seq,
  presets,
  connection,
  sampleMode,
  sampleIsWavetable,
  sampleBanks,
  onPlay,
  onStop,
  onSelectTrack,
  onSelectStep,
  onAddTrack,
  onRemoveTrack,
  onChange,
  onTrackVolume,
  onTrackPan,
  onTrackVelocityScale,
  onTrackSolo,
  onClearTrack,
  onClearAll,
  onRemoveAll,
  onSetTrackName,
  onSetSwing,
  onSetStepProbability,
  onCopyTrack,
  onPasteTrack,
  voiceSeq,
  onVoiceSeq,
}: {
  seq: RuntimeState['seq'] | undefined;
  presets: SoundfontPreset[];
  connection: ConnectionState;
  sampleMode: boolean;
  sampleIsWavetable: boolean;
  sampleBanks: RuntimeState['audio']['banks'];
  onPlay: () => void;
  onStop: () => void;
  onSelectTrack: (track: number) => void;
  onSelectStep: (step: number) => void;
  onAddTrack: () => void;
  onRemoveTrack: (track: number) => void;
  onChange: (cfg: { bpm: number; gatePct: number; stepCount: number; stepDivision: number; tracks: SeqTrackEdit[] }) => void;
  onTrackVolume: (track: number, volume: number) => void;
  onTrackPan: (track: number, pan: number) => void;
  onTrackVelocityScale: (track: number, scale: number) => void;
  onTrackSolo: (track: number, solo: boolean) => void;
  onClearTrack: (track: number) => void;
  onClearAll: () => void;
  onRemoveAll: () => void;
  onSetTrackName: (track: number, name: string) => void;
  onSetSwing: (swing: number) => void;
  onSetStepProbability: (track: number, step: number, probability: number) => void;
  onCopyTrack: (track: number) => void;
  onPasteTrack: (track: number) => void;
  voiceSeq?: VoiceSeqState;
  onVoiceSeq?: (next: Partial<{ enabled: boolean; recording: boolean; mode: "percussion" | "harmonic" | "hybrid"; snapToScale: boolean; sensitivity: number; timingOffsetMs: number }>) => void;
}) {
  const initialized = useRef(false);
  const tracksSignature = useRef('');
  const [bpm, setBpm] = useState(120);
  const [gate, setGate] = useState(50);
  const [swing, setSwing] = useState(0);
  const [stepCount, setStepCount] = useState(32);
  const [stepDivision, setStepDivision] = useState(16);
  const [stepPage, setStepPage] = useState(0);
  const [showMixer, setShowMixer] = useState(false);
  const [showActionsMenu, setShowActionsMenu] = useState(false);
  const actionsMenuRef = useRef<HTMLDivElement>(null);
  const [editingTrackName, setEditingTrackName] = useState<number | null>(null);
  const [trackNameDraft, setTrackNameDraft] = useState('');
  const [trackContextMenu, setTrackContextMenu] = useState<{ ti: number; x: number; y: number } | null>(null);
  const trackContextMenuRef = useRef<HTMLDivElement>(null);
  const [selection, setSelection] = useState<Set<string>>(new Set());
  const [lastAnchor, setLastAnchor] = useState<{ ti: number; si: number } | null>(null);
  const mouseDownRef = useRef<{ ti: number; si: number; dragged: boolean } | null>(null);
  const tracksRef = useRef<SeqTrackEdit[]>([]);
  const selectionRef = useRef<Set<string>>(new Set());
  const localDirtyRef = useRef(false);
  const [probLaneTracks, setProbLaneTracks] = useState<Set<number>>(new Set());
  const probPaintRef = useRef<Map<string, { ti: number; si: number; prob: number }> | null>(null);
  const onSetStepProbabilityRef = useRef(onSetStepProbability);
  onSetStepProbabilityRef.current = onSetStepProbability;
  const [tracks, setTracks] = useState<SeqTrackEdit[]>(
    Array.from({ length: 4 }, (_, ti) => ({
      midiChannel: ti, midiProgram: 0, midiBank: 0, sampleBank: -1, loopLength: 32,
      volume: 100, pan: 64, velocityScale: 100, muted: false, solo: false, name: '',
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
    setSwing(seq.swing ?? 0);
    setStepCount(nextStepCount);
    setStepDivision(nextStepDivision);
    const nextSignature = seqTracksSignature(seq.tracks);
    if (nextSignature !== tracksSignature.current) {
      tracksSignature.current = nextSignature;
      const newTracks = seq.tracks.map(t => ({
        midiChannel: t.midiChannel,
        midiProgram: t.midiProgram,
        midiBank: t.midiBank,
        sampleBank: t.sampleBank ?? -1,
        loopLength: Math.max(1, Math.min(nextStepCount, t.loopLength ?? nextStepCount)),
        volume: t.volume ?? 100,
        pan: t.pan ?? 64,
        velocityScale: t.velocityScale ?? 100,
        muted: t.muted,
        solo: t.solo ?? false,
        name: t.name ?? '',
        steps: resizeSeqSteps(t.steps.map(s => ({ ...s, probability: s.probability ?? 100 })), nextStepCount),
      }));
      const localSignature = seqTracksSignature(tracksRef.current);
      const trackCountChanged = seq.tracks.length !== tracksRef.current.length;
      if (!localDirtyRef.current || nextSignature === localSignature || trackCountChanged) {
        tracksRef.current = newTracks;
        localDirtyRef.current = false;
        setTracks(newTracks);
      }
    }
  }, [seq]);

  const tickSeq = useContext(TickContext)?.seq;
  const disabled = connection !== 'online';
  const currentStep = tickSeq?.currentStep ?? seq?.currentStep ?? -1;
  const playheadStep = tickSeq?.playheadStep ?? seq?.playheadStep ?? currentStep;
  const playing = tickSeq?.playing ?? seq?.playing ?? false;
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

  const stateRef = useRef({ tracks, bpm, gate, stepCount, stepDivision, selection, activeTrack, selectedStep, onChange, onSelectStep });
  stateRef.current = { tracks, bpm, gate, stepCount, stepDivision, selection, activeTrack, selectedStep, onChange, onSelectStep };
  selectionRef.current = selection;

  function applyTracks(next: SeqTrackEdit[]) {
    tracksRef.current = next;
    localDirtyRef.current = true;
    setTracks(next);
  }

  function calcProb(clientY: number, rect: DOMRect): number {
    return Math.max(1, Math.min(100, Math.round((1 - (clientY - rect.top) / rect.height) * 100)));
  }

  function setProbLocally(ti: number, si: number, prob: number) {
    const next = tracksRef.current.map((t, tIdx) =>
      tIdx === ti
        ? { ...t, steps: t.steps.map((s, sIdx) => sIdx === si ? { ...s, probability: prob } : s) }
        : t
    );
    applyTracks(next);
  }

  function onProbCellDown(e: React.PointerEvent<HTMLDivElement>, ti: number, si: number) {
    if (disabled) return;
    e.preventDefault();
    probPaintRef.current = new Map();
    const rect = e.currentTarget.getBoundingClientRect();
    const prob = calcProb(e.clientY, rect);
    setProbLocally(ti, si, prob);
    probPaintRef.current.set(`${ti}-${si}`, { ti, si, prob });
  }

  function onProbCellMove(e: React.PointerEvent<HTMLDivElement>, ti: number, si: number) {
    if (!probPaintRef.current || !(e.buttons & 1)) return;
    const rect = e.currentTarget.getBoundingClientRect();
    const prob = calcProb(e.clientY, rect);
    const key = `${ti}-${si}`;
    if (probPaintRef.current.get(key)?.prob === prob) return;
    setProbLocally(ti, si, prob);
    probPaintRef.current.set(key, { ti, si, prob });
  }

  useEffect(() => {
    const commit = () => {
      if (!probPaintRef.current) return;
      for (const { ti, si, prob } of probPaintRef.current.values()) {
        onSetStepProbabilityRef.current(ti, si, prob);
      }
      probPaintRef.current = null;
    };
    window.addEventListener('pointerup', commit);
    return () => window.removeEventListener('pointerup', commit);
  }, []);

  function applySelection(next: Set<string>) {
    selectionRef.current = next;
    setSelection(next);
  }

  function deleteSelectionFn() {
    const sel = selectionRef.current;
    const t = tracksRef.current;
    const { bpm: b, gate: g, stepCount: sc, stepDivision: sd, activeTrack: at, selectedStep: ss, onChange: oc, onSelectStep: osp } = stateRef.current;
    if (sel.size === 0) return;
    const next = t.map((track, ti) => ({
      ...track,
      steps: track.steps.map((s, si) => sel.has(`${ti}-${si}`) ? emptySeqStep() : s),
    }));
    applyTracks(next);
    applySelection(new Set());
    if (ss >= 0 && sel.has(`${at}-${ss}`)) osp(-1);
    oc({ bpm: b, gatePct: g, stepCount: sc, stepDivision: sd, tracks: next });
  }

  function shiftSelectionFn(delta: number) {
    const sel = selectionRef.current;
    const t = tracksRef.current;
    const { bpm: b, gate: g, stepCount: sc, stepDivision: sd, onChange: oc } = stateRef.current;
    if (sel.size === 0) return;
    const byTrack = new Map<number, number[]>();
    for (const key of sel) {
      const [ti, si] = key.split('-').map(Number);
      if (!byTrack.has(ti)) byTrack.set(ti, []);
      byTrack.get(ti)!.push(si);
    }
    const newSel = new Set<string>();
    const next = t.map((track, ti) => {
      const selected = byTrack.get(ti);
      if (!selected) return track;
      const loopLen = track.loopLength;
      if (selected.some(si => si + delta < 0 || si + delta >= loopLen)) {
        for (const si of selected) newSel.add(`${ti}-${si}`);
        return track;
      }
      const steps = track.steps.map(s => ({ ...s }));
      const selSet = new Set(selected);
      if (selected.some(si => !selSet.has(si + delta) && steps[si + delta]?.active)) {
        for (const si of selected) newSel.add(`${ti}-${si}`);
        return track;
      }
      const savedSel = new Map(selected.map(si => [si, { ...steps[si] }]));
      for (const si of selected) steps[si] = { ...steps[si], active: false };
      for (const si of selected) { steps[si + delta] = savedSel.get(si)!; newSel.add(`${ti}-${si + delta}`); }
      return { ...track, steps };
    });
    applyTracks(next);
    applySelection(newSel);
    if (newSel.size > 0) {
      const stepIndices = [...newSel].map(k => Number(k.split('-')[1]));
      const leadStep = delta > 0 ? Math.max(...stepIndices) : Math.min(...stepIndices);
      setStepPage(Math.floor(leadStep / SEQ_PAGE_SIZE));
    }
    oc({ bpm: b, gatePct: g, stepCount: sc, stepDivision: sd, tracks: next });
  }

  const deleteRef = useRef(deleteSelectionFn);
  deleteRef.current = deleteSelectionFn;
  const shiftRef = useRef(shiftSelectionFn);
  shiftRef.current = shiftSelectionFn;

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (selectionRef.current.size === 0) return;
      if (e.target instanceof HTMLInputElement || e.target instanceof HTMLSelectElement) return;
      if (e.key === 'Escape') { setSelection(new Set()); }
      else if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); deleteRef.current(); }
      else if (e.key === 'ArrowLeft') { e.preventDefault(); shiftRef.current(-1); }
      else if (e.key === 'ArrowRight') { e.preventDefault(); shiftRef.current(1); }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  useEffect(() => {
    const stop = () => { mouseDownRef.current = null; };
    window.addEventListener('mouseup', stop);
    return () => window.removeEventListener('mouseup', stop);
  }, []);

  useEffect(() => {
    if (!showActionsMenu) return;
    const handler = (e: MouseEvent) => {
      if (actionsMenuRef.current && !actionsMenuRef.current.contains(e.target as Node))
        setShowActionsMenu(false);
    };
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, [showActionsMenu]);

  useEffect(() => {
    if (!trackContextMenu) return;
    const handler = (e: MouseEvent) => {
      if (trackContextMenuRef.current && !trackContextMenuRef.current.contains(e.target as Node))
        setTrackContextMenu(null);
    };
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, [trackContextMenu]);

  function send(t: SeqTrackEdit[], b: number, g: number, count = stepCount, division = stepDivision) {
    onChange({ bpm: b, gatePct: g, stepCount: count, stepDivision: division, tracks: t });
  }

  function handleStepClick(trackIdx: number, stepIdx: number, e: React.MouseEvent) {
    if (mouseDownRef.current?.dragged) { mouseDownRef.current = null; return; }
    mouseDownRef.current = null;
    if (stepIdx >= (tracks[trackIdx]?.loopLength ?? stepCount)) return;

    if (e.shiftKey && lastAnchor) {
      const lo = Math.min(lastAnchor.si, stepIdx);
      const hi = Math.max(lastAnchor.si, stepIdx);
      const loY = Math.min(lastAnchor.ti, trackIdx);
      const hiY = Math.max(lastAnchor.ti, trackIdx);
      setSelection(prev => {
        const next = new Set(prev);
        for (let row = loY; row <= hiY; row++) {
          const loopLen = tracks[row]?.loopLength ?? stepCount;
          for (let col = lo; col <= hi && col < loopLen; col++) next.add(`${row}-${col}`);
        }
        return next;
      });
      return;
    }

    setSelection(new Set());
    setLastAnchor({ ti: trackIdx, si: stepIdx });
    if (activeTrack !== trackIdx) onSelectTrack(trackIdx);
    onSelectStep(activeTrack === trackIdx && selectedStep === stepIdx ? -1 : stepIdx);
  }

  function handleStepMouseDown(trackIdx: number, stepIdx: number) {
    if (disabled || stepIdx >= (tracks[trackIdx]?.loopLength ?? stepCount)) return;
    mouseDownRef.current = { ti: trackIdx, si: stepIdx, dragged: false };
  }

  function handleStepMouseEnter(trackIdx: number, stepIdx: number, e: React.MouseEvent) {
    const down = mouseDownRef.current;
    if (!down || e.buttons !== 1 || disabled) return;
    const loopLen = tracks[trackIdx]?.loopLength ?? stepCount;
    if (stepIdx >= loopLen) return;
    if (!down.dragged) {
      down.dragged = true;
      setLastAnchor({ ti: down.ti, si: down.si });
    }
    const loX = Math.min(down.si, stepIdx);
    const hiX = Math.max(down.si, stepIdx);
    const loY = Math.min(down.ti, trackIdx);
    const hiY = Math.max(down.ti, trackIdx);
    const next = new Set<string>();
    for (let row = loY; row <= hiY; row++) {
      const rowLoop = tracks[row]?.loopLength ?? stepCount;
      for (let col = loX; col <= hiX && col < rowLoop; col++) next.add(`${row}-${col}`);
    }
    setSelection(next);
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

  const stepClickRef = useRef(handleStepClick);
  stepClickRef.current = handleStepClick;
  const stepRightClickRef = useRef(handleStepRightClick);
  stepRightClickRef.current = handleStepRightClick;
  const stepMouseDownRef = useRef(handleStepMouseDown);
  stepMouseDownRef.current = handleStepMouseDown;
  const stepMouseEnterRef = useRef(handleStepMouseEnter);
  stepMouseEnterRef.current = handleStepMouseEnter;

  const onStepClick = useCallback((ti: number, si: number, e: React.MouseEvent) =>
    stepClickRef.current(ti, si, e), []);
  const onStepRightClick = useCallback((ti: number, si: number, e: React.MouseEvent) =>
    stepRightClickRef.current(ti, si, e), []);
  const onStepMouseDown = useCallback((ti: number, si: number) =>
    stepMouseDownRef.current(ti, si), []);
  const onStepMouseEnter = useCallback((ti: number, si: number, e: React.MouseEvent) =>
    stepMouseEnterRef.current(ti, si, e), []);

  function handleBpm(v: number) {
    const c = Math.max(20, Math.min(300, v));
    setBpm(c); send(tracks, c, gate);
  }

  function handleGate(v: number) {
    const nextGate = Math.max(5, Math.min(100, Math.round(v)));
    setGate(nextGate); send(tracks, bpm, nextGate);
  }

  function handleGatePointer(e: React.PointerEvent<HTMLSpanElement>) {
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

  function handleTrackSource(trackIdx: number, value: string) {
    const sampleBank = value === 'midi' ? -1 : Number(value);
    const next = tracks.map((t, i) => i === trackIdx ? { ...t, sampleBank } : t);
    tracksSignature.current = '';
    setTracks(next);
    send(next, bpm, gate);
    if (activeTrack !== trackIdx) onSelectTrack(trackIdx);
  }

  const armedStep = selectedStep >= 0 ? tracks[activeTrack]?.steps[selectedStep] : null;
  const trackPatchName = (track: SeqTrackEdit) =>
    track.sampleBank >= 0
      ? `B${track.sampleBank + 1} ${sampleBanks[track.sampleBank]?.isWavetable ? 'Wavetable' : 'Sample'}`
      : patchName(track.midiBank, track.midiProgram, presets);

  return (
    <div className="sequencer">
      <div className="seq-transport">

        {/* Play / Stop */}
        <button type="button"
          className={`seq-play-btn${playing ? ' seq-play-btn-stop' : ''}`}
          disabled={disabled}
          onClick={playing ? onStop : onPlay}
          title={playing ? 'Stop' : 'Play'}>
          {playing ? '\u25a0' : '\u25b6'}
        </button>

        <span className="seq-tsep" />

        {/* Tempo */}
        <div className="seq-tgroup">
          <span className="seq-tgroup-label">Tempo</span>
          <div className="seq-tgroup-row">
            <BpmField value={bpm} disabled={disabled} onChange={handleBpm} />
            <span className="seq-tunit">bpm</span>
          </div>
        </div>

        <span className="seq-tsep" />

        {/* Division */}
        <div className="seq-tgroup">
          <span className="seq-tgroup-label">Division</span>
          <div className="seq-tgroup-row">
            {SEQ_STEP_DIVISIONS.map(n => (
              <button key={n} type="button"
                className={`seq-tchip${stepDivision === n ? ' seq-tchip-on' : ''}`}
                disabled={disabled}
                onClick={() => handleStepDivision(n)}>
                1/{n}
              </button>
            ))}
          </div>
        </div>

        <span className="seq-tsep" />

        {/* Steps */}
        <div className="seq-tgroup">
          <span className="seq-tgroup-label">Steps</span>
          <div className="seq-tgroup-row">
            {SEQ_STEP_COUNTS.map(n => (
              <button key={n} type="button"
                className={`seq-tchip${stepCount === n ? ' seq-tchip-on' : ''}`}
                disabled={disabled}
                onClick={() => handleStepCount(n)}>
                {n}
              </button>
            ))}
          </div>
        </div>

        <span className="seq-tsep" />

        {/* Gate */}
        <div className="seq-tgroup">
          <span className="seq-tgroup-label">Gate <span className="seq-tgroup-val">{gate}%</span></span>
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

        <span className="seq-tsep" />

        {/* Track loop length */}
        <div className="seq-tgroup">
          <span className="seq-tgroup-label">T{activeTrack + 1} Loop</span>
          <div className="seq-tgroup-row">
            <button type="button" className="seq-loop-btn"
              disabled={disabled || activeLoopLength <= 1}
              onClick={() => handleActiveLoopLength(activeLoopLength - 1)}
              aria-label="Shorten active track loop">−</button>
            <input type="number" className="seq-loop-input"
              min={1} max={stepCount}
              value={activeLoopLength} disabled={disabled}
              onChange={e => handleActiveLoopLength(Number(e.target.value))} />
            <button type="button" className="seq-loop-btn"
              disabled={disabled || activeLoopLength >= stepCount}
              onClick={() => handleActiveLoopLength(activeLoopLength + 1)}
              aria-label="Lengthen active track loop">+</button>
          </div>
        </div>

        <span className="seq-tsep" />

        {/* Swing */}
        <div className="seq-tgroup">
          <span className="seq-tgroup-label">Swing <span className="seq-tgroup-val">{swing}%</span></span>
          <input type="range" min={0} max={50} step={1}
            className="seq-swing-slider"
            value={swing} disabled={disabled}
            onChange={e => {
              const v = Number(e.target.value);
              setSwing(v);
              onSetSwing(v);
            }} />
        </div>

        <span className="seq-transport-spacer" />

        {/* Mix + actions */}
        <button type="button"
          className={`seq-mixer-toggle${showMixer ? ' seq-mixer-toggle-active' : ''}`}
          disabled={disabled}
          onClick={() => setShowMixer(v => !v)}>
          Mix
        </button>
        <div className="seq-actions-menu" ref={actionsMenuRef}>
          <button type="button"
            className={`seq-actions-trigger${showActionsMenu ? ' seq-actions-trigger-open' : ''}`}
            disabled={disabled}
            title="Sequencer actions"
            onClick={() => setShowActionsMenu(v => !v)}>
            &#x22EE;
          </button>
          {showActionsMenu && (
            <div className="seq-actions-dropdown">
              <button type="button" className="seq-actions-item seq-actions-item-danger"
                onClick={() => { onClearAll(); setShowActionsMenu(false); }}>
                <EraserIcon /> Clear all tracks
              </button>
              <button type="button" className="seq-actions-item seq-actions-item-danger"
                onClick={() => { onRemoveAll(); setShowActionsMenu(false); }}>
                &#x2715; Remove all tracks
              </button>
            </div>
          )}
        </div>
      </div>

      {selectedStep >= 0 && (
        <div className="seq-status">
          <span>T{activeTrack + 1}{' \u00b7 '}step {selectedStep + 1}
            {armedStep && armedStep.midiNote >= 0
              ? ` \u00b7 ${midiNoteName(armedStep.midiNote)}`
              : armedStep?.active
                ? ` \u00b7 scale deg ${armedStep.degree}`
                : ' \u00b7 empty \u2014 press a face button'}
          </span>
        </div>
      )}

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

      <div className={`seq-tracks${selection.size > 0 ? ' has-selection' : ''}`}>
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
              onContextMenu={e => { e.preventDefault(); setTrackContextMenu({ ti, x: e.clientX, y: e.clientY }); }}
              title={`Track ${ti + 1} — ${track.sampleBank >= 0 ? `Bank ${track.sampleBank + 1}` : `MIDI ch ${(track.midiChannel ?? ti) + 1}`} — ${trackPatchName(track)}`}>
              <div className="seq-track-header-top">
                {editingTrackName === ti ? (
                  <input className="seq-track-name-input"
                    value={trackNameDraft}
                    autoFocus
                    onClick={e => e.stopPropagation()}
                    onChange={e => setTrackNameDraft(e.target.value)}
                    onBlur={() => { onSetTrackName(ti, trackNameDraft); setEditingTrackName(null); }}
                    onKeyDown={e => {
                      if (e.key === 'Enter') { onSetTrackName(ti, trackNameDraft); setEditingTrackName(null); }
                      if (e.key === 'Escape') setEditingTrackName(null);
                      e.stopPropagation();
                    }} />
                ) : (
                  <span className="seq-track-id"
                    onDoubleClick={e => { e.stopPropagation(); setEditingTrackName(ti); setTrackNameDraft(track.name ?? ''); }}>
                    {track.name || `T${ti + 1}`}
                    {!track.name && <small> ch{(track.midiChannel ?? ti) + 1}</small>}
                  </span>
                )}
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
                <button type="button" className={`seq-track-solo-btn${track.solo ? ' seq-track-solo-active' : ''}`}
                  disabled={disabled}
                  title={track.solo ? 'Unsolo track' : 'Solo track'}
                  onClick={e => {
                    e.stopPropagation();
                    const next = tracks.map((t, i) => i === ti ? { ...t, solo: !t.solo } : t);
                    setTracks(next);
                    onTrackSolo(ti, !track.solo);
                  }}>
                  S
                </button>
                <button type="button"
                  className={`seq-track-prob-btn${probLaneTracks.has(ti) ? ' seq-track-prob-active' : ''}`}
                  disabled={disabled}
                  title={probLaneTracks.has(ti) ? 'Hide probability lane' : 'Show probability lane'}
                  onClick={e => {
                    e.stopPropagation();
                    setProbLaneTracks(prev => {
                      const next = new Set(prev);
                      if (next.has(ti)) next.delete(ti); else next.add(ti);
                      return next;
                    });
                  }}>
                  <ChanceIcon />
                </button>
                <button type="button" className="seq-track-clear-btn"
                  disabled={disabled}
                  title="Clear steps"
                  onClick={e => { e.stopPropagation(); onClearTrack(ti); }}>
                  <EraserIcon />
                </button>
                <button type="button" className="seq-track-remove-btn"
                  disabled={disabled || tracks.length <= 1}
                  title="Remove track"
                  onClick={e => { e.stopPropagation(); onRemoveTrack(ti); }}>
                  &#x2715;
                </button>
              </div>
              <select
                className="seq-track-source"
                value={track.sampleBank >= 0 ? String(track.sampleBank) : 'midi'}
                disabled={disabled}
                onClick={e => e.stopPropagation()}
                onChange={e => handleTrackSource(ti, e.target.value)}>
                <option value="midi">MIDI</option>
                {sampleBanks.map((bank, bi) => (
                  <option key={bi} value={bi} disabled={!bank.hasData}>
                    B{bi + 1} {bank.hasData ? (bank.isWavetable ? 'Wavetable' : 'Sample') : 'Empty'}
                  </option>
                ))}
              </select>
              <span className="seq-track-name">{trackPatchName(track)}</span>
            </div>
            <div className="seq-steps-col">
              <div className="seq-track-steps" style={{
                '--seq-step-cols': visibleStepCount,
                '--seq-step-mobile-cols': Math.min(visibleStepCount, 16),
              } as CSSProperties}>
                {track.visibleSteps.map((step, offset) => {
                  const si = visibleStart + offset;
                  const label = step.midiNote >= 0 ? midiNoteName(step.midiNote) : '';
                  const isArmed = ti === activeTrack && selectedStep === si;
                  const isSelected = selection.has(`${ti}-${si}`);
                  const loopLength = Math.max(1, Math.min(stepCount, track.loopLength));
                  const isInLoop = si < loopLength;
                  const isCurrent = isInLoop && playheadStep >= 0 && playheadStep % loopLength === si && playing;
                  return (
                    <SeqStepCell
                      key={si}
                      si={si} ti={ti}
                      step={step}
                      label={label}
                      isArmed={isArmed}
                      isSelected={isSelected}
                      isInLoop={isInLoop}
                      isCurrent={isCurrent}
                      disabled={disabled}
                      onClick={onStepClick}
                      onContextMenu={onStepRightClick}
                      onMouseDown={onStepMouseDown}
                      onMouseEnter={onStepMouseEnter}
                    />
                  );
                })}
              </div>
              {probLaneTracks.has(ti) && (
                <div className="seq-prob-lane" style={{ '--seq-step-cols': visibleStepCount } as CSSProperties}>
                  {track.visibleSteps.map((step, offset) => {
                    const si = visibleStart + offset;
                    const loopLength = Math.max(1, Math.min(stepCount, track.loopLength));
                    const isInLoop = si < loopLength;
                    return (
                      <div
                        key={si}
                        className={`seq-prob-cell${!isInLoop ? ' seq-step-out' : ''}${!step.active ? ' seq-prob-cell-inactive' : ''}`}
                        onPointerDown={step.active && isInLoop ? e => onProbCellDown(e, ti, si) : undefined}
                        onPointerMove={step.active && isInLoop ? e => onProbCellMove(e, ti, si) : undefined}
                      >
                        {step.active && (
                          <div className="seq-prob-bar" style={{ height: `${step.probability ?? 100}%` }} />
                        )}
                      </div>
                    );
                  })}
                </div>
              )}
            </div>
          </div>
        ))}
      </div>

      {showMixer && (
        <div className="seq-mixer">
          <div className="seq-mixer-header">
            <span />
            <span className="seq-mixer-col-label">Vol</span>
            <span className="seq-mixer-col-label">Pan</span>
            <span className="seq-mixer-col-label">Accent</span>
          </div>
          {tracks.map((t, ti) => (
            <div key={ti} className="seq-mixer-row"
              style={{ '--seq-track-color': SEQ_TRACK_COLORS[ti % SEQ_TRACK_COLORS.length] } as CSSProperties}>
              <span className="seq-mixer-track-id">T{ti + 1}</span>

              <div className="seq-mixer-cell">
                <input type="range" min={0} max={127} step={1}
                  className="seq-mixer-slider seq-mixer-vol"
                  value={t.volume ?? 100} disabled={disabled}
                  title={`T${ti + 1} volume: ${t.volume ?? 100}`}
                  onChange={e => {
                    const vol = Number(e.target.value);
                    const next = tracks.map((tk, i) => i === ti ? { ...tk, volume: vol } : tk);
                    setTracks(next);
                    onTrackVolume(ti, vol);
                  }} />
                <span className="seq-mixer-val">{t.volume ?? 100}</span>
              </div>

              <div className="seq-mixer-cell">
                <input type="range" min={0} max={127} step={1}
                  className="seq-mixer-slider seq-mixer-pan"
                  value={t.pan ?? 64} disabled={disabled}
                  title={`T${ti + 1} pan: ${(t.pan ?? 64) - 64}`}
                  onChange={e => {
                    const pan = Number(e.target.value);
                    const next = tracks.map((tk, i) => i === ti ? { ...tk, pan } : tk);
                    setTracks(next);
                    onTrackPan(ti, pan);
                  }} />
                <span className="seq-mixer-val">
                  {(t.pan ?? 64) === 64 ? 'C' : (t.pan ?? 64) < 64 ? `L${64 - (t.pan ?? 64)}` : `R${(t.pan ?? 64) - 64}`}
                </span>
              </div>

              <div className="seq-mixer-cell">
                <input type="range" min={50} max={200} step={1}
                  className="seq-mixer-slider seq-mixer-accent"
                  value={t.velocityScale ?? 100} disabled={disabled}
                  title={`T${ti + 1} accent: ${t.velocityScale ?? 100}%`}
                  onChange={e => {
                    const scale = Number(e.target.value);
                    const next = tracks.map((tk, i) => i === ti ? { ...tk, velocityScale: scale } : tk);
                    setTracks(next);
                    onTrackVelocityScale(ti, scale);
                  }} />
                <span className="seq-mixer-val">{t.velocityScale ?? 100}%</span>
              </div>
            </div>
          ))}
        </div>
      )}

      <button type="button" className="seq-add-track-btn"
        disabled={disabled || tracks.length >= 16}
        onClick={onAddTrack}>
        + Track
      </button>

      {voiceSeq && onVoiceSeq && (
        <VoiceSection vs={voiceSeq} onChange={onVoiceSeq} disabled={disabled} />
      )}

      {trackContextMenu && (
        <div className="seq-track-ctx-menu" ref={trackContextMenuRef}
          style={{ position: 'fixed', left: trackContextMenu.x, top: trackContextMenu.y }}>
          <button type="button" className="seq-ctx-item"
            onClick={() => { onCopyTrack(trackContextMenu.ti); setTrackContextMenu(null); }}>
            &#x238C; Copy pattern
          </button>
          <button type="button" className="seq-ctx-item"
            disabled={!seq?.clipboardAvailable}
            onClick={() => { onPasteTrack(trackContextMenu.ti); setTrackContextMenu(null); }}>
            &#x2398; Paste pattern
          </button>
          <div className="seq-ctx-divider" />
          <button type="button" className="seq-ctx-item seq-ctx-item-danger"
            onClick={() => { onClearTrack(trackContextMenu.ti); setTrackContextMenu(null); }}>
            <EraserIcon /> Clear steps
          </button>
        </div>
      )}
    </div>
  );
}

function VoiceSection({
  vs,
  onChange,
  disabled,
}: {
  vs: VoiceSeqState;
  onChange: (next: Partial<{ enabled: boolean; recording: boolean; mode: "percussion" | "harmonic" | "hybrid"; snapToScale: boolean; sensitivity: number; timingOffsetMs: number }>) => void;
  disabled: boolean;
}) {
  const [expanded, setExpanded] = useState(false);

  const statusDot = vs.recording ? "seq-voice-dot-rec" : vs.enabled ? "seq-voice-dot-on" : "seq-voice-dot-off";
  const statusText = !vs.available
    ? (disabled ? "offline" : vs.compiled ? "init failed" : "no aubio")
    : vs.recording
      ? vs.lastNote >= 0 ? `${midiNoteName(vs.lastNote)} vel ${vs.lastVelocity}` : "listening…"
      : vs.enabled
        ? vs.lastNote >= 0 ? `${midiNoteName(vs.lastNote)} vel ${vs.lastVelocity}` : "ready"
        : "off";

  return (
    <div className="seq-voice">
      <div className="seq-voice-header">
        <span className="seq-voice-title">Voice input</span>
        <button
          type="button"
          className="seq-voice-expand"
          onClick={() => setExpanded(e => !e)}
          title={expanded ? "Collapse settings" : "Expand settings"}
        >
          {expanded ? "▴" : "▾"}
        </button>
      </div>

      <div className="seq-voice-bar">
        <button
          type="button"
          className={`seq-voice-btn${vs.enabled ? " seq-voice-btn-on" : ""}`}
          disabled={disabled || !vs.available}
          onClick={() => onChange({ enabled: !vs.enabled, recording: !vs.enabled ? vs.recording : false })}
        >
          {vs.enabled ? "Voice ON" : "Voice OFF"}
        </button>

        <button
          type="button"
          className={`seq-voice-btn seq-voice-rec${vs.recording ? " seq-voice-rec-active" : ""}`}
          disabled={disabled || !vs.available}
          onClick={() => onChange({ enabled: true, recording: !vs.recording })}
        >
          {vs.recording ? "■ Stop" : "● Rec"}
        </button>

        <span className="seq-voice-status">
          <span className={`seq-voice-dot ${statusDot}`} />
          {statusText}
        </span>

        {vs.available && (
          <span className="seq-voice-counters">
            {vs.acceptedNotes > 0 && <span className="seq-voice-count seq-voice-count-ok">{vs.acceptedNotes}</span>}
            {vs.rejectedNotes > 0 && <span className="seq-voice-count seq-voice-count-rej">−{vs.rejectedNotes}</span>}
            {vs.recordedSegments > 0 && <span className="seq-voice-count seq-voice-count-seg">{vs.recordedSegments}seg</span>}
          </span>
        )}
      </div>

      {expanded && (
        <div className="seq-voice-settings">
          <label className="seq-voice-select-field">
            <span>Input</span>
            <select
              value={vs.mode}
              disabled={disabled || !vs.available}
              onChange={e => onChange({ mode: e.target.value as "percussion" | "harmonic" | "hybrid" })}
            >
              <option value="percussion">Percussion</option>
              <option value="harmonic">Harmonic</option>
              <option value="hybrid">Hybrid</option>
            </select>
          </label>

          <label className="seq-voice-select-field">
            <span>Snap</span>
            <select
              value={vs.snap ? "scale" : "chromatic"}
              disabled={disabled || !vs.available}
              onChange={e => onChange({ snapToScale: e.target.value === "scale" })}
            >
              <option value="scale">Scale</option>
              <option value="chromatic">Chromatic</option>
            </select>
          </label>

          <div className="seq-voice-slider-field">
            <div className="seq-voice-slider-head">
              <span>Sensitivity</span>
              <strong>{Math.round(vs.sensitivity * 100)}%</strong>
            </div>
            <input
              type="range" min={0} max={100}
              value={Math.round(vs.sensitivity * 100)}
              disabled={disabled || !vs.available}
              onChange={e => onChange({ sensitivity: Number(e.target.value) / 100 })}
            />
          </div>

          <div className="seq-voice-slider-field">
            <div className="seq-voice-slider-head">
              <span>Timing offset</span>
              <strong>{Math.round(vs.timingOffsetMs)} ms</strong>
            </div>
            <input
              type="range" min={-120} max={120}
              value={Math.round(vs.timingOffsetMs)}
              disabled={disabled || !vs.available}
              onChange={e => onChange({ timingOffsetMs: Number(e.target.value) })}
            />
          </div>
        </div>
      )}
    </div>
  );
}
