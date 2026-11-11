#!/usr/bin/env python3
"""Complete the browser's device catalog.

Live's browser lists built-in devices (Instruments, Audio Effects, MIDI
Effects, Modulators, Max for Live) from entries the indexer creates out of the
device stubs in its Builtin folder (Builtin/Devices/<type>/<device>/Ableton
Folder Info/Properties.cfg, each declaring "String DeviceId = ..."). The Push 3
image's indexer never processes that folder, so those categories stay empty
while Sounds and Drums (Core Library content) work.

The content Live does index carries the other half of the declaration already:
every device folder in the Core Library says which device its presets belong
to ("DeviceEquivalentId"). This tool adds the missing "DeviceId" line, so the
indexer registers the device itself. Devices whose folder has no indexable
preset (racks, drum synths, MIDI utilities) are declared in the User Library
instead, which Live indexes completely.

Max devices (.amxd) and the MIDI tools are copied where Live indexes them, and
the Modulators category is keyword-driven, so its keyword entries are added to
the Core Library's metadata manifest (which Live does import).

Idempotent: only missing declarations and files are added, nothing is removed
or overwritten.
"""
import glob
import os
import re
import shutil
import sys
import xml.etree.ElementTree as ET

# Usage: browser-catalog.py [--program PUSH3_DIR] [--music MUSIC_DIR]
# Defaults suit an installed tree: this file sits in <prefix>/tools, Live in
# <prefix>/push3_opt/push3, and the content in <Music>/Ableton.
HERE = os.path.dirname(os.path.abspath(__file__))
PREFIX = os.path.dirname(HERE)
PROGRAM = os.path.join(PREFIX, "push3_opt", "push3")
MUSIC = os.path.expanduser("~/Music/Ableton")
args = sys.argv[1:]
while args:
    option = args.pop(0)
    if option in ("--program", "--music") and args:
        if option == "--program":
            PROGRAM = os.path.abspath(args.pop(0))
        else:
            MUSIC = os.path.abspath(args.pop(0))
    else:
        sys.exit(f"browser-catalog: unknown argument {option}")

BUILTIN = os.path.join(PROGRAM, "products/live/Live/AppLive/Resources/Builtin")
BUILTIN_DEVICES = os.path.join(BUILTIN, "Devices")
BUILTIN_XMP = glob.glob(os.path.join(BUILTIN, "Ableton Folder Info", "*.xmp"))

CORE = os.path.join(MUSIC, "Live 12 Core Library", "Devices")
CORE_XMP = os.path.join(MUSIC, "Live 12 Core Library", "Ableton Folder Info")
USER = os.path.join(MUSIC, "User Library")
USER_PRESETS = {"Instruments": "Instruments", "Audio Effects": "Audio Effects", "MIDI Effects": "MIDI Effects"}

# The Modulators category filters on this keyword; the devices are Max effects.
MODULATORS = [
    "Devices/Audio Effects/Envelope Follower",
    "Devices/Audio Effects/LFO",
    "Devices/Audio Effects/Shaper",
    "Devices/MIDI Effects/Expression Control",
    "Devices/MIDI Effects/Envelope MIDI",
    "Devices/MIDI Effects/Shaper MIDI",
]
MODULATOR_KEYWORD = "Devices|Modulator"

CONFIG = ('Ableton#05I\n\nFolderConfigData\n{{\n'
          '  Bool IsProtected = true;\n'
          '  String DeviceEquivalentId = "{id}";\n'
          '  String DeviceId = "{id}";\n'
          '}}\n')


def declare(path, device_id):
    """Make `path`'s Properties.cfg carry the device declaration pair."""
    text = open(path).read() if os.path.exists(path) else ""
    if 'String DeviceEquivalentId' in text and 'String DeviceId' in text:
        return "already"
    if 'FolderConfigData' in text and '}' in text:
        idx = text.rfind('}')
        add = ""
        if 'String DeviceEquivalentId' not in text:
            add += f'  String DeviceEquivalentId = "{device_id}";\n'
        if 'String DeviceId' not in text:
            add += f'  String DeviceId = "{device_id}";\n'
        open(path, "w").write(text[:idx] + add + text[idx:])
        return "merged"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w").write(CONFIG.format(id=device_id))
    return "written"


def has_presets(folder):
    for _, _, files in os.walk(folder):
        if any(f.endswith((".adv", ".adg", ".alc")) for f in files):
            return True
    return False


