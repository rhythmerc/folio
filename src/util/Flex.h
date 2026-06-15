#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "components/themes/BaseTheme.h"  // Rect

// =============================================================================
//  Flex — stack-allocated, allocator-free layout helpers (header-only).
// =============================================================================
//
// Activities compose a layout by constructing nested `Vstack` / `Hstack` /
// `Grid` objects on the stack. Each container takes a parent rect, a list of
// axis-sizing requests, and optional gap + padding. The constructor computes
// the child rects; consumers read them via `operator[]`. Containers carry no
// state beyond their child rect array; lifetime is the surrounding C++ scope.
//
// Use `flex::align(...)` / `flex::center(...)` to place intrinsically-sized
// content (text, icons, fixed-size widgets) inside a slot — containers handle
// structure, `align` handles in-slot positioning.
//
// -----------------------------------------------------------------------------
//  Quick start
// -----------------------------------------------------------------------------
//
//   const Rect screen{0, 0, w, h};
//   flex::Vstack top(screen, {flex::fixed(89), flex::grow(), flex::fixed(40)});
//   renderHeader(top[0]);
//   {
//     flex::Hstack body(top[1], {flex::grow(), flex::fixed(18)}, /*gap=*/10,
//                       flex::xy(/*x=*/18, /*y=*/8));
//     {
//       flex::Grid shelf(body[0], /*rows=*/3, /*cols=*/3,
//                        /*rowGap=*/14, /*colGap=*/14);
//       for (int i = 0; i < 9; ++i) renderTile(shelf[i], books[i]);
//     }
//     renderRail(body[1]);
//   }
//   renderFooter(top[2]);
//
// -----------------------------------------------------------------------------
//  Sizing (main axis only — cross-axis stretches to the inner span)
// -----------------------------------------------------------------------------
//
//   flex::fixed(px)        Exact pixel size.
//   flex::grow(weight=1)   Share of the leftover space, proportional to
//                          `weight`. Weight is uint8_t (1..255).
//   flex::percent(pct)     A percentage of the inner span (post-padding),
//                          before gaps are subtracted. CSS-ish.
//
//   Distribution: sum(fixed) + sum(percent) + sum(gaps) are consumed first;
//   the remainder is split among grow children by weight. Truncation rounding
//   can leave a few px unspoken-for in the trailing edge (invisible — the
//   cross-axis covers the full inner span anyway).
//
// -----------------------------------------------------------------------------
//  Padding (shrinks the inner rect on all sides)
// -----------------------------------------------------------------------------
//
//   flex::Padding{top, right, bottom, left}    Explicit four-side construct.
//   flex::all(n)                                All four sides = n.
//   flex::xy(x, y)                              Horizontal = x, vertical = y.
//
//   flex::inset(parent, pad) -> Rect            Shrink a rect by `pad` on all
//                          sides. Containers apply this internally; call it
//                          directly for site-specific padding outside a stack.
//
// -----------------------------------------------------------------------------
//  Containers
// -----------------------------------------------------------------------------
//
//   Vstack(parent, {sizes...}, gap=0, pad={})
//       Stack children top-to-bottom. `operator[i]` yields the i-th child rect.
//
//   Hstack(parent, {sizes...}, gap=0, pad={})
//       Stack children left-to-right. Same access pattern.
//
//   Grid(parent, rows, cols, rowGap=0, colGap=0, pad={})
//       Partition into uniform rows × cols cells, row-major. Use `cells[i]`
//       for flat-index access or `cells.at(row, col)` for 2D access.
//
//   Limits: Vstack/Hstack hold up to `kMaxChildren = 16` children; Grid holds
//   up to `kMaxCells = 16` (4×4). Exceeding these asserts in debug builds.
//
// -----------------------------------------------------------------------------
//  Alignment
// -----------------------------------------------------------------------------
//
//   flex::align(parent, w, h, hAlign=Center, vAlign=Center) -> Rect
//       Pure function. Places a {w × h} child rect inside `parent` aligned
//       per axis (Start / Center / End). Use it when a stack row's child
//       wants its natural size instead of the row's stretched cross-axis.
//
//   flex::center(parent, w, h) -> Rect
//       Shortcut for both-axes-center.
//
//   Compose freely with containers: containers position rows/columns, `align`
//   positions intrinsic content within a row. Vstack/Hstack do not embed
//   per-child cross-axis alignment by design — keep them simple.
//
// -----------------------------------------------------------------------------
//  Math & rounding
// -----------------------------------------------------------------------------
//
//   All math is `int`; division truncates. Containers produce byte-exact
//   results for the same inputs across runs and across builds.
//
// -----------------------------------------------------------------------------
//  Memory characteristics
// -----------------------------------------------------------------------------
//
//   Each Vstack/Hstack is ~280 B on the stack (16 × `Rect` + count); Grid is
//   ~268 B. Constructors do all math; accessors are O(1). No heap, no
//   thread-local state, no per-frame allocation. Peak nesting depth in
//   LibraryActivity is 4 → ~1.1 KB transient stack per frame.
//
namespace flex {

enum class Mode : uint8_t { Fixed, Grow, Percent };

struct Size {
  Mode mode;
  int16_t fixed;    // px when mode == Fixed
  uint8_t weight;   // 1..255 share when mode == Grow
  uint8_t percent;  // 0..100 when mode == Percent
};

inline constexpr Size fixed(int px) {
  return {Mode::Fixed, static_cast<int16_t>(px), 0, 0};
}
inline constexpr Size grow(uint8_t weight = 1) {
  return {Mode::Grow, 0, weight, 0};
}
inline constexpr Size percent(uint8_t pct) {
  return {Mode::Percent, 0, 0, pct};
}

struct Padding {
  int16_t top = 0;
  int16_t right = 0;
  int16_t bottom = 0;
  int16_t left = 0;
};

inline constexpr Padding all(int p) {
  return {static_cast<int16_t>(p), static_cast<int16_t>(p), static_cast<int16_t>(p),
          static_cast<int16_t>(p)};
}
inline constexpr Padding xy(int x, int y) {
  return {static_cast<int16_t>(y), static_cast<int16_t>(x), static_cast<int16_t>(y),
          static_cast<int16_t>(x)};
}

// Shrink `parent` by `pad` on all sides to produce the content rect. The same
// operation containers apply internally, exposed for call sites that need
// site-specific padding outside a stack/grid. Pure function, header-only.
inline Rect inset(const Rect& parent, const Padding& pad) {
  return Rect{parent.x + pad.left, parent.y + pad.top,
              parent.width - pad.left - pad.right,
              parent.height - pad.top - pad.bottom};
}

inline Rect offset(const Rect& parent, int x, int y) {
  return Rect{parent.x + x, parent.y + y,
              parent.width,
              parent.height};
}

// Bounding box: the smallest rect that fully covers both `a` and `b`. Pure
// function, header-only. Treats width/height as exclusive extents (a rect
// covers [x, x+width)), so abutting rects join without overlap or gap.
inline Rect join(const Rect& a, const Rect& b) {
  const int left = a.x < b.x ? a.x : b.x;
  const int top = a.y < b.y ? a.y : b.y;
  const int aRight = a.x + a.width, bRight = b.x + b.width;
  const int aBottom = a.y + a.height, bBottom = b.y + b.height;
  const int right = aRight > bRight ? aRight : bRight;
  const int bottom = aBottom > bBottom ? aBottom : bBottom;
  return Rect{left, top, right - left, bottom - top};
}

namespace detail {

// Resolve the main-axis size of every child given the available main-axis
// span and per-child sizing requests. Writes resolved px sizes into `out`.
// Returns the number of children resolved (same as sizes.size()).
//
// `mainSpan` is the inner span *after* padding has been removed.
// Children consume gaps between themselves (gap × (N-1)) before distribution.
inline size_t resolveMainAxis(std::initializer_list<Size> sizes, int mainSpan,
                              int gap, int* out) {
  const size_t n = sizes.size();
  if (n == 0) return 0;
  const int gapTotal = (n > 1) ? gap * static_cast<int>(n - 1) : 0;
  int consumed = 0;
  int growWeight = 0;
  size_t i = 0;
  for (const Size& s : sizes) {
    int px = 0;
    switch (s.mode) {
      case Mode::Fixed:
        px = s.fixed;
        break;
      case Mode::Percent:
        // Percent is of the inner span, not the inner-minus-gaps span. That's
        // a deliberate choice: percent is "of the parent" in the spirit of
        // CSS, and gaps belong to the layout, not to the parent's content.
        px = (mainSpan * s.percent) / 100;
        break;
      case Mode::Grow:
        growWeight += s.weight;
        break;
    }
    out[i++] = px;
    consumed += px;
  }
  // Remaining space after fixed + percent + gaps is distributed across grow
  // children proportional to their weight. Truncation rounding can leave a
  // few px unspoken-for — those land in the trailing edge and are invisible
  // (cross-axis covers the full inner span anyway).
  const int distributable = mainSpan - consumed - gapTotal;
  if (growWeight > 0 && distributable > 0) {
    i = 0;
    for (const Size& s : sizes) {
      if (s.mode == Mode::Grow) {
        out[i] = (distributable * s.weight) / growWeight;
      }
      ++i;
    }
  }
  return n;
}

}  // namespace detail

class Vstack {
 public:
  static constexpr size_t kMaxChildren = 16;

