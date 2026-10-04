#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/tools/.work"
FONT_DIR="$ROOT"
mkdir -p "$WORK" "$FONT_DIR"
command -v git >/dev/null || { echo "git is required"; exit 1; }
command -v curl >/dev/null || { echo "curl is required"; exit 1; }
command -v brew >/dev/null || { echo "Homebrew is required (https://brew.sh/)"; exit 1; }
if ! brew list freetype >/dev/null 2>&1; then brew install freetype; fi
TTF="$WORK/JetBrainsMono-Regular.ttf"
OFL="$ROOT/OFL-JetBrainsMono.txt"
curl -L --fail -o "$TTF" "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/master/fonts/ttf/JetBrainsMono-Regular.ttf"
curl -L --fail -o "$OFL" "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/master/OFL.txt"
ADA="$WORK/Adafruit-GFX-Library"
if [[ ! -d "$ADA/.git" ]]; then git clone --depth 1 https://github.com/adafruit/Adafruit-GFX-Library.git "$ADA"; fi
FT="$(brew --prefix freetype)"
cc -std=c99 -I"$FT/include/freetype2" "$ADA/fontconvert/fontconvert.c" -L"$FT/lib" -lfreetype -o "$WORK/fontconvert"
# ASCII 0x20..0x7E. Degree sign is drawn separately in the sketch.
"$WORK/fontconvert" "$TTF" 9 32 126 > "$FONT_DIR/JetBrainsMono9pt7b.h"
"$WORK/fontconvert" "$TTF" 12 32 126 > "$FONT_DIR/JetBrainsMono12pt7b.h"
"$WORK/fontconvert" "$TTF" 24 32 126 > "$FONT_DIR/JetBrainsMono24pt7b.h"
# Normalize generated symbol names so the sketch has stable identifiers.
sed -i '' -E 's/JetBrainsMono_Regular9pt7b/JetBrainsMono9pt7b/g' "$FONT_DIR/JetBrainsMono9pt7b.h"
sed -i '' -E 's/JetBrainsMono_Regular12pt7b/JetBrainsMono12pt7b/g' "$FONT_DIR/JetBrainsMono12pt7b.h"
sed -i '' -E 's/JetBrainsMono_Regular24pt7b/JetBrainsMono24pt7b/g' "$FONT_DIR/JetBrainsMono24pt7b.h"
echo "JetBrains Mono headers generated in: $FONT_DIR"
echo "Restart/reopen Arduino IDE, then compile the sketch."
