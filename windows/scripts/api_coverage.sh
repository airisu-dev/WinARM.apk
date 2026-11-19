#!/usr/bin/env bash
# api_coverage.sh -- how much of Android's public API (API 36) a staged runtime
# has, as an app would link against it, and how much of that does work
# (windows/tools/apicov):
#
#   windows/scripts/api_coverage.sh --runtime <staged runtime> --out <report dir>
#                                   [--jre <java home>] [--natives <file>] [--label <name>] [--self-check]
#
# <runtime> is what stage_runtime.sh or a release zip gives: lib/ (the jars and
# the java.base patches), jre/ and bin/. Without jre/ (a plain build_java.sh
# output), --jre names the Java home whose modules stand in for it; the default
# is the JDK running the tool.
#
# Each method that links is judged against the same method in the AOSP
# framework (fetch_android_api.sh --aosp-classes, about 200 MB once): real code,
# trivial as in AOSP, or a stub. Natives are judged by how the runtime binds
# them: bin/winapk-runtime.exe --list-natives, run here when the runtime has it,
# or a listing given with --natives; without either they are "unknown".
#
# Writes <out>/report.md, report.json, gaps.tsv and stubs.tsv. --self-check
# also matches the API against the AOSP framework itself into
# <out>/aosp-self-check: it should come out complete, and what it does not
# have is what the tool cannot judge.
#
# Needs a JDK 21 (JAVA_HOME, else javac and java from PATH) and curl.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
TOOL="$HERE/../tools/apicov"

runtime="" out="" jre="" natives="" label="" self_check=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --runtime) runtime="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --jre) jre="$2"; shift 2 ;;
        --natives) natives="$2"; shift 2 ;;
        --label) label="$2"; shift 2 ;;
        --self-check) self_check=true; shift ;;
        *) echo "usage: api_coverage.sh --runtime <dir> --out <dir> [--jre <home>] [--natives <file>]" \
                "[--label <name>] [--self-check]" >&2
           exit 2 ;;
    esac
done
[[ -n "$runtime" && -n "$out" ]] || { echo "api_coverage.sh: --runtime and --out are required" >&2; exit 2; }
# Absolute: winapk-runtime cannot load the JVM from a relative --jdk (jvm.dll's dependencies resolve from its
# own directory, which it must name in full).
runtime="$(cd -- "$runtime" && pwd)"
[[ -n "$jre" ]] && jre="$(cd -- "$jre" && pwd)"

JAVAC=javac
JAVA=java
if [[ -n "${JAVA_HOME:-}" ]]; then
    JAVAC="$JAVA_HOME/bin/javac"
    JAVA="$JAVA_HOME/bin/java"
fi
# javac and java on Windows want Windows paths; MSYS2 translates only plain arguments.
win() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
ASM="--add-exports java.base/jdk.internal.org.objectweb.asm=ALL-UNNAMED"

DATA="$("$HERE/fetch_android_api.sh" --aosp-classes | tail -n 1)"
mkdir -p "$out"

# The runtime's own binding of its natives: the JVM it would run an app on, with no app.
if [[ -z "$natives" && -x "$runtime/bin/winapk-runtime.exe" ]]; then
    natives="$out/natives.txt"
    rt_jre="${jre:-$runtime/jre}"
    "$runtime/bin/winapk-runtime.exe" --jdk "$(win "$rt_jre")" --lib "$(win "$runtime/lib")" \
        --list-natives "$(win "$natives")"
fi

CLASSES="$out/.tool"
rm -rf "$CLASSES"
mkdir -p "$CLASSES"
# shellcheck disable=SC2086
"$JAVAC" $ASM -Xlint:all -Werror -d "$(win "$CLASSES")" "$(win "$TOOL")"/*.java

args=(--api "$(win "$DATA/android-36.txt")" --runtime "$(win "$runtime")" --out "$(win "$out")" --strict
      --aosp "$(win "$DATA/android-all-36.jar")")
[[ -n "$jre" ]] && args+=(--jre "$(win "$jre")")
[[ -n "$natives" ]] && args+=(--natives "$(win "$natives")")
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
