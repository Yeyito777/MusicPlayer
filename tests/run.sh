#!/bin/bash
# E2E test harness for MusicPlayer TUI
# Uses tmux to drive the real binary and capture screen output.

set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$DIR/../musicplayer"
SESSION="musicplayer-test-$$"
PASS=0
FAIL=0

# --- helpers ---

cleanup() {
	tmux kill-session -t "$SESSION" 2>/dev/null || true
}
trap cleanup EXIT

start() {
	cleanup
	# Launch in a fixed 80x24 tmux pane with test songs dir
	tmux new-session -d -s "$SESSION" -x 80 -y 24 \
		"cd $DIR && SONGS_DIR=songs $BINARY --tmux; echo __EXITED__; sleep 10"
	sleep 0.4
}

send() {
	tmux send-keys -t "$SESSION" "$1"
}

capture() {
	tmux capture-pane -t "$SESSION" -p
}

wait_ms() {
	sleep "0.$1"
}

assert_contains() {
	local label="$1" needle="$2"
	local screen
	screen="$(capture)"
	if echo "$screen" | grep -qF "$needle"; then
		printf "  \033[32mPASS\033[0m %s\n" "$label"
		PASS=$((PASS + 1))
	else
		printf "  \033[31mFAIL\033[0m %s — expected to find: %s\n" "$label" "$needle"
		printf "  --- screen ---\n%s\n  --- end ---\n" "$screen"
		FAIL=$((FAIL + 1))
	fi
}

assert_not_contains() {
	local label="$1" needle="$2"
	local screen
	screen="$(capture)"
	if echo "$screen" | grep -qF "$needle"; then
		printf "  \033[31mFAIL\033[0m %s — should NOT contain: %s\n" "$label" "$needle"
		printf "  --- screen ---\n%s\n  --- end ---\n" "$screen"
		FAIL=$((FAIL + 1))
	else
		printf "  \033[32mPASS\033[0m %s\n" "$label"
		PASS=$((PASS + 1))
	fi
}

assert_session_dead() {
	local label="$1"
	sleep 0.3
	if tmux has-session -t "$SESSION" 2>/dev/null; then
		local screen
		screen="$(capture)"
		if echo "$screen" | grep -qF "__EXITED__"; then
			printf "  \033[32mPASS\033[0m %s\n" "$label"
			PASS=$((PASS + 1))
			return
		fi
		printf "  \033[31mFAIL\033[0m %s — session still alive\n" "$label"
		FAIL=$((FAIL + 1))
	else
		printf "  \033[32mPASS\033[0m %s\n" "$label"
		PASS=$((PASS + 1))
	fi
}

# --- tests ---

echo "MusicPlayer E2E Tests"
echo "====================="

echo ""
echo "Boot & rendering"
start
assert_contains "shows header" "MusicPlayer"
assert_contains "shows alpha.mp3" "alpha.mp3"
assert_contains "shows beta.flac" "beta.flac"
assert_contains "shows gamma.ogg" "gamma.ogg"
assert_contains "first song selected" "> alpha.mp3"

echo ""
echo "Navigation: j/k"
start
send j
wait_ms 200
assert_contains "j moves to beta" "> beta.flac"
assert_not_contains "alpha no longer selected" "> alpha.mp3"
send k
wait_ms 200
assert_contains "k moves back to alpha" "> alpha.mp3"

echo ""
echo "Navigation: g/G"
start
send j
send j
wait_ms 200
send g
wait_ms 200
assert_contains "g jumps to top" "> alpha.mp3"
send G
wait_ms 200
assert_contains "G jumps to bottom" "> gamma.ogg"

echo ""
echo "Boundary: k at top stays"
start
send k
wait_ms 200
assert_contains "k at top stays on alpha" "> alpha.mp3"

echo ""
echo "Boundary: j at bottom stays"
start
send G
wait_ms 200
send j
wait_ms 200
assert_contains "j at bottom stays on gamma" "> gamma.ogg"

echo ""
echo "Quit"
start
send q
assert_session_dead "q exits cleanly"

# --- summary ---

echo ""
TOTAL=$((PASS + FAIL))
echo "====================="
if [ "$FAIL" -eq 0 ]; then
	printf "\033[32mAll %d tests passed.\033[0m\n" "$TOTAL"
else
	printf "\033[31m%d/%d tests failed.\033[0m\n" "$FAIL" "$TOTAL"
	exit 1
fi
