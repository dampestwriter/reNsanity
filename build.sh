#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

WASI_SDK_VERSION=24.0
WABT_VERSION=1.0.42

case "$(uname -s)/$(uname -m)" in
  Linux/x86_64)
    WASI_ARCH=x86_64-linux; WASI_SHA=c6c38aab56e5de88adf6c1ebc9c3ae8da72f88ec2b656fb024eda8d4167a0bc5
    WABT_ARCH=linux-x64; WABT_SHA=84895407a6bbb80e918f33b16b2fb2206021c150b6bc9ff6f761263a745ab131 ;;
  *) echo "build.sh: an x86_64 Linux is needed (on Windows use WSL)" >&2; exit 1 ;;
esac

missing=()
for tool in curl tar sha256sum cmake ninja python3; do
  command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [ ${#missing[@]} -gt 0 ]; then
  echo "build.sh: please install: ${missing[*]}" >&2
  exit 1
fi

fetch() {
  local url=$1 sha=$2 dir=tools/toolchain/$3
  [ -d "$dir" ] && return
  mkdir -p "$dir.part"
  curl -fsSL -o "$dir.tar.gz" "$url"
  echo "$sha  $dir.tar.gz" | sha256sum -c --quiet -
  tar -xzf "$dir.tar.gz" -C "$dir.part" --strip-components=1
  rm "$dir.tar.gz"
  mv "$dir.part" "$dir"
}
fetch "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VERSION%%.*}/wasi-sdk-${WASI_SDK_VERSION}-${WASI_ARCH}.tar.gz" "$WASI_SHA" wasi-sdk
fetch "https://github.com/WebAssembly/wabt/releases/download/${WABT_VERSION}/wabt-${WABT_VERSION}-${WABT_ARCH}.tar.gz" "$WABT_SHA" wabt
python3 tools/fetch_devkitpro.py tools/toolchain/devkitpro

export DEVKITPRO="$PWD/tools/toolchain/devkitpro"
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
cmake -S . -B build -G Ninja
cmake --build build
mkdir -p out/switch/reNsanity
cp build/reNsanity.nro out/switch/reNsanity/reNsanity.nro
echo "build.sh: copy out/switch to the root of the SD card and put your PAL disc image at /switch/reNsanity/game.iso"
