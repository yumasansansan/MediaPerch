<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# How MediaPerch is put together

The shape of the program, and the two constraints that decided it. For what is built next
and why, see [the plan of record](plan.md).

## The two things that shape the design

### 1. The sink decides the format, and it is allowed to say no

Almost every media player is built as *decode → convert → output*, because a shared-mode
mixer accepts anything and quietly resamples it. **WASAPI exclusive mode has no mixer.**
The endpoint accepts exactly one format at a time; `IAudioClient::IsFormatSupported`
answers only about that one format, and drivers are known to answer it optimistically —
the only trustworthy test is `IAudioClient::Initialize` itself.

So negotiation runs **backwards, from the sink towards the source**, and it is a step that
can *fail*. A player that cannot fail here is a player that silently resamples, which is
the whole thing this project exists not to do. Every module boundary therefore carries a
negotiation call as well as a data call, and "this device will not play this file
unaltered" is a first-class, user-visible outcome — not an error to paper over.

### 2. The render thread may not allocate, lock, or fault

The buffer-servicing thread runs under MMCSS `Pro Audio` at a device period that can be
under 3 ms. Everything it touches must be wait-free: no heap, no mutex, no file, no COM
call that can block, no exception, no Rust panic. That single rule decides more of the
architecture than any taste question:

- the module ABI is plain C with caller-owned buffers, because a C++ exception or a Rust
  panic crossing a DLL boundary on that thread is unrecoverable;
- modules are loaded and unloaded only at **graph rebuild points**, never while a stream
  is running, because unloading a DLL whose code an RT thread might be executing is not a
  race you can win;
- the bit-exact path and the DSP path are two separate graphs, not one graph with a
  branch, because a branch is a thing that can be wrong at 3 ms.

## What follows from that

| | Passthrough (default) | Processed |
|---|---|---|
| Bus format | the device's, verbatim | canonical f32, deinterleaved |
| Sample touching | `memcpy`, or one byte move if the container differs | gain, resample, convolution, dither |
| Volume | `IAudioEndpointVolume`, or none | in the graph |
| Chosen | when the file's format survives negotiation | when the user asks, or negotiation failed |
| Code path | `src/core/passthrough.*` | `src/core/processed.*` |

They share the ring buffer, the sink module and the clock. Nothing else. The passthrough
graph contains no floating-point arithmetic on sample data at all, which is a property a
test can assert and a review can see.

**The render thread is the same code in both**, and that is the point of having two graphs
rather than one with a branch. `ProcessedGraph::render_loop` and
`PassthroughGraph::render_loop` both wait on the device, copy out of the ring and commit;
neither has any idea whether a conversion happened. The conversion runs on the decode
thread, in `Converter`, exactly where Path A's repack runs. The thread with the 3 ms
deadline cannot take a wrong turn because it has no turn to take.

### Why Path B had to exist

Every lossy decoder here reports `F32`, because that is what a lossy codec's output *is*.
The endpoint this was written on refuses `F32` in exclusive mode:

```
$ mediaperch-probe negotiate --rate 44100 --float --channels 2
1 exact          44100 Hz / 2 ch / F32              -> format refused
2 exact   +mask  44100 Hz / 2 ch / F32 / mask 0x3   -> format refused
```

Without Path B, MP3, AAC, Vorbis and Opus decode and hash on that machine and cannot be
played on it. With it, and only when asked:

```
$ mediaperch-probe negotiate --rate 44100 --float --channels 2 --path auto
3 CONVERT        44100 Hz / 2 ch / S32              -> ACCEPTED, buffer 132 frames
```

**What Path B does is narrow on purpose.** Sample type, a gain, dither and noise shaping.
It does **not** resample and does **not** change the channel count: both are real
conversions needing real implementations, and a bad one here would be worse than the
refusal it replaced, so negotiation still refuses a device that wants a different rate.

### The intermediate is binary64, and the read side is exact

Every sample is read into a `double`, and that is not a precaution — it is a guarantee that
can be stated:

- **float32 → double** is a widening conversion and is always exact.
- **any integer container here → double** is exact: binary64 has a 53-bit significand and
  the widest container is 32 bits.
- **the normalising divide by 2^(bits−1)** is exact, because it is a power of two: it
  changes the exponent and leaves the significand alone.

So nothing happens on the way *in*, and `tests/convert_test.cpp` proves it by taking every
integer type out to `f64` and back and comparing bytes. The only rounding in the whole
conversion is the one at the bottom of it.

