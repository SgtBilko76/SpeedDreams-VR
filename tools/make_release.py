#!/usr/bin/env python3
"""make_release.py - the Linux/macOS twin of make_release.ps1.

Packs stage/SpeedDreamsVR into the APK as assets/gamedata.zip (leaving out the
downloadable tracks and cars - anything with a .revision file), builds the
release APK, and puts it in dist/ with a README beside it. The README text is
read out of make_release.ps1, so the two scripts never disagree about it.

    python3 tools/stage_data.py
    python3 tools/make_release.py [--with-assets]
"""
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGE = os.path.join(ROOT, "stage", "SpeedDreamsVR")
APP = os.path.join(ROOT, "android", "app")
ZIP_DIR = os.path.join(APP, "build", "bundled-assets")
ZIP_PATH = os.path.join(ZIP_DIR, "gamedata.zip")
APK = os.path.join(APP, "build", "outputs", "apk", "release", "app-release.apk")
DIST = os.path.join(ROOT, "dist")


def main():
    with_assets = "--with-assets" in sys.argv[1:]
    if not os.path.isdir(STAGE):
        sys.exit("stage dir missing: run python3 tools/stage_data.py")

    gradle = open(os.path.join(APP, "build.gradle"), encoding="utf-8").read()
    m = re.search(r"versionName\s+'([^']+)'", gradle)
    if not m:
        sys.exit("could not read versionName from android/app/build.gradle")
    version = m.group(1)
    name = f"SpeedDreamsVR-{version}"
    print(f"Building {name}")

    # --- the data zip ---------------------------------------------------------
    skip = set()
    if not with_assets:
        for d, _, files in os.walk(STAGE):
            if ".revision" in files:
                skip.add(d)

    def skipped(d):
        return any(d == s or d.startswith(s + os.sep) for s in skip)

    print(f"  data   packing" + (f", skipping {len(skip)} downloadable assets" if skip else "") + "...")
    os.makedirs(ZIP_DIR, exist_ok=True)
    if os.path.exists(ZIP_PATH):
        os.remove(ZIP_PATH)
    count = 0
    with zipfile.ZipFile(ZIP_PATH, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for d, dirs, files in os.walk(STAGE):
            dirs.sort()
            if skipped(d):
                dirs[:] = []
                continue
            for f in sorted(files):
                full = os.path.join(d, f)
                z.write(full, os.path.relpath(full, STAGE).replace(os.sep, "/"))
                count += 1
    print(f"  data   {os.path.getsize(ZIP_PATH) / 2**20:.1f} MB, {count:,} files -> gamedata.zip")

    # --- the APK --------------------------------------------------------------
    print("  apk    building...")
    gradlew = os.path.join(ROOT, "android", "gradlew")
    r = subprocess.run([gradlew, "assembleRelease", "-PsdvrBundleData=true", "--console=plain"],
                       cwd=os.path.join(ROOT, "android"))
    if r.returncode != 0:
        sys.exit(f"gradle failed ({r.returncode})")
    if not os.path.exists(APK):
        sys.exit(f"no APK at {APK}")

    os.makedirs(DIST, exist_ok=True)
    out = os.path.join(DIST, name + ".apk")
    shutil.copyfile(APK, out)
    print(f"  ->     {out} ({os.path.getsize(out) / 2**20:.1f} MB)")

    # --- the README, from make_release.ps1 -------------------------------------
    ps1 = open(os.path.join(ROOT, "tools", "make_release.ps1"), encoding="utf-8").read()
    m = re.search(r'\$readme = @"\r?\n(.*?)\r?\n"@', ps1, re.S)
    if m:
        text = m.group(1).replace("$version", version).replace("$name", name)
        with open(os.path.join(DIST, "README.txt"), "w", encoding="utf-8", newline="\r\n") as f:
            f.write(text + "\n")
        print("  ->     dist/README.txt")
    else:
        print("  !!     README text not found in make_release.ps1 - dist/README.txt not updated")


if __name__ == "__main__":
    main()