  inline Vstack(const Rect& parent, std::initializer_list<Size> sizes, int gap = 0,
                Padding pad = {}) {
    assert(sizes.size() <= kMaxChildren);
    const Rect inner = inset(parent, pad);
    int heights[kMaxChildren] = {};
    count_ = detail::resolveMainAxis(sizes, inner.height, gap, heights);
    int y = inner.y;
    for (size_t i = 0; i < count_; ++i) {
      children_[i] = Rect{inner.x, y, inner.width, heights[i]};
      y += heights[i] + gap;
    }
  }

  const Rect& operator[](size_t i) const {
    assert(i < count_);
    return children_[i];
  }
  size_t size() const { return count_; }

 private:
  Rect children_[kMaxChildren];
  size_t count_ = 0;
};

class Hstack {
 public:
  static constexpr size_t kMaxChildren = 16;

  inline Hstack(const Rect& parent, std::initializer_list<Size> sizes, int gap = 0,
                Padding pad = {}) {
    assert(sizes.size() <= kMaxChildren);
    const Rect inner = inset(parent, pad);
    int widths[kMaxChildren] = {};
    count_ = detail::resolveMainAxis(sizes, inner.width, gap, widths);
    int x = inner.x;
    for (size_t i = 0; i < count_; ++i) {
      children_[i] = Rect{x, inner.y, widths[i], inner.height};
      x += widths[i] + gap;
    }
  }

