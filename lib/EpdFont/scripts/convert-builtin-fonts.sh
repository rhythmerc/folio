#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
# Literata is the single built-in serif: the reader serif face AND the faces the
# UI Title (14) / Body (10) / BodyLarge (12) / compact (5/6/8) roles resolve to.
LITERATA_FONT_SIZES=(5 6 8 10 12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)

# The Body (Literata 10) and Title (Literata 14) faces back UI chrome
# (UI_10/UI_12 ids, see UITheme::repointUiFonts), replacing the dropped Ubuntu UI
# font. Hebrew is a shipped UI language but Literata lacks it, so merge
# NotoSansHebrew (Regular/Bold only) into those two faces. Echoes extra
# fontstack+intervals args for the relevant faces.
maybe_hebrew() {
  local family="$1" size="$2" style="$3"
  if [[ "$family" == "literata" ]] && { [[ "$size" == "10" ]] || [[ "$size" == "14" ]]; } \
     && { [[ "$style" == "Regular" ]] || [[ "$style" == "Bold" ]]; }; then
    echo "../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-${style}.ttf --additional-intervals 0x05D0,0x05EA"
  fi
}

for size in ${LITERATA_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="literata_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Literata/Literata-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $(maybe_hebrew literata $size $style) --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  ../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf \
  --additional-intervals 0x05D0,0x05EA > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