def copy_stub_files(stub_dir, target_dir):
    """Copy a stub's own files (Max devices) next to the declaration."""
    copied = 0
    for src in glob.glob(os.path.join(stub_dir, "*")):
        if os.path.basename(src) == "Properties.cfg":
            continue
        dst = os.path.join(target_dir, os.path.basename(src))
        if os.path.isdir(src):
            for f in glob.glob(os.path.join(src, "**", "*"), recursive=True):
                if os.path.isfile(f):
                    d = os.path.join(dst, os.path.relpath(f, src))
                    os.makedirs(os.path.dirname(d), exist_ok=True)
                    if not os.path.exists(d):
                        shutil.copy2(f, d)
                        copied += 1
        elif not os.path.exists(dst):
            shutil.copy2(src, dst)
            copied += 1
    return copied


def stub_devices():
    """(device type, device name, device id, stub folder) for every stub."""
    found = []
    for cfg in sorted(glob.glob(os.path.join(BUILTIN_DEVICES, "**", "Properties.cfg"), recursive=True)):
        if os.path.basename(os.path.dirname(cfg)) != "Ableton Folder Info":
            continue
        match = re.search(r'String DeviceId = "([^"]+)"', open(cfg).read())
        if not match:
            continue
        stub_dir = os.path.dirname(cfg)
        rel = os.path.relpath(os.path.dirname(stub_dir), BUILTIN_DEVICES)
        parts = rel.split(os.sep)
        if parts[0] not in USER_PRESETS:
            continue  # MIDI Tools and other trees are handled separately
        # nested stubs (Drum Synth/DS Clap) are declared by their leaf name
        found.append((parts[0], parts[-1], match.group(1), stub_dir))
    return found


def check_paths():
    for path in (BUILTIN_DEVICES, CORE_XMP):
        if not os.path.isdir(path):
            sys.exit(f"browser-catalog: {path} not found — is Live installed?")


def main():
    check_paths()
    stats = {"core": 0, "user": 0, "copied": 0}
    for kind, name, device_id, stub_dir in stub_devices():
        core_folder = os.path.join(CORE, kind, name)
        if has_presets(core_folder):
            declare(os.path.join(core_folder, "Ableton Folder Info", "Properties.cfg"), device_id)
            stats["core"] += 1
        else:
            # no presets to hang the entry on: the User Library indexes
            # everything, including Max devices
            target = os.path.join(USER, "Presets", USER_PRESETS[kind], name, "Ableton Folder Info")
            os.makedirs(target, exist_ok=True)
            declare(os.path.join(target, "Properties.cfg"), device_id)
            stats["copied"] += copy_stub_files(stub_dir, target)
            stats["user"] += 1

    # MIDI tools: Live indexes .amxd only in the User Library
    for sub, folder in (("Generators", "Max Generators"), ("Transformations", "Max Transformations")):
        for amxd in glob.glob(os.path.join(BUILTIN_DEVICES, "MIDI Tools", sub, "*", "*.amxd")):
            target = os.path.join(USER, "MIDI Tools", folder)
            os.makedirs(target, exist_ok=True)
            if not os.path.exists(os.path.join(target, os.path.basename(amxd))):
                shutil.copy2(amxd, target)
                stats["copied"] += 1

    # Modulators: the category filters on a keyword, added to the manifest Live imports
    xmp = glob.glob(os.path.join(CORE_XMP, "*.xmp"))
    if xmp:
        path = xmp[0]
        text = open(path).read()
        missing = [p for p in MODULATORS if p not in text]
        if missing:
            items = "\n".join(
                f'<rdf:li rdf:parseType="Resource"><ablFR:filePath>{p}</ablFR:filePath>'
                f'<ablFR:keywords><rdf:Bag><rdf:li>{MODULATOR_KEYWORD}</rdf:li></rdf:Bag>'
                f'</ablFR:keywords></rdf:li>' for p in missing)
            match = re.search(r"\s*</rdf:Bag>\s*</ablFR:items>", text)
            if not match:
                sys.exit(f"browser-catalog: unexpected structure in {path}")
            text = text[:match.start()] + "\n" + items + text[match.start():]
            open(path, "w").write(text)
            ET.parse(path)  # refuse to leave broken XML behind

    print(f"browser-catalog: {stats['core']} devices declared in the Core Library, "
          f"{stats['user']} in the User Library, {stats['copied']} files copied")


if __name__ == "__main__":
    main()