  const Rect& operator[](size_t i) const {
    assert(i < count_);
    return children_[i];
  }
  size_t size() const { return count_; }

 private:
  Rect children_[kMaxChildren];
  size_t count_ = 0;
};

// Place an intrinsically-sized child rect inside a parent rect, aligned per
// axis. Use this when a Vstack/Hstack row's child wants its natural size
// instead of the row's full cross-axis dimension — common for centered text,
// right-anchored icons, etc. Pure function, stateless, header-only.
enum class HAlign : uint8_t { Start, Center, End };
enum class VAlign : uint8_t { Start, Center, End };

inline Rect align(const Rect& parent, int childWidth, int childHeight,
                  HAlign h = HAlign::Center, VAlign v = VAlign::Center) {
  int x = parent.x;
  if (h == HAlign::Center) x = parent.x + (parent.width - childWidth) / 2;
  else if (h == HAlign::End) x = parent.x + parent.width - childWidth;
  int y = parent.y;
  if (v == VAlign::Center) y = parent.y + (parent.height - childHeight) / 2;
  else if (v == VAlign::End) y = parent.y + parent.height - childHeight;
  return Rect{x, y, childWidth, childHeight};
}

// Shortcut for the both-axes-center case.
inline Rect center(const Rect& parent, int childWidth, int childHeight) {
  return align(parent, childWidth, childHeight, HAlign::Center, VAlign::Center);
}

class Grid {
 public:
  static constexpr size_t kMaxCells = 16;  // 4×4 max

  inline Grid(const Rect& parent, int rows, int cols, int rowGap = 0, int colGap = 0,
              Padding pad = {})
      : rows_(rows), cols_(cols) {
    assert(rows > 0 && cols > 0);
    assert(static_cast<size_t>(rows * cols) <= kMaxCells);
    const Rect inner = inset(parent, pad);
    const int cellW = (inner.width - colGap * (cols - 1)) / cols;
    const int cellH = (inner.height - rowGap * (rows - 1)) / rows;
    for (int r = 0; r < rows; ++r) {
      for (int c = 0; c < cols; ++c) {
        children_[r * cols + c] = Rect{inner.x + c * (cellW + colGap),
                                       inner.y + r * (cellH + rowGap), cellW, cellH};
      }
    }
  }

  const Rect& operator[](size_t i) const {
    assert(i < static_cast<size_t>(rows_ * cols_));
    return children_[i];
  }
  const Rect& at(int row, int col) const { return children_[row * cols_ + col]; }
  int rows() const { return rows_; }
  int cols() const { return cols_; }

 private:
  Rect children_[kMaxCells];
  int rows_;
  int cols_;
};

}  // namespace flex
