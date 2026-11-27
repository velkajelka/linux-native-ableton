#!/usr/bin/env python3
# fix-pack-paths.py [--dry-run] [--quiet] DIR...
#
# Ableton's factory packs ship their browser metadata (Ableton Folder Info/
# Metadata.cfg) with Windows path separators: a UTF-16LE string in Beat Tools
# reads "Ableton Folder Info\Previews\Drums". The Core Library's Metadata.cfg
# uses forward slashes, and so does everything Live writes itself.
#
# Live resolves what a pack folder contains from that map, not from the
# directory. On Linux a key with a backslash never matches the path it asks
# for, so a pack's first-level folders are listed and everything inside them
# is empty. Rewriting the separators in the stored strings is all that is
# needed: after this, Live indexes the pack contents normally.
#
# Each DIR is a pack root (a folder holding "Ableton Folder Info/Metadata.cfg")
# or a parent of pack roots; the parents are one level down, which covers
# "Factory Packs" as well as an individual pack.
#
# Idempotent: a second run reports nothing to do. Only U+005C encoded as
# UTF-16LE (5C 00) is replaced, so an already-fixed file is left untouched.
#
# Exit status: 0 on success (including nothing to do), 1 on an unreadable file.
import os
import sys

META = os.path.join("Ableton Folder Info", "Metadata.cfg")
BACKSLASH = b"\x5c\x00"
SLASH = b"\x2f\x00"


def pack_roots(paths):
    """Yield every pack root given, plus every child that looks like one."""
    for path in paths:
        if os.path.isfile(os.path.join(path, META)):
            yield path
            continue
        if not os.path.isdir(path):
            continue
        for name in sorted(os.listdir(path)):
            child = os.path.join(path, name)
            if os.path.isfile(os.path.join(child, META)):
                yield child


def main(argv):
    dry_run = "--dry-run" in argv
    quiet = "--quiet" in argv
    paths = [a for a in argv if not a.startswith("--")]
    if not paths:
        print("usage: fix-pack-paths.py [--dry-run] [--quiet] DIR...", file=sys.stderr)
        return 2

    changed = fixed = 0
    for root in pack_roots(paths):
        meta = os.path.join(root, META)
        try:
            with open(meta, "rb") as f:
                data = f.read()
        except OSError as e:
            print(f"{root}: cannot read {meta}: {e}", file=sys.stderr)
            return 1
        count = data.count(BACKSLASH)
        name = os.path.basename(root.rstrip("/"))
        if not count:
            if not quiet:
                print(f"{name}: already uses '/'")
            continue
        fixed += 1
        changed += count
        if dry_run:
            print(f"{name}: would replace {count} separators")
            continue
        try:
            with open(meta, "wb") as f:
                f.write(data.replace(BACKSLASH, SLASH))
        except OSError as e:
            print(f"{root}: cannot write {meta}: {e}", file=sys.stderr)
            return 1
        print(f"{name}: replaced {count} separators")
    if not quiet:
        verb = "would fix" if dry_run else "fixed"
        print(f"{verb} {fixed} pack(s), {changed} separators")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
