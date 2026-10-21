#!/usr/bin/env python3
"""Package the iPhone game (build_arm64.py --target ios or ios-simulator).

A flat iOS .app: the executable, Info.plist and the program files at the
bundle's root, which paths.c takes as the program directory. The iPhone
build also becomes an .ipa (Payload/YFM.app) that a sideloading tool
(SideStore, AltStore, Sideloadly) signs with the player's own Apple ID;
here it is only ad-hoc signed. The simulator's .app runs as it is.

The player's disc goes in the Files app, On My iPhone > (the game) > game
(platform/ios.c, Info.plist UIFileSharingEnabled), or Choose ROM... copies
it there (sdl.c, ios_picker.m).

The mods are the repository's (mods/) and this fork's own (ios/mods/). An
iPhone runs only code signed with the app, so a code mod is built for the
iPhone (build_mod_arm64.py, with the game's exports) and goes in as a
framework, Frameworks/<id>.framework/<id>, where mods.c loads it from; a
sideloading tool signs it with the app. ios/app.json may name the app
("name") and the mods to switch on ("enable"); ios/icon.png is its icon.
"""

import argparse
import hashlib
import json
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

from build_config import IOS_MINIMUM, ROOT

APP = "YFM.app"
EXECUTABLE = "memories-arm64"
EXTRA = ROOT / "ios"  # this fork's own: mods/, icon.png, app.json
ICONS = {"AppIcon60x60@2x.png": 120, "AppIcon60x60@3x.png": 180,
         "AppIcon76x76@2x~ipad.png": 152, "AppIcon83.5x83.5@2x~ipad.png": 167}
PLATFORMS = {2: "iphoneos", 7: "iphonesimulator"}  # LC_BUILD_VERSION platform


def build_platform(data):
    """The executable's LC_BUILD_VERSION platform: iPhone or its simulator."""
    if len(data) < 32 or struct.unpack_from("<I", data, 0)[0] != 0xFEEDFACF:
        raise ValueError("executable is not a 64-bit Mach-O")
    if struct.unpack_from("<I", data, 4)[0] != 0x0100000C:
        raise ValueError("executable is not arm64")
    count, offset = struct.unpack_from("<I", data, 16)[0], 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, offset)
        if command == 0x32:
            platform = struct.unpack_from("<I", data, offset + 8)[0]
            if platform not in PLATFORMS:
                raise ValueError(f"executable is for platform {platform}, not the iPhone")
            return PLATFORMS[platform]
        offset += size
    raise ValueError("executable has no LC_BUILD_VERSION")


def settings():
    path = EXTRA / "app.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}


def shipped_mods():
    """{source folder: [its tracked files, from the folder]}: mods/<mod> and
    ios/mods/<mod>, which goes in as mods/<mod> too (and wins)."""
    tracked = subprocess.check_output(["git", "ls-files", "-z", "mods", "ios/mods"], cwd=ROOT, text=True).split("\0")
    mods = {}
    for name in filter(None, tracked):
        parts = Path(name).parts
        depth = 3 if parts[0] == "ios" else 2
        if len(parts) > depth:
            mods.setdefault(Path(*parts[:depth]), []).append(Path(*parts[depth:]))
    return dict(sorted(mods.items(), key=lambda item: item[0].parts[0] == "ios"))


def framework(app, mod_id, library, sdk):
    """The mod's dylib as the app's framework, Frameworks/<id>.framework."""
    folder = app / "Frameworks" / f"{mod_id}.framework"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    executable = folder / mod_id
    shutil.copyfile(library, executable)
    executable.chmod(0o755)
    subprocess.run(["install_name_tool", "-id", f"@rpath/{mod_id}.framework/{mod_id}", str(executable)], check=True)
    info = dict(
        CFBundleExecutable=mod_id,
        CFBundleIdentifier="org.yfm-redecomp.memories.mod." + re.sub(r"[^A-Za-z0-9.-]", "-", mod_id),
        CFBundleName=mod_id,
        CFBundlePackageType="FMWK",
        CFBundleInfoDictionaryVersion="6.0",
        CFBundleVersion="1",
        CFBundleShortVersionString="1.0",
        CFBundleSupportedPlatforms=["iPhoneSimulator" if sdk == "iphonesimulator" else "iPhoneOS"],
        MinimumOSVersion=IOS_MINIMUM,
    )
    (folder / "Info.plist").write_bytes(plistlib.dumps(info))
    subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none", str(folder)], check=True)
    return executable


def stage_mods(app, game, sdk, enable):
    """The mods' files into the app, each code mod built for this SDK."""
    from build_mod_arm64 import build

    staged, libraries = [], {}
    for source, files in shipped_mods().items():
        target = Path("mods") / source.name
        shutil.rmtree(app / target, ignore_errors=True)
        for relative in files:
            if relative.suffix in (".c", ".h", ".o", ".dylib"):
                continue
            destination = app / target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / source / relative, destination)
            staged.append(target / relative)
        manifest_path = app / target / "mod.json"
        if not manifest_path.is_file():
            continue
        text = manifest_path.read_text(encoding="utf-8-sig")
        manifest = json.loads(text)
        if manifest.get("id") in enable:
            text = re.sub(r'"enabled"\s*:\s*false', '"enabled": true', text)
            if '"enabled"' not in text:
                text = re.sub(r"^\s*\{", '{\n  "enabled": true,', text, count=1)
        manifest_path.write_text(text, encoding="utf-8")
        if manifest.get("library") or manifest.get("libraries"):
            built = build(ROOT / source, out=game / "ios-mods" / source.name, game=game, sdk_name=sdk)
            libraries[manifest["id"]] = framework(app, manifest["id"], built, sdk)
    return staged, libraries


