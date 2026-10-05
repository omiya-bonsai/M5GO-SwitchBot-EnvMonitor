# JetBrains Mono

The sketch includes the generated `JetBrainsMono9pt7b.h`, `JetBrainsMono12pt7b.h` and `JetBrainsMono24pt7b.h` headers from the repository root. No LittleFS upload is needed. If any header is missing, the sketch selects M5GFX FreeMono fonts.

Optional regeneration on macOS, from the repository root:

```sh
bash tools/install_jetbrains_mono.sh
```

The script requires Git, curl, Homebrew and a C compiler. It installs FreeType through Homebrew if needed, downloads the official JetBrains Mono Regular TTF and OFL text, builds Adafruit GFX fontconvert, and regenerates ASCII glyphs (0x20–0x7E) at 9/12/24 pt. The sketch draws the degree symbol separately. The script overwrites the root headers and `OFL-JetBrainsMono.txt`; restart/reopen Arduino IDE afterward.

The TTF and conversion tooling are stored in `tools/.work/`. That directory is **not ignored** by the current `.gitignore`; inspect Git changes before committing regenerated assets. See [font notice](../FONT-NOTICE.md) and [OFL](../OFL-JetBrainsMono.txt).
