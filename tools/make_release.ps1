# Build a self-contained release APK: the app with the game data inside it.
#
# The data is packed as assets/gamedata.zip and unpacked to /sdcard/SpeedDreamsVR
# on first run (SDVRActivity.unpackGameData). It cannot simply live inside the
# app: the game reaches its data through ordinary paths, and keeping it outside
# also means it survives reinstalling, and that cars and tracks fetched later by
# the in-game download manager land in the same tree as the ones that shipped.
#
# What goes in is the 230 MB Speed Dreams itself ships - one track, one car, and
# everything that is not a track or a car: menus, fonts, sounds, drivers, car
# categories. That is a complete, working game. The other 74 tracks and 90 cars
# are published separately and the player fetches the ones they want from
# Options -> Downloads in the headset.
#
# tools\fetch_assets.py stages those downloads too, so anything with a .revision
# file in it came from there and is left out. -WithAssets packs the lot instead,
# which does not fit: an APK is a zip that Android's package parser reads without
# zip64, so it tops out near 4 GB and the full set is 5.6 GB. It exists for
# packing some of them by hand, not all.
#
#   tools\make_release.ps1              -> dist\SpeedDreamsVR-<version>.apk
#   tools\make_release.ps1 -SkipBuild   -> repack without recompiling
#
# Run tools\stage_data.py first; this packs what is in stage\.

param(
    # Kept for the callers that pass it; it no longer skips packaging, only the
    # waiting - gradle does not rebuild what has not changed.
    [switch]$SkipBuild,
    [switch]$WithAssets
)

$ErrorActionPreference = "Stop"
$root    = Split-Path -Parent $PSScriptRoot
$stage   = Join-Path $root "stage\SpeedDreamsVR"
$appDir  = Join-Path $root "android\app"
$zipDir  = Join-Path $appDir "build\bundled-assets"
$zipPath = Join-Path $zipDir "gamedata.zip"
$apk     = Join-Path $appDir "build\outputs\apk\release\app-release.apk"

if (-not (Test-Path $stage)) { throw "stage dir missing: run python tools\stage_data.py" }

$gradle  = Get-Content (Join-Path $appDir "build.gradle") -Raw
$version = ([regex]::Match($gradle, "versionName\s+'([^']+)'")).Groups[1].Value
if (-not $version) { throw "could not read versionName from android\app\build.gradle" }

$name = "SpeedDreamsVR-$version"
Write-Host "Building $name"

# --- the data zip ------------------------------------------------------------
# Written straight from stage\, skipping the downloadable assets, so nothing is
# copied to a staging folder first.
$skip = @()
if (-not $WithAssets) {
    $skip = Get-ChildItem -Recurse -Force -Filter ".revision" -File $stage |
            ForEach-Object { $_.DirectoryName }
}
Write-Host ("  data   packing{0}..." -f $(if ($skip.Count) { ", skipping $($skip.Count) downloadable assets" } else { "" }))

