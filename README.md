# Camera Shake

A free camera-shake plug-in for **VEGAS Pro**, built on the open [OpenFX](https://openeffects.org/)
standard.

Drop it on a clip and the image shakes — random hand-held motion, regular wave wobble, zoom
and rotation shake, optional per-channel RGB separation and motion blur.

## This is an alternative to Sapphire's S_Shake, not a clone of it

If you have been looking for S_Shake and do not want to buy Sapphire, this does the same
*job*. It is **not** a replication of it:

- The internals are an independent implementation. The random motion, interpolation and
  sampling are my own, so **the output will not match Sapphire frame for frame** — the same
  settings will not give you the same shake.
- Parameter names and default values deliberately follow the [documented S_Shake
  controls](https://borisfx.com/documentation/sapphire/ofx/shake/), so if you already know
  S_Shake you will feel at home and your settings will translate roughly. "Roughly" is the
  operative word.
- Several S_Shake features are missing on purpose — see [Differences](#differences-from-s_shake).

Sapphire and S_Shake are trademarks of Boris FX. This project is not affiliated with,
endorsed by, or derived from Boris FX or Sapphire.

## Install

### Option A — download the prebuilt plug-in (easiest)

1. Download `CameraShake.ofx.bundle.zip` from the
   [Releases](../../releases) page and unzip it.
2. Copy the whole **`CameraShake.ofx.bundle`** folder into your VEGAS OFX folder — you will
   need to confirm an administrator prompt:

   | VEGAS version | Folder |
   | --- | --- |
   | VEGAS Pro 2026 (Boris FX) | `C:\Program Files\BorisFX\Vegas Pro 2026\OFX Video Plug-Ins\` |
   | VEGAS Pro 14–22 (MAGIX) | `C:\Program Files\VEGAS\VEGAS Pro <version>\OFX Video Plug-Ins\` |

   Keep the folder structure intact — it must end up as
   `...\OFX Video Plug-Ins\CameraShake.ofx.bundle\Contents\Win64\CameraShake.ofx`.
3. **Restart VEGAS.** It only scans for plug-ins at startup.
4. The effect appears in **Video FX → Distort → Camera Shake**.

There is nothing else to install — the plug-in is self-contained and needs no Visual C++
redistributable.

### Option B — build it yourself

See [Building](#building).

## Using it

Apply it to an event, track or the project, then just press play — the defaults are a strong
hand-held shake, so you should see it immediately without touching anything.

The controls, grouped as they appear in VEGAS:

| Group | Controls |
| --- | --- |
| **Main** | `Style` (Normal / Twitchy / Jumpy), `Amplitude`, `Frequency`, `Phase`, `Stillness`, `Twitch Frequency`, `Drift`, `Center Bias`, `Z Dist`, `Auto Zoom`, `Zoom`, `Motion Blur`, `Mo Blur Length`, `Mo Blur Samples`, `Seed`, `Wrap X`, `Wrap Y` |
| **X / Y / Z / Tilt Shake** | `Rand Amp`, `Rand Freq`, `Wave Amp`, `Wave Freq`, `Phase` for each axis — X and Y are position, Z is zoom, Tilt is rotation |
| **Channels** | `Red/Green/Blue Amplitude`, `Red/Green/Blue Phase`, `RGB Randomness`, `RGB Frequency` |
| **Crop Input** | `Crop Top / Bottom / Left / Right` |

The ones worth knowing:

- **`Amplitude`** scales the whole effect — the fastest way to dial the shake up or down.
- **`Frequency`** is the speed. Lower it for a slow drifting camera, raise it for a jitter.
- **`Style`** changes the character: *Normal* shakes continuously, *Twitchy* holds still and
  then snaps (`Stillness`, `Twitch Frequency`), *Jumpy* cuts between positions (`Drift`,
  `Center Bias`).
- **`Seed`** picks a different random shake from the same settings. Change it if two clips
  shake identically.
- **`Motion Blur`** blurs along the motion. Looks great, costs render time — raise
  `Mo Blur Samples` if it looks steppy.

Units: X/Y/Z amplitudes are fractions of the frame width, Tilt amplitudes are in degrees,
all `Phase` values are time shifts in seconds, and `Wave Freq` is in cycles per second.

### Edges and `Auto Zoom`

An OpenFX effect only receives the finished frame at project resolution — it cannot reach
the original, higher-resolution media. So when the shake moves the image, there is no extra
material to reveal and the edge of frame would otherwise go empty or mirrored.

**`Auto Zoom` is on by default** and solves this the way camera shake is normally done: it
zooms in by just enough to keep the frame covered however far the shake pushes the image.
It is worked out from your settings and stays constant for the whole clip, so it never reads
as the image breathing. At the defaults on 1920×1080 that is a 110% zoom.

The cost is a thin sliver of the border and a slight softening from the upscale. If you want
to control framing yourself, turn `Auto Zoom` off; `Zoom` always multiplies on top.

## Differences from S_Shake

- **No Mocha tracking.** The entire Mocha section (Mocha Project, Blur/Dilate/Resize Mocha,
  and so on) is absent. Mocha is proprietary.
- **No Mask input.** VEGAS cannot route a second clip into a Video FX chain, so a mask input
  would be unusable there — the `Mask Use` / `Blur Mask` / `Invert Mask` controls are gone
  with it.
- **`Auto Zoom` and `Zoom` are additions**, to deal with the frame-edge problem above.
- **`Mo Blur Samples` is an addition.** Sapphire chooses its sample count internally.
- **Amplitude strength is approximate.** Sapphire's internal amplitude scaling is not
  published, so the mapping here is my own estimate. If the shake feels stronger or weaker
  than you expect at a given `Amplitude`, that is why.

## Compatibility

Built and tested against **VEGAS Pro 2026** (64-bit, Windows). It is a standard OpenFX
plug-in, so it should also load in older VEGAS versions with OFX support (14 and up) and in
other OFX hosts such as DaVinci Resolve — neither is tested, and reports are welcome.

## Building

Needs **Visual Studio** with the *Desktop development with C++* workload (which provides the
MSVC toolset and a Windows SDK). No other dependencies — the OpenFX SDK is vendored in
`vendor/openfx`.

```bat
build.bat          :: incremental
build.bat clean    :: wipe build\ first
```

That produces `build\CameraShake.ofx.bundle` and runs the test suite; the build fails if any
test fails. To install it into VEGAS, from an **administrator** PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1
```

It finds your VEGAS OFX folder automatically. Use `-VegasOfxDir "<path>"` to override, or
`-Uninstall` to remove it.

### Project layout

```
src/ShakeCore.h       shake motion + pixel sampling, no OFX types so it is unit-testable
src/CameraShake.cpp   OFX plumbing: parameters, render dispatch, multithreading
test/core_test.cpp    unit tests for the motion and sampling maths
test/smoke.cpp        loads the built .ofx and validates its descriptor
vendor/openfx/        OpenFX SDK (BSD-3-Clause)
```

### How the shake works

Each axis (X, Y, Z, Tilt) combines a random term and a regular wave:

```
value = RandAmp * noise(t * Frequency * RandFreq) + WaveAmp * sin(2π * t * WaveFreq)
```

The noise is Catmull-Rom-interpolated value noise over a hashed integer lattice, clamped to
[-1, 1]. The four axis values become a single affine warp (offset, zoom, rotation) that is
sampled bilinearly. *Twitchy* warps time so it stands still then catches up; *Jumpy* swaps
the smooth noise for random targets blended by `Drift`. Channel separation builds a separate
warp per RGB channel, and motion blur averages several warps across the shutter interval.

## Troubleshooting

**It does not appear in Video FX.** Restart VEGAS — it only scans at startup. Then check the
file really is at
`...\OFX Video Plug-Ins\CameraShake.ofx.bundle\Contents\Win64\CameraShake.ofx`.

**I updated it and nothing changed.** Remove and re-add the effect. VEGAS caches each
effect instance's settings, so an instance created with an older build can keep them.

**I can still see an edge.** Raise `Zoom` slightly. If `Auto Zoom` is on and you still see
an edge, that is a bug — please open an issue with your settings.

## Licence

[MIT](LICENSE). The bundled OpenFX SDK in `vendor/openfx` is BSD-3-Clause, © OpenFX and
contributors.
