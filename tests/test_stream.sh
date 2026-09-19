#!/bin/bash
# ============================================================================
#  test_stream.sh -- what a display in another process sees from --stream.
#
#  End to end, on the real binary, against a generated tree. The contract is
#  the byte stream on stdout, and the ways it breaks -- an escape sequence
#  leaking into the JSON, a nan no parser accepts, a monitor that keeps
#  reading 116 files after its reader has gone -- are properties of the whole
#  program, not of any one function.
#
#  Run:  make test        (or  bash tests/test_stream.sh ./his_monitor )
# ============================================================================
set -u
M=${1:-./his_monitor}
HERE=$(dirname "$0")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0
ok  () { pass=$((pass+1)); }
bad () { fail=$((fail+1)); printf '  FAIL %s\n' "$*"; }

sh "$HERE/make_his.sh" "$TMP" synth 12 1600 >/dev/null
OUT=$TMP/out.jsonl

# A normal run, ended from outside the way the GUI ends it: SIGTERM.
timeout 3 "$M" "$TMP/synth" --from-start --stream --period 50 >"$OUT" 2>"$TMP/err"
rc=$?
[ "$rc" = 124 ] && ok || bad "monitor exited $rc before it was stopped: $(head -c 200 "$TMP/err")"

# The first line announces the tree.
head -1 "$OUT" | grep -q '^{"type":"init",.*"ndom":12,' && ok \
    || bad "first line is not an init for 12 domains: $(head -c 120 "$OUT")"

# Not one terminal escape sequence anywhere in the stream.
if LC_ALL=C grep -q "$(printf '\033')" "$OUT"; then
    bad "an escape sequence leaked into the stream"
else ok; fi

# Every line is exactly one complete object.
n=$(grep -cv '^{.*}$' "$OUT")
[ "$n" = 0 ] && ok || bad "$n line(s) are not a single {...} object"

# JSON has no nan and no inf; a value that is not finite must go out as null.
if grep -qiE '[,:[]-?(nan|inf)' "$OUT"; then
    bad "a nan or inf token is on the wire"
else ok; fi

# Every tick carries exactly one pressure per domain.
lens=$(grep -o '"P":\[[^]]*\]' "$OUT" | awk -F, '{print NF}' | sort -u | tr '\n' ' ')
[ "$lens" = "12 " ] && ok || bad "P arrays have lengths '$lens', want 12"

# The waveform history arrives: the first tick after a from-start sweep
# carries samples for the first domain.
grep -q '"hist":\[\[[0-9]' "$OUT" && ok || bad "no waveform history in any tick"

# A normal stop says so: the last line is the exit record.
tail -1 "$OUT" | grep -q '^{"type":"exit",' && ok \
    || bad "last line is not the exit record: $(tail -1 "$OUT" | head -c 120)"

# A real JSON parser accepts every line. python3 is only the referee here,
# not part of the product; without it the check is skipped, not passed.
if command -v python3 >/dev/null 2>&1; then
    if python3 - "$OUT" <<'PY'
import json, sys
for n, line in enumerate(open(sys.argv[1]), 1):
    try:
        json.loads(line)
    except ValueError as e:
        sys.exit("line %d: %s" % (n, e))
PY
    then ok; else bad "a JSON parser rejected the stream"; fi
fi

# A finished run ends a scripted monitor by itself, with the run's own
# verdict -- not STALLED twenty seconds later, and not never. The generated
# tree ends exactly at its final time, so this is a finished, settled run.
timeout 10 "$M" "$TMP/synth" --from-start --stream --abort-on-fail \
    --period 50 >"$TMP/done.jsonl" 2>/dev/null
rc=$?
[ "$rc" = 0 ] && ok || bad "a finished, settled run should end the monitor with 0, got $rc"
tail -1 "$TMP/done.jsonl" | grep -q '^{"type":"exit","code":0,"verdict":"CONVERGED"' \
    && ok || bad "the exit record should carry the run's verdict: $(tail -1 "$TMP/done.jsonl" | head -c 120)"

# The reader going away stops the monitor, instead of leaving it to read 116
# files for nobody. `head` takes two lines and closes the pipe.
timeout 5 "$M" "$TMP/synth" --from-start --stream --period 50 2>/dev/null \
    | head -n 2 >/dev/null
rc=${PIPESTATUS[0]}
[ "$rc" != 124 ] && ok || bad "monitor kept running after its reader closed the pipe"

printf 'test_stream\n%d checks, %d failed\n' $((pass + fail)) "$fail"
[ "$fail" = 0 ]
