#!/usr/bin/env bash
set -euo pipefail

# Long-running randomized differential test for hashmap.h/hashmap.c.
#
# It compiles a tiny command-driven C harness against hashmap.c, then keeps
# issuing random operations and compares the hashmap result with a simple Bash
# reference model. It is intended to run until Ctrl+C.
#
# Usage:
#   bash fuzz_diff.sh
#   bash fuzz_diff.sh --seed 123 --log fuzz.log
#   bash fuzz_diff.sh --max-ops 100000
#
# The C hashmap is not modified to run forever. This script is the long-run
# driver. It keeps an independent external reference database in a TSV file and
# periodically dumps the C hashmap through its iterator so the two files can be
# compared with ordinary shell tools.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SRC:-$ROOT_DIR/hashmap.c}"
HDR="${HDR:-$ROOT_DIR/hashmap.h}"
CC_BIN="${CC:-cc}"
SEED="${SEED:-}"
LOG_FILE="${LOG_FILE:-fuzz_diff.log}"
REF_DB="${REF_DB:-fuzz_ref.tsv}"
ACTUAL_DB="${ACTUAL_DB:-fuzz_hashmap.tsv}"
MAX_OPS="${MAX_OPS:-0}"
KEY_SPACE="${KEY_SPACE:-200}"
ITER_EVERY="${ITER_EVERY:-97}"
VALIDATE_EVERY="${VALIDATE_EVERY:-113}"

usage() {
    cat <<'EOF'
Usage: bash fuzz_diff.sh [options]

Options:
  --seed N           Random seed. Defaults to current shell RANDOM state.
  --log FILE         Action log path. Defaults to fuzz_diff.log.
  --ref-db FILE      External reference DB path. Defaults to fuzz_ref.tsv.
  --actual-db FILE   Latest hashmap iterator dump. Defaults to fuzz_hashmap.tsv.
  --max-ops N        Stop after N operations. Defaults to 0, meaning forever.
  --key-space N      Number of generated keys. Defaults to 200.
  --iter-every N     Force an iterator check every N ops. Defaults to 97.
  --validate-every N Force hm_validate() every N ops. Defaults to 113.
  -h, --help         Show this help.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --seed) SEED=${2:?missing seed}; shift 2 ;;
        --log) LOG_FILE=${2:?missing log file}; shift 2 ;;
        --ref-db) REF_DB=${2:?missing reference DB file}; shift 2 ;;
        --actual-db) ACTUAL_DB=${2:?missing actual DB file}; shift 2 ;;
        --max-ops) MAX_OPS=${2:?missing max ops}; shift 2 ;;
        --key-space) KEY_SPACE=${2:?missing key space}; shift 2 ;;
        --iter-every) ITER_EVERY=${2:?missing iter interval}; shift 2 ;;
        --validate-every) VALIDATE_EVERY=${2:?missing validate interval}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$KEY_SPACE:$ITER_EVERY:$VALIDATE_EVERY:$MAX_OPS" in
    *[!0-9:]*|'') echo "numeric options must be non-negative integers" >&2; exit 2 ;;
esac
[ "$KEY_SPACE" -gt 0 ] || { echo "--key-space must be > 0" >&2; exit 2; }
[ "$ITER_EVERY" -gt 0 ] || { echo "--iter-every must be > 0" >&2; exit 2; }
[ "$VALIDATE_EVERY" -gt 0 ] || { echo "--validate-every must be > 0" >&2; exit 2; }

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/hm_fuzz.XXXXXX")"
HARNESS_C="$TMP_DIR/hm_fuzz_harness.c"
HARNESS_BIN="$TMP_DIR/hm_fuzz_harness"

if [[ ! -f "$SRC" ]]; then
  echo "error: source file not found: $SRC" >&2
  exit 1
fi
if [[ ! -f "$HDR" ]]; then
  echo "error: header file not found: $HDR" >&2
  exit 1
