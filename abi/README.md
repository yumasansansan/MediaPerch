# Crossing the ABI

> **An ABI that has never been crossed from a second language is an ABI that does not work
> yet.** — [docs/plan.md](../docs/plan.md) §2

Two modules live here whose only purpose is to settle that. Neither is part of the product,
neither runs in CI, and neither is needed to play a single file. What they are is the
evidence for a decision plan.md made and promised to keep reversible: the module boundary is
a `.dll` on disk with a plain C ABI, so a module written in another language in two years is
exactly as much work as one written today.

The plan said to delete them afterwards. They are kept instead, because deleting them would
delete the evidence, and an off-by-default target costs the same as a deleted one until
somebody turns it on.

## What was measured

Both probes are the same DSP stage — it halves what it is given and counts what it was
handed — so the two runs can be compared to each other and to the C++ modules beside them.
Each reports, through `MpDspVtbl::describe`, **how many times the host broke a promise the
header makes**: a null buffer, a block bigger than the capacity it agreed to, a format that
is not the f64 bus.

On a FiiO KA5 in WASAPI exclusive mode, four seconds of a 44.1 kHz file:

| Probe | Blocks | Frames | Complaints | Underruns |
|---|---|---|---|---|
| `dsp_probe_c` (C11, MSVC) | 1355 | 178,860 | 0 | 0 |
| `dsp_probe_rust` (rustc 1.98, `cdylib`) | 1355 | 178,860 | 0 | 0 |

Identical, which is the answer: the host calls a C module and a Rust module the same way it
calls its own, and neither of them can tell the difference.

### What the C probe caught, and it was one thing

`module.h` compiled as C11 with no extensions, from a C compiler, with `offsetof` assertions
on every field the host reads. It built and ran first time. The one thing worth recording is
that this is now a standing check rather than a one-off: the file is compiled as C on every
build with the probes on, so a C++-only construct sneaking into the header is a build error
rather than a discovery somebody makes in two years.

### What the Rust probe caught, and it was the interesting one

Layout and calling convention were uneventful — `#[repr(C)]` structs transcribed by hand
from the header matched, and the host called them without ceremony. The question that
actually needed an answer was **panic containment**, because a panic unwinding into C++ is
undefined behaviour and, on Windows, a corrupted stack.

```
mediaperch-probe play --file X --dsp probe_rust:panic=1 --verbose
```

makes the next `process` panic on purpose. What happens:

```
played     264 frames (0.01 s)
underruns  0
stopped    internal error
```

The panic hit `catch_unwind` at the boundary, came back as `MP_ERR_INVALID`, and the host
treated it as what it is — a stage that returned an error — stopped the graph, printed why,
and exited normally. No crash, no corruption, and the device was released properly.

**So the boundary is pleasant as well as possible**, which is one of the three conditions
§2 names for revisiting the language decision. It does not on its own make `decode_native`
a Rust module: the argument in §2 was that Rust would protect the small parsing surface we
write and not the large one we link, and that is still true. What has changed is that the
option is now known to be open rather than assumed to be.

## Running them

```bash
cmake --preset ninja-msvc -DMEDIAPERCH_BUILD_ABI_PROBES=ON
cmake --build --preset ninja-msvc-release
```

```bash
cargo build --release --manifest-path abi/probe_rust/Cargo.toml
```

Cargo puts `mp_probe_rust.dll` in `abi/probe_rust/target/release`; copy it beside the
binaries, where the host scans for `mp_*.dll`. Then:

```bash
mediaperch-probe --dsp list
```

Both appear at priority 0, which is the lowest there is: they are probes, not filters
anybody wants chosen for them.
