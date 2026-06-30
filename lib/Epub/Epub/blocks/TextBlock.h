#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

// Represents a line of text on a page
class TextBlock final : public Block {
 private:
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  // Per-word focus boundary: N > 0 means the first N bytes of words[i] are rendered bold,
  // the remainder in the base style. 0 means no split (whole word uses wordStyles[i]).
  // N encodes the bold PREFIX length only — bounded to 9 codepoints (≤36 UTF-8 bytes) by
  // FOCUS_READING_PERCENT's 1..9 clamp in ParsedText::addWord, so it always fits in uint8_t.
  // Vector is empty when no focus splits exist anywhere in the block (zero per-word RAM cost
  // when focus reading is disabled, or on lines that happen to contain no splittable words).
  std::vector<uint8_t> wordFocusBoundary;
  // Pre-computed pixel offset from word start to the regular suffix, stored when boundary > 0.
  // Eliminates getTextAdvanceX from the render path. 0 when boundary == 0.
  // Empty in lockstep with wordFocusBoundary.
  std::vector<uint16_t> wordFocusSuffixX;
  BlockStyle blockStyle;
  // Registered font id this whole line is rendered with. Set from the block's resolved (snapped)
  // CSS font-size; equals the body font for normal text.
  int fontId;

 public:
  explicit TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, std::vector<uint8_t> focus_boundary,
                     std::vector<uint16_t> focus_suffix_x, int font_id, const BlockStyle& blockStyle = BlockStyle())
      : words(std::move(words)),
        wordXpos(std::move(word_xpos)),
        wordStyles(std::move(word_styles)),
        wordFocusBoundary(std::move(focus_boundary)),
        wordFocusSuffixX(std::move(focus_suffix_x)),
        blockStyle(blockStyle),
        fontId(font_id) {}
  ~TextBlock() override = default;
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  int getFontId() const { return fontId; }
  const std::vector<std::string>& getWords() const { return words; }
  bool isEmpty() override { return words.empty(); }
  size_t wordCount() const { return words.size(); }
  // Renders this line at its own fontId (the int x,y are page offsets).
  void render(const GfxRenderer& renderer, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(HalFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(HalFile& file);
};
