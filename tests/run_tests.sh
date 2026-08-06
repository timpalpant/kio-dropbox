#!/usr/bin/env bash
# Drives the built worker against tests/fake_dropbox.py using kioclient, so the
# whole stack -- KIO, the worker, the HTTP layer -- is exercised end to end.
#
# Usage: tests/run_tests.sh [build-dir]
set -uo pipefail

BUILD_DIR="${1:-build}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

KIOCLIENT="$(command -v kioclient6 || command -v kioclient)" || {
    echo "kioclient not found; install kde-cli-tools" >&2
    exit 1
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; [[ -n ${SERVER_PID:-} ]] && kill "$SERVER_PID" 2>/dev/null' EXIT

# --- fake Dropbox -------------------------------------------------------------

python3 "$SRC_DIR/tests/fake_dropbox.py" > "$WORK/port" &
SERVER_PID=$!
for _ in {1..50}; do [[ -s "$WORK/port" ]] && break; sleep 0.1; done
PORT="$(cat "$WORK/port")"
[[ -n "$PORT" ]] || { echo "fake Dropbox failed to start" >&2; exit 1; }
BASE="http://127.0.0.1:$PORT"

# --- a sandboxed, pre-linked account -----------------------------------------

export XDG_CONFIG_HOME="$WORK/config"
export XDG_RUNTIME_DIR="$WORK/run"
export XDG_DATA_HOME="$WORK/data"
mkdir -p "$XDG_CONFIG_HOME/kio-dropbox" "$XDG_RUNTIME_DIR" "$XDG_DATA_HOME"
chmod 700 "$XDG_RUNTIME_DIR"

cat > "$XDG_CONFIG_HOME/kio-dropboxrc" <<EOF
[Account]
AppKey=fake-app-key
Email=tester@example.com
DisplayName=Test User
EOF
printf 'fake-refresh-token' > "$XDG_CONFIG_HOME/kio-dropbox/refresh-token"

export KIO_DROPBOX_NO_WALLET=1
# Everything here is a Qt GUI application under the hood, kioclient included.
# Rendering offscreen keeps the suite runnable on a headless CI container and
# stops windows flashing up during a local run.
export QT_QPA_PLATFORM=offscreen
export KIO_DROPBOX_API_BASE="$BASE/2/"
export KIO_DROPBOX_CONTENT_BASE="$BASE/content/2/"
export KIO_DROPBOX_TOKEN_ENDPOINT="$BASE/oauth2/token"
export QT_PLUGIN_PATH="$BUILD_DIR/src/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"

# Plugins are found by directory layout, so mirror the build output into the
# structure KIO and System Settings expect rather than installing system-wide.
mkdir -p "$BUILD_DIR/src/plugins/kf6/kio" "$BUILD_DIR/src/plugins/plasma/kcms/systemsettings_qwidgets"
cp "$BUILD_DIR/src/dropbox.so" "$BUILD_DIR/src/plugins/kf6/kio/dropbox.so"
cp "$BUILD_DIR/src/kcm_dropbox.so" "$BUILD_DIR/src/plugins/plasma/kcms/systemsettings_qwidgets/kcm_dropbox.so"

AUTH="$BUILD_DIR/src/kio-dropbox-auth"

# --- harness ------------------------------------------------------------------

PASS=0 FAIL=0

ok()   { PASS=$((PASS+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; [[ -n ${2:-} ]] && printf '       %s\n' "$2"; }

# check <name> <expected> <actual>
check() { [[ "$2" == "$3" ]] && ok "$1" || bad "$1" "expected [$2], got [$3]"; }

# check_contains <name> <needle> <haystack>
check_contains() { [[ "$3" == *"$2"* ]] && ok "$1" || bad "$1" "[$2] not found in [$3]"; }

kc() { timeout 120 "$KIOCLIENT" "$@" 2>&1; }

# kioclient's `ls` prints a blank line after each batch of entries the worker
# reports, so strip those before comparing.
kls() { kc ls "$1" | grep -v '^$'; }

reset_server() { curl -sf -X POST "$BASE/__control/reset" > /dev/null; }
inject_fault() { curl -sf -X POST "$BASE/__control/fault/$1" > /dev/null; }

echo "kio-dropbox integration tests (fake Dropbox on port $PORT)"

# --- listing ------------------------------------------------------------------

reset_server
out="$(kls dropbox:/ | sort | tr '\n' ' ')"
check "list root" "big.bin documents photos " "$out"

out="$(kls dropbox:/documents | sort | tr '\n' ' ')"
check "list subfolder with non-ASCII names" "notes.txt résumé — draft.pdf " "$out"

out="$(kls dropbox:/photos | wc -l)"
check "list_folder pagination (2500 entries)" "2500" "$out"

out="$(kls dropbox:/photos | sort -u | wc -l)"
check "pagination yields no duplicates" "2500" "$out"

out="$(kc ls dropbox:/nope 2>&1)"
check_contains "listing a missing folder errors" "does not exist" "$out"

out="$(kc stat dropbox:/documents/notes.txt)"
check_contains "stat reports the size" "19" "$out"
check_contains "stat reports a file, not a folder" "notes.txt" "$out"

# --- reading ------------------------------------------------------------------

out="$(kc cat dropbox:/documents/notes.txt)"
check "read a small file" "hello from dropbox" "$out"

out="$(kc cat 'dropbox:/documents/résumé — draft.pdf' | head -c 8)"
check "read a file with a non-ASCII name" "%PDF-1.4" "$out"

kc copy dropbox:/big.bin "file://$WORK/big.out" > /dev/null
check "read a 1 MiB file (size)" "1048576" "$(stat -c %s "$WORK/big.out" 2>/dev/null)"

out="$(kc cat dropbox:/documents/missing.txt 2>&1)"
check_contains "reading a missing file errors" "does not exist" "$out"

# --- writing ------------------------------------------------------------------

printf 'uploaded contents\n' > "$WORK/upload.txt"
kc copy "file://$WORK/upload.txt" dropbox:/documents/ > /dev/null
check "upload a small file" "uploaded contents" "$(kc cat dropbox:/documents/upload.txt)"

# 20 MiB exceeds the 8 MiB chunk, so this goes through upload_session/*
head -c $((20 * 1024 * 1024)) /dev/urandom > "$WORK/large.bin"
kc copy "file://$WORK/large.bin" dropbox:/ > /dev/null
kc copy dropbox:/large.bin "file://$WORK/large.out" > /dev/null
if cmp -s "$WORK/large.bin" "$WORK/large.out"; then
    ok "chunked upload of 20 MiB round-trips byte for byte"
else
    bad "chunked upload of 20 MiB round-trips byte for byte"
fi

# --- folders, moving, deleting ------------------------------------------------

kc mkdir dropbox:/newfolder > /dev/null
check_contains "mkdir" "newfolder" "$(kls dropbox:/)"

kc move dropbox:/documents/upload.txt dropbox:/newfolder/moved.txt > /dev/null
check "server-side move keeps contents" "uploaded contents" "$(kc cat dropbox:/newfolder/moved.txt)"
if kls dropbox:/documents | grep -q upload.txt; then
    bad "server-side move removes the source"
else
    ok "server-side move removes the source"
fi

kc copy dropbox:/newfolder/moved.txt dropbox:/newfolder/copied.txt > /dev/null
check "server-side copy" "uploaded contents" "$(kc cat dropbox:/newfolder/copied.txt)"
check "server-side copy leaves the source" "uploaded contents" "$(kc cat dropbox:/newfolder/moved.txt)"

kc remove dropbox:/newfolder/copied.txt > /dev/null
if kls dropbox:/newfolder | grep -q copied.txt; then
    bad "delete a file"
else
    ok "delete a file"
fi

# Dropbox deletes recursively, and the worker advertises deleteRecursive.
kc remove dropbox:/newfolder > /dev/null 2>&1
if kls dropbox:/ | grep -q newfolder; then
    bad "delete a non-empty folder"
else
    ok "delete a non-empty folder"
fi

# --- overwrite semantics ------------------------------------------------------

printf 'first\n' > "$WORK/dup.txt"
kc copy "file://$WORK/dup.txt" dropbox:/ > /dev/null
printf 'second\n' > "$WORK/dup.txt"
out="$(kc --noninteractive copy "file://$WORK/dup.txt" dropbox:/ 2>&1)"
check "upload without --overwrite leaves the original" "first" "$(kc cat dropbox:/dup.txt)"
kc --overwrite copy "file://$WORK/dup.txt" dropbox:/ > /dev/null
check "upload with --overwrite replaces it" "second" "$(kc cat dropbox:/dup.txt)"

# --- resilience ---------------------------------------------------------------

inject_fault expire_token_once
out="$(kc cat dropbox:/documents/notes.txt)"
check "recovers from an expired access token" "hello from dropbox" "$out"

inject_fault rate_limit_once
out="$(kc cat dropbox:/documents/notes.txt)"
check "retries after HTTP 429" "hello from dropbox" "$out"

# --- sidebar and System Settings ----------------------------------------------

# These run offscreen against the sandboxed XDG_DATA_HOME above, so the real
# Places sidebar is never touched.
auth() { timeout 60 "$AUTH" "$@" 2>&1; }
# grep -c prints 0 and *also* exits non-zero when there are no matches, so the
# exit status has to be swallowed rather than treated as "no file".
places_count() {
    local xbel="$XDG_DATA_HOME/user-places.xbel"
    [[ -f "$xbel" ]] || { echo 0; return; }
    grep -c 'href="dropbox:/"' "$xbel" || true
}

check_contains "sidebar starts out absent" "Not in the file manager sidebar" "$(auth --status)"

auth --sidebar show > /dev/null
check "adding the sidebar entry writes one bookmark" "1" "$(places_count)"
check_contains "status reports the sidebar entry" "Shown in the file manager sidebar" "$(auth --status)"

auth --sidebar show > /dev/null
check "adding twice does not duplicate it" "1" "$(places_count)"

auth --sidebar hide > /dev/null
check "removing the sidebar entry" "0" "$(places_count)"

auth --sidebar bogus > /dev/null 2>&1
check "an invalid --sidebar value is rejected" "2" "$?"

if command -v kcmshell6 > /dev/null; then
    check_contains "System Settings discovers the module" "kcm_dropbox" "$(timeout 60 kcmshell6 --list 2>&1 | grep -i dropbox)"
else
    ok "System Settings discovery (skipped: no kcmshell6)"
fi

# --- report -------------------------------------------------------------------

echo
echo "$PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
