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
| Bus format | the device's, verbatim | deinterleaved binary64, one plane per channel |
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

**What the conversion does is narrow on purpose.** Sample type, a gain, dither and noise
shaping. It does **not** resample and does **not** change the channel count: both are real
conversions needing real implementations, and a bad one built into the converter would be
worse than the refusal it replaced. Both are the *chain's* job, below -- a stage may answer
`configure` with a different rate and negotiation will offer the device that rate instead.
No stage that does so is written yet, so today negotiation still refuses a device that wants
a rate the file does not have.

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

**Path B is not only for narrowing.** A 64-bit float source on a 64-bit float device, sent
through Path B for a volume control, gets *the multiply and nothing else*: there is no
quantiser, so no dither, no shaping and no clamp run, and the arithmetic is one
correctly-rounded binary64 operation per sample — about −320 dB of error, and exact for any
gain that is a power of two. A value of 3.5, past what an integer device could hold, comes
out as 3.5. The same is true of f32 → f64. `tests/convert_test.cpp` asserts equality rather
than closeness for both.

The one narrowing that gets no dither is **f64 or f32 → f32**, and that is deliberate: a
float destination's error is proportional to the signal rather than fixed, so it is already
uncorrelated in the way dither exists to produce, and its 24-bit significand puts it about
144 dB down at every level.

**And nothing useful can be done on the way in.** Widening is exact, so there is no
information to recover and nothing to improve: what is sometimes done at this point —
"bit-depth expansion", guessing at the dither that was removed — is speculative
reconstruction rather than fidelity, and this program does not do it. The processing that
is worth having lives at the *other* end, where bits are actually being discarded, which is
what the rest of this section is about. The one genuine exception is **de-emphasis**, which
is not a width question at all: it is a defined format property that a source either carries
or does not.

### The DSP chain

Path A exists so that a file can reach a device with nothing in between. The chain is the
other side of that decision: the place where things are *meant* to touch the samples. A
resampler, a ReplayGain, an equaliser and a convolver are all the same shape of thing, and
the shape is `MpDspVtbl` -- the third module kind, beside decoders and sinks.

| Call | What it is for |
|---|---|
| `configure` | the stage is handed its input format and the largest block it will be given, and answers with the format **it** produces and how much room that needs. This is the call that lets a resampler exist: the answer may carry a different sample rate or channel count |
| `process` | deinterleaved `double` in, deinterleaved `double` out. It may produce fewer frames than it was given while it fills its history, or more when it is upsampling, which is why `configure` has to answer with a capacity |
| `flush` | the end of the stream: whatever is still inside, called until it reports zero |
| `set` / `describe` | one setting as text, and the list of them. Text because the shell is a separate process behind a versioned wire format and has to be able to drive a stage nobody had written when the shell was built |

**The bus is deinterleaved `double`.** plan.md says f32, and this is a deliberate departure
from it: the conversions at both ends of Path B already work in binary64, so an f32 bus
would put a second rounding in the one path whose whole argument is that it has exactly one.
The cost is memory bandwidth on a workload measured in tens of megabytes a second. The
deinterleaving is what plan.md asked for and is kept, because it is what a filter wants.

**There is still exactly one quantiser.** The shape is source → widen to the f64 bus →
chain → the wire format, and only the last of those rounds. The widening is exact for every
source type this program has (§ above), so the dither and the noise shaping stay in one
place, at the bottom, where the bits are actually being discarded.

**The chain decides what the device is asked for.** Negotiation runs against
`DspChain::output_format()`, not the file's format, so a stage that resamples is offered to
the device as the rate it produces. `ProcessedGraph` then builds its output converter from
the chain's output rather than from the source, and refuses to start if it was handed a
chain configured for some other stream -- a mismatch there would otherwise arrive as noise
rather than as an error.

**A chain is drained, not truncated.** At the end of a stream each stage is flushed until it
reports zero, head first, with whatever comes out fed to the stages behind it. A delay line
is the plain case: without the drain the last frames of every file are simply missing, and
nothing else in the program would notice. `tests/dsp_test.cpp` plays a file through a
delaying stage and requires every input frame to come out the far end.

**Asking for a stage is asking for Path B.** `--dsp` implies `--path processed`, and says so
in the report: a stage exists in order to change the samples, and a bit-exact claim over
audio that has been through a filter would be a lie no format comparison could catch.

```
$ mediaperch-probe play --dsp gain:gain_db=-6 --verbose
path       PROCESSED -- the samples are changed  [--path processed]
           chain dsp_gain on an f64 bus, 44100 Hz / 2 ch / F64
           --dsp implied --path processed
dsp_gain   peak	0.250578	loudest sample seen (read only)
```

`--dsp list` prints every stage that is loaded with every setting it has, read out of the
module through `describe` rather than out of a table in the host -- which is the point of
the call: a shell can offer a stage it was never compiled against. `--shape list` does the
same for the dither and shaping algorithms, of which there are rather a lot.

### The resampler

The stage `MpDspVtbl` was shaped for, and the first one whose answer to
`configure` is a different sample rate. `--dsp resample:rate=48000`.

It is a **polyphase FIR with a Kaiser-windowed sinc, designed at `configure`
rather than transcribed**. That is the opposite choice from the noise-shaping
curves and for the same reason: those are measurements of hearing and cannot be
derived, this has a closed form four lines long, and a table of numbers copied
out of another project is a table nobody in this tree could check.

