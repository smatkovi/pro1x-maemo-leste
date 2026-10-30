#!/bin/sh
# pro1x-callaudio against a stand-in for pro1x-audiod's control socket
# (fakeaudiod.py): the requests and their order for a call (the earpiece,
# the loudspeaker on SIGUSR1 and back, SIGTERM: the voice down and the
# route of before), a voice session that does not start (exit 1, the route
# restored), no daemon (exit 1).
set -u
D=$(cd "$(dirname "$0")" && pwd)
A=${A:-$D/pro1x-callaudio}
T=$(mktemp -d /tmp/pro1x-callaudio-test.XXXXXX)
fake=
trap '[ -n "$fake" ] && kill $fake 2>/dev/null; rm -rf "$T"' EXIT
fails=0
check() { if [ "$1" = "$2" ]; then echo "ok   $3"; else echo "FAIL $3: '$1' (wanted '$2')"; fails=$((fails + 1)); fi; }
fake_start() {
	rm -f "$T/req"
	python3 "$D/fakeaudiod.py" "$T/sock" "$T/req" "$@" &
	fake=$!
	for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$T/sock" ] && break; sleep 0.1; done
}

fake_start --route spk
"$A" -S "$T/sock" > "$T/out" 2>&1 &
pid=$!
for i in 1 2 3 4 5 6 7 8 9 10; do grep -q ready "$T/out" && break; sleep 0.2; done
check "$(grep -c 'callaudio: ready' "$T/out")" 1 "ready"
kill -USR1 $pid; sleep 0.5; kill -USR1 $pid; sleep 0.5
kill -TERM $pid; wait $pid; st=$?
check $st 0 "SIGTERM: exit 0"
check "$(tr '\n' ',' < "$T/req")" "status,route ear,voice start,route spk,route ear,voice stop,route spk," \
    "the requests of a call"
kill $fake; wait $fake 2>/dev/null

fake_start --voice-fails --route hph
"$A" -S "$T/sock" > "$T/out" 2>&1; st=$?
check $st 1 "voice refused: exit 1"
check "$(tr '\n' ',' < "$T/req")" "status,route ear,voice start,route hph," "the route restored"
grep -q "no sound" "$T/out" && echo "ok   says the call has no sound" ||
    { echo "FAIL no message"; fails=$((fails + 1)); }
kill $fake; wait $fake 2>/dev/null

rm -f "$T/sock"
"$A" -S "$T/sock" -w 1 > "$T/out" 2>&1; st=$?
check $st 1 "no pro1x-audiod: exit 1"
fake=
[ $fails -eq 0 ] && echo "test_callaudio: all passed"
[ $fails -eq 0 ]
