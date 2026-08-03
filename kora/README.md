# Kora — Yart OS icon & wallpaper assets

Source artwork for Yart OS:

```
kora/
├── README.md             (this file)
├── icons/
│   └── kora/             Kora icon theme (trimmed: scalable/ + symbolic/
│                         SVGs only, no duplicate @2x or pixel-size folders).
│       ├── actions/      (~1500 SVGs)
│       ├── apps/         (~3300 SVGs — every app icon)
│       ├── categories/   (~44 SVGs)
│       ├── devices/      (~85 SVGs)
│       ├── emblems/      (~67 SVGs)
│       ├── emotes/       (~62 SVGs)
│       ├── mimetypes/    (~384 SVGs)
│       ├── panel/        (~1400 SVGs — tray icons, panel status)
│       ├── places/       (~130 SVGs)
│       ├── status/       (~120 SVGs)
│       └── animations/   (~12 SVGs)
├── wallpapers/
│   └── default.png       default 1280x800 dune wallpaper
└── cursors/              (reserved for cursor themes)
```

Total size: ~30 MB for 7,000+ icons. The compositor only uses 111 of them today;
the rest are kept so future apps/dialogs/devices/mimetypes have icons ready.

`scripts/gen_assets.py` renders the icons used by the compositor into
`build/kora.bin` + `build/kora.h` at build time (rsvg-convert scales from SVG
to the requested pixel size, so we don't need multiple raster sizes here).

Do NOT commit `build/`, `yart.iso`, `limine/` (bootloader downloaded by
`scripts/get-limine.sh`), or `iso_root/` — they are generated.
