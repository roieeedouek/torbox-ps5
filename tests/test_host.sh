#!/usr/bin/env bash
# End-to-end test of the host build against tests/mock_torbox.py: login, lists, folder picker, downloads
# (multi-connection, single connection without Range, broken connections, pause/resume, cancel, restart resume).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bin=$here/build/host/torbox-ps5
[[ -x $bin ]] || "$here/build.sh" host
work=$(mktemp -d "${TMPDIR:-/tmp}/torbox-test.XXXXXX")
mport=18765 port=18080
base=http://127.0.0.1:$port
pids=()
cleanup() { for p in ${pids[@]+"${pids[@]}"}; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; }
trap cleanup EXIT

python3 -I "$here/tests/mock_torbox.py" $mport > "$work/mock.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do curl -s -o /dev/null http://127.0.0.1:$mport/ && break; sleep 0.1; done
start_app() {
    TORBOX_API_BASE=http://127.0.0.1:$mport/v1/api RD_API_BASE=http://127.0.0.1:$mport/rest/1.0 TORBOX_APP_DIR=$work/app TORBOX_DOWNLOADS=$work/dl \
        TORBOX_ROOTS=$work TORBOX_PORT=$port TORBOX_LOW_SPEED_TIME=2 TORBOX_STALL_MS=4000 TORBOX_RETRY_MAX_MS=500 ${1+"$@"} "$bin" >> "$work/app.log" 2>&1 &
    app_pid=$!
    pids+=($app_pid)
    for _ in $(seq 50); do curl -s "$base/api/status" > /dev/null && return; sleep 0.1; done
    echo "app did not start" >&2; exit 1
}
start_app

