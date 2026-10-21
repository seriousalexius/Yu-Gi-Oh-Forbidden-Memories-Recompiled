"""Pinned static macOS ARM64 dependencies; no package manager required.

The iPhone targets (sdk_name iphoneos, iphonesimulator) build the same
libraries for the iOS SDK into tmp/pc/ios-deps/<sdk>: the macOS install
keeps the zstd the host guest pass links (llvm_guest.compiler)."""

import hashlib
import json
import os
import platform
import shutil
import subprocess
import tarfile
from pathlib import Path

from build_config import IOS_MINIMUM, MACOS_MINIMUM
from dependency_archives import ARCHIVES
from fetch_tools import download, unpack
from fetch_tools import ensure as ensure_tool

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tmp/pc/macos-deps"
INSTALL = OUT / "install"


def sdk_path(sdk_name="macosx"):
    sdk = Path(
        subprocess.check_output(
            ["xcrun", "--sdk", sdk_name, "--show-sdk-path"], text=True
        ).strip()
    )
    if not (sdk / "usr/include/stdlib.h").is_file():
        raise RuntimeError(f"xcrun returned an invalid {sdk_name} SDK: {sdk}")
    return sdk


def install_dir(sdk_name="macosx"):
    if sdk_name == "macosx":
        return INSTALL
    return ROOT / "tmp/pc/ios-deps" / sdk_name / "install"


def source(name):
    url, digest = ARCHIVES[name]
    archive = download(url, digest)
    folder = OUT / "src" / name
    stamp = folder / ".archive-sha256"
    if stamp.is_file() and stamp.read_text() == digest:
        return folder
    with tarfile.open(archive) as bundle:
        top = bundle.getnames()[0].split("/")[0]
    temporary = OUT / "src" / (name + ".extract")
    shutil.rmtree(temporary, ignore_errors=True)
    unpack(archive, str(temporary))
    shutil.rmtree(folder, ignore_errors=True)
    (temporary / top).rename(folder)
    shutil.rmtree(temporary)
    stamp.write_text(digest)
    return folder