New-Item -ItemType Directory -Force -Path $zipDir | Out-Null
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$stageLen = $stage.Length
$archive = [System.IO.Compression.ZipFile]::Open($zipPath, 'Create')
try {
    $files = 0
    foreach ($file in Get-ChildItem -Recurse -File $stage) {
        $dir = $file.DirectoryName
        $isAsset = $false
        foreach ($s in $skip) {
            if ($dir -eq $s -or $dir.StartsWith($s + [IO.Path]::DirectorySeparatorChar)) { $isAsset = $true; break }
        }
        if ($isAsset) { continue }

        # Zip entries use forward slashes and are relative to the data dir.
        $entry = $file.FullName.Substring($stageLen).TrimStart('\').Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $file.FullName, $entry, 'Optimal') | Out-Null
        $files++
    }
} finally {
    $archive.Dispose()
}
Write-Host ("  data   {0:N1} MB, {1:N0} files -> {2}" -f ((Get-Item $zipPath).Length / 1MB), $files, "gamedata.zip")

# --- the APK -----------------------------------------------------------------
# Always, even with -SkipBuild. The flag means "do not wait for the native code
# to rebuild", and gradle's own up-to-date checks give you that for free - but
# skipping assembleRelease altogether meant the freshly packed gamedata.zip was
# never put into an APK, and the old one was copied to dist and installed. A
# data-only change then appeared to ship and did nothing, which is a hard thing
# to see from the outside: the build says it packed 4,349 files and the headset
# carries on with the ones it had.
Write-Host "  apk    building..."
Push-Location (Join-Path $root "android")
try {
    # Gradle writes warnings to stderr, and under ErrorActionPreference=Stop
    # PowerShell turns any native stderr line into a terminating error - so a
    # harmless SDK version notice would read as a failed build. The exit code
    # is the thing that actually says.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & .\gradlew.bat assembleRelease -PsdvrBundleData=true 2>&1 |
            ForEach-Object { Write-Host "         $_" }
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($LASTEXITCODE -ne 0) { throw "gradle failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
if (-not (Test-Path $apk)) { throw "no APK at $apk" }

$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$out = Join-Path $dist "$name.apk"
Copy-Item $apk $out -Force

Write-Host ("  ->     {0} ({1:N1} MB)" -f $out, ((Get-Item $out).Length / 1MB))

# --- a note to go beside it --------------------------------------------------
$readme = @"
Speed Dreams VR $version
========================

Speed Dreams 2.4 as a standalone headset application: stereo rendering
through OpenXR, a curved menu panel, and the full game in one APK.

Quest 2, Quest 3, Quest Pro, Pico 4 and Pico Neo 3. About 500 MB free on the
headset.

One APK covers all of them. The game asks the headset's OpenXR runtime what it
supports and configures itself from the answer, so there is no separate Pico
download and nothing to choose at install time.


Installing
----------

Sideload it, however you normally do - SideQuest, or:

    adb install -r $name.apk

The game appears in the headset's library under Unknown Sources.

On a Pico the adb line is the same; SideQuest is a Quest tool, so use adb or
whichever sideloader you already use. Developer mode has to be on for adb to
see the headset, on either make.

The first launch takes an extra half minute or so: the game data is unpacked
out of the APK to /sdcard/SpeedDreamsVR, where it stays. Later versions reuse
that directory, so an update is just the new APK.


More cars and tracks
--------------------

This carries what Speed Dreams itself ships: one track and one car, which is
enough to race. Another 74 tracks and 90 cars are published separately - get
them from inside the headset, in Options -> Downloads, over wifi. Pick the ones
you want; the whole set is about 5.4 GB.


Controls
--------

    Right trigger        throttle          Left trigger      brake
    Left stick           steer             Left stick click  reverse gear
    Right grip           shift up          Left grip         shift down
    A                    ABS / confirm     X                 ASR
    Right stick click    rear-view mirror
    Y                    next camera
    Meta button (right)  hold to recentre the view and the menu panel

If the view or the menu panel is not in front of you, hold the Meta button on
the right controller (Home button on Pico), as in other VR apps. The panel is anchored
to the room, not to your head, so it stays where it was when you last moved.

Pico controllers carry the same buttons in the same places, so the table above
reads the same on a Pico 4. The driving controls are a virtual joystick either
way, so anything that does not suit you can be rebound in Options -> Players.


Settings
--------

/sdcard/SpeedDreamsVR/vr.cfg holds the things worth trying without a new build:
eye buffer scale, refresh rate, antialiasing, where the mirror sits, and the
steering curve. It is commented; edit it and restart the app. Updates leave it
alone once it is there.

Everything else is in the game's own Options menu. If a track will not hold
frame rate, turn the sky dome off first - with it on, nothing is distance
culled and the whole track is drawn every frame, twice.


Online races
------------

There is no fixed server to race on: anyone can host, and anyone can join.

    Host   Race select -> Online Race, pick "Host a race" in the list next to
           it, choose the track, the race settings and the drivers (yourself
           and any robots), then wait in the lobby. The line at the top says
           when the race is listed online; start it when people are in.
    Join   Race select -> Online Race, pick "Join a race". The open races are
           listed; pick one to go to its lobby, tick Ready.

Robots you add when hosting race with everyone; each player who joins adds a
car. The best lap of each player goes to the online leaderboard (main menu ->
Leaderboard).

Headsets cannot reach each other directly from behind home routers, so a
relay server in the middle lists the races and passes the traffic along. If
the main relay is down, the game uses the backup on its own.


Known limits
------------

- Practice and qualifying run one car at a time. That is how the game works,
  not a fault of the port: use Quick Race or a championship to race opponents.

- The Pico build has not been run on a Pico. It is built and wired up - the
  manifest, the controller bindings and the runtime detection are all in - but
  every test so far has been on a Quest 3, so treat the first Pico launch as
  unproven. If it misbehaves, the useful line is

      adb logcat -s SpeedDreamsVR:V TBXR:V

  which prints the runtime it found and the controller profile it bound.
"@
Set-Content -Path (Join-Path $dist "README.txt") -Value $readme -Encoding UTF8
