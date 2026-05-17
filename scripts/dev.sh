#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANALOGNO_BIN="${ROOT_DIR}/build/debug/analogno"
WEB_DIR="${ROOT_DIR}/apps/analogno-web"
SOUNDFONT="/usr/share/sounds/sf2/FluidR3_GM.sf2"

FLUID_LOG="${TMPDIR:-/tmp}/analogno-fluidsynth.log"
WEB_LOG="${TMPDIR:-/tmp}/analogno-web.log"
ANALOGNO_LOG="${TMPDIR:-/tmp}/analogno-runtime.log"

WEB_HOST="127.0.0.1"
WEB_PORT="5173"
RUNTIME_HOST="127.0.0.1"
RUNTIME_PORT="8765"

FLUID_FIFO=""

echo "building Analogno..."
cmake --preset debug
cmake --build --preset debug

if [[ ! -x "${ANALOGNO_BIN}" ]]; then
  echo "error: missing ${ANALOGNO_BIN}"
  exit 1
fi

if [[ ! -f "${SOUNDFONT}" ]]; then
  echo "error: missing soundfont: ${SOUNDFONT}"
  echo "run: sudo apt install fluid-soundfont-gm"
  exit 1
fi

if [[ ! -d "${WEB_DIR}" ]]; then
  echo "error: missing web dir: ${WEB_DIR}"
  exit 1
fi

if [[ ! -d "${WEB_DIR}/node_modules" ]]; then
  echo "installing web dependencies..."
  (cd "${WEB_DIR}" && npm install)
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

  if [[ -n "${FLUID_STDIN_FD:-}" ]]; then
    eval "exec ${FLUID_STDIN_FD}>&-" 2>/dev/null || true
  fi

  if [[ -n "${FLUID_FIFO:-}" && -p "${FLUID_FIFO}" ]]; then
    rm -f "${FLUID_FIFO}" 2>/dev/null || true
  fi

  pkill -x fluidsynth 2>/dev/null || true
  pkill -f "${ANALOGNO_BIN}" 2>/dev/null || true
  pkill -f "node.*vite" 2>/dev/null || true
  pkill -f "vite.*${WEB_PORT}" 2>/dev/null || true

  wait 2>/dev/null || true
}

trap cleanup EXIT INT TERM

kill_port() {
  local port="$1"
  local pids=""

  pids="$(lsof -ti "tcp:${port}" 2>/dev/null || true)"

  if [[ -n "${pids}" ]]; then
    echo "freeing port ${port}..."
    echo "${pids}" | xargs -r kill 2>/dev/null || true
    sleep 0.5
  fi

  pids="$(lsof -ti "tcp:${port}" 2>/dev/null || true)"

  if [[ -n "${pids}" ]]; then
    echo "${pids}" | xargs -r kill -9 2>/dev/null || true
    sleep 0.5
  fi
}

kill_old_processes() {
  pkill -x fluidsynth 2>/dev/null || true
  pkill -f "${ANALOGNO_BIN}" 2>/dev/null || true
  pkill -f "node.*vite" 2>/dev/null || true
  pkill -f "vite.*${WEB_PORT}" 2>/dev/null || true
  sleep 0.5
}

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

  for _ in {1..100}; do
    client="$(find_alsa_client "${name}" || true)"

    if [[ -n "${client}" ]]; then
      echo "${client}"
      return 0
    fi

    sleep 0.1
  done

  return 1
}

is_port_listening() {
  local port="$1"

  ss -ltnH 2>/dev/null | awk '{print $4}' |
    grep -Eq "(\[::\]:${port}|0\.0\.0\.0:${port}|127\.0\.0\.1:${port}|\[::1\]:${port}|:${port})$"
}

wait_for_listen_port() {
  local port="$1"

  for _ in {1..100}; do
    if is_port_listening "${port}"; then
      return 0
    fi

    sleep 0.1
  done

  return 1
}

is_midi_connected() {
  local from_client="$1"
  local to_client="$2"

  aconnect -l 2>/dev/null | awk \
    -v from="${from_client}" \
    -v to="${to_client}:0" '
      $1 == "client" {
        current = $2
        gsub(":", "", current)
      }

      current == from && index($0, "Connecting To:") && index($0, to) {
        found = 1
      }

      END {
        exit found ? 0 : 1
      }
    '
}

