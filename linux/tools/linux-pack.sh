#!/bin/sh
# Prepares a game pack on aarch64 (or x86_64) Linux with the same packer the Android app and the Mac use (scripts/pack_android.py):
# the code rewrite (Python, Capstone 5, lz4, zbic) and the Vulkan shader library (the translator's portable .NET dlls under the .NET 10
# runtime, SPIRV-Tools' spirv-val). Everything it installs stays in one folder, no root needed.
#   linux/tools/linux-pack.sh setup                     once: <env>/venv (Python packages), <env>/dotnet (.NET 10 runtime), the translator wrapper
#   linux/tools/linux-pack.sh <game.nup or dump folder> [pack_android.py options]
#                                                       the pack goes to <title ID>-work/android-pack beside the game file (the launcher's
#                                                       picker finds it under ~/Games); shaders take minutes (Mario Kart World: 11,603)
# NX_PACK_ENV: the environment folder (default ~/.local/share/banananative/packer). Needs python3 (3.12+; 3.14 builds the shader library without
# backports.zstd), curl, spirv-val on PATH (Ubuntu/Debian: spirv-tools) and a checkout of this repository (this script's own).
# The pins setup downloads, each checked against its hash before use (bump: the version and the hash from pypi.org/pypi/pip/<version>/json, the same tag
# of github.com/pypa/get-pip, and the runtime's linux tar.gz in builds.dotnet.microsoft.com/dotnet/release-metadata/10.0/releases.json). The Python
# packages are exact versions, wheels only, not hash-checked.
PIP_VERSION=26.3   # (only where the system Python has no ensurepip: get-pip.py installs this wheel, with no index)
PIP_URL=https://files.pythonhosted.org/packages/07/25/ee8c38ae72d66b844ce8eb9711ba905dba21ca1a8a0c960057c41cd23337/pip-26.3-py3-none-any.whl
PIP_SHA256=f3b60b20ffaadd4415ed7957212a38ac63fa4c5fa375bd0f4a797777940fad13
GETPIP_URL=https://raw.githubusercontent.com/pypa/get-pip/26.3/public/get-pip.py
GETPIP_SHA256=f7654c5589318e1d0789575b2586ce76937eb9943602adda8b15b3be3aa0dac3
PACKAGES="capstone==5.0.9 lz4==4.4.5 zbic==1.0.1.post1"
DOTNET_VERSION=10.0.15   # (the runtime, glibc only: Microsoft's linux-arm64 and linux-x64 archives)
DOTNET_ARM64_SHA512=e2e8b907d9eb6c239cccf5c1fe67fd9a72c56fddccedca9ba41c8a53ff034293b335fdd9f7ab504f09bf7b3f165e0d14e34fc9ea1f06cc0e86d0dfb06d274b6a
DOTNET_X64_SHA512=a90177d75bc891f71aa814dde3bcf76a356f5a506824669f1891754457954a3d13b72f3ca04c98fa0fe588514a5a781f8a87b17930ee802105c14f9edb28318a
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ENV=${NX_PACK_ENV:-${XDG_DATA_HOME:-$HOME/.local/share}/banananative/packer}
XBIN="$ROOT/dm/gfx/tools/dmsh_xlate_bin"
WRAP="$XBIN/dmsh_xlate-linux"   # (beside the dlls: vk_android_build.py checks the dlls next to the translator it is given)

# fetch <url> <file> <256|512> <hash>: the download goes to <file>.part and is renamed only when its hash is the pin, else it is deleted and the
# script stops (exit, not set -e: setup also runs from a || list)
digest() { if command -v "sha$1sum" >/dev/null; then "sha$1sum"; else shasum -a "$1"; fi | cut -d' ' -f1; }
fetch() {
  rm -f "$2.part"; curl -fsSL --retry 3 -o "$2.part" "$1" || { rm -f "$2.part"; echo "[RED] $1: download failed"; exit 1; }
  got=$(digest "$3" < "$2.part"); [ "$got" = "$4" ] || { rm -f "$2.part"; echo "[RED] $(basename "$2"): sha$3 $got, expected $4"; exit 1; }
  mv "$2.part" "$2"
}

