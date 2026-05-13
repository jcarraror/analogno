#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANALOGNO_BIN="${ROOT_DIR}/build/debug/analogno"
SOUNDFONT="/usr/share/sounds/sf2/FluidR3_GM.sf2"
FLUID_LOG="${TMPDIR:-/tmp}/analogno-fluidsynth.log"

if [[ ! -x "${ANALOGNO_BIN}" ]]; then
  echo "error: missing ${ANALOGNO_BIN}"
  echo "run: cmake --build --preset debug"
  exit 1
fi

if [[ ! -f "${SOUNDFONT}" ]]; then
  echo "error: missing soundfont: ${SOUNDFONT}"
  echo "run: sudo apt install fluid-soundfont-gm"
  exit 1
fi

cleanup() {
  echo
  echo "stopping..."
  if [[ -n "${ANALOGNO_PID:-}" ]]; then
    kill "${ANALOGNO_PID}" 2>/dev/null || true
  fi
  if [[ -n "${FLUID_PID:-}" ]]; then
    kill "${FLUID_PID}" 2>/dev/null || true
  fi
}

trap cleanup EXIT INT TERM

find_alsa_client() {
  local name="$1"

  aconnect -l 2>/dev/null | awk -v needle="$name" '
    $1 == "client" && index($0, needle) {
      gsub(":", "", $2)
      print $2
      exit
    }
  '
}

echo "starting FluidSynth..."

fluidsynth \
  --server \
  --no-shell \
  -a pulseaudio \
  -m alsa_seq \
  -o synth.polyphony=512 \
  "${SOUNDFONT}" >"${FLUID_LOG}" 2>&1 &

FLUID_PID=$!

echo "waiting for FluidSynth MIDI port..."

FLUID_CLIENT=""

for _ in {1..50}; do
  if ! kill -0 "${FLUID_PID}" 2>/dev/null; then
    echo "error: FluidSynth failed to start"
    echo "log:"
    cat "${FLUID_LOG}"
    exit 1
  fi

  FLUID_CLIENT="$(find_alsa_client "FLUID Synth" || true)"

  if [[ -n "${FLUID_CLIENT}" ]]; then
    break
  fi

  sleep 0.1
done

if [[ -z "${FLUID_CLIENT}" ]]; then
  echo "error: could not find FluidSynth MIDI client"
  echo
  aconnect -l
  echo
  echo "FluidSynth log:"
  cat "${FLUID_LOG}"
  exit 1
fi

echo "starting Analogno..."

"${ANALOGNO_BIN}" &
ANALOGNO_PID=$!

echo "waiting for Analogno MIDI port..."

ANALOGNO_CLIENT=""

for _ in {1..50}; do
  ANALOGNO_CLIENT="$(find_alsa_client "Analogno" || true)"

  if [[ -n "${ANALOGNO_CLIENT}" ]]; then
    break
  fi

  sleep 0.1
done

if [[ -z "${ANALOGNO_CLIENT}" ]]; then
  echo "error: could not find Analogno MIDI client"
  echo
  aconnect -l
  exit 1
fi

echo "connecting MIDI: Analogno ${ANALOGNO_CLIENT}:0 -> FluidSynth ${FLUID_CLIENT}:0"

aconnect "${ANALOGNO_CLIENT}:0" "${FLUID_CLIENT}:0"

echo
echo "ready."
echo "Play with the controller."
echo "Press Ctrl+C here to stop both Analogno and FluidSynth."
echo

wait "${ANALOGNO_PID}"
