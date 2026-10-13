#!/bin/sh
# x45upnp against a fake ContentDirectory server on loopback (host build with ASan; run from src/).
#   make upnptest
set -u
cd "$(dirname "$0")/.."
OUT=out
mkdir -p $OUT
gcc -std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined -o $OUT/x45upnp_native x45upnp/main.c || exit 2
PORT=18765
LOG=$OUT/upnp_ids.log
: > $LOG
python3 tests/upnp_fake.py $PORT $LOG &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1
CTRL=http://127.0.0.1:$PORT/cd
fails=0
check() { if [ "$2" = ok ]; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }

R=$(setarch -R $OUT/x45upnp_native browse $CTRL root 2>/dev/null)
echo "$R" | grep -q "^D	Simon & Garfunkel	Simon & Garfunkel$" && check "folder id with an ampersand comes back unescaped" ok || check "folder id with an ampersand comes back unescaped" no
echo "$R" | grep -q "^F	http://127.0.0.1:1/a.flac	Track one$" && check "audio item listed" ok || check "audio item listed" no
echo "$R" | grep -q "^T	300$" && check "TotalMatches larger than the rows is reported" ok || check "TotalMatches larger than the rows is reported" no

R=$(setarch -R $OUT/x45upnp_native browse $CTRL "Simon & Garfunkel" 2>/dev/null)
grep -q "^Simon &amp; Garfunkel$" $LOG && check "the object id is XML-escaped in the request" ok || check "the object id is XML-escaped in the request" no
echo "$R" | grep -q "^F	http://127.0.0.1:1/boxer.mp3?q" && check "a stream address of 330 characters is kept" ok || check "a stream address of 330 characters is kept" no
echo "$R" | grep -q "^T" && check "no count line when everything was returned" no || check "no count line when everything was returned" ok

setarch -R $OUT/x45upnp_native browse $CTRL chunked-short >/dev/null 2>$OUT/upnp_err.log
rc=$?
if [ $rc -le 1 ] && ! grep -q "AddressSanitizer\|runtime error" $OUT/upnp_err.log; then check "a chunk announced but not sent does not read out of bounds (rc $rc)" ok; else check "a chunk announced but not sent does not read out of bounds (rc $rc)" no; cat $OUT/upnp_err.log | head -5; fi

setarch -R $OUT/x45upnp_native browse $CTRL nothing >/dev/null 2>&1
[ $? -eq 1 ] && check "an HTTP error is a failure, not an empty folder" ok || check "an HTTP error is a failure, not an empty folder" no

echo "upnp_test: $fails failed"
exit $fails
