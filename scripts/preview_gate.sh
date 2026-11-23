#!/bin/bash
# Gate before a preview or release build: the checks that the unit tests do not make. Run from the repository root:
#   bash scripts/preview_gate.sh <base> [<head>=HEAD] [<previous apk>]
# 1. pr_risk.py over the whole range (the lines a reviewer reads; the report goes to $OUT/pr_risk.txt, it never passes by itself)
# 2. what runs differently: new game profiles, nxGames, new Settings keys, manifest or Gradle changes, binaries
# 3. the APK builds (the host's Vulkan and Android C is compiled only here; the unit tests do not compile it). Needs the pack stage: NX_PACK_STAGE
# 4. with a previous APK: the permissions of both (apkanalyzer), a new one fails the gate
# Exit 0 only when 2 to 4 pass; 1 prints the list to read, and the verdict per line is still the reviewer's
set -u
BASE=${1:?usage: preview_gate.sh <base> [head] [previous apk]}; HEAD=${2:-HEAD}; PREV=${3:-}
OUT=${OUT:-$(mktemp -d)}; fail=0; mkdir -p "$OUT"
echo "== 1. pr_risk $BASE..$HEAD"
python3 dm/tools/pr_risk.py "$BASE" "$HEAD" > "$OUT/pr_risk.txt" 2>&1; echo "   $(head -1 "$OUT/pr_risk.txt" | cut -c1-120) -> $OUT/pr_risk.txt"
echo "== 2. what changes"
echo "   binaries added or changed:"; git diff --numstat "$BASE" "$HEAD" | awk '$1=="-"{print "     " $3}'
echo "   third_party / manifest / gradle / workflow files:"; git diff --name-only "$BASE" "$HEAD" | grep -E 'third_party/|AndroidManifest|\.gradle|gradle\.properties|(^|/)\.(github|forgejo)/' | sed 's/^/     /'
echo "   new game profiles:"; git diff --diff-filter=A --name-only "$BASE" "$HEAD" | grep -E '^dm/games/[0-9A-F]{16}/game\.tsv$' | sed 's/^/     /'
echo "   nxGames:"; git diff -U0 "$BASE" "$HEAD" -- android/app/gradle.properties | grep '^[+-]nxGames' | sed 's/^/     /'
if git diff --name-only "$BASE" "$HEAD" | grep -qE 'third_party/|AndroidManifest'; then echo "   [GATE] third_party or manifest changed: read it"; fail=1; fi
echo "== 3. APK build"
if [ -z "${NX_PACK_STAGE:-}" ]; then echo "   [GATE] NX_PACK_STAGE is not set (the pack stage of android/ondevice/build.sh)"; fail=1
else ( cd android/app && rm -rf out && nice ./gradlew -q -PnxVersion=0.0-gate -PnxPackStage="$NX_PACK_STAGE" apk > "$OUT/gradle.log" 2>&1 ) \
  && echo "   built: $(ls android/app/out/*.apk)" || { echo "   [GATE] the APK does not build: $(grep -m3 -E 'error:|FAILED' "$OUT/gradle.log")"; fail=1; }; fi
echo "== 4. permissions"
APK=$(ls android/app/out/*.apk 2>/dev/null | head -1)
if [ -n "$PREV" ] && [ -n "$APK" ]; then
  diff <(apkanalyzer manifest permissions "$PREV" | sort) <(apkanalyzer manifest permissions "$APK" | sort) > "$OUT/perms.diff" \
    && echo "   same permissions as $(basename "$PREV")" || { echo "   [GATE] permissions differ:"; sed 's/^/     /' "$OUT/perms.diff"; fail=1; }
else echo "   skipped (no previous APK given or no APK)"; fi
echo "== result: $([ $fail = 0 ] && echo pass || echo 'FAIL, see [GATE] lines')   (report folder $OUT)"; exit $fail
