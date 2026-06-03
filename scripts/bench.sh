#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT}/build/debug/stress_sampler"

echo "=== analogno sampler benchmark ==="
echo

# System info
echo "System:"
echo "  CPU  : $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //' || echo 'unknown')"
echo "  Cores: $(nproc)"
echo "  OS   : $(uname -sr)"
echo "  Mem  : $(awk '/MemTotal/{printf "%.0f MiB\n", $2/1024}' /proc/meminfo)"
echo

# Build
echo "Building stress_sampler (debug)..."
cmake --preset debug -Wno-dev > /dev/null
cmake --build --preset debug --target stress_sampler 2>&1 | tail -5
echo

if [[ ! -x "${BIN}" ]]; then
    echo "ERROR: ${BIN} not found after build."
    exit 1
fi

# Run with /usr/bin/time for RSS + CPU summary
echo "Running benchmark..."
echo

if command -v /usr/bin/time &>/dev/null; then
    /usr/bin/time -v "${BIN}" 2>&1 | awk '
        /^\s*Maximum resident/ { mem=$0 }
        /^\s*Percent of CPU/   { cpu=$0 }
        { print }
        END {
            if (mem) print "\n[resource summary]"
            if (cpu) print cpu
            if (mem) print mem
        }
    '
else
    "${BIN}"
fi