fi

read -r -a EXTRA_CFLAGS <<< "${CFLAGS:--O2}"
COMMON_FLAGS=(-std=c99 -Wall -Wextra -pedantic "${EXTRA_CFLAGS[@]}")

cp "$SRC" "$TMP_DIR/hashmap.c"
cp "$HDR" "$TMP_DIR/hashmap.h"

cleanup() {
    local status=$?
    if [ -n "${HM_PID:-}" ] && kill -0 "$HM_PID" 2>/dev/null; then
        kill "$HM_PID" 2>/dev/null || true
        wait "$HM_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT INT TERM

cat > "$HARNESS_C" <<'C_EOF'
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashmap.h"

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1u;
    char *p = (char *)malloc(n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

static void free_value(void *value, void *ctx) {
    (void)ctx;
    free(value);
}

int main(void) {
    hashmap hm;
    hm_config cfg;
    char line[4096];

    memset(&cfg, 0, sizeof(cfg));
    cfg.value_free = free_value;

    if (hm_init_with_config(&hm, &cfg) != HM_OK) {
        puts("ERR init");
        fflush(stdout);
        return 1;
    }

    while (fgets(line, sizeof(line), stdin)) {
        char *cmd = strtok(line, " \t\r\n");
        if (!cmd) continue;

        if (strcmp(cmd, "SET") == 0) {
            char *key = strtok(NULL, " \t\r\n");
            char *value_text = strtok(NULL, " \t\r\n");
            char *value;
            void *old_value = NULL;
            hm_result rc;
            if (!key || !value_text) { puts("ERR bad-set"); fflush(stdout); continue; }
            value = xstrdup(value_text);
            if (!value) { puts("ERR oom"); fflush(stdout); continue; }
            rc = hm_put(&hm, key, value, &old_value);
            if (rc != HM_OK) {
                free(value);
                printf("ERR set %d\n", (int)rc);
            } else {
                free(old_value);
                puts("OK");
            }
            fflush(stdout);
        } else if (strcmp(cmd, "GET") == 0) {
            char *key = strtok(NULL, " \t\r\n");
            void *value = NULL;
            hm_result rc;
            if (!key) { puts("ERR bad-get"); fflush(stdout); continue; }
            rc = hm_get(&hm, key, &value);
            if (rc == HM_OK) printf("OK %s\n", value ? (char *)value : "");
            else if (rc == HM_ERR_NOT_FOUND) puts("NF");
            else printf("ERR get %d\n", (int)rc);
            fflush(stdout);
        } else if (strcmp(cmd, "DEL") == 0) {
            char *key = strtok(NULL, " \t\r\n");
            void *old_value = NULL;
            hm_result rc;
            if (!key) { puts("ERR bad-del"); fflush(stdout); continue; }
            rc = hm_remove(&hm, key, &old_value);
            if (rc == HM_OK) {
                printf("OK %s\n", old_value ? (char *)old_value : "");
                free(old_value);
            } else if (rc == HM_ERR_NOT_FOUND) {
                puts("NF");
            } else {
                printf("ERR del %d\n", (int)rc);
            }
            fflush(stdout);
        } else if (strcmp(cmd, "LEN") == 0) {
            printf("OK %zu\n", hm_len(&hm));
            fflush(stdout);
        } else if (strcmp(cmd, "ITER") == 0) {
            hm_iter it;
            const char *key;
            void *value;
            hm_result rc;
            rc = hm_iter_init(&hm, &it);
            if (rc != HM_OK) {
                printf("ERR iter-init %d\n", (int)rc);
                puts("END");
                fflush(stdout);
                continue;
            }
            while ((rc = hm_iter_next(&hm, &it, &key, &value)) == HM_OK) {
                printf("%s=%s\n", key, value ? (char *)value : "");
            }
            if (rc != HM_ERR_NOT_FOUND) printf("ERR iter-next %d\n", (int)rc);
            puts("END");
            fflush(stdout);
        } else if (strcmp(cmd, "VALIDATE") == 0) {
            hm_result rc = hm_validate(&hm);
            if (rc == HM_OK) puts("OK");
            else printf("ERR validate %d\n", (int)rc);
            fflush(stdout);
        } else if (strcmp(cmd, "QUIT") == 0) {
            puts("OK");
            fflush(stdout);
            break;
        } else {
            puts("ERR unknown-command");
            fflush(stdout);
        }
    }

    hm_free(&hm);
    return 0;
}
C_EOF

echo "[fuzz] cc=$CC_BIN flags=${COMMON_FLAGS[*]}"
"$CC_BIN" "${COMMON_FLAGS[@]}" -I"$TMP_DIR" -c "$TMP_DIR/hashmap.c" -o "$TMP_DIR/hashmap.o"
"$CC_BIN" "${COMMON_FLAGS[@]}" -I"$TMP_DIR" "$HARNESS_C" "$TMP_DIR/hashmap.o" -o "$HARNESS_BIN"

if [ -n "$SEED" ]; then
    RANDOM=$SEED
else
    SEED=$RANDOM
    RANDOM=$SEED
fi

: > "$LOG_FILE"
{
    echo "# fuzz_diff.sh started $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "# seed=$SEED key_space=$KEY_SPACE max_ops=$MAX_OPS iter_every=$ITER_EVERY validate_every=$VALIDATE_EVERY"
    echo "# ref_db=$REF_DB actual_db=$ACTUAL_DB"
    echo "# replay: bash fuzz_diff.sh --seed $SEED --max-ops <op-count-before-failure>"
} >> "$LOG_FILE"

: > "$REF_DB"
: > "$ACTUAL_DB"

declare -A REF=()

coproc HM_PROC { "$HARNESS_BIN"; }
HM_PID=$!

hm_send() {
    printf '%s\n' "$1" >&"${HM_PROC[1]}"
}

hm_read_line() {
    if ! IFS= read -r HM_LINE <&"${HM_PROC[0]}"; then
        echo "hashmap harness exited unexpectedly" >&2
        exit 1
    fi
}

fail() {
    local op=$1
    local msg=$2
    echo "FAIL at op $op: $msg" >&2
    echo "# FAIL at op $op: $msg" >> "$LOG_FILE"
    echo "# seed=$SEED" >> "$LOG_FILE"
    echo "# ref_db=$REF_DB" >> "$LOG_FILE"
    echo "# actual_db=$ACTUAL_DB" >> "$LOG_FILE"
    exit 1
}

sync_ref_db() {
    local key tmp
    tmp="$REF_DB.tmp.$$"
    : > "$tmp"
    for key in "${!REF[@]}"; do
        printf '%s\t%s\n' "$key" "${REF[$key]}" >> "$tmp"
    done
    sort "$tmp" -o "$tmp"
    mv "$tmp" "$REF_DB"
}

ref_has_key() {
    [[ ${REF[$1]+present} == present ]]
}

ref_set() {
    local key=$1 value=$2
    REF[$key]=$value
}

ref_del() {
    local key=$1
    unset 'REF[$key]'
}

check_get() {
    local op=$1 key=$2 expect got expect_line
    hm_send "GET $key"
    hm_read_line
    got=$HM_LINE
    if ref_has_key "$key"; then
        expect_line="OK ${REF[$key]}"
    else
        expect_line="NF"
    fi
    [ "$got" = "$expect_line" ] || fail "$op" "GET $key expected '$expect_line' got '$got'"
}

check_len() {
    local op=$1 got expect
    hm_send "LEN"
    hm_read_line
    got=$HM_LINE
    expect="OK ${#REF[@]}"
    [ "$got" = "$expect" ] || fail "$op" "LEN expected '$expect' got '$got'"
}

check_validate() {
    local op=$1 got
    hm_send "VALIDATE"
    hm_read_line
    got=$HM_LINE
    [ "$got" = "OK" ] || fail "$op" "VALIDATE expected OK got '$got'"
}

check_iter() {
    local op=$1 got_file expect_file line
    got_file="$TMP_DIR/iter.got"
    expect_file="$TMP_DIR/iter.expect"
    : > "$got_file"
    : > "$expect_file"

    hm_send "ITER"
    while :; do
        hm_read_line
        line=$HM_LINE
        [ "$line" = "END" ] && break
        case "$line" in
            ERR*) fail "$op" "ITER returned '$line'" ;;
        esac
        printf '%s\n' "$line" | sed 's/=/\t/' >> "$got_file"
    done

    sync_ref_db
    cp "$REF_DB" "$expect_file"

    sort "$got_file" -o "$got_file"
    sort "$expect_file" -o "$expect_file"
    cp "$got_file" "$ACTUAL_DB"
    if ! cmp -s "$got_file" "$expect_file"; then
        {
            echo "# iterator mismatch at op $op"
            echo "# expected reference DB: $REF_DB"
            sed 's/^/#   /' "$expect_file"
            echo "# actual hashmap DB: $ACTUAL_DB"
            sed 's/^/#   /' "$got_file"
        } >> "$LOG_FILE"
        fail "$op" "ITER mismatch; compare: diff -u '$REF_DB' '$ACTUAL_DB'"
    fi
}

op=0
echo "running randomized differential test forever; press Ctrl+C to stop"
echo "log file: $LOG_FILE"
echo "reference DB: $REF_DB"
echo "latest hashmap dump: $ACTUAL_DB"
echo "seed: $SEED"

while :; do
    op=$((op + 1))
    if [ "$MAX_OPS" -gt 0 ] && [ "$op" -gt "$MAX_OPS" ]; then
        break
    fi

    key=$(printf 'k%04d' $((RANDOM % KEY_SPACE)))
    action=$((RANDOM % 100))

    if [ $((op % ITER_EVERY)) -eq 0 ]; then
        echo "$op ITER" >> "$LOG_FILE"
        check_iter "$op"
    elif [ $((op % VALIDATE_EVERY)) -eq 0 ]; then
        echo "$op VALIDATE" >> "$LOG_FILE"
        check_validate "$op"
    elif [ "$action" -lt 45 ]; then
        value=$(printf 'v%08d_%05d' "$op" $((RANDOM % 100000)))
        echo "$op SET $key $value" >> "$LOG_FILE"
        hm_send "SET $key $value"
        hm_read_line
        got=$HM_LINE
        [ "$got" = "OK" ] || fail "$op" "SET $key $value expected OK got '$got'"
        ref_set "$key" "$value"
        sync_ref_db
    elif [ "$action" -lt 78 ]; then
        # Mix existing and non-existing gets. Random key selection already
        # naturally produces both as the reference map evolves.
        echo "$op GET $key" >> "$LOG_FILE"
        check_get "$op" "$key"
    elif [ "$action" -lt 94 ]; then
        echo "$op DEL $key" >> "$LOG_FILE"
        hm_send "DEL $key"
        hm_read_line
        got=$HM_LINE
        if ref_has_key "$key"; then
            expect="OK ${REF[$key]}"
            [ "$got" = "$expect" ] || fail "$op" "DEL $key expected '$expect' got '$got'"
            ref_del "$key"
            sync_ref_db
        else
            [ "$got" = "NF" ] || fail "$op" "DEL $key expected NF got '$got'"
        fi
    else
        echo "$op LEN" >> "$LOG_FILE"
        check_len "$op"
    fi

    if [ $((op % 10000)) -eq 0 ]; then
        echo "ok ops=$op live=${#REF[@]}"
    fi
done

hm_send "QUIT"
hm_read_line
echo "completed ops=$((op - 1)) live=${#REF[@]}"
