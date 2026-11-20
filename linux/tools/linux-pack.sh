#!/bin/sh
# Prepares a game pack on aarch64 (or x86_64) Linux with the same packer the Android app and the Mac use (scripts/pack_android.py).
# Python, Tkinter, Capstone, lz4, zbic, SPIRV-Tools and the .NET 10 runtime are installed into one folder.
#
#   linux/tools/linux-pack.sh setup
#       Install/update the environment.
#
#   linux/tools/linux-pack.sh
#   linux/tools/linux-pack.sh gui
#       Launch the graphical packer.
#
#   linux/tools/linux-pack.sh pack <game.nup or dump folder> [pack_android.py options]
#   linux/tools/linux-pack.sh <game.nup or dump folder> [pack_android.py options]
#       Build a game pack using the command-line packer.
#
# NX_PACK_ENV: environment folder (default ~/.local/share/banananative/packer).
# Requires curl, tar, and a glibc-based Linux distribution.
# Micromamba and .NET downloads are pinned and hash-verified.
# Conda-forge environment packages are solver-selected.

PIP_VERSION=26.3
PACKAGES="capstone==5.0.9 lz4==4.4.5 zbic==1.0.1.post1"

MICROMAMBA_VERSION=2.10.0-0
MICROMAMBA_LINUX64_SHA256=701f39c06f8e6a670a0e186c8479b9e5c0502a2f0cda6cef65d83bdd2875e8bd
MICROMAMBA_ARM64_SHA256=3bb56786e21b9b24024f599fe9185b4a59dc3f9f59937a42311ee583adbbb23c

DOTNET_VERSION=10.0.15
DOTNET_ARM64_SHA512=e2e8b907d9eb6c239cccf5c1fe67fd9a72c56fddccedca9ba41c8a53ff034293b335fdd9f7ab504f09bf7b3f165e0d14e34fc9ea1f06cc0e86d0dfb06d274b6a
DOTNET_X64_SHA512=a90177d75bc891f71aa814dde3bcf76a356f5a506824669f1891754457954a3d13b72f3ca04c98fa0fe588514a5a781f8a87b17930ee802105c14f9edb28318a

set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ENV=${NX_PACK_ENV:-${XDG_DATA_HOME:-$HOME/.local/share}/banananative/packer}
XBIN="$ROOT/dm/gfx/tools/dmsh_xlate_bin"
WRAP="$XBIN/dmsh_xlate-linux"

# fetch <url> <file> <256|512> <hash>
digest() {
  if command -v "sha$1sum" >/dev/null; then
    "sha$1sum"
  else
    shasum -a "$1"
  fi | cut -d' ' -f1
}

fetch() {
  rm -f "$2.part"

  curl -fsSL --retry 3 -o "$2.part" "$1" || {
    rm -f "$2.part"
    echo "[RED] $1: download failed" >&2
    exit 1
  }

  got=$(digest "$3" < "$2.part")

  [ "$got" = "$4" ] || {
    rm -f "$2.part"
    echo "[RED] $(basename "$2"): sha$3 $got, expected $4" >&2
    exit 1
  }

  mv "$2.part" "$2"
}

# Download or reuse only the pinned Micromamba executable.
get_micromamba() {
  case $(uname -m) in
    x86_64)
      a=linux-64
      h=$MICROMAMBA_LINUX64_SHA256
      ;;
    aarch64|arm64)
      a=linux-aarch64
      h=$MICROMAMBA_ARM64_SHA256
      ;;
    *)
      echo "[RED] Unsupported CPU: $(uname -m)" >&2
      exit 1
      ;;
  esac

  if [ -x "$ENV/micromamba" ]; then
    got=$(digest 256 < "$ENV/micromamba")

    if [ "$got" = "$h" ]; then
      printf '%s\n' "$ENV/micromamba"
      return
    fi

    echo "[RED] Existing Micromamba has an incorrect hash; downloading again." >&2
    rm -f "$ENV/micromamba"
  fi

  echo "Downloading pinned Micromamba $MICROMAMBA_VERSION ($a)..." >&2

  fetch \
    "https://github.com/mamba-org/micromamba-releases/releases/download/$MICROMAMBA_VERSION/micromamba-$a" \
    "$ENV/micromamba" 256 "$h"

  chmod +x "$ENV/micromamba"

  printf '%s\n' "$ENV/micromamba"
}