**`double` is not narrowed on the way out either, except to the format that was asked for.**
A 32-bit integer device is written from the `double` directly — there is no float32 anywhere
in that path, and the cast to `float` exists on exactly one branch, the one whose
destination *is* `f32`. An `f64` device, if one existed, would take the source unaltered
through Path A: `f64` is a sample type in the ABI, `classify` calls an `f64` source on an
`f64` device `exact`, and that is a `memcpy`.

**And nothing useful can be done on the way in.** Widening is exact, so there is no
information to recover and nothing to improve: what is sometimes done at this point —
"bit-depth expansion", guessing at the dither that was removed — is speculative
reconstruction rather than fidelity, and this program does not do it. The processing that
is worth having lives at the *other* end, where bits are actually being discarded, which is
what the rest of this section is about. The one genuine exception is **de-emphasis**, which
is not a width question at all: it is a defined format property that a source either carries
or does not.

### Dither

Rounding alone is not a small error, it is a *correlated* one: the residue is a function of
the signal, so it reaches the ear as harmonics that were not in the music. Dither makes the
residue depend on a random value instead — half a bit of noise floor bought with the
distortion removed.

| `--dither` | What it is | When |
|---|---|---|
| `none` | rounding | a measurement, and the only setting where two runs give identical bytes |
| `rectangular` | one uniform draw, ±½ LSB | decorrelates the error but leaves its *variance* following the signal, heard as a floor that breathes. Here because it is what everybody reaches for first |
| `triangular` **(default)** | two independent draws | decorrelates the error and its variance both, at 4.77 dB more noise. The standard answer |
| `highpass` | one draw differenced with the last | the same triangular distribution, with a first-order highpass spectrum instead of a white one. Same total power, moved off the midband |
| `gaussian` | σ = ½ LSB, Box–Muller | the distribution a sum of independent sources tends to, and so the right shape when something downstream will quantise again |

Every one of them is seeded rather than drawn from a clock: two decodes of one file have to
produce the same bytes, or a difference between them means nothing.

### Noise shaping

The quantiser's error is unavoidable; *where in the spectrum it lands* is not. Feeding it
back through a filter moves it out of the band the ear is sensitive to. Total noise power
goes up and audible noise goes down.

`--shape N` gives a noise transfer function of exactly **(1 − z⁻¹)ᴺ**. The coefficients are
the binomial expansion, `h[k] = (−1)^(k+1) C(N,k)`, **derived rather than tabulated** — there
is nothing here to mistype.

**Order is not quality**, and this is the part to read before turning it up. Each order
tilts the noise further out of the midband and each order also multiplies the total: a
9th-order binomial shaper puts far more noise above 15 kHz than it removes below. Orders 1
to 3 are useful. Past that, this is the mechanism rather than a recommendation.

#### The measured curves

