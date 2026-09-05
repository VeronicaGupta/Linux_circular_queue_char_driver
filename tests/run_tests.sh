#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="$ROOT_DIR/tests"
RESULT_DIR="$TEST_DIR/results"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
REPORT="$RESULT_DIR/test_report_$TIMESTAMP.txt"

mkdir -p "$RESULT_DIR"

log() {
    printf '%s\n' "$*" | tee -a "$REPORT"
}

run_logged() {
    log ""
    log "> $*"
    "$@" 2>&1 | tee -a "$REPORT"
    return "${PIPESTATUS[0]}"
}

run_capture() {
    local output_file="$1"
    shift
    log ""
    log "> $*"
    "$@" 2>&1 | tee -a "$REPORT" "$output_file"
    return "${PIPESTATUS[0]}"
}

module_loaded() {
    lsmod | awk '{print $1}' | grep -qx "$1"
}

unload_modules() {
    module_loaded lock_free_char && sudo rmmod lock_free_char || true
    module_loaded thread_safe_char && sudo rmmod thread_safe_char || true
    module_loaded simple_char && sudo rmmod simple_char || true
}

load_module() {
    local module_path="$1"
    sudo insmod "$module_path"
    sleep 0.2
}

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Linux is required."
    exit 1
fi

make -C "$TEST_DIR" all
sudo -v
unload_modules
trap unload_modules EXIT

log "Linux Character Device Test Report"
log "Generated: $(date --iso-8601=seconds)"
log "Kernel: $(uname -r)"
log "CPU: $(nproc) logical CPUs"

overall_status=0

log ""
log "=== BASIC DRIVER ==="
load_module "$ROOT_DIR/01_simple_char/simple_char.ko"
run_logged "$TEST_DIR/boundary_tests" simple || overall_status=1
run_logged "$TEST_DIR/stress_tests" simple-race 4 10 || overall_status=1
sudo rmmod simple_char

log ""
log "=== THREAD-SAFE DRIVER ==="
load_module "$ROOT_DIR/02_thread_safe_char/thread_safe_char.ko"
run_logged "$TEST_DIR/boundary_tests" safe || overall_status=1
SAFE_STREAM_FILE="$RESULT_DIR/safe_stream_$TIMESTAMP.txt"
run_capture "$SAFE_STREAM_FILE" "$TEST_DIR/stress_tests" stream safe 64 1024 || overall_status=1
run_logged "$TEST_DIR/stress_tests" mpmc 4 4 50000 || overall_status=1
sudo rmmod thread_safe_char

log ""
log "=== LOCK-FREE SPSC DRIVER ==="
load_module "$ROOT_DIR/03_lock_free_char/lock_free_char.ko"
run_logged "$TEST_DIR/boundary_tests" lockfree || overall_status=1
LOCKFREE_STREAM_FILE="$RESULT_DIR/lockfree_stream_$TIMESTAMP.txt"
run_capture "$LOCKFREE_STREAM_FILE" "$TEST_DIR/stress_tests" stream lockfree 64 1024 || overall_status=1
sudo rmmod lock_free_char


log ""
log "=== COMPARISON SUMMARY ==="
SAFE_RATE="$(awk '/Throughput:/ {print $2; exit}' "$SAFE_STREAM_FILE" 2>/dev/null || true)"
LOCKFREE_RATE="$(awk '/Throughput:/ {print $2; exit}' "$LOCKFREE_STREAM_FILE" 2>/dev/null || true)"
log "simple_char       : no concurrent-correctness contract; overlapping writes are observable"
log "thread_safe_char  : MPMC correctness required and tested"
log "lock_free_char    : SPSC correctness required; additional readers/writers rejected"
log "thread-safe SPSC throughput: ${SAFE_RATE:-N/A} MiB/s"
log "lock-free SPSC throughput : ${LOCKFREE_RATE:-N/A} MiB/s"

if [[ -n "${SAFE_RATE:-}" && -n "${LOCKFREE_RATE:-}" ]]; then
    awk -v safe="$SAFE_RATE" -v lockfree="$LOCKFREE_RATE" \
        'BEGIN { if (safe > 0) printf "lock-free throughput delta: %+.2f%% versus mutex SPSC baseline\n", ((lockfree-safe)/safe)*100.0 }' \
        | tee -a "$REPORT"
fi

log ""
log "=== OPTIONAL PERF COUNTERS ==="
if command -v perf >/dev/null 2>&1; then
    log "perf is installed. Example comparison commands:"
    log "  perf stat -e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions,cache-misses $TEST_DIR/stress_tests stream safe 64 1024"
    log "  perf stat -e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions,cache-misses $TEST_DIR/stress_tests stream lockfree 64 1024"
else
    log "perf is not installed; kernel-independent stress metrics were still collected."
fi

log ""
if [[ "$overall_status" -eq 0 ]]; then
    log "OVERALL RESULT: PASS"
else
    log "OVERALL RESULT: FAIL"
fi
log "Report: $REPORT"

exit "$overall_status"
