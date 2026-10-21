#!/usr/bin/env python3
"""Package the iPhone game (build_arm64.py --target ios or ios-simulator).

A flat iOS .app: the executable, Info.plist and the program files at the
bundle's root, which paths.c takes as the program directory. The iPhone
build also becomes an .ipa (Payload/YFM.app) that a sideloading tool
(SideStore, AltStore, Sideloadly) signs with the player's own Apple ID;
here it is only ad-hoc signed. The simulator's .app runs as it is.

The player's disc goes in the Files app, On My iPhone > YFM > game
(platform/ios.c, Info.plist UIFileSharingEnabled).
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


def shipped_mod_files():
    """The repository's data mods. Code mods (a library) need their iPhone
    build, which is not made yet: they are left out."""
    tracked = subprocess.check_output(["git", "ls-files", "-z", "mods"], cwd=ROOT, text=True).split("\0")
    files = [Path(name) for name in tracked if name and Path(name).suffix not in (".c", ".h", ".o", ".dylib")]
    code = set()
    for relative in files:
        if relative.name == "mod.json":
            manifest = json.loads((ROOT / relative).read_text(encoding="utf-8-sig"))
            if manifest.get("library"):
                code.add(relative.parent)
    return [path for path in files if not any(path.is_relative_to(folder) for folder in code)]


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
    mod_files = shipped_mod_files()
    for relative in mod_files:
        destination = app / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / relative, destination)
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
        CFBundleDisplayName="Forbidden Memories",
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
        PackageVersion=label,
    )
    (app / "Info.plist").write_bytes(plistlib.dumps(info))
    (app / "PkgInfo").write_bytes(b"APPL????")
    manifest = dict(
        version=label,
        platform=sdk,
        minimum_ios=IOS_MINIMUM,
        source_binary_sha256=hashlib.sha256(data).hexdigest(),
        mod_files={str(path): hashlib.sha256((app / path).read_bytes()).hexdigest() for path in mod_files},
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
