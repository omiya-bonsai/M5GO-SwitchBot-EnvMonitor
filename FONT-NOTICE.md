# JetBrains Mono notice

JetBrains Mono is licensed under SIL Open Font License 1.1; see [OFL-JetBrainsMono.txt](OFL-JetBrainsMono.txt).

This repository includes generated GFXfont headers and a TTF under `tools/.work/`. The optional `tools/install_jetbrains_mono.sh` helper downloads JetBrains Mono Regular and the OFL text from the official JetBrains/JetBrainsMono repository, then converts the font locally. See [font instructions](fonts/README.md).

The firmware uses the embedded headers, with M5GFX FreeMono fallback when any required header is missing.
