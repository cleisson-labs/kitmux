#!/bin/sh
# Run cppcheck over Kitmux's own C sources. Error and warning findings fail;
# style and performance findings are left to review. Vendored tomlc99 code
# (src/adapters_toml_vendor.c and its header) keeps its upstream form.
set -eu
cppcheck=${CPPCHECK:-cppcheck}
command -v "$cppcheck" >/dev/null 2>&1 || {
  echo "static-analysis.sh: $cppcheck not found; install cppcheck first" >&2
  exit 1
}
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
work=$(mktemp -d "${TMPDIR:-/tmp}/kitmux-analysis.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

# Library headers are not given to cppcheck, so describe json-c's iteration
# macro (its strict-ANSI definition); otherwise loops abort as unknownMacro.
cat >"$work/json-c-macros.h" <<'EOF'
#define json_object_object_foreach(obj, key, val) \
    char *key = NULL; \
    struct json_object *val = NULL; \
    struct lh_entry *entry##key; \
    struct lh_entry *entry_next##key = NULL; \
    for (entry##key = json_object_get_object(obj)->head; \
         (entry##key ? (key = (char *)lh_entry_k(entry##key), \
                        val = (struct json_object *)lh_entry_v(entry##key), \
                        entry_next##key = entry##key->next, entry##key) : 0); \
         entry##key = entry_next##key)
EOF

set --
for source in src/*.c; do
  [ "$source" = src/adapters_toml_vendor.c ] || set -- "$@" "$source"
done

"$cppcheck" --version
# C17 adds no language features over C11, which every cppcheck release accepts.
"$cppcheck" --quiet --error-exitcode=1 --enable=warning \
  --language=c --std=c11 --platform=unix64 --library=posix \
  --include="$work/json-c-macros.h" -I include \
  --suppress='*:*adapters_toml_vendor.h' \
  --template=gcc "$@"
