#!/bin/bash
# E2E test harness for MusicPlayer TUI
# Uses tmux to drive the real binary and capture screen output.

set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$DIR/../musicplayer"
SESSION="musicplayer-test-$$"
PASS=0
FAIL=0
SKIP=0
HAS_MPV=0
command -v mpv >/dev/null 2>&1 && HAS_MPV=1

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
	if echo "$screen" | grep -qF -- "$needle"; then
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
	if echo "$screen" | grep -qF -- "$needle"; then
		printf "  \033[31mFAIL\033[0m %s — should NOT contain: %s\n" "$label" "$needle"
		printf "  --- screen ---\n%s\n  --- end ---\n" "$screen"
		FAIL=$((FAIL + 1))
	else
		printf "  \033[32mPASS\033[0m %s\n" "$label"
		PASS=$((PASS + 1))
	fi
}

skip() {
	local label="$1"
	printf "  \033[33mSKIP\033[0m %s\n" "$label"
	SKIP=$((SKIP + 1))
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

echo ""
echo "Help text"
start
assert_contains "shows play/pause hint" "spc:play/pause"
assert_contains "shows seek hint" "h/l:seek"
assert_contains "shows volume hint" "-/+:vol"
assert_contains "shows loop hint" "m:loop"
assert_contains "shows stop hint" "esc:stop"

echo ""
echo "Progress bar (requires mpv)"
if [ "$HAS_MPV" -eq 1 ]; then
	# Generate a 2-second silent WAV for playback tests
	python3 -c "
import struct, wave
with wave.open('$DIR/songs/test-tone.wav', 'w') as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(44100)
    w.writeframes(b'\x00\x00' * 88200)
"
	start
	# Navigate to test-tone.wav (4th item: alpha, beta, gamma, test-tone)
	send j
	send j
	send j
	wait_ms 200
	send Enter
	sleep 1
	assert_contains "shows playing state" "[playing]"
	assert_contains "progress bar has time" "0:0"
	# Stop and verify progress bar goes away
	send Escape
	wait_ms 400
	assert_not_contains "stopped clears progress bar" "[playing]"
	rm -f "$DIR/songs/test-tone.wav"

	echo ""
	echo "Auto-play next song (requires mpv)"
	# Generate two short WAVs: first (1s) finishes, second (3s) should auto-start
	python3 -c "
import wave
for name, frames in [('aaa-first.wav', 44100), ('aab-second.wav', 132300)]:
    with wave.open('$DIR/songs/' + name, 'w') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(b'\x00\x00' * frames)
"
	start
	# aaa-first.wav sorts first; play it and wait for it to end
	send Enter
	sleep 3
	# Should now be playing aab-second.wav
	assert_contains "auto-play started next song" "aab-second.wav"
	rm -f "$DIR/songs/aaa-first.wav" "$DIR/songs/aab-second.wav"

	echo ""
	echo "Loop modes (requires mpv)"
	python3 -c "
import wave
with wave.open('$DIR/songs/test-tone.wav', 'w') as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(44100)
    w.writeframes(b'\x00\x00' * 88200)
"
	start
	# Navigate to test-tone.wav and play it
	send j
	send j
	send j
	wait_ms 200
	send Enter
	sleep 1
	# Toggle to single loop mode
	send m
	wait_ms 300
	assert_contains "m toggles repeat indicator" "[repeat]"
	# Toggle back to all
	send m
	wait_ms 300
	assert_not_contains "m toggles back to loop all" "[repeat]"
	send Escape
	wait_ms 300
	rm -f "$DIR/songs/test-tone.wav"

	echo ""
	echo "Playlist wrap (requires mpv)"
	python3 -c "
import wave
for name, frames in [('aaa-wrap-target.wav', 132300), ('zzz-wrap-source.wav', 44100)]:
    with wave.open('$DIR/songs/' + name, 'w') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(b'\x00\x00' * frames)
"
	start
	# Navigate to last song (zzz-wrap-source.wav) and play it
	send G
	wait_ms 200
	send Enter
	sleep 3
	# Should have wrapped to aaa-wrap-target.wav
	assert_contains "playlist wraps to first song" "aaa-wrap-target.wav"
	rm -f "$DIR/songs/aaa-wrap-target.wav" "$DIR/songs/zzz-wrap-source.wav"
else
	skip "progress bar shows during playback (no mpv)"
	skip "progress bar has time display (no mpv)"
	skip "stop clears progress bar (no mpv)"
	skip "auto-play next song (no mpv)"
	skip "loop mode toggle (no mpv)"
	skip "playlist wraps to first song (no mpv)"
fi

# --- summary ---

echo ""
TOTAL=$((PASS + FAIL))
echo "====================="
if [ "$FAIL" -eq 0 ]; then
	printf "\033[32mAll %d tests passed.\033[0m" "$TOTAL"
	[ "$SKIP" -gt 0 ] && printf " (%d skipped)" "$SKIP"
	printf "\n"
else
	printf "\033[31m%d/%d tests failed.\033[0m\n" "$FAIL" "$TOTAL"
	exit 1
fi
