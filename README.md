<img width="800" height="450" alt="speed-dreams-20-splash" src="https://github.com/user-attachments/assets/a8886516-eb6b-4012-8361-5c5cb174e9e9" />

<img width="1024" height="768" alt="collage" src="https://github.com/user-attachments/assets/c3bc830d-822a-415c-9c5e-75ac61d7a95c" />

Speed Dreams VR 0.9.3-beta
========================

[![Sponsor](https://img.shields.io/badge/Sponsor-SgtBilko76-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/SgtBilko76)


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

    adb install -r SpeedDreamsVR-0.9.3-beta.apk

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

![Speed Dreams VR controller mapping](docs/controls.webp)

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
