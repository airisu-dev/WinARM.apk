#!/usr/bin/env bash
# fetch_android_api.sh -- the reference data the API coverage report
# (windows/tools/apicov) measures the runtime against:
#
#   android-36.txt     the public API of Android 16 (API 36), Mainline modules
#                      included, as AOSP's prebuilts/sdk publishes it
#                      (36/public/api/android.txt), in metalava's signature format.
#                      Generated from AOSP sources (Apache 2.0).
#   android-all-36.jar with --aosp-classes: Robolectric's build of the AOSP
#                      framework for Android 16 (org.robolectric:android-all,
#                      Apache 2.0, about 200 MB). The report's self-check reads
#                      it: matched against android-36.txt, the real framework
#                      must come out complete.
#
#   windows/scripts/fetch_android_api.sh [--aosp-classes]
#
# Each file is pinned and checked against its SHA-256, and kept in
# thirdparty/android-api/, which is not in git, so a second run downloads
# nothing. Prints the directory on its last line.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
DIR="$HERE/../../thirdparty/android-api"
mkdir -p "$DIR"
DIR="$(cd -- "$DIR" && pwd)"

# AOSP platform/prebuilts/sdk at this commit; gitiles serves a file base64-encoded with ?format=TEXT.
SDK_COMMIT=3af7c93524be6f51e092b87b17f009f13ee98b43
API_URL="https://android.googlesource.com/platform/prebuilts/sdk/+/$SDK_COMMIT/36/public/api/android.txt?format=TEXT"
API_SHA256=c90c76f4aba36b78659643533aacb5028e3178258bb4a83f6de23d8b35a83f38

AOSP_VERSION=16-robolectric-13921718-2
AOSP_URL="https://repo1.maven.org/maven2/org/robolectric/android-all/$AOSP_VERSION/android-all-$AOSP_VERSION.jar"
AOSP_SHA256=ead18e465e8a4c2b93a59d7d7a68c1830f3cd8f1b6bcdfd96f45d658ba979ce3

aosp=false
for a in "$@"; do
    case "$a" in
        --aosp-classes) aosp=true ;;
        *) echo "usage: fetch_android_api.sh [--aosp-classes]" >&2; exit 2 ;;
    esac
done

ok() { [[ -f "$1" ]] && echo "$2  $1" | sha256sum -c --status; }

# fetch <file> <sha256> <url> <decode: base64|none>
fetch() {
    local file="$1" sha="$2" url="$3" decode="$4"
    ok "$file" "$sha" && return 0
    echo "[android-api] downloading $(basename "$file")" >&2
    if [[ "$decode" == base64 ]]; then
        curl -sSfL "$url" | base64 -d > "$file.part"
    else
        curl -sSfL -o "$file.part" "$url"
    fi
    if ! echo "$sha  $file.part" | sha256sum -c --status; then
        echo "[android-api] checksum mismatch for $(basename "$file")" >&2
        rm -f "$file.part"
        exit 1
    fi
    mv "$file.part" "$file"
}

fetch "$DIR/android-36.txt" "$API_SHA256" "$API_URL" base64
if $aosp; then fetch "$DIR/android-all-36.jar" "$AOSP_SHA256" "$AOSP_URL" none; fi
echo "$DIR"
