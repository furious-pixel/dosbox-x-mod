# Experimental DOSBox-X fork with mod support

An **experimental fork** of [DOSBox-X](https://github.com/joncampbell123/dosbox-x)
to add in-process Python mod support and an OpenGL presentation path that a mod
renderer can take over.

This is not the upstream project and is not an official DOSBox-X release.
Vanilla emulator bugs belong on
[joncampbell123/dosbox-x](https://github.com/joncampbell123/dosbox-x/issues).

## What this fork adds

- Python mods, off by default. `-python` starts Python from `./.venv` in the
  working directory. `-moddir <path>` loads mods from that directory.
- OpenGL presentation views when a mod renderer is present:
  - **game-only** — native VGA output
  - **mod-only** — mod compositor output
  - **side-by-side** — native and mod panes for comparison
- Native CRT `.glslp` presets apply only to native VGA output. They enhance
  the game's own rendering. The mod renderer is a replacement OpenGL path and
  does not use those shaders.
- `[render]` config: `mod renderer start view`,
  `mod renderer comparison resolution`, `mod renderer target fps`.

### New keyboard shortcuts

Presentation:

- Ctrl+/ — toggle game-only and mod-only
- Ctrl+Shift+/ — side-by-side comparison
- Ctrl+Alt+/ — side-by-side with scene suppression allowed

Shader presets (native CRT `.glslp`, native output only):

- Ctrl+[ — previous preset
- Ctrl+] — next preset

## Support

- **Now:** Windows 64-bit, Visual Studio SDL2.
- **Not yet:** macOS (planned, arm64). Python loading is Windows-only.
  Linux Autotools should compile the emulator without the Python host.

See [BUILD.md](BUILD.md) to compile. This tree is GPL-2; see [COPYING](COPYING)
and [CREDITS.md](CREDITS.md). CRT preset licenses are in
[contrib/glshaders/NOTICE](contrib/glshaders/NOTICE).

## Upstream DOSBox-X

DOSBox-X itself is a cross-platform DOS emulator. Documentation, Wiki, Discord,
and official binaries:

- https://github.com/joncampbell123/dosbox-x
- https://dosbox-x.com
- https://dosbox-x.com/wiki
