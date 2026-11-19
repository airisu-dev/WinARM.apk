#!/usr/bin/env bash
# api_coverage.sh -- how much of Android's public API (API 36) a staged runtime
# has, as an app would link against it (windows/tools/apicov):
#
#   windows/scripts/api_coverage.sh --runtime <staged runtime> --out <report dir>
#                                   [--jre <java home>] [--label <name>] [--self-check]
#
# <runtime> is what stage_runtime.sh or a release zip gives: lib/ (the jars and
# the java.base patches) and jre/. Without jre/ (a plain build_java.sh output),
# --jre names the Java home whose modules stand in for it; the default is the
# JDK running the tool.
#
# Writes <out>/report.md, report.json and gaps.tsv. --self-check also matches
# the API against the real AOSP framework (fetch_android_api.sh --aosp-classes,
# about 200 MB once) into <out>/aosp-self-check: it should come out complete,
# and what it does not have is what the tool cannot judge.
#
# Needs a JDK 21 (JAVA_HOME, else javac and java from PATH) and curl.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
TOOL="$HERE/../tools/apicov"

runtime="" out="" jre="" label="" self_check=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --runtime) runtime="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --jre) jre="$2"; shift 2 ;;
        --label) label="$2"; shift 2 ;;
        --self-check) self_check=true; shift ;;
        *) echo "usage: api_coverage.sh --runtime <dir> --out <dir> [--jre <home>] [--label <name>] [--self-check]" >&2
           exit 2 ;;
    esac
done
[[ -n "$runtime" && -n "$out" ]] || { echo "api_coverage.sh: --runtime and --out are required" >&2; exit 2; }

JAVAC=javac
JAVA=java
if [[ -n "${JAVA_HOME:-}" ]]; then
    JAVAC="$JAVA_HOME/bin/javac"
    JAVA="$JAVA_HOME/bin/java"
fi
# javac and java on Windows want Windows paths; MSYS2 translates only plain arguments.
win() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
ASM="--add-exports java.base/jdk.internal.org.objectweb.asm=ALL-UNNAMED"

if $self_check; then
    DATA="$("$HERE/fetch_android_api.sh" --aosp-classes | tail -n 1)"
else
    DATA="$("$HERE/fetch_android_api.sh" | tail -n 1)"
fi

mkdir -p "$out"
CLASSES="$out/.tool"
rm -rf "$CLASSES"
mkdir -p "$CLASSES"
# shellcheck disable=SC2086
"$JAVAC" $ASM -Xlint:all -Werror -d "$(win "$CLASSES")" "$(win "$TOOL")"/*.java

args=(--api "$(win "$DATA/android-36.txt")" --runtime "$(win "$runtime")" --out "$(win "$out")" --strict)
[[ -n "$jre" ]] && args+=(--jre "$(win "$jre")")
[[ -n "$label" ]] && args+=(--label "$label")
# shellcheck disable=SC2086
"$JAVA" $ASM -cp "$(win "$CLASSES")" com.winapk.apicov.ApiCoverage "${args[@]}"

if $self_check; then
    # shellcheck disable=SC2086
    "$JAVA" $ASM -cp "$(win "$CLASSES")" com.winapk.apicov.ApiCoverage --api "$(win "$DATA/android-36.txt")" \
        --cp "$(win "$DATA/android-all-36.jar")" --label "AOSP framework (self-check)" \
        --out "$(win "$out/aosp-self-check")" --strict
fi
rm -rf "$CLASSES"
