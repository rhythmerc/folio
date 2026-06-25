#pragma once
#include "activities/Activity.h"

// Full-screen library indexing / progress screen.
//
// Entered first (by ActivityManager::goHome). Synchronously populates the global
// LIBRARY_INDEX from the SD card — painting a determinate progress screen while it
// works — then hands off to LibraryActivity via replaceActivity. The index lives in a
// global singleton, so the populated data persists across that hand-off.
//
// Synchronous on purpose: blocking the main loop here keeps the cold index running at
// full clock (it never reaches the idle-detect that would drop the CPU to 10 MHz). The
// parse/refresh-overlap optimization (a worker + power lock) is intentionally deferred.
class LibraryIndexingActivity final : public Activity {
 public:
  explicit LibraryIndexingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LibraryIndexing", renderer, mappedInput) {}

  void onEnter() override;
  AppId appId() const override { return AppId::Library; }

 private:
  // Full-screen progress paint (header + centered heading/percent/bar/count/label),
  // drawn directly via displayBuffer from the refresh callback. Self-contained — reads
  // no member state.
  void renderIndexingProgress(int done, int total, const char* label);
};