connect_midi() {
  local analogno_client="$1"
  local fluid_client="$2"

  echo "connecting MIDI: Analogno ${analogno_client}:0 -> FluidSynth ${fluid_client}:0"

  aconnect "${analogno_client}:0" "${fluid_client}:0"

  if ! is_midi_connected "${analogno_client}" "${fluid_client}"; then
    echo "error: MIDI connection did not stick"
    echo
    aconnect -l || true
    exit 1
  fi
}

: > "${FLUID_LOG}"
: > "${WEB_LOG}"
: > "${ANALOGNO_LOG}"

echo "resetting old processes and ALSA MIDI connections..."
kill_old_processes
kill_port "${WEB_PORT}"
kill_port "${RUNTIME_PORT}"
aconnect -x 2>/dev/null || true

if lsof -ti "tcp:${WEB_PORT}" &>/dev/null; then
  echo "error: port ${WEB_PORT} is still in use"
  lsof -nP -iTCP:"${WEB_PORT}" -sTCP:LISTEN || true
  exit 1
fi

if lsof -ti "tcp:${RUNTIME_PORT}" &>/dev/null; then
  echo "error: port ${RUNTIME_PORT} is still in use"
  lsof -nP -iTCP:"${RUNTIME_PORT}" -sTCP:LISTEN || true
  exit 1
fi

echo "starting FluidSynth..."

FLUID_FIFO="$(mktemp -u "${TMPDIR:-/tmp}/analogno-fluidsynth-stdin.XXXXXX")"
mkfifo "${FLUID_FIFO}"

fluidsynth \
  -a pulseaudio \
  -m alsa_seq \
  -o midi.autoconnect=0 \
  -o synth.polyphony=512 \
  "${SOUNDFONT}" < "${FLUID_FIFO}" > >(tee "${FLUID_LOG}") 2>&1 &

FLUID_PID=$!

exec {FLUID_STDIN_FD}>"${FLUID_FIFO}"

echo "waiting for FluidSynth MIDI port..."

if ! FLUID_CLIENT="$(wait_for_client "FLUID Synth")"; then
  echo "error: could not find FluidSynth MIDI client"
  echo
  echo "ALSA clients:"
  aconnect -l || true
  echo
  echo "FluidSynth log:"
  cat "${FLUID_LOG}"
  exit 1
fi

if ! kill -0 "${FLUID_PID}" 2>/dev/null; then
  echo "error: FluidSynth died immediately"
  echo
  echo "FluidSynth log:"
  cat "${FLUID_LOG}"
  exit 1
fi

echo "starting Analogno runtime..."

"${ANALOGNO_BIN}" --soundfont "${SOUNDFONT}" > >(tee "${ANALOGNO_LOG}") 2>&1 &

ANALOGNO_PID=$!

echo "waiting for Analogno WebSocket listener on ws://${RUNTIME_HOST}:${RUNTIME_PORT}..."

for _ in {1..100}; do
  if ! kill -0 "${ANALOGNO_PID}" 2>/dev/null; then
    echo "error: Analogno runtime stopped before opening WebSocket"
    echo
    echo "Analogno runtime log:"
    cat "${ANALOGNO_LOG}"
    exit 1
  fi

  if wait_for_listen_port "${RUNTIME_PORT}"; then
    break
  fi

  sleep 0.1
done

if ! wait_for_listen_port "${RUNTIME_PORT}"; then
  echo "error: Analogno WebSocket did not listen on ws://${RUNTIME_HOST}:${RUNTIME_PORT}"
  echo
  echo "Listening ports:"
  ss -ltnp | grep -E "${WEB_PORT}|${RUNTIME_PORT}" || true
  echo
  echo "Analogno runtime log:"
  cat "${ANALOGNO_LOG}"
  exit 1
fi

echo "waiting for Analogno MIDI port..."

if ! ANALOGNO_CLIENT="$(wait_for_client "Analogno")"; then
  echo "error: could not find Analogno MIDI client"
  echo
  echo "ALSA clients:"
  aconnect -l || true
  echo
  echo "Analogno runtime log:"
  cat "${ANALOGNO_LOG}"
  exit 1
