#!/usr/bin/env bash
# run_test.sh -- builds the API coverage tool (windows/tools/apicov) and runs
# ApiCovTest against the fixture beside this script. Needs a JDK 21 (JAVA_HOME,
# else javac and java from PATH). Exit code 0 when every expectation holds.
#
#   windows/test/apicov/run_test.sh [work dir]

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
TOOL="$HERE/../../tools/apicov"
WORK="${1:-$(mktemp -d)}"
JAVAC=javac
JAVA=java
if [[ -n "${JAVA_HOME:-}" ]]; then
    JAVAC="$JAVA_HOME/bin/javac"
    JAVA="$JAVA_HOME/bin/java"
fi
# javac and java on Windows want Windows paths; MSYS2 translates only plain arguments.
win() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
SEP=":"
command -v cygpath > /dev/null 2>&1 && SEP=";"
ASM="--add-exports java.base/jdk.internal.org.objectweb.asm=ALL-UNNAMED"

rm -rf "$WORK/tool" "$WORK/fixture"
mkdir -p "$WORK/tool" "$WORK/fixture"
# shellcheck disable=SC2086
"$JAVAC" $ASM -Xlint:all -Werror -d "$(win "$WORK/tool")" "$(win "$TOOL")"/*.java "$(win "$HERE/ApiCovTest.java")"
"$JAVAC" -d "$(win "$WORK/fixture")" "$(win "$HERE/src/fx")"/*.java
# shellcheck disable=SC2086
"$JAVA" $ASM -cp "$(win "$WORK/tool")" com.winapk.apicov.ApiCovTest "$(win "$HERE")" "$(win "$WORK/fixture")"
