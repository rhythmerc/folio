#pragma once
#include <FontCacheManager.h>  // for TextCollector
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

  // Declarative prewarm hook — called by the render pipeline immediately
  // before each render(). Override to enumerate the text this paint will
  // draw via `tc.use(fontId, style, text)` calls; the framework then
  // batched-loads any missing glyphs into the font cache LRU so the
  // upcoming drawText calls hit warm.
  //
  // This is a *pure data API* — declareText must not draw or mutate the
  // framebuffer (drawText/drawRect/etc. should not be called here). Only
  // the (fontId, style, text) declarations matter; SdCardFont's idempotent
  // prewarm hashes the resulting codepoint set and short-circuits when it
  // matches the prior paint, so stable scenes cost only the hash compare.
  //
  // Default implementation declares nothing — activities that render only
  // embedded (flash) fonts don't need to override.
  virtual void declareText(TextCollector&) {}

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
};
