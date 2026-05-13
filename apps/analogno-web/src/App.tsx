import { useEffect, useMemo, useState } from "react";

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
  };
  audio: {
    devices: Array<{
      index: number;
      name: string;
      isDefault: boolean;
    }>;
    selectedDeviceIndex: number | null;
    captureRunning: boolean;
    captureDevice: string;
    micLevel: number;
    waveform: number[];
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
  children
}: {
  title: string;
  children: React.ReactNode;
}) {
  return (
    <section className="panel">
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

  const controller = runtime?.controller;
  const music = runtime?.music;
  const audio = runtime?.audio;

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
          <Meter label="L2 / expression" value={controller?.l2 ?? 0} />
          <Meter label="R2 / modulation" value={controller?.r2 ?? 0} />
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

        <Panel title="Music">
          <StateLine label="Root" value={music ? `${midiNoteName(music.rootMidiNote)} / ${music.rootMidiNote}` : "—"} />
          <StateLine label="Scale" value={music?.scale ?? "—"} />
          <StateLine label="Octave offset" value={music?.octaveOffset ?? "—"} />
          <StateLine label="Active notes" value={activeNoteText} />
        </Panel>

        <Panel title="Continuous controls">
          <BipolarMeter label="Pitch bend" value={music?.pitchBend ?? 0} />
          <Meter label="Expression" value={music?.expression ?? 0} />
          <Meter label="Filter cutoff" value={music?.filterCutoff ?? 0} />
          <Meter label="Resonance" value={music?.filterResonance ?? 0} />
          <Meter label="Modulation" value={music?.modulation ?? 0} />
          <Meter label="Vibrato" value={music?.vibrato ?? 0} />
        </Panel>

        <Panel title="Microphone">
          <StateLine
            label="Capture"
            value={audio?.captureRunning ? "running" : "stopped"}
          />
          <StateLine label="Device" value={audio?.captureDevice ?? "none"} />
          <label className="field">
            <span>Input</span>
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
          <Waveform samples={audio?.waveform ?? []} />
        </Panel>
      </div>
    </main>
  );
}
