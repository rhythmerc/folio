#pragma once

#include <ReaderFontManager.h>
#include <ReaderFontRegistry.h>

#include <atomic>

class GfxRenderer;

/// Reader-side facade. Owns the ReaderFontRegistry (SD-card font discovery),
/// the ReaderFontManager (one-loaded-font-at-a-time orchestrator), and the
/// resolver callback wired into CrossPointSettings. Lives behind a single
/// begin() + ensureLoaded() API used by main.cpp and the reader activities.
///
/// Counterpart on the UI side is UiThemeLoader; the two systems are
/// intentionally separate because they have different constraints
/// (one font vs many) and different lifecycles (per-book session vs
/// per-theme with mid-session eviction).
class ReaderFontSystem {
 public:
  ReaderFontSystem() = default;
  ReaderFontSystem(const ReaderFontSystem&) = delete;
  ReaderFontSystem& operator=(const ReaderFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + fontSize enum.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t fontSizeEnum) const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const ReaderFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  ReaderFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

 private:
  ReaderFontRegistry registry_;
  ReaderFontManager manager_;
  std::atomic<bool> registryDirty_{false};
};

// Global reader-side font system (defined in main.cpp).
extern ReaderFontSystem readerFontSystem;
