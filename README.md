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

**Status: milestone 1, built and not yet heard.** The WASAPI sink is a loadable module
behind the C ABI, the passthrough graph runs on two threads with a lock-free ring between
them, and the negotiation of §6 — including the buffer-alignment retry on a fresh client —
is implemented. 51 tests pass on MSVC and on clang-cl, among them an end-to-end
bit-exactness check against a fake device: every byte the source produced, compared with
every byte that reached `commit`, with no hardware involved.

What has not happened yet is a tone coming out of a real endpoint. Exclusive mode silences
every other application on the device it takes, so that is a deliberate step rather than a
side effect of running the test suite.

```
mediaperch-probe devices     # opens nothing, disturbs nothing
mediaperch-probe negotiate   # offers every candidate to a real device
mediaperch-probe play        # takes the endpoint for the duration
```

- [docs/design.md](docs/design.md) — the shape of the program, and the two constraints that
  decided it.
- [docs/plan.md](docs/plan.md) — the plan of record: the language decision, the module ABI,
  the two audio paths, the HDR path, milestones, and the findings worth carrying forward.
- [docs/building.md](docs/building.md) — how to build it.
- [docs/devices.md](docs/devices.md) — what real hardware actually accepts, measured rather
  than inferred.

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