`--shape shibata[:N]` selects one of **67 ATH-weighted curves over 8 sample rates**,
transcribed by `tools/gen_shaper_tables.py` from
[SSRC](https://github.com/shibatch/ssrc) — Naoki Shibata's, under the Boost Software
Licence 1.0, which is GPL-3 compatible. They are *measured against the absolute threshold
of hearing* rather than derived, which is also why they are indexed by sample rate: where
the ear stops listening is a fixed frequency, and where that lands in the spectrum depends
on how fast the samples go past. Intensity 0 is gentle and 16 is aggressive; 4 to 40 taps.

**A curve is never substituted across rates.** A shaper fitted for 44.1 kHz applied to a
96 kHz stream puts its noise an octave from where it belongs, so a rate with no curve gets
no shaping and the report says so rather than sounding plausible.

The transcription is checked rather than trusted, and it earned that: the first run of the
generator matched 65 of the 67 entries — two carry a trailing comment between their closing
braces — and wrote the file **without a word**. It now counts the entries in the source
before it counts the ones it parsed, and refuses if they differ. `tests/convert_test.cpp`
holds spot values read out of the source by eye.

`ReSampler` (LGPL-2.1, also compatible) is the other implementation worth reading here and
its curves are not transcribed yet.

### Choosing the path

Which graph a stream may take is a setting, not only an inference. `--path` on the probe
today, a config key when there is a config:

| `--path` | Allows | Graph |
|---|---|---|
| `bitexact` **(default)** | a `memcpy` or a container repack | A |
| `exact` | a `memcpy`, and not even a repack | A |
| `auto` | bit-exact if the device takes it, converted if it does not | A, then B |
| `processed` | any format the device takes, and Path B regardless | B |

The default is the one that can **refuse**, and §2 is why: a player that cannot fail here
is a player that converts quietly. `auto` is how somebody says "just play it", and it
prints `PROCESSED -- the samples are changed` with the gain and the dither setting when it
takes that option.

`processed` and `auto` are also not the same thing when both would work. A 16-bit file on a
16-bit device at a gain of 0.5 is `Fidelity::exact` — the *format* relationship is
untouched — and it is emphatically Path B, because a gain is a change no format comparison
can see. `use_processed` is that distinction and `classify` is the other one.

`exact` shortens the candidate list, which matters more than it sounds: every candidate is
a real `IAudioClient::Initialize`, and in exclusive mode each one takes the device away
from whatever else is using it.

## Layout

```
src/core/            portable. No OS headers — CI builds this alone to keep it that way.
                     The graph, negotiation, the ring, the clock model, the playlist,
                     the module registry, the config schema.
src/win/             Windows head: MMDevice/WASAPI, Media Foundation, D3D11,
                     DirectComposition, MMCSS, paths, logging, the IPC server.
src/linux/           Linux head. Not started. The core is shaped so that it can be.
include/mediaperch/  the module ABI. Pure C, one header, versioned. The only file a
                     third-party module has to read.
modules/             everything that can be loaded and unloaded at runtime.
  decode_native/     FLAC and WAV from two single headers. No build system at all: an
                     install with nothing else on disk still plays music.
  decode_flac/       libFLAC, the Xiph reference, as a submodule. Outranks the above
                     wherever it is installed, and checks the file\'s own MD5.
  decode_mp3/        MP3 through dr_mp3, which is already in external/dr_libs. It
                     exists because Media Foundation does not implement gapless
                     metadata and starts every MP3 36 ms late; dr_mp3 reads the
                     LAME tag and costs no new dependency.

  decode_alac/       ALAC, written here: the codec and the slice of MP4 that finds
                     its packets. No submodule, no runtime library, no OS codec --
                     the reference implementation is the specification and has
                     been unmaintained since 2011, so it was read, not linked.

  decode_aac/        AAC-LC, written here as well: the codec, the ADTS framing and the
                     same slice of MP4. Not an unmaintained reference this time --
                     four maintained libraries were measured and each produced the
                     wrong thing rather than a wrong sound. SBR and PS are refused
                     and go to decode_ffmpeg.

  decode_ogg/        libvorbis and libopus, the Xiph reference decoders, as submodules.
                     Reports F32 because that is what they produce, which puts
                     every file it reads on Path B. Permutes Ogg channel order
                     into WAVE order and changes nothing else.

  decode_mf/         Media Foundation source reader, and now the last resort rather than
                     the answer for MP3 and AAC. Hardware video decode comes with it
                     and it is measurably bit-exact for WAV and FLAC -- but it reads
                     gapless metadata in no codec, clips float WAV, and scrambles
                     multichannel ALAC. It stays because it needs nothing installed.
  decode_ffmpeg/     the long tail, through the ffmpeg and ffprobe *programs*, found at
                     run time and never shipped. Nothing to build against, no ABI to
                     track, and the LGPL-or-GPL question stays with whoever installs it.
  sink_wasapi/       exclusive and shared, event-driven, both.
  sink_asio/         someday. Not for accuracy -- exclusive mode is already exact --
                     but for native DSD above what DoP can carry. Practical since
                     Steinberg relicensed the ASIO SDK under GPLv3 in October 2025.
  dsp_*/             gain, resample, convolve, crossfeed. Never present in passthrough.
  video_d3d11/       presentation, and the three tone-map providers.
shell/windows/       the WinUI 3 window. C#, Native AOT, **optional**: the engine runs
                     with none of it on disk, the same way DragonPerch's daemon does.
shell/cli/           the shell that is always there. Same IPC, no toolkit.
tests/               Catch2. The graph, the negotiation and the ring, with no device.
fuzz/                libFuzzer. Every parser that reads a file someone else wrote.
cmake/               compiler flags, packaging, the module install layout.
docs/plan.md         the plan of record, including findings that cost real time.
```

Paths above are relative to the repository root.

## Processes

```
        ┌──────────────────────────────┐
        │ mediaperchd  (the engine)    │   no window, no toolkit, no UI framework
        │  core + win head + modules   │   works alone; this is the product
        └───────┬──────────────┬───────┘
                │ IPC          │ IPC
   ┌────────────┴───┐   ┌──────┴──────────────────────┐
   │ mediaperch-cli │   │ mediaperch-shell (WinUI 3)  │  optional, ~40 MB of XAML,
   │ always present │   │ started on demand, killable │  killed without the audio noticing
   └────────────────┘   └─────────────────────────────┘
```

The reason the shell is a separate process is the one DragonPerch measured rather than
assumed: initialising XAML costs a process about 40 MB of private bytes permanently, and
closing the window returns none of it. An audio engine that must not miss a 3 ms deadline
does not get to carry that, and does not get to be restarted when the settings window
crashes.
