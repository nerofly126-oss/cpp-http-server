#!/usr/bin/env bash
# Benchmarks this server against nginx: same machine, same file, same wrk settings.
#
#   bench/compare_nginx.sh              (about 3 minutes)
#   DURATION=5 RUNS=1 bench/compare_nginx.sh   (quick smoke run)
#
# Needs: wrk, nginx, curl, g++.  Installs nothing and touches no system config:
# nginx runs from a temp directory as your own user, on ports 8081 and 8082.
set -euo pipefail

DURATION="${DURATION:-10}"
RUNS="${RUNS:-3}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NGINX="$(command -v nginx || echo /usr/sbin/nginx)"
WORK="$(mktemp -d)"
PIDS=()

cleanup() {
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

for tool in wrk curl g++; do
    command -v "$tool" > /dev/null || { echo "missing tool: $tool"; exit 1; }
done
[ -x "$NGINX" ] || { echo "missing tool: nginx (sudo apt install nginx)"; exit 1; }

start_nginx() {  # $1 = port, $2 = worker_processes
    local conf="$WORK/nginx-$1.conf"
    cat > "$conf" << NGINX_END
worker_processes $2;
daemon off;
pid $WORK/nginx-$1.pid;
error_log stderr warn;
events { worker_connections 1024; }
http {
    access_log off;
    sendfile on;
    tcp_nopush on;
    keepalive_timeout 65;
    keepalive_requests 1000000;
    server {
        listen $1;
        root $ROOT/static;
        location = /health { default_type text/plain; return 200 "ok\n"; }
        location / { }
    }
}
NGINX_END
    mkdir -p "$WORK/nginx-$1"
    "$NGINX" -p "$WORK/nginx-$1/" -c "$conf" -e stderr &
    PIDS+=($!)
}

wait_for() {  # $1 = port
    for _ in $(seq 100); do
        curl -s -o /dev/null "http://127.0.0.1:$1/health" && return 0
        sleep 0.1
    done
    echo "nothing answered on port $1"
    exit 1
}

echo "== environment"
echo "cpu:      $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
echo "threads:  $(nproc)"
echo "kernel:   $(uname -sr)"
echo "compiler: $(g++ --version | head -1)"
echo "nginx:    $("$NGINX" -v 2>&1)"
echo "wrk:      $(wrk --version 2>&1 | head -1)"
echo "settings: wrk -t4 -c100 -d${DURATION}s, median of $RUNS runs, 2s warm-up"
echo

echo "== building (-O2)"
g++ -std=c++17 -O2 -pthread "$ROOT"/src/*.cpp -o "$WORK/httpserver_fast"

"$WORK/httpserver_fast" 8080 "$ROOT/static" -q > /dev/null &
PIDS+=($!)
"$WORK/httpserver_fast" 8083 "$ROOT/static" -q -l 1 > /dev/null &
PIDS+=($!)
start_nginx 8081 1
start_nginx 8082 auto
for port in 8080 8081 8082 8083; do wait_for "$port"; done

# Make sure all three really serve the same bytes.
cmp <(curl -s http://127.0.0.1:8080/index.html) <(curl -s http://127.0.0.1:8081/index.html) \
    || { echo "index.html differs between servers, aborting"; exit 1; }
cmp <(curl -s http://127.0.0.1:8081/index.html) <(curl -s http://127.0.0.1:8082/index.html) \
    || { echo "index.html differs between nginx instances, aborting"; exit 1; }

TABLE=""
bench() {  # $1 = label, $2 = port, $3 = path
    local url="http://127.0.0.1:$2$3" rps=() out
    echo "== $1  $url"
    wrk -t4 -c100 -d2s "$url" > /dev/null
    for _ in $(seq "$RUNS"); do
        out="$(wrk -t4 -c100 -d"${DURATION}s" "$url")"
        grep -E "Requests/sec|^ +Latency|Socket errors|Non-2xx" <<< "$out" | sed 's/^ */   /'
        rps+=("$(awk '/Requests\/sec/ {print $2}' <<< "$out")")
    done
    local median
    median="$(printf '%s\n' "${rps[@]}" | sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}')"
    echo "   median: $median req/s"
    echo
    TABLE+="| $1 | \`$3\` | $median |"$'\n'
}

for path in /health /index.html; do
    bench "cpp-http-server ($(nproc) loops)" 8080 "$path"
    bench "cpp-http-server (1 loop)"        8083 "$path"
    bench "nginx (1 worker)"                8081 "$path"
    bench "nginx (auto: $(nproc) workers)"  8082 "$path"
done

echo "== summary (median requests/sec)"
echo "| Server | Path | Requests/sec |"
echo "|---|---|---|"
printf '%s' "$TABLE"
