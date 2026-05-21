#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"
LOGDIR="$ROOT/logs"
mkdir -p "$LOGDIR"
RESULT="$ROOT/result.txt"
PERF_FILE="$LOGDIR/perf_results.csv"
CONFIG="$ROOT/config"
SOCKET_PATH=$(grep '^SOCKET_PATH=' "$CONFIG" | cut -d'=' -f2-)

function die {
    echo "ERROR: $*" >&2
    exit 1
}

function wait_for_socket {
    local timeout=10
    local waited=0
    while [ ! -S "$SOCKET_PATH" ]; do
        if [ "$waited" -ge "$timeout" ]; then
            die "Server socket did not appear after $timeout seconds"
        fi
        sleep 0.2
        waited=$((waited + 1))
    done
}

function generate_numbers {
    local file="$ROOT/numbers.txt"
    if [ -f "$file" ]; then
        return
    fi
    echo "Generating numbers file $file"
    > "$file"
    for i in $(seq 1 500); do
        echo 1 >> "$file"
    done
    for i in $(seq 1 500); do
        echo -1 >> "$file"
    done
}

function start_server {
    rm -f "$SOCKET_PATH"
    echo "Building binaries..."
    make
    echo "Starting server..."
    ./brownian_server -c "$CONFIG" -l "$LOGDIR/server.log" > "$LOGDIR/server_stdout.log" 2>&1 &
    SERVER_PID=$!
    trap 'stop_server' EXIT
    wait_for_socket
    echo "Server started with PID $SERVER_PID"
}

function stop_server {
    if [ -n "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        unset SERVER_PID
    fi
}

function run_clients {
    local clients="$1"
    local delay="$2"
    local runlabel="$3"
    local dest="$LOGDIR/$runlabel"
    mkdir -p "$dest"
    local pids=()
    for i in $(seq 1 "$clients"); do
        ./test_client -c "$CONFIG" -f "$ROOT/numbers.txt" -d "$delay" -l "$dest/client_${i}.log" > "$dest/client_${i}.out" 2>&1 &
        pids+=("$!")
    done
    for pid in "${pids[@]}"; do
        wait "$pid"
    done
}

function verify_state {
    local expected="$1"
    local actual
    actual=$(printf "%s\n" "$expected" | ./brownian_client -c "$CONFIG" -m "$expected" | tr -d '\r')
    if [ "$actual" != "$expected" ]; then
        die "Verification failed: expected '$expected', got '$actual'"
    fi
}

function collect_perf {
    local runlabel="$1"
    local dest="$LOGDIR/$runlabel"
    local times_file="$dest/summary.txt"
    local min_start=""
    local max_end=""
    local max_delay=""
    local start
    local end
    local delay

    > "$times_file"
    for log in "$dest"/client_*.log; do
        [ -f "$log" ] || continue
        start=$(awk -F'=' '/^START=/ {print $2; exit}' "$log")
        end=$(awk -F'=' '/^END=/ {print $2; exit}' "$log")
        delay=$(awk -F'=' '/^DELAY=/ {print $2; exit}' "$log")
        if [ -z "$start" ] || [ -z "$end" ] || [ -z "$delay" ]; then
            echo "WARNING: missing START/END/DELAY in $log" >&2
            continue
        fi
        printf "%s %s %s %s\n" "$log" "$start" "$end" "$delay" >> "$times_file"
        if [ -z "$min_start" ] || awk "BEGIN{exit !($start < $min_start)}"; then
            min_start="$start"
        fi
        if [ -z "$max_end" ] || awk "BEGIN{exit !($end > $max_end)}"; then
            max_end="$end"
        fi
        if [ -z "$max_delay" ] || awk "BEGIN{exit !($delay > $max_delay)}"; then
            max_delay="$delay"
        fi
    done

    if [ -z "$min_start" ] || [ -z "$max_end" ]; then
        echo "run=$runlabel clients=? delay=? wall_time=0.000 max_delay=0.000 effective=0.000"
        return
    fi

    local wall_time
    wall_time=$(python3 - <<PY
print(float(${max_end}) - float(${min_start}))
PY
)
    local effective
    effective=$(python3 - <<PY
print(float(${wall_time}) - float(${max_delay}))
PY
)
    local run_name clients_val delay_val
    run_name=$(basename "$dest")
    clients_val=$(echo "$run_name" | awk -F'_' '{print $3}')
    delay_val=$(echo "$run_name" | awk -F'_' '{print $5}')
    printf "run=%s clients=%s delay=%s wall_time=%.3f max_delay=%.3f effective=%.3f\n" "$runlabel" "$clients_val" "$delay_val" "$wall_time" "$max_delay" "$effective"
}

function emit_result {
    echo "Test run: $1" >> "$RESULT"
    echo "Expected: $2" >> "$RESULT"
    echo "Actual: $3" >> "$RESULT"
    echo "" >> "$RESULT"
}

# Main execution
rm -f "$RESULT"
start_server
generate_numbers

# Run the main large test once and twice to ensure server persistence
run_clients 100 0.1 "run100_delay0.1"
verify_state 0
run_clients 100 0.1 "run100_delay0.1_repeat"
verify_state 0

# Collect resource usage lines
first_fd_line=$(grep '^CONNECT ' "$LOGDIR/server.log" | head -n1 || true)
last_fd_line=$(grep '^CONNECT ' "$LOGDIR/server.log" | tail -n1 || true)

cat > "$RESULT" <<EOF
Test summary for brownian bot server
===============================

1) Correctness
- Expected final state: 0 after all clients complete.
- Verification: server returned 0 for a test query 0.

2) Repeated execution
- Server run twice with the same test script without restart.

3) Resource log snapshot
- First connection entry: $first_fd_line
- Last connection entry: $last_fd_line

Experiment results:
EOF

# Performance experiments
rm -f "$PERF_FILE"
printf "run,clients,delay,wall_time,max_delay,effective\n" > "$PERF_FILE"
for clients in 1 20 40 60 80 100; do
    for delay in 0 0.2 0.4 0.6 0.8 1.0; do
        runlabel="perf_clients_${clients}_delay_${delay}"
        run_clients "$clients" "$delay" "$runlabel"
        summary=$(collect_perf "$runlabel")
        echo "$summary" >> "$RESULT"
        # Extract CSV fields and append to perf file
        # Expected summary format: run=... clients=... delay=... wall_time=... max_delay=... effective=...
        runname=$(echo "$summary" | awk -F' ' '{print $1}' | cut -d'=' -f2)
        clients_v=$(echo "$summary" | awk -F' ' '{print $2}' | cut -d'=' -f2)
        delay_v=$(echo "$summary" | awk -F' ' '{print $3}' | cut -d'=' -f2)
        wall_v=$(echo "$summary" | awk -F' ' '{print $4}' | cut -d'=' -f2)
        maxd_v=$(echo "$summary" | awk -F' ' '{print $5}' | cut -d'=' -f2)
        eff_v=$(echo "$summary" | awk -F' ' '{print $6}' | cut -d'=' -f2)
        printf "%s,%s,%s,%.3f,%.3f,%.3f\n" "$runname" "$clients_v" "$delay_v" "$wall_v" "$maxd_v" "$eff_v" >> "$PERF_FILE"
    done
done

echo "Results written to $RESULT"

echo "Server logs in $LOGDIR/server.log"

echo "Client logs in $LOGDIR"
