#!/bin/sh
# vm-autosleep.sh BIN: pro1x-autosleep (the FreeBSD/arm64 binary) in the
# test VM (freebsd/vm, as root), in its test mode (-T): the display's state
# from files, a stand-in for pro1x-suspend -k that "sleeps" 3 s and says
# what ended it. Checks: no suspend with the display on, the delay after
# it goes off, the stay after a wake-up source (the power key) and after
# another interrupt, a call (a process named pro1x-callaudio) and the
# inhibit file holding it up, the statistics on SIGUSR1.
set -u
BIN=${1:-/root/pro1x-autosleep}
T=/tmp/autosleep-test
rm -rf "$T"
mkdir -p "$T"
echo 1 > "$T/display"; echo 0 > "$T/lpm"; echo 0 > "$T/acline"
cat > "$T/suspend" <<EOS
#!/bin/sh
echo "\$(date +%s) \$*" >> $T/suspend.log
sleep 3
end=\$(cat $T/end 2>/dev/null)
echo "suspended 3.000 s in 0 epochs, ended by \${end:-INTID 215 (a device: the power key)}; test" > $T/last
EOS
chmod 755 "$T/suspend"
n() { [ -f "$T/suspend.log" ] && wc -l < "$T/suspend.log" | tr -d ' ' || echo 0; }
fail=0
check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1 ($(n) suspends)"; fail=1; fi; }
"$BIN" -d -D 5 -a 8 -s 1 -w "215:8 100:8" -T "$T" > "$T/log" 2>&1 &
pid=$!
sleep 8
check "display on 8 s: no suspend" '[ "$(n)" -eq 0 ]'
echo 0 > "$T/display"
sleep 3
check "display off 3 s: not yet (delay 5 s)" '[ "$(n)" -eq 0 ]'
sleep 4
check "display off 7 s: suspended" '[ "$(n)" -eq 1 ]'
# ended by the power key at about 10 s: up 8 s, so none before about 18 s
sleep 6
check "the power key's stay (8 s): no second suspend yet" '[ "$(n)" -eq 1 ]'
echo "INTID 309 (a device)" > "$T/end"
sleep 6
check "after the stay: suspended again" '[ "$(n)" -ge 2 ]'
# ended by INTID 309 (not a wake-up source): up 1 s, so again soon
sleep 6
check "after another interrupt: up 1 s only" '[ "$(n)" -ge 3 ]'
cp /bin/sleep "$T/pro1x-callaudio"
"$T/pro1x-callaudio" 60 &
call=$!
sleep 5
a=$(n)
sleep 8
check "a call: no suspend" '[ "$(n)" -eq "$a" ]'
kill $call
touch /var/run/pro1x-autosleep.inhibit
sleep 6
a=$(n)
sleep 8
check "the inhibit file: no suspend" '[ "$(n)" -eq "$a" ]'
rm -f /var/run/pro1x-autosleep.inhibit
sleep 8
check "inhibit gone: suspends again" '[ "$(n)" -gt "$a" ]'
echo 1 > "$T/display"
kill -USR1 $pid
sleep 6
check "statistics" 'grep -q "ended by INTID 215" /var/run/pro1x-autosleep.stats &&
    grep -q "ended by INTID 309" /var/run/pro1x-autosleep.stats'
kill $pid
wait $pid 2> /dev/null
echo "== log"; cat "$T/log"
echo "== stats"; cat /var/run/pro1x-autosleep.stats
exit $fail
