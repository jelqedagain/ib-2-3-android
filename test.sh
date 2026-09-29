#!/usr/bin/env bash
# Runs a hidden test session with its own working folder and save data (never touches build/userdata):
#   ./test.sh <seconds> [extra ib3rt args...]     -> log in build/testrun/run.log
ROOT="$(cd "$(dirname "$0")" && pwd)"
SECS="${1:-60}"
shift
mkdir -p "$ROOT/build/testrun"
cd "$ROOT/build/testrun"
timeout "$SECS" ../ib3rt.exe -test -home userdata "$@" "$ROOT/game/Payload/SwordGame.app" > run.log 2>&1
echo "exit=$?" >> run.log