The ratio is rational and reduced — 44100 → 48000 is 160/147 — and each output
sample costs one phase of the filter, not the whole of it. Two numbers decide
everything else:

| | Stopband | Passband | Taps at 160/147 |
|---|---|---|---|
| `quality=fast` | 96 dB | 91% of Nyquist | 70 |
| `quality=good` **(default)** | 120 dB | 95% | 158 |
| `quality=best` | 144 dB | 98% | 474 |

`attenuation` and `bandwidth` are also settable directly, which is what the
presets set.

**A rate pair that does not reduce is refused, by name.** 44100 → 44101 reduces
to 44101/44100: every phase is a separate filter and there are 44101 of them.
`configure` says so and names the count rather than allocating seven million
coefficients. An arbitrary-ratio resampler interpolates between phases and is a
different program, worth writing deliberately rather than discovering by
accident.

**It is never inserted automatically.** A resampler that appears whenever a
device is fussy is how a bit-exact player stops being one quietly. `--path auto`
still refuses a device that wants a rate the file does not have; asking for the
stage is what changes that, and the report then says `PROCESSED`.

#### What it measures

`tests/resample_test.cpp` takes the response apart with a coherent single-bin
DFT — every frequency it uses completes a whole number of cycles in the window
analysed, so there is no window function anywhere to explain a result away.
Against the `good` preset, whose design target is 120 dB:

| Measured | Design asked for | Got |
|---|---|---|
| THD+N under a 1 kHz tone, 44.1 → 48 kHz | −120 dB | **−152 dB** |
| A 30 kHz tone downsampled 96 → 44.1 kHz, folded to 14.1 kHz | −120 dB | **−138 dB** |
| Passband error, 50 Hz to 20 kHz | flat | **< 5 × 10⁻⁶ dB** |
| 44.1 → 48 → 44.1 kHz round trip, `best` | — | **161 dB SNR** |

Kaiser's order estimate is conservative, which is why every row beats its
target. Three more properties are asserted rather than measured, and they are
the ones that would otherwise be found on somebody else's hardware:

- **1:1 is a unit impulse.** Not a branch — the filter is really designed at
  44100 → 44100, and every tap but the centre one is exactly zero, because an
  integer centre and a cutoff at Nyquist put them on the sinc's own zeros.
  `process` does take the branch; the test is there so that the branch is
  skipping a redundant filter rather than hiding a broken one.
- **The block size changes nothing, bit for bit.** The device decides the
  period: 132 frames here, 4096 elsewhere. The same input in blocks of 1, 7,
  132, 1000 and 4096 produces byte-identical output.
- **The output is exactly `ceil(n × L / M)` frames.** A frame either way is a
  click at a track boundary, so it is an equality and not a tolerance. The
  filter's tail is drained rather than truncated, and not padded past its end.

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

#### The published curves

`--shape <name>` selects one of **12 curves from
[ReSampler](https://github.com/jniemann66/ReSampler)** (Judd Niemann, LGPL-2.1, also
compatible): Lipshitz, Vanderkooy and Wannamaker's from *Minimally Audible Noise Shaping*,
Wannamaker's from *Psychoacoustically Optimal Noise Shaping*, and ReSampler's own.

```
flat, modified-e, lipshitz, improved-e, wannamaker3, wannamaker9,
wannamaker24, standard, high28, high30, high32, blue
```

These are designed for 44.1 kHz and are usable at other rates, where the shape stretches
and the notches move off the frequencies they were placed at — ReSampler's own documented
position, and the report says `designed for 44.1 kHz and stretched to 48000` rather than
leaving it to be discovered. That is the difference from SSRC's: those are fitted per rate
and are never substituted across one.

**The two sources do not share a sign convention, and this is the thing to get right.**
SSRC *adds* the filtered error history to the sample before quantising; ReSampler
*subtracts* it. The same coefficients under the wrong convention shape the noise **into**
the midband instead of out of it — which is worse than no shaping and sounds like nothing
is wrong. ReSampler's are negated at transcription so one convention, SSRC's, is the only
one at run time, and `tests/convert_test.cpp` pins three of Wannamaker's numbers with their
signs so the day somebody "fixes" it is the day a test goes red.

`classic` is the one profile not transcribed: it is a cascaded biquad in ReSampler rather
than an FIR, so its numbers are not taps and carrying them across would build a filter that
is not what the name means.

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
                     the answer for MP3 and AAC. Its probe once claimed 100 for
                     anything beginning "ID3", which took every tagged MP3 — that
                     is, nearly all of them — away from the decoder that does
                     gapless: a tag with cover art in it is larger than the four
                     kilobytes a probe is given, so dr_mp3 can only claim 60 there.
                     An ID3 tag identifies nothing, so this claims 60 as well and
                     priority decides, which is what priority is for. Hardware video decode comes with it
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
  dsp_gain/          the first MpDspVtbl module: a gain, and the peak it saw.
  dsp_resample/      polyphase, Kaiser-windowed sinc, designed at configure. The
                     stage that answers with a rate it was not given.
  dsp_*/             convolve, crossfeed, ReplayGain. Never present in passthrough.
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
