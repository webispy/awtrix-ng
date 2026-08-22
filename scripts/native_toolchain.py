
import os
import shutil
import subprocess
import sys

Import("env")

if sys.platform == "win32":
    if shutil.which("g++") is None:
        kit = os.environ.get("W64DEVKIT", r"D:\tools\w64devkit")
        bindir = os.path.join(kit, "bin")
        if os.path.exists(os.path.join(bindir, "g++.exe")):
            env.PrependENVPath("PATH", bindir)
        else:
            print("native_toolchain.py: no g++ in PATH and no w64devkit at %s" % bindir)

    env.Append(LINKFLAGS=["-static-libstdc++", "-static-libgcc"])

    env.Append(LIBS=["ws2_32"])

elif sys.platform == "darwin":
    # Recent Command Line Tools keep libc++ headers in the SDK but clang's compatibility `g++`
    # driver still probes the old CLT-wide include directory first. Ask xcrun for the active SDK
    # instead of baking a macOS version into the project.
    try:
        sdk = subprocess.check_output(
            ["xcrun", "--show-sdk-path"], text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        sdk = ""
    libcxx = os.path.join(sdk, "usr", "include", "c++", "v1")
    if os.path.isdir(libcxx):
        env.Append(CXXFLAGS=["-isystem", libcxx])
