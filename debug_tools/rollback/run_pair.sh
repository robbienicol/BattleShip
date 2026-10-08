#!/bin/bash
# Runs two BattleShip instances against each other over localhost UDP with
# rollback netcode, each driving one player from the same replay file.
# Usage: run_pair.sh REPLAY OUTDIR [extra env assignments...]
set -u
REPLAY="$1"; OUT="$2"; shift 2
BIN="$(cd "$(dirname "$0")/../../build-us" && pwd)/BattleShip"
cd "$(dirname "$BIN")"
run() {
	local name=$1 local_player=$2 port=$3 peer=$4; shift 4
	env SSB64_ROLLBACK=1 SSB64_ROLLBACK_SCRIPTED=1 SSB64_ROLLBACK_PLAYERS=2 \
		SSB64_REPLAY_PLAY="$REPLAY" SSB64_RIG_EXIT=1 SSB64_LOG_PATH="$OUT/$name.log" \
		SSB64_ROLLBACK_LOCAL=$local_player SSB64_ROLLBACK_PORT=$port SSB64_ROLLBACK_PEER=127.0.0.1:$peer \
		"$@" timeout 240 "$BIN" > /dev/null 2>&1
	echo "$name exit $?"
}
run A 0 7100 7101 "$@" &
run B 1 7101 7100 "$@" &
wait