fi

connect_midi "${ANALOGNO_CLIENT}" "${FLUID_CLIENT}"

echo "starting React web UI..."

(
  cd "${WEB_DIR}"
  export VITE_ANALOGNO_WS_URL="ws://${RUNTIME_HOST}:${RUNTIME_PORT}"
  exec npm run dev -- --port "${WEB_PORT}" --strictPort
) > >(tee "${WEB_LOG}") 2>&1 &

WEB_PID=$!

echo "waiting for React web UI listener on http://${WEB_HOST}:${WEB_PORT}..."

for _ in {1..100}; do
  if ! kill -0 "${WEB_PID}" 2>/dev/null; then
    echo "error: web UI failed to start"
    echo
    echo "Web UI log:"
    cat "${WEB_LOG}"
    exit 1
  fi

  if wait_for_listen_port "${WEB_PORT}"; then
    break
  fi

  sleep 0.1
done

if ! wait_for_listen_port "${WEB_PORT}"; then
  echo "error: web UI did not listen on http://${WEB_HOST}:${WEB_PORT}"
  echo
  echo "Web UI log:"
  cat "${WEB_LOG}"
  exit 1
fi

echo
echo "Analogno dev stack ready."
echo
echo "Runtime:"
echo "  WebSocket: ws://${RUNTIME_HOST}:${RUNTIME_PORT}"
echo "  Web UI:    http://${WEB_HOST}:${WEB_PORT}"
echo
echo "MIDI:"
echo "  Analogno:   ${ANALOGNO_CLIENT}:0"
echo "  FluidSynth: ${FLUID_CLIENT}:0"
echo
echo "Logs:"
echo "  FluidSynth: ${FLUID_LOG}"
echo "  Runtime:    ${ANALOGNO_LOG}"
echo "  Web UI:     ${WEB_LOG}"
echo
echo "Press Ctrl+C here to stop everything."
echo

while true; do
  if ! kill -0 "${WEB_PID}" 2>/dev/null; then
    echo "error: web UI stopped"
    echo
    echo "Web UI log:"
    cat "${WEB_LOG}"
    exit 1
  fi

  if ! kill -0 "${ANALOGNO_PID}" 2>/dev/null; then
    echo "error: Analogno runtime stopped"
    echo
    echo "Analogno runtime log:"
    cat "${ANALOGNO_LOG}"
    exit 1
  fi

  if ! kill -0 "${FLUID_PID}" 2>/dev/null; then
    echo "error: FluidSynth stopped"
    echo
    echo "FluidSynth log:"
    cat "${FLUID_LOG}"
    exit 1
  fi

  CURRENT_FLUID_CLIENT="$(find_alsa_client "FLUID Synth" || true)"
  CURRENT_ANALOGNO_CLIENT="$(find_alsa_client "Analogno" || true)"

  if [[ -z "${CURRENT_FLUID_CLIENT}" ]]; then
    echo "error: FluidSynth ALSA client disappeared"
    echo
    aconnect -l || true
    exit 1
  fi

  if [[ -z "${CURRENT_ANALOGNO_CLIENT}" ]]; then
    echo "error: Analogno ALSA client disappeared"
    echo
    aconnect -l || true
    exit 1
  fi

  if [[ "${CURRENT_FLUID_CLIENT}" != "${FLUID_CLIENT}" || "${CURRENT_ANALOGNO_CLIENT}" != "${ANALOGNO_CLIENT}" ]]; then
    echo "MIDI client number changed; reconnecting..."
    FLUID_CLIENT="${CURRENT_FLUID_CLIENT}"
    ANALOGNO_CLIENT="${CURRENT_ANALOGNO_CLIENT}"
    connect_midi "${ANALOGNO_CLIENT}" "${FLUID_CLIENT}"
  fi

  if ! is_midi_connected "${ANALOGNO_CLIENT}" "${FLUID_CLIENT}"; then
    echo "MIDI connection missing; reconnecting..."
    connect_midi "${ANALOGNO_CLIENT}" "${FLUID_CLIENT}"
  fi

  sleep 1
done