setup() {
  mkdir -p "$ENV"

  # Conda-forge Python includes Tkinter and Tcl/Tk.
  # Replace an existing environment if it does not contain Tkinter.
  if [ ! -f "$ENV/venv/conda-meta/history" ] ||
     ! "$ENV/venv/bin/python" -c 'import tkinter' >/dev/null 2>&1; then

    rm -rf "$ENV/venv"

    MM=$(get_micromamba)

    echo "Creating Python 3.14 environment with Tkinter and SPIRV-Tools..."

    MAMBA_ROOT_PREFIX="$ENV/mamba" \
      "$MM" create -y -p "$ENV/venv" \
      -c conda-forge \
      python=3.14 tk pip spirv-tools
  fi

  # Update existing environments that predate SPIRV-Tools.
  if [ ! -x "$ENV/venv/bin/spirv-val" ]; then
    MM=$(get_micromamba)

    echo "Installing SPIRV-Tools..."

    MAMBA_ROOT_PREFIX="$ENV/mamba" \
      "$MM" install -y -p "$ENV/venv" \
      -c conda-forge spirv-tools
  fi

  "$ENV/venv/bin/python" -c \
    'import tkinter; print("Tkinter:", tkinter.TkVersion)'

  [ -x "$ENV/venv/bin/spirv-val" ] || {
    echo "[RED] spirv-val is missing from the packer environment." >&2
    exit 1
  }

  "$ENV/venv/bin/spirv-val" --version

  "$ENV/venv/bin/python" -m pip -q install \
    --only-binary :all: $PACKAGES

  # Install the pinned .NET 10 runtime if necessary.
  if [ ! -x "$ENV/dotnet/dotnet" ] ||
     ! "$ENV/dotnet/dotnet" --list-runtimes | grep -q '^Microsoft.NETCore.App 10\.'; then

    case $(uname -m) in
      aarch64|arm64)
        a=arm64
        h=$DOTNET_ARM64_SHA512
        ;;
      x86_64)
        a=x64
        h=$DOTNET_X64_SHA512
        ;;
      *)
        echo "[RED] $(uname -m): there is no pinned .NET runtime for this CPU" >&2
        exit 1
        ;;
    esac

    # musl is not supported by the pinned .NET runtime.
    if (ldd --version 2>&1 || true) | grep -qi musl ||
       { ! command -v ldd >/dev/null && ls /lib/ld-musl-* >/dev/null 2>&1; }; then
      echo "[RED] musl is not supported by the pinned .NET runtime" >&2
      exit 1
    fi

    fetch \
      "https://builds.dotnet.microsoft.com/dotnet/Runtime/$DOTNET_VERSION/dotnet-runtime-$DOTNET_VERSION-linux-$a.tar.gz" \
      "$ENV/dotnet.tar.gz" 512 "$h"

    rm -rf "$ENV/dotnet.part"
    mkdir "$ENV/dotnet.part"

    tar -xzf "$ENV/dotnet.tar.gz" -C "$ENV/dotnet.part"
    rm -f "$ENV/dotnet.tar.gz"

    [ -x "$ENV/dotnet.part/dotnet" ] || {
      rm -rf "$ENV/dotnet.part"
      echo "[RED] The .NET archive has no dotnet at its root." >&2
      exit 1
    }

    rm -rf "$ENV/dotnet"
    mv "$ENV/dotnet.part" "$ENV/dotnet"
  fi

  # Keep the translator wrapper beside its portable DLLs.
  mkdir -p "$XBIN"

  printf '#!/bin/sh\n# Linux shader translator using the portable DLLs beside this script.\nexec "${DOTNET_ROOT:-%s}/dotnet" "$(dirname "$0")/dmsh_xlate.dll" "$@"\n' \
    "$ENV/dotnet" > "$WRAP"

  chmod +x "$WRAP"

  expected=$(cd "$ROOT/dm/gfx/tools" &&
    "$ENV/venv/bin/python" -c \
      'from dmsh_chain import xlate_source_sha as s; print(s() if callable(s) else s)')

  actual=$("$WRAP" --source-sha)

  echo "translator: $actual (the tree expects $expected)"

  [ "$actual" = "$expected" ] || {
    echo "[RED] Translator source hash mismatch." >&2
    exit 1
  }

  echo "ready: $ENV"
}

# GUI is the default; preserve existing CLI behavior for game paths.

CMD=${1:-gui}

case "$CMD" in
  setup)
    setup
    ;;

  gui)
    if [ "$#" -gt 0 ]; then shift; fi

    [ -x "$ENV/venv/bin/python" ] &&
    [ -x "$ENV/venv/bin/spirv-val" ] &&
    [ -x "$WRAP" ] || setup

    PATH="$ENV/venv/bin:$PATH" \
    DOTNET_ROOT="$ENV/dotnet" XLATE="$WRAP" \
      exec "$ENV/venv/bin/python" "$ROOT/scripts/pack_gui.py" "$@"
    ;;

  pack)
    if [ "$#" -gt 0 ]; then shift; fi

    [ -x "$ENV/venv/bin/python" ] &&
    [ -x "$ENV/venv/bin/spirv-val" ] &&
    [ -x "$WRAP" ] || setup

    PATH="$ENV/venv/bin:$PATH" \
    DOTNET_ROOT="$ENV/dotnet" XLATE="$WRAP" \
      exec "$ENV/venv/bin/python" "$ROOT/scripts/pack_android.py" "$@"
    ;;

  -h|--help)
    sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
    echo "Commands: gui (default), pack, setup"
    ;;

  *)
    [ -x "$ENV/venv/bin/python" ] &&
    [ -x "$ENV/venv/bin/spirv-val" ] &&
    [ -x "$WRAP" ] || setup

    PATH="$ENV/venv/bin:$PATH" \
    DOTNET_ROOT="$ENV/dotnet" XLATE="$WRAP" \
      exec "$ENV/venv/bin/python" "$ROOT/scripts/pack_android.py" "$@"
    ;;
esac
