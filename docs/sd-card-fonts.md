# SD Card Fonts

CrossPoint supports loading additional fonts from the SD card, including fonts
with extended Unicode coverage (CJK, Cyrillic, Greek, etc.).

## Installing Fonts

There are three ways to install fonts:

### Option 1: Download from device (recommended)

1. Connect your CrossPoint reader to WiFi
2. Go to **Settings > System > Manage Fonts**
3. Browse available font families and tap to download
4. Downloaded fonts appear immediately in **Settings > Reader > Font Family**

### Option 2: Upload via web browser

1. Connect your CrossPoint reader to WiFi
2. Open the web interface in your browser (shown on the WiFi screen)
3. Navigate to the **Fonts** tab
4. Upload `.cpfont` files using the upload form

### Option 3: Manual SD card copy

1. Download font files from the
   [crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts)
2. Copy font family folders to one of two locations on your SD card:

   - `/.fonts/` — hidden directory (preferred; keeps the SD root tidy
     when mounted on a desktop)
   - `/fonts/` — visible directory (use this if your OS hides dot-files
     and you'd rather see the folder in your file manager)

   Both roots are always scanned at boot and the results are merged: a
   family installed in `/fonts/` shows up even when `/.fonts/` also
   exists, and vice versa. The two roots only collide if the same family
   name appears in both — in that case the copy in `/.fonts/` wins and
   the duplicate in `/fonts/` is ignored.

       SD Card Root/
       ├── .fonts/                     ← Hidden root (preferred)
       │   └── Literata/
       │       ├── Literata_12.cpfont
       │       ├── Literata_14.cpfont
       │       ├── Literata_16.cpfont
       │       └── Literata_18.cpfont
       └── fonts/                      ← Visible root (equally valid)
           └── Merriweather/
               ├── Merriweather_12.cpfont
               └── ...

3. Insert the SD card and power on your CrossPoint reader

## Available Pre-Built Fonts

The current list of pre-built fonts is maintained in the
[crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts).

## Converting Custom Fonts

To convert your own TrueType/OpenType fonts:

### Prerequisites

    pip install freetype-py fonttools

### Single font (one style)

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### Multi-style font

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

### Available Unicode interval presets

| Preset | Coverage |
|--------|----------|
| `ascii` | U+0020-U+007E (Basic Latin) |
| `latin-ext` | European languages (Latin + Extended-A/B) |
| `greek` | Greek + Extended Greek |
| `cyrillic` | Cyrillic + Supplement |
| `cjk` | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth |
| `hangul` | Korean Hangul syllables |
| `builtin` | Matches built-in Bookerly coverage exactly |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

Install custom fonts via WiFi upload or manual SD card copy.

## Theme role fonts (progressive enhancement)

Themes expose font *roles* — semantic names like `title`, `heading`, `body`,
`caption`, `accent` — instead of hard-coded font IDs. Each theme picks an
embedded face per role by default. Users can drop role-specific `.cpfont`
files on the SD card to override those defaults without re-flashing.

### File layout

    SD Card Root/
    └── .fonts/
        └── themes/
            └── folio/
                ├── caption.cpfont      ← Folio uses for author labels, subtitles, etc.
                ├── accent.cpfont       ← Folio italic accents
                └── title.cpfont        ← Folio big display title

The directory name under `themes/` matches the theme's name (`base`,
`folio`, etc.). The file basename matches the role name (`title`,
`heading`, `body`, `caption`, `accent`).

Files in `/.fonts/themes/<theme>/` and `/fonts/themes/<theme>/` are both
scanned at boot. The hidden root wins on collision.

### When to use this

- Match the prototype's typography at sizes the firmware doesn't ship
  (e.g. an 8-pt serif for the Library's author labels).
- Try a different display face for `title` without recompiling.
- Localize a single role to a script-specific face (CJK title) while
  keeping the rest of the theme's embedded fonts.

### Creating a theme role font

Use the same `fontconvert_sdcard.py` script as for reader fonts. Convert at
a single size — the file basename determines the role, not the family
name. Example for an 8-pt Folio caption:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular NotoSerif-Regular.ttf \
      --italic NotoSerif-Italic.ttf \
      --bold NotoSerif-Bold.ttf \
      --bolditalic NotoSerif-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 8 \
      --name FolioCaption \
      --output-dir ./out/

Then rename `FolioCaption_8.cpfont` to `caption.cpfont` and copy to
`/.fonts/themes/folio/`. On the next boot the file will be discovered and
Folio's Caption role will resolve to it instead of NOTOSERIF_10.

### Memory cost

Each loaded role font carries the usual SdCardFont overhead (~10–30 KB of
kern tables and glyph caches). The registry never auto-loads — it only
loads files the user has explicitly placed on the SD card. Plan for at
most 1–2 roles per theme to stay within the 380 KB device budget.
