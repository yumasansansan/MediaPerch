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

**Status: a FLAC reaches a real device with every byte intact, proved by hash.**

```
decoded    44100 Hz / 2 ch / S16, 352800 frames, 8.00 s
           6ad3ba5878b1de9d97e7867553050a60bffb1ea52e38522eac6bd9f07812366a
expected   6ad3ba5878b1de9d97e7867553050a60bffb1ea52e38522eac6bd9f07812366a
committed  6ad3ba5878b1de9d97e7867553050a60bffb1ea52e38522eac6bd9f07812366a   <- handed to ReleaseBuffer

BIT-EXACT to the device buffer: 1411200 bytes, not one byte altered.
```

That is one hash for the decoder's output, for FFmpeg's decode of the same file, and for the
bytes handed to `IAudioRenderClient::ReleaseBuffer` on a real endpoint in WASAPI exclusive
mode. `ReleaseBuffer` is where the claim stops and the driver begins, and
[docs/devices.md](docs/devices.md) records the measurement that established why nothing on
an ordinary machine can see past it — including why a virtual cable, the obvious instrument,
turns out not to be one.

Working: three decoders chosen by probe — libFLAC, two single headers, and Media Foundation
— all hash-identical to the reference on everything they read, up to 32-bit at 1,048,575 Hz;
format negotiation against real drivers; the passthrough graph on two threads; WASAPI
exclusive down to a 2 ms period; 768 kHz / 32-bit on a USB DAC. 62 tests plus libFuzzer
targets, on MSVC and clang-cl.

```
mediaperch-probe devices     # opens nothing, disturbs nothing
mediaperch-probe decode      # decode a file and hash it. No device involved
mediaperch-probe negotiate   # offer every candidate format to a real device
mediaperch-probe play        # a test tone. Takes the endpoint for the duration
mediaperch-probe verify      # a file, hashed at the device boundary
```

- [docs/design.md](docs/design.md) — the shape of the program, and the two constraints that
  decided it.
- [docs/plan.md](docs/plan.md) — the plan of record: the language decision, the module ABI,
  the two audio paths, the HDR path, milestones, and the findings worth carrying forward.
- [docs/building.md](docs/building.md) — how to build it.
- [docs/devices.md](docs/devices.md) — what real hardware actually accepts, measured rather
  than inferred.
- [docs/formats.md](docs/formats.md) — what the decoders actually produce, hashed against
  FFmpeg.

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
