#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANALOGNO_BIN="${ROOT_DIR}/build/debug/analogno"
WEB_DIR="${ROOT_DIR}/apps/analogno-web"
SOUNDFONT="/usr/share/sounds/sf2/FluidR3_GM.sf2"

FLUID_LOG="${TMPDIR:-/tmp}/analogno-fluidsynth.log"
WEB_LOG="${TMPDIR:-/tmp}/analogno-web.log"

if [[ ! -x "${ANALOGNO_BIN}" ]]; then
  echo "building Analogno..."
  cmake --build --preset debug
fi

if [[ ! -x "${ANALOGNO_BIN}" ]]; then
  echo "error: missing ${ANALOGNO_BIN}"
  echo "run: cmake --preset debug && cmake --build --preset debug"
  exit 1
fi

if [[ ! -f "${SOUNDFONT}" ]]; then
  echo "error: missing soundfont: ${SOUNDFONT}"
  echo "run: sudo apt install fluid-soundfont-gm"
  exit 1
fi

if [[ ! -d "${WEB_DIR}/node_modules" ]]; then
  echo "installing web dependencies..."
  (cd "${WEB_DIR}" && npm install)
fi

# Free port 5173 if a stale process holds it.
if lsof -ti tcp:5173 &>/dev/null; then
  echo "freeing port 5173..."
  kill $(lsof -ti tcp:5173) 2>/dev/null || true
  sleep 0.5
fi

cleanup() {
  echo
  echo "stopping Analogno dev stack..."

  if [[ -n "${WEB_PID:-}" ]]; then
    kill "${WEB_PID}" 2>/dev/null || true
  fi

  if [[ -n "${ANALOGNO_PID:-}" ]]; then
    kill "${ANALOGNO_PID}" 2>/dev/null || true
  fi

  if [[ -n "${FLUID_PID:-}" ]]; then
    kill "${FLUID_PID}" 2>/dev/null || true
  fi

  wait 2>/dev/null || true
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

wait_for_client() {
  local name="$1"
  local client=""

  for _ in {1..80}; do
    client="$(find_alsa_client "${name}" || true)"

    if [[ -n "${client}" ]]; then
      echo "${client}"
      return 0
    fi

    sleep 0.1
  done

  return 1
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

for _ in {1..80}; do
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

echo "starting Analogno runtime..."

fuser -k 8765/tcp 2>/dev/null || true

"${ANALOGNO_BIN}" --soundfont "${SOUNDFONT}" &
ANALOGNO_PID=$!

echo "waiting for MIDI ports..."

if ! ANALOGNO_CLIENT="$(wait_for_client "Analogno")"; then
  echo "error: could not find Analogno MIDI client"
  echo
  aconnect -l
  exit 1
fi

echo "connecting MIDI: Analogno ${ANALOGNO_CLIENT}:0 -> FluidSynth ${FLUID_CLIENT}:0"

aconnect "${ANALOGNO_CLIENT}:0" "${FLUID_CLIENT}:0"

echo "starting React web UI..."

(
  cd "${WEB_DIR}"
  npm run dev
) >"${WEB_LOG}" 2>&1 &

WEB_PID=$!

sleep 1

if ! kill -0 "${WEB_PID}" 2>/dev/null; then
  echo "error: web UI failed to start"
  echo "log:"
  cat "${WEB_LOG}"
  exit 1
fi

echo
echo "Analogno dev stack ready."
echo
echo "Runtime:"
echo "  WebSocket: ws://127.0.0.1:8765"
echo "  Web UI:    http://127.0.0.1:5173"
echo
echo "Logs:"
echo "  FluidSynth: ${FLUID_LOG}"
echo "  Web UI:     ${WEB_LOG}"
echo
echo "Press Ctrl+C here to stop everything."
echo

wait "${ANALOGNO_PID}"