setup() {
  mkdir -p "$ENV"
  if [ ! -x "$ENV/venv/bin/python" ]; then
    python3 -m venv "$ENV/venv" 2>/dev/null || {   # (no ensurepip, as on Ubuntu without python3-venv: the pinned pip wheel through its bootstrap script)
      rm -rf "$ENV/venv" "$ENV/pip"; mkdir -p "$ENV/pip"
      fetch "$GETPIP_URL" "$ENV/get-pip.py" 256 "$GETPIP_SHA256"
      fetch "$PIP_URL" "$ENV/pip/pip-$PIP_VERSION-py3-none-any.whl" 256 "$PIP_SHA256"
      python3 -m venv --without-pip "$ENV/venv"
      "$ENV/venv/bin/python" "$ENV/get-pip.py" -q --no-index --find-links "$ENV/pip" "pip==$PIP_VERSION" || { rm -rf "$ENV/venv"; exit 1; }; }
  fi
  "$ENV/venv/bin/python" -m pip -q install --only-binary :all: $PACKAGES
  if [ ! -x "$ENV/dotnet/dotnet" ] || ! "$ENV/dotnet/dotnet" --list-runtimes | grep -q '^Microsoft.NETCore.App 10\.'; then
    case $(uname -m) in
      aarch64|arm64) a=arm64 h=$DOTNET_ARM64_SHA512 ;;
      x86_64) a=x64 h=$DOTNET_X64_SHA512 ;;
      *) echo "[RED] $(uname -m): there is no pinned .NET runtime for this CPU"; exit 1 ;;
    esac
    # (ldd names musl on musl systems; /lib/ld-musl-* only counts without ldd, as glibc systems can have musl installed beside)
    if (ldd --version 2>&1 || true) | grep -qi musl || { ! command -v ldd >/dev/null && ls /lib/ld-musl-* >/dev/null 2>&1; }; then
      echo "[RED] musl is not supported by the pinned .NET runtime"; exit 1
    fi
    fetch "https://builds.dotnet.microsoft.com/dotnet/Runtime/$DOTNET_VERSION/dotnet-runtime-$DOTNET_VERSION-linux-$a.tar.gz" "$ENV/dotnet.tar.gz" 512 "$h"
    rm -rf "$ENV/dotnet.part"; mkdir "$ENV/dotnet.part"; tar -xzf "$ENV/dotnet.tar.gz" -C "$ENV/dotnet.part"; rm -f "$ENV/dotnet.tar.gz"
    [ -x "$ENV/dotnet.part/dotnet" ] || { rm -rf "$ENV/dotnet.part"; echo "[RED] the .NET archive has no dotnet at its root"; exit 1; }
    rm -rf "$ENV/dotnet"; mv "$ENV/dotnet.part" "$ENV/dotnet"
  fi
  printf '#!/bin/sh\n# The shader translator on Linux (linux/tools/linux-pack.sh): the portable dlls beside this script under the .NET 10 runtime\nexec "${DOTNET_ROOT:-%s}/dotnet" "$(dirname "$0")/dmsh_xlate.dll" "$@"\n' "$ENV/dotnet" > "$WRAP"
  chmod +x "$WRAP"
  command -v spirv-val >/dev/null || echo "[RED] spirv-val is not on PATH: install SPIRV-Tools (Ubuntu/Debian: sudo apt install spirv-tools) before packing shaders"
  echo "translator: $("$WRAP" --source-sha) (the tree expects $(cd "$ROOT/dm/gfx/tools" && "$ENV/venv/bin/python" -c 'from dmsh_chain import xlate_source_sha as s; print(s() if callable(s) else s)'))"
  echo "ready: $ENV"
}

case "$1" in
  setup) setup ;;
  ""|-h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//' ;;
  *) [ -x "$ENV/venv/bin/python" ] && [ -x "$WRAP" ] || setup
     DOTNET_ROOT="$ENV/dotnet" XLATE="$WRAP" exec "$ENV/venv/bin/python" "$ROOT/scripts/pack_android.py" "$@" ;;
esac
