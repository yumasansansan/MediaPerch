<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# MediaPerch

A music and media player that takes the short path: when nothing needs to be done to the
samples, they go from the file to WASAPI exclusive mode with one `memcpy` and no
arithmetic. When something does need doing, that is a different code path, and the player
says which one it used.

Video follows the same principle in a different direction: HDR is handled by the tone
mappers Windows already ships, so content looks the way it does in the system's own player
— including on an SDR display — rather than the way one more hand-written colour pipeline
decided it should.

Not started yet. This repository currently holds the design.

- [docs/design.md](docs/design.md) — the shape of the program, and the two constraints that
  decided it.
- [docs/plan.md](docs/plan.md) — the plan of record: the language split, the module ABI, the
  two audio paths, the HDR path, milestones, and the findings worth carrying forward.

## The shape of it, in one paragraph

A headless engine process (`src/core`, portable, no OS headers; plus a platform head) loads
decoders, sinks, DSP and the video presenter at run time as shared libraries behind a plain
C ABI — so a module can be replaced, or written by somebody else. Shells are separate
processes and are optional: a small always-present CLI, and a WinUI 3 window that can be
started on demand and killed without playback noticing. It is the same separation as
[DragonPerch](https://github.com/yumasansansan/DragonPerch), applied to a problem with more
moving parts.

## Licence

`GPL-3.0-or-later`.
