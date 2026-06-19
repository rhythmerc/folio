#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"
#include "lib/MenuRegistry.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Optional per-activity power-button override. The "Short Power Button
  // Click" setting (SETTINGS.shortPwrBtn) defines a global default behavior
  // (Ignore / Sleep / Page-turn / Force-refresh); overriding this hook lets
  // an activity claim the short-press for its own action. Return true to
  // suppress the global FORCE_REFRESH path; return false to fall through.
  // Long-press / sleep detection is intentionally still global. The
  // side-rail hint label is the activity's concern — it already calls
  // `ButtonHints::renderSide`, so it can pass its own power-slot label
  // there directly.
  virtual bool handlePowerShortPress() { return false; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome();
  void onSelectBook(const std::string& path);

  virtual std::vector<MenuRegistryEntry> getGlobalMenuEntries() {
    return std::vector<MenuRegistryEntry>{};
  }

  virtual std::optional<GlobalMenuConfig> getGlobalMenuConfig() {
    return std::nullopt;
  }
};
