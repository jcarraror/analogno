import React, { useCallback, useEffect, useRef, useState } from "react";

type StemSplitState = "idle" | "uploading" | "running" | "done" | "error";

interface Props {
  stemSplitState: string;
  stemSplitError: string;
  stemSplitProgress: number;
  stemSplitDetail: string;
  stemSplitLog: string[];
  sampleWaveform: number[];
  sampleTrimStart: number;
  sampleTrimEnd: number;
  stems: Array<{ name: string; frames: number; isActive: boolean; waveform: number[] }>;
  activeBank: number;
  send: (msg: object) => void;
}

const STEM_LABELS = ["drums", "bass", "vocals", "guitar", "piano", "other"] as const;
const BANK_COLORS = ["#6ea8fe", "#86efac", "#f59e0b", "#f472b6", "#a78bfa", "#fb923c"] as const;

function TrimWave({ waveform, trimStart, trimEnd, color, playPosition, onChange }: {
  waveform: number[];
  trimStart: number;
  trimEnd: number;
  color: string;
  playPosition?: number;
  onChange: (next: { start?: number; end?: number }) => void;
}) {
  const svgRef = useRef<SVGSVGElement>(null);
  const dragging = useRef<"start" | "end" | null>(null);

  if (waveform.length === 0) {
    return (
      <div style={{ height: 56, background: "rgba(255,255,255,0.04)", borderRadius: 6, display: "flex", alignItems: "center", justifyContent: "center", fontSize: "0.72rem", color: "rgba(255,255,255,0.2)" }}>
        loading waveform…
      </div>
    );
  }

  const W = 1000, H = 56, n = waveform.length, HIT = 18;

  function xFrom(e: MouseEvent): number {
    const r = svgRef.current?.getBoundingClientRect();
    if (!r) return 0;
    return Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
  }

  function onMouseDown(e: React.MouseEvent) {
    const r = svgRef.current?.getBoundingClientRect();
    if (!r) return;
    const px = (e.clientX - r.left) / r.width * W;
    const ds = Math.abs(px - trimStart * W);
    const de = Math.abs(px - trimEnd * W);
    dragging.current = ds < de && ds < HIT ? "start" : de < HIT ? "end" : null;
    if (!dragging.current) return;
    e.preventDefault();
    const move = (me: MouseEvent) => {
      const x = xFrom(me);
      if (dragging.current === "start") onChange({ start: Math.min(x, trimEnd - 0.01) });
      else onChange({ end: Math.max(x, trimStart + 0.01) });
    };
    const up = () => {
      dragging.current = null;
      window.removeEventListener("mousemove", move);
      window.removeEventListener("mouseup", up);
    };
    window.addEventListener("mousemove", move);
    window.addEventListener("mouseup", up);
  }

  return (
    <svg ref={svgRef} viewBox={`0 0 ${W} ${H}`}
      style={{ width: "100%", height: 56, display: "block", cursor: "ew-resize", borderRadius: 6 }}
      preserveAspectRatio="none" onMouseDown={onMouseDown}>
      <rect x={0} y={0} width={W} height={H} fill="rgba(255,255,255,0.04)" />
      <rect x={0} y={0} width={trimStart * W} height={H} fill="rgba(0,0,0,0.55)" />
      <rect x={trimEnd * W} y={0} width={(1 - trimEnd) * W} height={H} fill="rgba(0,0,0,0.55)" />
      {waveform.map((v, i) => {
        const x = (i / n) * W;
        const bw = W / n;
        const h = Math.max(1, v * H * 0.9);
        const active = (i + 0.5) / n >= trimStart && (i + 0.5) / n <= trimEnd;
        return <rect key={i} x={x} y={(H - h) / 2} width={Math.max(0.5, bw - 0.8)} height={h} fill={active ? color : "rgba(255,255,255,0.18)"} />;
      })}
      <line x1={trimStart * W} y1={0} x2={trimStart * W} y2={H} stroke={color} strokeWidth={2.5} />
      <line x1={trimEnd * W} y1={0} x2={trimEnd * W} y2={H} stroke={color} strokeWidth={2.5} />
      {playPosition !== undefined && (
        <line x1={playPosition * W} y1={0} x2={playPosition * W} y2={H} stroke="rgba(255,255,255,0.85)" strokeWidth={1.5} />
      )}
    </svg>
  );
}

