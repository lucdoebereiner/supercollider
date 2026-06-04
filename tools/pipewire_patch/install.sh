#!/usr/bin/env bash
# Install (symlink) PipeWirePatch.sc into the SuperCollider Extensions dir so
# sclang compiles it on startup. Symlinking (default) means edits to the file
# in this repo take effect on the next sclang recompile with no reinstall.
#
# Usage:
#   ./install.sh              # symlink into the per-user Extensions dir
#   ./install.sh --copy       # copy instead of symlink
#   ./install.sh --system     # install into the system-wide Extensions dir
#   ./install.sh --uninstall  # remove a previous install
#   SC_EXTENSIONS_DIR=/path ./install.sh   # override the target dir
#
# After installing, recompile the class library in the IDE (Lang > Recompile
# Class Library) or restart sclang.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/PipeWirePatch.sc"
HELP_SRC="$HERE/HelpSource"
SUBDIR="PipeWirePatch"
MODE="symlink"
SCOPE="user"
UNINSTALL=0

for arg in "$@"; do
	case "$arg" in
		--copy) MODE="copy" ;;
		--system) SCOPE="system" ;;
		--uninstall) UNINSTALL=1 ;;
		-h|--help) sed -n '2,18p' "$0"; exit 0 ;;
		*) echo "unknown option: $arg" >&2; exit 2 ;;
	esac
done

# Resolve the Extensions directory.
if [ -n "${SC_EXTENSIONS_DIR:-}" ]; then
	EXT="$SC_EXTENSIONS_DIR"
else
	case "$(uname -s)" in
		Darwin)
			if [ "$SCOPE" = system ]; then
				EXT="/Library/Application Support/SuperCollider/Extensions"
			else
				EXT="$HOME/Library/Application Support/SuperCollider/Extensions"
			fi
			;;
		*)
			if [ "$SCOPE" = system ]; then
				EXT="/usr/local/share/SuperCollider/Extensions"
			else
				EXT="${XDG_DATA_HOME:-$HOME/.local/share}/SuperCollider/Extensions"
			fi
			;;
	esac
fi

DEST="$EXT/$SUBDIR"

if [ "$UNINSTALL" = 1 ]; then
	rm -rf "$DEST"
	echo "removed $DEST"
	exit 0
fi

[ -f "$SRC" ] || { echo "source not found: $SRC" >&2; exit 1; }

mkdir -p "$DEST"
rm -f "$DEST/PipeWirePatch.sc"
rm -rf "$DEST/HelpSource"
if [ "$MODE" = copy ]; then
	cp "$SRC" "$DEST/PipeWirePatch.sc"
	[ -d "$HELP_SRC" ] && cp -r "$HELP_SRC" "$DEST/HelpSource"
	echo "copied   -> $DEST/PipeWirePatch.sc (+ HelpSource)"
else
	ln -s "$SRC" "$DEST/PipeWirePatch.sc"
	[ -d "$HELP_SRC" ] && ln -s "$HELP_SRC" "$DEST/HelpSource"
	echo "symlinked -> $DEST/PipeWirePatch.sc -> $SRC (+ HelpSource)"
fi
echo "Recompile the class library (or restart sclang) to load it."
echo "Help: search for 'PipeWirePatch' in the Help browser (re-index help if needed)."
