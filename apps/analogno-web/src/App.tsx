type ControlValue = {
  label: string;
  value: string;
};

const controllerValues: ControlValue[] = [
  { label: "Left Stick", value: "pitch bend" },
  { label: "Right Stick", value: "filter / resonance" },
  { label: "L2", value: "expression + vibrato depth" },
  { label: "R2", value: "modulation" },
  { label: "Face Buttons", value: "scale notes" },
  { label: "D-pad", value: "root / octave" },
  { label: "L1 / R1", value: "scale select" },
  { label: "Gyro", value: "motion vibrato" }
];

const midiValues: ControlValue[] = [
  { label: "Port", value: "Analogno MIDI Out" },
  { label: "Channel", value: "1" },
  { label: "Notes", value: "polyphonic" },
  { label: "Pitch Bend", value: "enabled" },
  { label: "CC 11", value: "expression" },
  { label: "CC 74", value: "filter cutoff" },
  { label: "CC 71", value: "resonance" },
  { label: "CC 1", value: "modulation" }
];

function StatusPill({ connected }: { connected: boolean }) {
  return (
    <div className={connected ? "pill pill-ok" : "pill pill-wait"}>
      {connected ? "runtime online" : "waiting for runtime"}
    </div>
  );
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

function ValueGrid({ values }: { values: ControlValue[] }) {
  return (
    <div className="value-grid">
      {values.map((item) => (
        <div className="value-card" key={item.label}>
          <span>{item.label}</span>
          <strong>{item.value}</strong>
        </div>
      ))}
    </div>
  );
}

function SequencerMock() {
  return (
    <div className="sequencer">
      {Array.from({ length: 16 }, (_, index) => (
        <button
          className={index % 4 === 0 ? "step step-accent" : "step"}
          key={index}
          type="button"
        >
          {index + 1}
        </button>
      ))}
    </div>
  );
}

export function App() {
  return (
    <main className="app">
      <header className="hero">
        <div>
          <p className="eyebrow">DualSense MIDI workstation</p>
          <h1>Analogno</h1>
          <p className="subtitle">
            A controller-first music system. C++ owns the runtime. This page is
            the cockpit.
          </p>
        </div>

        <StatusPill connected={false} />
      </header>

      <div className="layout">
        <Panel title="Controller mapping">
          <ValueGrid values={controllerValues} />
        </Panel>

        <Panel title="MIDI output">
          <ValueGrid values={midiValues} />
        </Panel>

        <Panel title="Current musical state">
          <div className="state-line">
            <span>Root</span>
            <strong>C2 / MIDI 48</strong>
          </div>
          <div className="state-line">
            <span>Scale</span>
            <strong>minor pentatonic</strong>
          </div>
          <div className="state-line">
            <span>Octave offset</span>
            <strong>0</strong>
          </div>
          <div className="state-line">
            <span>Active notes</span>
            <strong>none</strong>
          </div>
        </Panel>

        <Panel title="Sequencer preview">
          <SequencerMock />
          <p className="hint">
            Static mock for now. Next UI milestone connects this to the C++
            runtime over WebSocket.
          </p>
        </Panel>
      </div>
    </main>
  );
}
