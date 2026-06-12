#pragma once

#include "activities/Activity.h"
#include "components/ui/List/List.h"
#include "util/ButtonNavigator.h"

// Full-screen Collections list: auto groups (By Series / By Author / By Genre)
// followed by the user's manual collections, with a "New Collection…" action.
// Selecting an auto group opens its group list; selecting a manual collection
// filters the library to it; "New Collection…" names a new (empty) collection.
class CollectionsActivity final : public Activity {
 public:
  explicit CollectionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Collections", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  List list;
  ButtonNavigator buttonNavigator;

  void buildList();
  void promptNewCollection();
  // Result handler for a launched auto-group list: returns to Library when a
  // group was chosen, stays on the collections list when the user backed out.
  void onGroupPicked(const ActivityResult& res);
};
