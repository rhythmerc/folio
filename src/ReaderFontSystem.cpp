#include "ReaderFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"

void ReaderFontSystem::begin(GfxRenderer& renderer) {
  (void)renderer;  // binding is deferred to reader entry (ensureLoaded)
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<ReaderFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  // NB: the saved family is intentionally NOT loaded here. Eager-loading every
  // shipped size at boot cost ~110KB and OOM'd the library. bindFamily runs
  // lazily on reader entry via ensureLoaded().
  LOG_DBG("RDRFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void ReaderFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // A registry refresh also forces a re-bind below even when the family name is
  // unchanged — the file on disk may have been replaced (new content hash).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("RDRFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;

  if (wantedFamily[0] == '\0') {
    if (manager_.isBound()) manager_.unloadAll(renderer);
    return;
  }

  // Already bound to this family, nothing changed on disk: all sizes are
  // resident. A body-size change needs no reload — it just re-snaps ids (and
  // re-keys the section cache).
  if (manager_.isBound() && manager_.currentFamilyName() == wantedFamily && !registryWasDirty) {
    return;
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (!family) {
    LOG_DBG("RDRFS", "SD font family not found: %s (clearing)", wantedFamily);
    manager_.unloadAll(renderer);
    SETTINGS.sdFontFamilyName[0] = '\0';
    return;
  }
  if (!manager_.bindFamily(*family, renderer)) {
    LOG_ERR("RDRFS", "Failed to bind SD font family: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
    return;
  }
  LOG_DBG("RDRFS", "Bound SD card font family: %s", wantedFamily);
}

int ReaderFontSystem::resolveFontId(const char* familyName, uint8_t pointSize) const {
  // Deterministic id for the nearest shipped size. bindFamily has already
  // loaded + registered every size, so this id is directly renderable at both
  // layout (measurement) and paint — no per-page loading hook required.
  return manager_.idForSize(familyName, pointSize);
}