export function StemSplitter({
  stemSplitState,
  stemSplitError,
  stemSplitProgress,
  stemSplitDetail,
  stemSplitLog,
  sampleWaveform,
  sampleTrimStart,
  sampleTrimEnd,
  stems,
  activeBank,
  send,
}: Props) {
  const [localState, setLocalState] = useState<StemSplitState>("idle");
  const [uploadProgress, setUploadProgress] = useState(0);
  const [dragging, setDragging] = useState(false);
  const [playing, setPlaying] = useState<boolean[]>(Array(6).fill(false));
  const [playPositions, setPlayPositions] = useState<number[]>(Array(6).fill(0));
  const [editingBank, setEditingBank] = useState<number | null>(null);
  const [showLog, setShowLog] = useState(false);
  const fileRef = useRef<HTMLInputElement>(null);
  const audioRefs = useRef<Array<HTMLAudioElement | null>>(Array(6).fill(null));
  const logRef = useRef<HTMLDivElement>(null);
  const rafRef = useRef<number | null>(null);

  useEffect(() => {
    if (stemSplitState !== "done") {
      audioRefs.current.forEach(a => { if (a) { a.pause(); a.src = ""; } });
      audioRefs.current = Array(6).fill(null);
      setPlaying(Array(6).fill(false));
      setPlayPositions(Array(6).fill(0));
      setEditingBank(null);
    }
  }, [stemSplitState]);

  useEffect(() => {
    const anyPlaying = playing.some(Boolean);
    if (!anyPlaying) {
      if (rafRef.current !== null) { cancelAnimationFrame(rafRef.current); rafRef.current = null; }
      setPlayPositions(Array(6).fill(0));
      return;
    }
    const tick = () => {
      setPlayPositions(() => {
        const next = Array(6).fill(0) as number[];
        for (let i = 0; i < 6; i++) {
          const a = audioRefs.current[i];
          if (a && Number.isFinite(a.duration) && a.duration > 0)
            next[i] = a.currentTime / a.duration;
        }
        return next;
      });
      rafRef.current = requestAnimationFrame(tick);
    };
    rafRef.current = requestAnimationFrame(tick);
    return () => { if (rafRef.current !== null) { cancelAnimationFrame(rafRef.current); rafRef.current = null; } };
  }, [playing]);

  useEffect(() => {
    if (showLog && logRef.current) {
      logRef.current.scrollTop = logRef.current.scrollHeight;
    }
  }, [stemSplitLog, showLog]);

  const stopStem = useCallback((index: number) => {
    const a = audioRefs.current[index];
    if (a) { a.pause(); a.src = ""; audioRefs.current[index] = null; }
    setPlaying(p => p.map((v, j) => j === index ? false : v));
  }, []);

  const playStem = useCallback((index: number, trimStart: number, trimEnd: number, inEditMode: boolean) => {
    audioRefs.current.forEach((a, i) => {
      if (i !== index && a) { a.pause(); a.src = ""; audioRefs.current[i] = null; }
    });
    setPlaying(p => p.map((_, j) => j === index));

    const label = STEM_LABELS[index];
    const audio = new Audio(`http://127.0.0.1:8765/api/stems/${label}.wav`);
    audioRefs.current[index] = audio;

    if (inEditMode) {
      audio.addEventListener("loadedmetadata", () => {
        if (Number.isFinite(audio.duration) && audio.duration > 0) {
          audio.currentTime = trimStart * audio.duration;
        }
      }, { once: true });

      const onTime = () => {
        if (Number.isFinite(audio.duration) && audio.currentTime >= trimEnd * audio.duration) {
          audio.pause();
          audio.src = "";
          audio.removeEventListener("timeupdate", onTime);
          if (audioRefs.current[index] === audio) audioRefs.current[index] = null;
          setPlaying(p => p.map((v, j) => j === index ? false : v));
        }
      };
      audio.addEventListener("timeupdate", onTime);
    }

    audio.onended = () => {
      if (audioRefs.current[index] === audio) audioRefs.current[index] = null;
      setPlaying(p => p.map((v, j) => j === index ? false : v));
    };
    audio.onerror = () => {
      if (audioRefs.current[index] === audio) audioRefs.current[index] = null;
      setPlaying(p => p.map((v, j) => j === index ? false : v));
    };
    void audio.play();
  }, []);

  const openEdit = useCallback((index: number) => {
    setEditingBank(prev => prev === index ? null : index);
  }, []);

  const state: StemSplitState =
    localState === "uploading" ? "uploading"
    : (stemSplitState as StemSplitState) ?? "idle";

  const upload = useCallback(async (file: File) => {
    setLocalState("uploading");
    setUploadProgress(0);
    try {
      await new Promise<void>((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open("POST", "http://127.0.0.1:8765/api/split-audio");
        xhr.setRequestHeader("Content-Type", file.type || "audio/wav");
        xhr.upload.onprogress = ev => {
          if (ev.lengthComputable && ev.total > 0)
            setUploadProgress(Math.min(0.98, ev.loaded / ev.total));
        };
        xhr.onload = () => xhr.status >= 200 && xhr.status < 300
          ? (setUploadProgress(1), resolve())
          : reject(new Error(`${xhr.status}`));
        xhr.onerror = () => reject(new Error("upload failed"));
        xhr.send(file);
      });
      setLocalState("idle");
    } catch {
      setLocalState("error");
    }
  }, []);

  const onFiles = useCallback((files: FileList | null) => {
    if (files?.length) upload(files[0]);
  }, [upload]);

  const busy = state === "uploading" || state === "running";
  const progress =
    state === "uploading" ? Math.max(0.02, uploadProgress)
    : state === "running" ? Math.max(0.02, Math.min(0.98, stemSplitProgress || 0))
    : state === "done" ? 1 : 0;
  const progressPct = Math.round(progress * 100);

  const statusLabel = (() => {
    if (state === "uploading") return "Uploading…";
    if (state === "running") return stemSplitDetail || "Separating stems…";
    if (state === "done") return "Stems loaded into banks 1–6";
    if (state === "error") return stemSplitError || "Upload failed";
    return null;
  })();

  const hasLog = stemSplitLog.length > 0;
  const showLogBtn = (state === "running" || state === "done" || state === "error") && hasLog;
  const borderColor = dragging ? "#9ca7ff"
    : state === "done" ? "rgba(134,239,172,0.4)"
    : state === "error" ? "rgba(251,113,133,0.4)"
    : "rgba(255,255,255,0.18)";

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
      <div
        onDragOver={e => { e.preventDefault(); setDragging(true); }}
        onDragLeave={() => setDragging(false)}
        onDrop={e => { e.preventDefault(); setDragging(false); onFiles(e.dataTransfer.files); }}
        onClick={() => !busy && fileRef.current?.click()}
        style={{
          border: `1.5px dashed ${borderColor}`,
          borderRadius: 12, padding: "14px 16px",
          cursor: busy ? "default" : "pointer",
          background: dragging ? "rgba(156,167,255,0.07)" : "rgba(255,255,255,0.03)",
          transition: "border-color 0.15s, background 0.15s",
          fontSize: "0.82rem",
        }}
      >
        {state === "idle" || state === "uploading" ? (
          state === "uploading" ? (
            <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
              <div style={{ display: "flex", justifyContent: "space-between", color: "rgba(255,255,255,0.6)" }}>
                <span>Uploading…</span>
                <span style={{ fontVariantNumeric: "tabular-nums" }}>{progressPct}%</span>
              </div>
              <div style={{ height: 5, borderRadius: 999, overflow: "hidden", background: "rgba(255,255,255,0.1)" }}>
                <div style={{ width: `${progressPct}%`, height: "100%", borderRadius: 999, background: "#9ca7ff", transition: "width 0.2s ease" }} />
              </div>
            </div>
          ) : (
            <span style={{ color: "rgba(255,255,255,0.5)" }}>Drop audio file or click to browse</span>
          )
        ) : (
          <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
            <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", gap: 10 }}>
              <span style={{
                fontSize: "0.78rem", minWidth: 0, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap",
                color: state === "done" ? "#86efac" : state === "error" ? "#fb7185" : "rgba(255,255,255,0.7)",
              }}>
                {statusLabel}
              </span>
              <div style={{ display: "flex", alignItems: "center", gap: 8, flexShrink: 0 }}>
                {showLogBtn && (
                  <button
                    type="button"
                    onClick={e => { e.stopPropagation(); setShowLog(v => !v); }}
                    style={{
                      padding: "2px 8px", borderRadius: 4, fontSize: "0.65rem",
                      background: showLog ? "rgba(156,167,255,0.18)" : "rgba(255,255,255,0.07)",
                      border: `1px solid ${showLog ? "rgba(156,167,255,0.5)" : "rgba(255,255,255,0.15)"}`,
                      color: showLog ? "#9ca7ff" : "rgba(255,255,255,0.4)",
                      cursor: "pointer",
                    }}
                  >log</button>
                )}
                {state === "running" && (
                  <span style={{ fontVariantNumeric: "tabular-nums", fontSize: "0.75rem", color: "rgba(255,255,255,0.45)" }}>{progressPct}%</span>
                )}
              </div>
            </div>
            {state === "running" && (
              <div style={{ height: 5, borderRadius: 999, overflow: "hidden", background: "rgba(255,255,255,0.1)" }}>
                <div style={{ width: `${progressPct}%`, height: "100%", borderRadius: 999, background: "#9ca7ff", transition: "width 0.25s ease" }} />
              </div>
            )}
            {(state === "done" || state === "error") && (
              <span style={{ fontSize: "0.7rem", color: "rgba(255,255,255,0.3)" }}>drop a new file to replace</span>
            )}
          </div>
        )}
        <input ref={fileRef} type="file" accept="audio/*" style={{ display: "none" }} onChange={e => onFiles(e.target.files)} />
      </div>

      {showLog && hasLog && (
        <div
          ref={logRef}
          style={{
            maxHeight: 140, overflowY: "auto", borderRadius: 6,
            background: "rgba(0,0,0,0.45)", border: "1px solid rgba(255,255,255,0.1)",
            padding: "6px 8px", fontFamily: "monospace", fontSize: "0.65rem",
            color: "rgba(255,255,255,0.55)", lineHeight: 1.5,
            whiteSpace: "pre-wrap", wordBreak: "break-all",
          }}
        >
          {stemSplitLog.map((line, i) => <div key={i}>{line}</div>)}
        </div>
      )}

      {state === "done" && (
        <>
          <div style={{ display: "flex", gap: 5 }}>
            {STEM_LABELS.map((label, i) => {
              const isPlaying = playing[i];
              const isEditing = editingBank === i;
              const inEditMode = isEditing;
              return (
                <div key={label} style={{ flex: 1, minWidth: 0, display: "flex", flexDirection: "column", gap: 4 }}>
                  <button
                    type="button"
                    onClick={() => isPlaying
                      ? stopStem(i)
                      : playStem(i, sampleTrimStart, sampleTrimEnd, inEditMode)}
                    style={{
                      padding: "5px 0", borderRadius: 6,
                      background: isPlaying ? `${BANK_COLORS[i]}33` : `${BANK_COLORS[i]}11`,
                      border: `1px solid ${isPlaying ? BANK_COLORS[i] : BANK_COLORS[i] + "55"}`,
                      textAlign: "center", fontSize: "0.68rem",
                      color: BANK_COLORS[i], cursor: "pointer",
                      display: "flex", flexDirection: "column", alignItems: "center", gap: 2,
                      position: "relative", overflow: "hidden",
                    }}
                  >
                    {isPlaying && (
                      <div style={{
                        position: "absolute", bottom: 0, left: 0, height: 2,
                        width: `${(playPositions[i] * 100).toFixed(1)}%`,
                        background: BANK_COLORS[i], borderRadius: "0 0 6px 6px",
                      }} />
                    )}
                    <span>{label}</span>
                    <span style={{ fontSize: "0.58rem", opacity: 0.7 }}>{isPlaying ? "stop" : "play"}</span>
                  </button>
                  <button
                    type="button"
                    onClick={() => openEdit(i)}
                    style={{
                      padding: "4px 0", borderRadius: 6,
                      background: isEditing ? `${BANK_COLORS[i]}22` : "rgba(255,255,255,0.04)",
                      border: `1px solid ${isEditing ? BANK_COLORS[i] + "88" : "rgba(255,255,255,0.12)"}`,
                      textAlign: "center", fontSize: "0.62rem",
                      color: isEditing ? BANK_COLORS[i] : "rgba(255,255,255,0.55)",
                      cursor: "pointer",
                    }}
                  >
                    {isEditing ? "close" : "edit"}
                  </button>
                </div>
              );
            })}
          </div>

          {editingBank !== null && (
            <div style={{ display: "flex", flexDirection: "column", gap: 8, paddingTop: 6 }}>
              <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
                <span style={{ fontSize: "0.72rem", color: BANK_COLORS[editingBank], fontWeight: 600, letterSpacing: "0.06em", textTransform: "uppercase" }}>
                  {STEM_LABELS[editingBank]} · trim to sample
                </span>
                <span style={{ fontSize: "0.68rem", color: "rgba(255,255,255,0.35)" }}>drag handles · play previews trimmed region</span>
              </div>
              <TrimWave
                waveform={stems[editingBank]?.waveform ?? sampleWaveform}
                trimStart={sampleTrimStart}
                trimEnd={sampleTrimEnd}
                color={BANK_COLORS[editingBank]}
                playPosition={playing[editingBank] ? playPositions[editingBank] : undefined}
                onChange={next => send({
                  type: "setSampleTrim",
                  start: next.start ?? sampleTrimStart,
                  end: next.end ?? sampleTrimEnd,
                })}
              />
              <button
                type="button"
                onClick={() => send({
                  type: "loadStemToBank",
                  stemIdx: editingBank,
                  bank: activeBank,
                  trimStart: sampleTrimStart,
                  trimEnd: sampleTrimEnd,
                })}
                style={{
                  padding: "6px 0", borderRadius: 6,
                  background: `${BANK_COLORS[editingBank]}22`,
                  border: `1px solid ${BANK_COLORS[editingBank]}88`,
                  color: BANK_COLORS[editingBank],
                  fontSize: "0.75rem", fontWeight: 600, cursor: "pointer",
                }}
              >
                Save to bank {activeBank + 1}
              </button>
            </div>
          )}
        </>
      )}
    </div>
  );
}