def icons(app):
    """ios/icon.png at the iPhone's and iPad's sizes (sips), for CFBundleIcons."""
    source = EXTRA / "icon.png"
    if not source.is_file():
        return {}
    for name, size in ICONS.items():
        subprocess.run(["sips", "-s", "format", "png", "-z", str(size), str(size), str(source),
                        "--out", str(app / name)], check=True, capture_output=True)
    return {
        "CFBundleIcons": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon60x60"]}},
        "CFBundleIcons~ipad": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon60x60", "AppIcon76x76",
                                                                             "AppIcon83.5x83.5"]}},
    }


def package(binary, out, label):
    if sys.platform != "darwin":
        raise ValueError("iPhone packaging needs codesign on a Mac")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", label):
        raise ValueError("version must be a filename-safe release label")
    data = binary.read_bytes()
    sdk = build_platform(data)
    stage = out / "stage"
    shutil.rmtree(stage, ignore_errors=True)
    app = stage / APP
    app.mkdir(parents=True)
    shutil.copy2(binary, app / EXECUTABLE)
    (app / EXECUTABLE).chmod(0o755)
    languages = app / "languages"
    languages.mkdir()
    for name in ("en-eu", "fr", "de", "it", "es"):
        shutil.copyfile(ROOT / f"languages/{name}.txt", languages / f"{name}.txt")
    extra = settings()
    mod_files, mod_libraries = stage_mods(app, binary.parent, sdk, set(extra.get("enable", ())))
    app_icons = icons(app)
    shutil.copyfile(ROOT / "LICENSE", app / "LICENSE.txt")
    licenses = app / "licenses"
    licenses.mkdir()
    sources = ROOT / "tmp/pc/macos-deps/src"
    for name, source in {"SDL3.txt": "sdl-source/LICENSE.txt", "libpng.txt": "libpng/LICENSE",
                         "zlib.txt": "zlib/LICENSE", "FreeType.txt": "freetype/docs/FTL.TXT"}.items():
        shutil.copyfile(sources / source, licenses / name)
    build_number = subprocess.check_output(["git", "rev-list", "--count", "HEAD"], cwd=ROOT, text=True).strip()
    release = re.match(r"^v?(\d+\.\d+\.\d+)(?:[-.].*)?$", label)
    landscape = ["UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"]
    info = dict(
        CFBundleName="YFM",
        CFBundleDisplayName=extra.get("name", "Forbidden Memories"),
        CFBundleIdentifier="org.yfm-redecomp.memories",
        CFBundleExecutable=EXECUTABLE,
        CFBundlePackageType="APPL",
        CFBundleInfoDictionaryVersion="6.0",
        CFBundleDevelopmentRegion="en",
        CFBundleVersion=build_number,
        CFBundleShortVersionString=release.group(1) if release else "0.0.0",
        CFBundleSupportedPlatforms=["iPhoneSimulator" if sdk == "iphonesimulator" else "iPhoneOS"],
        DTPlatformName=sdk,
        MinimumOSVersion=IOS_MINIMUM,
        UIDeviceFamily=[1, 2],
        UIRequiredDeviceCapabilities=["arm64"],
        UILaunchScreen={},
        UIRequiresFullScreen=True,
        UIStatusBarHidden=True,
        UISupportedInterfaceOrientations=landscape,
        **{"UISupportedInterfaceOrientations~ipad": landscape},
        # The Files app shows Documents (the user folder): saves, mods and
        # the disc's game folder.
        UIFileSharingEnabled=True,
        LSSupportsOpeningDocumentsInPlace=True,
        GCSupportsControllerUserInteraction=True,
        # A mouse or trackpad's own events (SDL asks for it).
        UIApplicationSupportsIndirectInputEvents=True,
        PackageVersion=label,
        **app_icons,
    )
    (app / "Info.plist").write_bytes(plistlib.dumps(info))
    (app / "PkgInfo").write_bytes(b"APPL????")
    manifest = dict(
        version=label,
        platform=sdk,
        minimum_ios=IOS_MINIMUM,
        source_binary_sha256=hashlib.sha256(data).hexdigest(),
        mod_files={str(path): hashlib.sha256((app / path).read_bytes()).hexdigest() for path in mod_files},
        mod_libraries={mod: hashlib.sha256(path.read_bytes()).hexdigest() for mod, path in mod_libraries.items()},
    )
    (app / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none", str(app)], check=True)
    subprocess.run(["codesign", "--verify", "--strict", str(app)], check=True)
    if sdk == "iphonesimulator":
        archive = out / f"yfm-{label}-ios-simulator.zip"
        prefix = Path()
    else:
        archive = out / f"yfm-{label}-iphone.ipa"
        prefix = Path("Payload")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for path in sorted(app.rglob("*")):
            if path.is_file():
                member = zipfile.ZipInfo.from_file(path, prefix / path.relative_to(stage))
                member.compress_type = zipfile.ZIP_DEFLATED
                bundle.writestr(member, path.read_bytes())
    print(archive)
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "tmp/pc/ios/memories-arm64")
    parser.add_argument("--out", type=Path, default=ROOT / "tmp/pc-release/ios")
    parser.add_argument("--version", default=None)
    args = parser.parse_args()
    label = args.version or "dev-" + subprocess.check_output(
        ["git", "rev-parse", "--short=12", "HEAD"], cwd=ROOT, text=True).strip()
    package(args.binary.resolve(), args.out.resolve(), label)


if __name__ == "__main__":
    main()