def ensure(names=("zstd", "zlib", "libpng", "freetype", "sdl-source"), jobs=8, sdk_name="macosx"):
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise RuntimeError("macOS dependency builder requires an ARM64 Mac")
    from llvm_guest import toolchain

    llvm = toolchain()
    sdk = sdk_path(sdk_name)
    install = install_dir(sdk_name)
    if sdk_name == "macosx":
        system = [f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_MINIMUM}"]
    else:
        system = ["-DCMAKE_SYSTEM_NAME=iOS", f"-DCMAKE_OSX_DEPLOYMENT_TARGET={IOS_MINIMUM}"]
    cmake, ninja = ensure_tool("cmake", pinned=True), ensure_tool("ninja", pinned=True)
    options = {
        "zstd": [
            "-DZSTD_BUILD_SHARED=OFF",
            "-DZSTD_BUILD_STATIC=ON",
            "-DZSTD_BUILD_PROGRAMS=OFF",
            "-DZSTD_BUILD_TESTS=OFF",
        ],
        "zlib": [
            "-DZLIB_BUILD_SHARED=OFF",
            "-DZLIB_BUILD_TESTING=OFF",
            "-DBUILD_SHARED_LIBS=OFF",
        ],
        "libpng": [
            "-DPNG_SHARED=OFF",
            "-DPNG_STATIC=ON",
            "-DPNG_TESTS=OFF",
            "-DPNG_TOOLS=OFF",
            "-DPNG_FRAMEWORK=OFF",
            f"-DZLIB_LIBRARY={install}/lib/libz.a",
            f"-DZLIB_INCLUDE_DIR={install}/include",
        ],
        "freetype": [
            "-DBUILD_SHARED_LIBS=OFF",
            "-DFT_DISABLE_ZLIB=ON",
            "-DFT_DISABLE_BZIP2=ON",
            "-DFT_DISABLE_PNG=ON",
            "-DFT_DISABLE_HARFBUZZ=ON",
            "-DFT_DISABLE_BROTLI=ON",
        ],
        "sdl-source": [
            "-DSDL_STATIC=ON",
            "-DSDL_SHARED=OFF",
            "-DSDL_TESTS=OFF",
            "-DSDL_TEST_LIBRARY=OFF",
            "-DSDL_EXAMPLES=OFF",
            "-DSDL_INSTALL=ON",
        ],
    }
    libraries = dict(
        zip(
            options, ("libzstd.a", "libz.a", "libpng16.a", "libfreetype.a", "libSDL3.a")
        )
    )

    def tool_identity(path, *version_args):
        executable = Path(path).resolve()
        version = subprocess.check_output(
            [str(executable), *version_args], text=True
        ).splitlines()[0]
        return {"path": str(executable), "version": version}

    compiler_identity = {
        "clang": tool_identity(llvm / "bin/clang", "--version"),
        "clang++": tool_identity(llvm / "bin/clang++", "--version"),
        "llvm": tool_identity(llvm / "bin/llvm-config", "--version"),
        "cmake": tool_identity(cmake, "--version"),
        "ninja": tool_identity(ninja, "--version"),
    }
    for name in names:
        directory = source(name)
        if name == "zstd":
            directory /= "build/cmake"
        build = install.parent / "build" / name
        stamp = build / "complete.json"
        command = [
            cmake,
            "-S",
            str(directory),
            "-B",
            str(build),
            "-G",
            "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_C_COMPILER={llvm}/bin/clang",
            f"-DCMAKE_CXX_COMPILER={llvm}/bin/clang++",
            "-DCMAKE_OSX_ARCHITECTURES=arm64",
            *system,
            f"-DCMAKE_OSX_SYSROOT={sdk}",
            f"-DCMAKE_INSTALL_PREFIX={install}",
            f"-DCMAKE_PREFIX_PATH={install}",
            f"-DCMAKE_FIND_ROOT_PATH={install};{sdk}",
            "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",
            "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY",
            "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY",
            "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER",
            "-DCMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH=OFF",
            "-DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF",
            "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
            "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
            f"-DCMAKE_SYSTEM_PREFIX_PATH={install};/usr",
            "-DPKG_CONFIG_EXECUTABLE=/usr/bin/false",
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            *options[name],
        ]
        fingerprint = json.dumps(
            {
                "command": command,
                "archive": ARCHIVES[name][1],
                "builder": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                "compiler_identity": compiler_identity,
            },
            sort_keys=True,
        )
        if (
            stamp.is_file()
            and stamp.read_text() == fingerprint
            and (install / "lib" / libraries[name]).is_file()
        ):
            continue
        # CMake preserves cached compiler and install-prefix values in its
        # build tree. Reconfigure only from a clean build directory whenever
        # any part of the command, archive, builder, or toolchain changes.
        shutil.rmtree(build, ignore_errors=True)
        environment = {
            key: value
            for key, value in os.environ.items()
            if key
            not in (
                "CPATH",
                "C_INCLUDE_PATH",
                "CPLUS_INCLUDE_PATH",
                "LIBRARY_PATH",
                "CFLAGS",
                "CXXFLAGS",
                "LDFLAGS",
                "SDKROOT",
                "MACOSX_DEPLOYMENT_TARGET",
                "IPHONEOS_DEPLOYMENT_TARGET",
            )
        }
        subprocess.run(command, check=True, env=environment)
        subprocess.run(
            [cmake, "--build", str(build), "--parallel", str(jobs)],
            check=True,
            env=environment,
        )
        subprocess.run([cmake, "--install", str(build)], check=True, env=environment)
        if not (install / "lib" / libraries[name]).is_file():
            raise RuntimeError(f"{name} did not install {libraries[name]}")
        stamp.write_text(fingerprint)
    return install
