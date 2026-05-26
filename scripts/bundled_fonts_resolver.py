#!/usr/bin/env python3
"""Resolve BUNDLED_FONTS declarations into .cpfont files for theme builds.

This module handles the two-level schema:
  - Top-level BUNDLED_FONTS declares which families to fetch and which styles.
  - Per-role bundled: references a family by name + desired size.

The resolver collects needed sizes from font roles, queries the Google Fonts
CSS API, downloads TTFs (with caching), converts them to .cpfont via
fontconvert_sdcard.py, and writes results to the theme's fonts/ dir.

The CSS API is treated as authoritative: if a requested style isn't returned,
the user asked for a style that doesn't exist for that family — we warn and
skip it rather than trying to synthesize one.

Usage as a library:
    from bundled_fonts_resolver import resolve_bundled_fonts
    paths = resolve_bundled_fonts(bundled_fonts, font_roles, cache_root, theme_dir)

Dependencies:
    - PyYAML (for YAML parsing in build_cptheme.py integration)
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import urllib.request
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Dependency check
# ---------------------------------------------------------------------------

try:
    import freetype
except ImportError:
    # freetype-py is needed by fontconvert_sdcard.py; warn but don't abort here
    # since this module may be imported in contexts where conversion isn't triggered.
    print(
        "WARNING: freetype-py not found. fontconvert_sdcard.py will fail if called.",
        file=sys.stderr,
    )

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

GOOGLE_FONTS_CSS_API = (
    "https://fonts.googleapis.com/css2?family={family}"
    ":ital,wght@0,400;0,700;1,400;1,700"
)

# Regex to extract @font-face blocks and their URLs from CSS.
_CSS_FACE_RE = re.compile(
    r"@font-face\s*\{([^}]*)\}", re.DOTALL
)
_CSS_SRC_RE = re.compile(r"src:\s*url\(([^)]+)\)")
_CSS_STYLE_RE = re.compile(r"font-style:\s*(normal|italic)")
_CSS_WEIGHT_RE = re.compile(r"font-weight:\s*(\d+)")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _download_with_cache(
    url: str, cache_dir: Path, filename: str | None = None
) -> Path:
    """Download a file with caching. Returns the cached path."""
    if filename is None:
        # Derive filename from URL or use a generic name
        filename = "font.ttf"

    dest = cache_dir / filename
    if dest.exists():
        return dest

    cache_dir.mkdir(parents=True, exist_ok=True)
    print(f"  Downloading {url} ...", file=sys.stderr)
    try:
        urllib.request.urlretrieve(url, str(dest))
    except Exception as exc:
        # Clean up partial download on failure
        if dest.exists():
            dest.unlink()
        raise RuntimeError(
            f"Failed to download from Google Fonts API: {exc}"
        ) from exc
    return dest


def _parse_css_faces(css_text: str) -> list[dict[str, Any]]:
    """Parse @font-face blocks from CSS text.

    Returns a list of dicts with keys: style (normal/italic), weight (int), url.
    """
    faces = []
    for match in _CSS_FACE_RE.finditer(css_text):
        block = match.group(1)
        src_m = _CSS_SRC_RE.search(block)
        if not src_m:
            continue
        style_m = _CSS_STYLE_RE.search(block)
        weight_m = _CSS_WEIGHT_RE.search(block)
        faces.append({
            "style": style_m.group(1) if style_m else "normal",
            "weight": int(weight_m.group(1)) if weight_m else 400,
            "url": src_m.group(1),
        })
    return faces


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def resolve_bundled_fonts(
    bundled_fonts: list[dict[str, Any]],
    font_roles: dict[str, Any],
    cache_root: Path,
    theme_dir: Path,
) -> list[str]:
    """Resolve BUNDLED_FONTS + per-role bundled references into .cpfont file paths.

    Args:
        bundled_fonts: The top-level BUNDLED_FONTS list from the theme YAML.
            Each entry has at least ``name`` (family name), ``styles``, and
            optionally ``source`` (default: "google-fonts").
        font_roles: The ``fonts:`` section of the theme YAML.  Roles with a
            ``bundled:`` key reference a family by name + desired size.
        cache_root: Shared build cache directory for downloaded/instanced TTFs.
        theme_dir: Theme source directory; .cpfont files are written here.

    Returns:
        List of absolute paths to generated .cpfont files.
    """
    # ------------------------------------------------------------------
    # 1. Collect needed sizes per family from font roles.
    # ------------------------------------------------------------------
    family_sizes: dict[str, set[int]] = {}
    for role_name, role_spec in font_roles.items():
        if not isinstance(role_spec, dict):
            continue
        bundled = role_spec.get("bundled")
        if not isinstance(bundled, dict):
            continue
        fam = bundled.get("family")
        sz = bundled.get("size")
        if not fam or sz is None:
            continue
        family_sizes.setdefault(fam, set()).add(int(sz))

    # ------------------------------------------------------------------
    # 2. Resolve each family declared in BUNDLED_FONTS.
    # ------------------------------------------------------------------
    all_cpfont_paths: list[str] = []

    for fb_entry in bundled_fonts:
        family_name = fb_entry.get("name", "")
        styles_requested = fb_entry.get("styles", ["regular"])
        source = fb_entry.get("source", "google-fonts")

        if not family_name:
            print(
                f"WARNING: BUNDLED_FONTS entry has no 'name', skipping.",
                file=sys.stderr,
            )
            continue

        needed_sizes = family_sizes.get(family_name)
        if not needed_sizes:
            # No font role references this family — nothing to generate.
            print(
                f"  Skipping {family_name}: no font roles reference it.",
                file=sys.stderr,
            )
            continue

        sizes_str = ",".join(str(s) for s in sorted(needed_sizes))
        print(
            f"\nResolving '{family_name}' from Google Fonts "
            f"(sizes: {sizes_str}) ...",
            file=sys.stderr,
        )

        if source == "google-fonts":
            # ------------------------------------------------------------------
            # 2a. Query Google Fonts CSS API for available styles.
            # ------------------------------------------------------------------
            api_url = GOOGLE_FONTS_CSS_API.format(family=family_name)

            try:
                req = urllib.request.Request(
                    api_url, headers={"User-Agent": "CrossPointReader/1.0"}
                )
                with urllib.request.urlopen(req, timeout=30) as resp:
                    css_text = resp.read().decode("utf-8")
            except Exception as exc:
                print(
                    f"ERROR: Failed to query Google Fonts CSS API for '{family_name}': {exc}",
                    file=sys.stderr,
                )
                continue

            faces = _parse_css_faces(css_text)

            # ------------------------------------------------------------------
            # 2b. Organize available styles by our internal style name.
            # ------------------------------------------------------------------
            available_styles: dict[str, list[dict]] = {}
            for face in faces:
                is_italic = face["style"] == "italic"
                weight = face["weight"]

                if not is_italic and weight == 400:
                    available_styles.setdefault("regular", []).append(face)
                elif not is_italic and weight == 700:
                    available_styles.setdefault("bold", []).append(face)
                elif is_italic and weight == 400:
                    available_styles.setdefault("italic", []).append(face)
                elif is_italic and weight == 700:
                    available_styles.setdefault("bolditalic", []).append(face)

            # ------------------------------------------------------------------
            # 2c. Download TTFs (with caching).
            # ------------------------------------------------------------------
            # Cache filename is keyed by our internal style name, not the URL
            # basename — Google Fonts CSS URLs embed a version hash that rotates
            # between API queries, so a URL-derived filename would miss the
            # cache on every run.
            downloaded_ttls: dict[str, Path] = {}  # style -> local path

            for our_style in styles_requested:
                faces_list = available_styles.get(our_style, [])
                if not faces_list:
                    print(
                        f"  WARNING: '{family_name}' has no '{our_style}' face "
                        f"available from Google Fonts; check the styles list "
                        f"in BUNDLED_FONTS. Skipping this style.",
                        file=sys.stderr,
                    )
                    continue

                # Use the first available URL.
                face = faces_list[0]
                url = face["url"]
                cache_dir = cache_root / "downloaded" / family_name
                local_path = _download_with_cache(url, cache_dir, f"{our_style}.ttf")

                downloaded_ttls[our_style] = local_path

            # ------------------------------------------------------------------
            # 2d. Build the style-to-TTF map for fontconvert_sdcard.py.
            # ------------------------------------------------------------------
            style_to_ttf: dict[str, str] = {}
            for our_style in ["regular", "bold", "italic", "bolditalic"]:
                if our_style in downloaded_ttls and downloaded_ttls[our_style].exists():
                    style_to_ttf[our_style] = str(downloaded_ttls[our_style])

            if not style_to_ttf:
                print(
                    f"ERROR: No TTF files available for '{family_name}'. "
                    f"Skipping this family.",
                    file=sys.stderr,
                )
                continue

            # ------------------------------------------------------------------
            # 2e. Call fontconvert_sdcard.py as a subprocess (multi-style mode).
            # ------------------------------------------------------------------
            theme_fonts_dir = theme_dir / "fonts"
            theme_fonts_dir.mkdir(parents=True, exist_ok=True)

            cmd: list[str] = [
                sys.executable,
                str(Path(__file__).parent.parent / "lib" / "EpdFont" / "scripts" / "fontconvert_sdcard.py"),
            ]

            for our_style in ["regular", "bold", "italic", "bolditalic"]:
                if our_style in style_to_ttf:
                    cmd.extend([f"--{our_style}", style_to_ttf[our_style]])

            cmd.extend(["--intervals", "reading"])
            cmd.extend(["--sizes", sizes_str])
            cmd.extend(["--name", family_name])
            cmd.extend(["--output-dir", str(theme_fonts_dir)])

            print(
                f"  Generating .cpfont files via fontconvert_sdcard.py ...",
                file=sys.stderr,
            )
            try:
                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=600,  # 10 min max for large fonts.
                )
                if result.returncode != 0:
                    print(
                        f"ERROR: fontconvert_sdcard.py failed for '{family_name}':\n"
                        f"{result.stderr}",
                        file=sys.stderr,
                    )
                    continue
            except subprocess.TimeoutExpired:
                print(
                    f"ERROR: fontconvert_sdcard.py timed out for '{family_name}'.",
                    file=sys.stderr,
                )
                continue
            except Exception as exc:
                print(
                    f"ERROR: Failed to run fontconvert_sdcard.py for "
                    f"'{family_name}': {exc}",
                    file=sys.stderr,
                )
                continue

            # ------------------------------------------------------------------
            # 2f. Collect generated .cpfont paths.
            # ------------------------------------------------------------------
            for fname in sorted(theme_fonts_dir.iterdir()):
                if fname.suffix == ".cpfont":
                    all_cpfont_paths.append(str(fname.resolve()))

    return all_cpfont_paths


# ---------------------------------------------------------------------------
# CLI entry point (for standalone testing)
# ---------------------------------------------------------------------------


def main() -> None:
    """Standalone CLI for testing the resolver."""
    import argparse  # noqa: local import to avoid hard dependency.
    try:
        import yaml  # noqa: local import to avoid hard dependency.
    except ImportError:
        print("PyYAML is required: pip install pyyaml", file=sys.stderr)
        sys.exit(1)

    parser = argparse.ArgumentParser(
        description="Resolve BUNDLED_FONTS declarations into .cpfont files."
    )
    parser.add_argument("theme_dir", help="Theme source directory (contains theme.yml)")
    parser.add_argument(
        "--cache-dir",
        default=None,
        help="Shared build cache root (default: <repo>/build/bundled_fonts/)",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).parent.parent
    theme_path = Path(args.theme_dir)
    if not args.cache_dir:
        cache_root = repo_root / "build" / "bundled_fonts"
    else:
        cache_root = Path(args.cache_dir)

    yml_file = theme_path / "theme.yml"
    with open(yml_file, "r") as f:
        theme = yaml.safe_load(f)

    bundled_fonts = theme.get("BUNDLED_FONTS", [])
    if not bundled_fonts:
        print("No BUNDLED_FONTS found in theme.yml. Nothing to resolve.", file=sys.stderr)
        sys.exit(0)

    font_roles = theme.get("fonts", {})
    paths = resolve_bundled_fonts(bundled_fonts, font_roles, cache_root, theme_path)

    print(f"\nGenerated {len(paths)} .cpfont file(s):")
    for p in paths:
        print(f"  {p}")


if __name__ == "__main__":
    main()
