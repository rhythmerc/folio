#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "components/ui/List/List.h"
#include "util/ButtonNavigator.h"

// Full-screen picker for editing one book's manual-collection membership.
//   Add    — lists every collection (member ones marked ✓) plus a
//            "New Collection…" action; selecting adds the book and returns.
//   Remove — lists only the collections the book is in; selecting removes it
//            and returns. Shows an empty state when the book is in none.
// The book is referenced by its path hash so the picker is decoupled from the
// LibraryBook lifetime. Mutates CollectionStore directly, then finish()es.
class CollectionPickerActivity final : public Activity {
 public:
  enum class Mode { Add, Remove };

  explicit CollectionPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint32_t bookHash,
                                    Mode mode)
      : Activity("CollectionPicker", renderer, mappedInput), bookHash(bookHash), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  uint32_t bookHash;
  Mode mode;
  List list;
  ButtonNavigator buttonNavigator;
  bool empty_ = false;  // Remove mode with no memberships

  void buildList();
  void promptNewCollection();
};
