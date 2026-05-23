#!/usr/bin/env python3
"""Package a theme source directory into a .cptheme archive.

Usage:
    python3 scripts/build_cptheme.py themes/classic/ -o classic.cptheme
    python3 scripts/build_cptheme.py themes/classic/          # outputs classic.cptheme
    python3 scripts/build_cptheme.py --all -d build/themes/   # build all themes in themes/

A .cptheme is a ZIP archive (STORED, no DEFLATE) containing:
    theme.json   - converted from the source theme.yml
    fonts/       - optional .cpfont files copied verbatim
"""

import argparse
import json
import os
import sys
import zipfile

try:
    import yaml
except ImportError:
    print("PyYAML is required: pip install pyyaml", file=sys.stderr)
    sys.exit(1)


def build_cptheme(src_dir: str, output_path: str) -> None:
    yml_path = os.path.join(src_dir, "theme.yml")
    if not os.path.isfile(yml_path):
        raise FileNotFoundError(f"No theme.yml found in {src_dir}")

    with open(yml_path, "r") as f:
        theme = yaml.safe_load(f)

    theme_json = json.dumps(theme, indent=2, ensure_ascii=False).encode("utf-8")

    with zipfile.ZipFile(output_path, "w", compression=zipfile.ZIP_STORED) as zf:
        zf.writestr("theme.json", theme_json)

        fonts_dir = os.path.join(src_dir, "fonts")
        if os.path.isdir(fonts_dir):
            for fname in sorted(os.listdir(fonts_dir)):
                if not fname.endswith(".cpfont"):
                    continue
                fpath = os.path.join(fonts_dir, fname)
                zf.write(fpath, f"fonts/{fname}")

    print(f"  {output_path}  ({os.path.getsize(output_path)} bytes)")


def main():
    parser = argparse.ArgumentParser(description="Build .cptheme archives")
    parser.add_argument("src_dir", nargs="?", help="Theme source directory")
    parser.add_argument("-o", "--output", help="Output .cptheme path")
    parser.add_argument("--all", action="store_true",
                        help="Build all themes in the themes/ directory")
    parser.add_argument("-d", "--dest", default=".",
                        help="Output directory for --all mode")
    args = parser.parse_args()

    if args.all:
        themes_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "themes")
        if not os.path.isdir(themes_root):
            print(f"No themes/ directory found at {themes_root}", file=sys.stderr)
            sys.exit(1)

        os.makedirs(args.dest, exist_ok=True)
        count = 0
        for entry in sorted(os.listdir(themes_root)):
            theme_dir = os.path.join(themes_root, entry)
            if not os.path.isdir(theme_dir):
                continue
            if not os.path.isfile(os.path.join(theme_dir, "theme.yml")):
                continue
            out = os.path.join(args.dest, f"{entry}.cptheme")
            build_cptheme(theme_dir, out)
            count += 1
        print(f"\nBuilt {count} theme(s)")
    elif args.src_dir:
        if not os.path.isdir(args.src_dir):
            print(f"Not a directory: {args.src_dir}", file=sys.stderr)
            sys.exit(1)
        if args.output:
            out = args.output
        else:
            name = os.path.basename(os.path.normpath(args.src_dir))
            out = f"{name}.cptheme"
        build_cptheme(args.src_dir, out)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