fails=0
check() { # name, condition-command...
    local name=$1; shift
    if "$@"; then echo "ok   $name"; else echo "FAIL $name"; fails=$((fails + 1)); fi
}
get() { curl -s "$base/api/$1"; }
post() { local p=$1; shift; curl -s -H 'X-TorBox-PS5: 1' "$@" "$base/api/$p"; }
jq_() { python3 -I -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
file_mode() {
    if [[ $(uname -s) == Darwin ]]; then stat -f %Lp "$1"; else stat -c %a "$1"; fi
}
export -f file_mode

check "UI page served" bash -c "curl -s $base/ | grep -q 'TorBox'"
check "app.js served" bash -c "curl -s $base/app.js | grep -q loadStatus"
check "not logged in at start" bash -c "[[ \$(curl -s $base/api/status | python3 -I -c 'import json,sys; print(json.load(sys.stdin)[\"providers\"][\"torbox\"][\"logged_in\"])') == False ]]"
check "POST without app header refused" bash -c "[[ \$(curl -s -o /dev/null -w '%{http_code}' -d key=x $base/api/login) == 403 ]]"
r=$(post login --data-urlencode key=wrong-key-123)
check "wrong key refused" bash -c "grep -q 'invalid' <<<'$r'"
check "wrong key not stored" test ! -e "$work/app/torbox-key.txt"
r=$(post login --data-urlencode "key= test-key-0000 ")
check "right key accepted" bash -c "grep -q '\"torbox\":{[^}]*\"logged_in\":true' <<<'$r' && grep -q tester@example.com <<<'$r'"
check "key stored with mode 600" bash -c "[[ \$(file_mode $work/app/torbox-key.txt) == 600 ]]"
check "key never in status" bash -c "! curl -s $base/api/status | grep -q test-key-0000"
check "torrent list proxied" bash -c "curl -s '$base/api/list?kind=torrents&fresh=1' | grep -q 'Big Buck Bunny'"
check "usenet list (empty)" bash -c "curl -s '$base/api/list?kind=usenet' | grep -q '\"data\": \[\]'"
check "bad kind refused" bash -c "curl -s '$base/api/list?kind=nope' | grep -q 'kind must'"
mkdir -p "$work/dl/sub1" "$work/dl/sub2"
check "folder picker lists dirs" bash -c "curl -s '$base/api/dirs?path=$work/dl' | grep -q '\"dirs\":\[\"sub1\",\"sub2\"\]'"
check "folder picker refuses outside roots" bash -c "curl -s '$base/api/dirs?path=/etc' | grep -q outside"
check "folder picker refuses dot-dot" bash -c "curl -s '$base/api/dirs?path=$work/dl/../..' | grep -q outside"
check "mkdir works" bash -c "curl -s -H 'X-TorBox-PS5: 1' --data-urlencode 'path=$work/dl/new/deep' $base/api/mkdir | grep -q ok && test -d $work/dl/new/deep"

post settings -d connections=6 -d parallel=2 > /dev/null
# Whole torrent (2 files, with folder structure), multi-connection.
post download -d kind=torrents -d id=101 -d file_id=0 --data-urlencode "folder=$work/dl/a" \
    --data-urlencode "subdir=Big Buck Bunny (test)" -d name=movie.mkv > /dev/null
r=$(post download -d kind=torrents -d id=101 -d file_id=0 --data-urlencode "folder=$work/dl/a" \
    --data-urlencode "subdir=Big Buck Bunny (test)" -d name=movie.mkv)
check "duplicate refused" bash -c "grep -q 'already in the queue' <<<'$r'"
post download -d kind=torrents -d id=101 -d file_id=1 --data-urlencode "folder=$work/dl/a" \
    --data-urlencode "subdir=Big Buck Bunny (test)/extras" -d name=poster.jpg > /dev/null
# Web download whose server drops every connection after 7 MiB.
post download -d kind=webdl -d id=201 -d file_id=5 --data-urlencode "folder=$work/dl/b" -d name=flaky.bin > /dev/null
# Direct link without Range support (single stream).
post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/5000000?norange=1" \
    --data-urlencode "folder=$work/dl/c" -d name=norange.bin > /dev/null
r=$(post download -d kind=url --data-urlencode "url=ftp://x/y" --data-urlencode "folder=$work/dl/c")
check "non-http link refused" bash -c "grep -q 'must start' <<<'$r'"
r=$(post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/10" --data-urlencode "folder=/etc")
check "download folder outside roots refused" bash -c "grep -q 'not allowed' <<<'$r'"

wait_jobs() { # until no job is queued/starting/downloading, max $1 s
    for _ in $(seq $(($1 * 5))); do
        get jobs | grep -q '"state":"\(queued\|starting\|downloading\)"' || return 0
        sleep 0.2
    done
    return 1
}
# Server that allows only 2 parallel connections (answers 429 beyond): the other segments wait their turn.
post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/$((64 * 1048576))?maxconn=2&rate=20000000" \
    --data-urlencode "folder=$work/dl/f" -d name=limited.bin > /dev/null
check "downloads finish" wait_jobs 120
verify() { # file size: byte i must be i % 251
    python3 -I - "$1" "$2" <<'EOF'
import sys
path, size = sys.argv[1], int(sys.argv[2])
data = open(path, "rb").read()
pat = bytes(i % 251 for i in range(251 * 4096))
ok = len(data) == size and all(data[o:o + len(pat)] == pat[:len(data[o:o + len(pat)])] for o in range(0, len(data), len(pat)))
sys.exit(0 if ok else 1)
EOF
}
check "movie.mkv content (6 connections)" verify "$work/dl/a/Big Buck Bunny (test)/movie.mkv" $((60 * 1048576))
check "poster.jpg in subfolder" verify "$work/dl/a/Big Buck Bunny (test)/extras/poster.jpg" $((3 * 1048576))
check "flaky.bin survived broken connections" verify "$work/dl/b/flaky.bin" $((40 * 1048576))
check "norange.bin single stream" verify "$work/dl/c/norange.bin" 5000000
want=$(python3 -I -c "import hashlib; print(hashlib.sha256(bytes(i % 251 for i in range(5000000))).hexdigest())")
check "sha256 endpoint" bash -c "curl -s '$base/api/sha256?path=$work/dl/c/norange.bin' | grep -q $want"
check "picker lists files" bash -c "curl -s '$base/api/dirs?path=$work/dl/c' | grep -q '\"files\":\[{\"name\":\"norange.bin\",\"size\":5000000}'"
check "limited.bin with a 2-connection server limit" verify "$work/dl/f/limited.bin" $((64 * 1048576))
check "no .part left" bash -c "! find $work/dl -name '*.part*' | grep -q ."

# Pause / resume with a throttled multi-connection download.
id=$(post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/$((48 * 1048576))?rate=1000000" \
    --data-urlencode "folder=$work/dl/d" -d name=slow.bin | jq_ 'd["id"]')
sleep 3
post jobs/pause -d id="$id" > /dev/null
sleep 1.5
check "paused keeps partial + state" bash -c "get() { curl -s $base/api/jobs; }; get | grep -q '\"state\":\"paused\"' && test -f $work/dl/d/slow.bin.part && test -f $work/dl/d/slow.bin.part.state"
done1=$(get jobs | jq_ "[j['done'] for j in d['jobs'] if j['id']==$id][0]")
check "some bytes before pause" test "$done1" -gt 1000000
post jobs/resume -d id="$id" > /dev/null
sleep 3
done2=$(get jobs | jq_ "[j['done'] for j in d['jobs'] if j['id']==$id][0]")
check "resume continued (not restarted)" test "$done2" -gt "$done1"
# Restart the app in the middle of the download: the queue and the partial file survive.
kill "$app_pid"; wait "$app_pid" 2>/dev/null || true
start_app
check "job restored after restart" bash -c "curl -s $base/api/jobs | grep -q slow.bin"
check "resumed download finishes" wait_jobs 120
check "slow.bin content after pause+restart" verify "$work/dl/d/slow.bin" $((48 * 1048576))

# Pause with 2 connections, resume with 8: idle connections split the remaining segments.
post settings -d connections=2 > /dev/null
id=$(post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/$((64 * 1048576))?rate=1500000" \
    --data-urlencode "folder=$work/dl/g" -d name=steal.bin | jq_ 'd["id"]')
sleep 3
post jobs/pause -d id="$id" > /dev/null
sleep 1.5
post settings -d connections=8 > /dev/null
post jobs/resume -d id="$id" > /dev/null
conns=0 # highest count seen: the download can finish within a few seconds
for _ in $(seq 20); do
    sleep 0.2
    c=$(get jobs | jq_ "[j['conns'] for j in d['jobs'] if j['id']==$id][0]")
    ((c > conns)) && conns=$c
    ((conns >= 6)) && break
done
check "resume with more connections splits segments ($conns)" test "$conns" -ge 6
check "split download finishes" wait_jobs 120
check "steal.bin content" verify "$work/dl/g/steal.bin" $((64 * 1048576))
post settings -d connections=6 > /dev/null

# ---------- Real-Debrid ----------
rdq() { curl -s "$base/api/rd/$1"; }
r=$(post login -d provider=realdebrid --data-urlencode key=wrong-token-123)
check "RD: bad token refused, readable" bash -c "grep -q 'bad or expired API token' <<<'$r'"
r=$(post login -d provider=realdebrid --data-urlencode key=rd-test-token)
check "RD: token accepted, user shown" bash -c "grep -q '\"realdebrid\":{[^}]*\"logged_in\":true' <<<'$r' && grep -q rdtester <<<'$r' && grep -q premium <<<'$r'"
check "RD: TorBox still logged in" bash -c "grep -q '\"torbox\":{[^}]*\"logged_in\":true' <<<'$r'"
check "RD: token stored separately (600)" bash -c "[[ \$(file_mode $work/app/realdebrid-token.txt) == 600 ]] && test -e $work/app/torbox-key.txt"
check "RD: token never in status" bash -c "! curl -s $base/api/status | grep -q rd-test-token"
r=$(rdq "torrents?page=1&limit=2")
check "RD: paged torrents (page 1 of 5)" bash -c "python3 -I -c 'import json,sys; d=json.loads(sys.argv[1]); assert d[\"total\"]==5 and len(d[\"data\"])==2 and d[\"data\"][0][\"id\"]==\"RDT1\"' '$r'"
r=$(rdq "torrents?page=3&limit=2")
check "RD: last page" bash -c "python3 -I -c 'import json,sys; d=json.loads(sys.argv[1]); assert len(d[\"data\"])==1 and d[\"data\"][0][\"id\"]==\"RDT5\"' '$r'"
r=$(rdq "info?id=RDT1")
check "RD: selected files paired with links" bash -c "python3 -I -c '
import json,sys; d=json.loads(sys.argv[1]); f=d[\"files\"]
assert d[\"paired\"] and [x[\"id\"] for x in f]==[1,3], f
assert f[0][\"link\"].endswith(\"EPISODE1\") and f[1][\"link\"].endswith(\"SUBS1\") and d[\"links\"]==[]' '$r'"
r=$(rdq "info?id=RDT2")
check "RD: packed torrent gives its links apart" bash -c "python3 -I -c '
import json,sys; d=json.loads(sys.argv[1])
assert not d[\"paired\"] and len(d[\"files\"])==3 and all(x[\"link\"] is None for x in d[\"files\"]) and len(d[\"links\"])==1' '$r'"
check "RD: history" bash -c "rdq() { curl -s $base/api/rd/\$1; }; rdq 'downloads?page=1' | grep -q history.iso"
check "RD: bad id refused" bash -c "curl -s '$base/api/rd/info?id=../x' | grep -q 'bad torrent id'"
curl -s "http://127.0.0.1:$mport/control?rd_429=2" > /dev/null
check "RD: 429 waited out" bash -c "curl -s -m 30 '$base/api/rd/torrents?page=1' | grep -q RDT1"
post settings -d connections=6 > /dev/null
# Download all of RDT1 the way the page does: selected files with folder structure; one hoster link named by RD.
post download -d kind=rd --data-urlencode "url=https://real-debrid.com/d/EPISODE1" -d name=e01.mkv \
    --data-urlencode "subdir=Show S01" --data-urlencode "folder=$work/dl/rd" > /dev/null
post download -d kind=rd --data-urlencode "url=https://real-debrid.com/d/SUBS1" -d name=e01.srt \
    --data-urlencode "subdir=Show S01/Subs" --data-urlencode "folder=$work/dl/rd" > /dev/null
post download -d kind=rd --data-urlencode "url=https://real-debrid.com/d/PACKED" -d name= \
    --data-urlencode "folder=$work/dl/rd" > /dev/null
post download -d kind=rd --data-urlencode "url=https://unsupported.example/file/1" -d name= \
    --data-urlencode "folder=$work/dl/rd" > /dev/null
check "RD: downloads finish" wait_jobs 120
check "RD: e01.mkv content" verify "$work/dl/rd/Show S01/e01.mkv" $((20 * 1048576))
check "RD: e01.srt in Subs" verify "$work/dl/rd/Show S01/Subs/e01.srt" $((3 * 1048576))
check "RD: unnamed link named by Real-Debrid" verify "$work/dl/rd/Packed.rar" $((5 * 1048576))
check "RD: unsupported hoster fails readably" bash -c "curl -s $base/api/jobs | grep -q 'unsupported hoster'"
# A direct link that expires while the download is paused: resumed with a fresh unrestrict, not restarted.
curl -s "http://127.0.0.1:$mport/control?rd_ttl=4" > /dev/null
id=$(post download -d kind=rd --data-urlencode "url=https://real-debrid.com/d/SLOW" -d name=slow-rd.bin \
    --data-urlencode "folder=$work/dl/rd2" | jq_ 'd["id"]')
sleep 3
post jobs/pause -d id="$id" > /dev/null
sleep 1.5
before=$(get jobs | jq_ "[j['done'] for j in d['jobs'] if j['id']==$id][0]")
sleep 4
post jobs/resume -d id="$id" > /dev/null
check "RD: expired link download finishes" wait_jobs 120
check "RD: slow-rd.bin content" verify "$work/dl/rd2/slow-rd.bin" $((40 * 1048576))
n=$(curl -s "http://127.0.0.1:$mport/control/stats" | jq_ 'd["unrestrict"]["https://real-debrid.com/d/SLOW"]')
check "RD: expired link unrestricted again ($n calls, $before bytes kept)" bash -c "test $n -ge 2 && test $before -gt 1000000"
curl -s "http://127.0.0.1:$mport/control?rd_ttl=600" > /dev/null
check "RD: queue file has no direct links" bash -c "! grep -q rdfile $work/app/queue.tsv"
r=$(post logout -d provider=realdebrid)
check "RD: logout keeps TorBox" bash -c "test ! -e $work/app/realdebrid-token.txt && test -e $work/app/torbox-key.txt"
check "RD: lists refused after logout" bash -c "curl -s '$base/api/rd/torrents' | grep -q 'not logged in'"
check "RD: token and direct links not in logs" bash -c "! grep -rqE 'rd-test-token|rdfile' $work/app/log.txt $work/app.log"

# Cancel deletes the partial file.
id=$(post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/$((48 * 1048576))?rate=2000000" \
    --data-urlencode "folder=$work/dl/e" -d name=cancel.bin | jq_ 'd["id"]')
sleep 2
cancel_state() { get jobs | jq_ "[j['state']+' '+str(j['done']) for j in d['jobs'] if j['id']==$id][0]"; }
before=$(cancel_state)
r=$(post jobs/cancel -d id="$id")
for _ in $(seq 50); do [[ $(cancel_state) == cancelled* ]] && break; sleep 0.1; done
after=$(cancel_state)
check "cancel removes partial" bash -c "[[ '$after' == cancelled* ]] && ! ls $work/dl/e/cancel.bin.part* 2>/dev/null"
[[ $after == cancelled* ]] || echo "     before cancel: $before; cancel reply: $r; 5 s later: $after"

# Existing file is not downloaded again.
post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/5000000" \
    --data-urlencode "folder=$work/dl/c" -d name=norange.bin > /dev/null
wait_jobs 10 || true
check "existing file kept" bash -c "curl -s $base/api/jobs | grep -q 'already there'"

# Auto connections (0), Retry-After + ETag validation, and TorBox stalled CDN link rotation.
r=$(post settings -d connections=0 -d parallel=2)
check "Auto connections (0) accepted" bash -c "grep -q '\"connections\":0' <<<'$r'"
post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/$((24 * 1048576))?maxconn=2&retry_after=1&etag=v1" \
    --data-urlencode "folder=$work/dl/h" -d name=retry-after.bin > /dev/null
post download -d kind=torrents -d id=103 -d file_id=0 --data-urlencode "folder=$work/dl/h" -d name=stalled-cdn.bin > /dev/null
check "Auto / Retry-After / TorBox CDN rotation finish" wait_jobs 60
check "retry-after.bin content" verify "$work/dl/h/retry-after.bin" $((24 * 1048576))
check "stalled-cdn.bin content" verify "$work/dl/h/stalled-cdn.bin" $((20 * 1048576))
tb_calls=$(curl -s "http://127.0.0.1:$mport/control/stats" | jq_ 'd["tb_requestdl"]["103"]')
check "TorBox rotated stalled CDN link ($tb_calls calls)" test "$tb_calls" -ge 2

# Check /api/log returns telemetry and never leaks credentials or RD direct paths.
check "log endpoint works" bash -c "curl -s '$base/api/log' | grep -q 'rcvbuf='"
check "log has no secrets" bash -c "! curl -s '$base/api/log' | grep -qE 'test-key-0000|rd-test-token|rdfile'"

# HTTP 400 mid-download -> TorBox relink & resume without losing bytes.
post download -d kind=torrents -d id=104 -d file_id=0 --data-urlencode "folder=$work/dl/r400" -d name=http400-rotate.bin > /dev/null
check "HTTP 400 mid-download relinks and finishes" wait_jobs 60
check "http400-rotate.bin content" verify "$work/dl/r400/http400-rotate.bin" $((24 * 1048576))
tb400_calls=$(curl -s "http://127.0.0.1:$mport/control/stats" | jq_ 'd["tb_requestdl"]["104"]')
check "TorBox rotated HTTP 400 link ($tb400_calls calls)" test "$tb400_calls" -ge 2
check "HTTP 400 resume logged" bash -c "curl -s '$base/api/log' | grep -q 'resumed'"

# Probe fallback (Range 0-0 -> 400 falls back to Range 0-1023).
post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/$((16 * 1048576))?bad_probe_zero=1" \
    --data-urlencode "folder=$work/dl/pf" -d name=probe-fallback.bin > /dev/null
check "probe 0-0 -> 0-1023 fallback finishes" wait_jobs 60
check "probe-fallback.bin content" verify "$work/dl/pf/probe-fallback.bin" $((16 * 1048576))
check "probe fallback logged" bash -c "curl -s '$base/api/log' | grep -q 'retrying with Range 0-1023'"

# HTTP 400 on If-Range -> auto-disable If-Range and succeed.
post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/$((16 * 1048576))?etag=v1&bad_if_range=1" \
    --data-urlencode "folder=$work/dl/ifr" -d name=bad-if-range.bin > /dev/null
check "bad If-Range download finishes" wait_jobs 60
check "bad-if-range.bin content" verify "$work/dl/ifr/bad-if-range.bin" $((16 * 1048576))
check "bad If-Range auto-disable logged" bash -c "curl -s '$base/api/log' | grep -q 'disabling If-Range'"

# Transient HTTP 200 (no Range) on retry NEVER deletes .part; also verify paused restart preserves progress.
id=$(post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/$((32 * 1048576))?rate=1500000&tag=keep200" \
    --data-urlencode "folder=$work/dl/k200" -d name=keep-partial.bin | jq_ 'd["id"]')
sleep 2.5
post jobs/pause -d id="$id" > /dev/null
sleep 1.5
kill "$app_pid"; wait "$app_pid" 2>/dev/null || true
start_app
peek_done=$(get jobs | jq_ "[j['done'] for j in d['jobs'] if j['name']=='keep-partial.bin'][0]")
peek_tot=$(get jobs | jq_ "[j['total'] for j in d['jobs'] if j['name']=='keep-partial.bin'][0]")
check "restarted paused job shows partial progress ($peek_done/$peek_tot)" test "$peek_done" -gt 1000000 -a "$peek_tot" -eq $((32 * 1048576))
id=$(get jobs | jq_ "[j['id'] for j in d['jobs'] if j['name']=='keep-partial.bin'][0]")
curl -s "http://127.0.0.1:$mport/control?once_200=keep200" > /dev/null
post jobs/resume -d id="$id" > /dev/null
check "transient HTTP 200 kept partial and recovered" wait_jobs 60
check "keep-partial.bin content" verify "$work/dl/k200/keep-partial.bin" $((32 * 1048576))
check "keeping partial logged" bash -c "curl -s '$base/api/log' | grep -q 'keeping partial'"

# jobs/remove keeps .part and .part.state on disk so re-queuing resumes.
id=$(post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/$((32 * 1048576))?rate=1500000" \
    --data-urlencode "folder=$work/dl/rem" -d name=remove-keeps.bin | jq_ 'd["id"]')
sleep 2.5
post jobs/pause -d id="$id" > /dev/null
sleep 1.5
rem_done=$(get jobs | jq_ "[j['done'] for j in d['jobs'] if j['id']==$id][0]")
post jobs/remove -d id="$id" > /dev/null
check "jobs/remove keeps partial + state ($rem_done bytes)" bash -c "test -f $work/dl/rem/remove-keeps.bin.part && test -f $work/dl/rem/remove-keeps.bin.part.state && test $rem_done -gt 1000000"
post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/$((32 * 1048576))?rate=1500000" \
    --data-urlencode "folder=$work/dl/rem" -d name=remove-keeps.bin > /dev/null
check "re-queued job after remove finishes" wait_jobs 60
check "remove-keeps.bin content" verify "$work/dl/rem/remove-keeps.bin" $((32 * 1048576))

# USB sequential sliding-window mode + async writer latency decoupling test.
kill "$app_pid"; wait "$app_pid" 2>/dev/null || true
start_app env TORBOX_FORCE_USB_MODE=1 TORBOX_TEST_REJECT_GAPS=1 TORBOX_TEST_WRITE_DELAY_MS=5
post settings -d connections=8 > /dev/null
post download -d kind=url --data-urlencode "url=http://127.0.0.1:$mport/files/$((32 * 1048576))" \
    --data-urlencode "folder=$work/dl/usb" -d name=usb-seq.bin > /dev/null
check "USB sequential download finishes with reject_gaps + write delay" wait_jobs 60
check "usb-seq.bin content" verify "$work/dl/usb/usb-seq.bin" $((32 * 1048576))
check "USB sequential telemetry logged" bash -c "curl -s $base/api/log | grep -q 'usb=seq'"

check "logout removes key" bash -c "curl -s -H 'X-TorBox-PS5: 1' -X POST $base/api/logout | grep -q ok && test ! -e $work/app/torbox-key.txt"
check "list after logout refused" bash -c "curl -s '$base/api/list?kind=torrents' | grep -q 'not logged in'"
check "key not in log" bash -c "! grep -rq test-key-0000 $work/app/log.txt $work/app.log"
post quit -X POST > /dev/null
sleep 1
check "quit stops server" bash -c "! curl -s -m 2 $base/api/status > /dev/null"
echo "$fails failure(s); work dir $work"
[[ $fails == 0 ]] && rm -rf "$work"
exit $((fails > 0))
