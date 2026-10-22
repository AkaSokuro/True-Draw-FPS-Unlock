<h1 align="center">True Draw FPS Unlock</h1>

<p align="center">
  <b>More framerate options for Need for Speed: Hot Pursuit Remastered while maintaining the vanilla physics.</b>
</p>

<p align="center">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows-blue">
  <img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-green">
</p>

## Description

Hot Pursuit Remastered simulates its world in fixed 1/60 s steps. Raising the frame rate the usual way speeds up
or destabilises physics. True Draw FPS Unlock takes a different approach: it renders additional frames between
simulation steps and interpolates what is drawn. The game runs at 120, 144, 240 FPS or higher, while handling,
AI and timing remain identical to the original 60 FPS game.

**Interpolated:** camera and mirror, player car, traffic and police, wheels, driver, vehicle lights and effects,
props, particles, name and health tags, and menu animations.

**Not interpolated:** Most HUD elements, which still update at 60 Hz.

As with any frame interpolation, the displayed image trails the simulation by one physics step (about 17 ms).

> [!NOTE]
> This mod has been tested against recorded input on multiple runs. Keyboard and controller input recorded at 60 FPS
> was replayed at 60, 144 and uncapped (~100–200) FPS. In every run the player car's path matched the 60 FPS replay
> to within 1 cm for the full run the game itself stays deterministic, confirming the frame rate does not
> change the physics. Frame rates above ~200 FPS have not been tested this way.

## Requirements

- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (provided with the package).

## Installation

1. Copy the files in the archive into the game's directory beside `NFS11Remastered.exe`.
2. Start the game and open **Settings > Display > Max Framerate**.
3. Select 120, 144, 165, 240 or UNLIMITED.

## Configuration

Settings are read from `TrueDrawFPS.ini` at startup. The defaults are recommended for most players.

| Setting | Default | Description |
|---|---|---|
| `SmoothCamera` | `1` | Interpolate the camera. |
| `SmoothObjects` | `1` | Interpolate cars, traffic, police, driver, and vehicle lights and effects. |
| `SmoothWheels` | `1` | Interpolate player wheel spin, steering and suspension. |
| `SmoothParticles` | `1` | Interpolate smoke, sparks, nitrous flames and trails. |
| `SmoothUI` | `1` | Update menu and HUD animations and the career map every frame. |
| `Lookahead` | `0` | Draw slightly ahead to make steering feel more responsive (`0` to `1`; `0.5` is a good start). Higher values can cause brief jumps on crashes. |
| `FrameLatency` | `1` | Lower input delay when the GPU is the bottleneck (`1` = lowest delay, `0` = game default). |
| `UpdateGates` | `7FFFFFF` | Keeps per-frame game logic at 60 Hz. Leave unchanged; `0` disables it. |
| `Log` | `0` | Write `TrueDrawFPS.log` for troubleshooting. |
| `InputReplay` | `0` | Testing only: `1` records input in an offline event, `2` replays it. |
| `Probe` | `0` | Debug only. Leave at `0`. |

## Reporting a problem

1. Set `Log=1` in `TrueDrawFPS.ini`.
2. Reproduce the issue.
3. Open an issue and attach `TrueDrawFPS.log`, found next to the plugin.

The log lists which patches were applied and, if the game crashed, a crash report.

> [!IMPORTANT]
> Any issue that doesn’t show actual proof or attach TrueDrawFPS.log will be ignored. Proof means something that
can be checked, such as a video, screenshots, or exact steps to reproduce the problem.

## Building

Requires Visual Studio 2022 with the MSVC x64 toolset.

```bat
build.bat
```

The plugin is written to `build\TrueDrawFPS.asi`. To rebuild and run the self-tests:

```bat
tests\run.bat
```

## Repository layout

```
src/          Plugin core: startup, configuration, frame pacing, patching, crash reporting
  blend/      Interpolation of everything drawn between physics steps
tests/        Self-tests and the captured game data they use
build/        Build output and the default TrueDrawFPS.ini
build.bat     Build script
```

## License

Released under the [MIT License](LICENSE).
