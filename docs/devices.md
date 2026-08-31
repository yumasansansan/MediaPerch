<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# What real devices actually do

§12 of [the plan](plan.md) calls for a device matrix, on the grounds that it cannot be
automated and that exclusive-mode behaviour is decided by drivers rather than by
documentation. This is that matrix. Every row was measured with `mediaperch-probe`, not
inferred from a specification.

Two things it has already established that no amount of reading would have:

- **"24-bit" names two different wire formats**, and both devices measured so far want the
  three-byte packed one — while the machine's onboard codec is configured for the four-byte
  one. A candidate list that offers only one of them refuses playable audio, and the refusal
  looks like a device limitation.
- **The channel mask requirement is per-device and goes both ways.** The virtual cable
  refuses the plain `WAVEFORMATEX` and takes only `WAVEFORMATEXTENSIBLE`; the USB DAC takes
  the plain form and needs no mask at all. Neither order of trying them is right for both,
  which is why each container is offered in both forms before the next container is tried.

## VB-Audio Virtual Cable — "CABLE Input"

Configured (shared-mode engine format): 192000 Hz, 24-bit, `nBlockAlign = 6`.

| Asked for | Accepted as | Candidate | Period |
|---|---|---|---|
| 44100 / 16 | `S16` **+ mask** | 2 of 6 | 88 frames, 2.00 ms |
| 44100 / 24 | `S24_PACKED` **+ mask** | 4 of 4 | 88 frames, 2.00 ms |
| 192000 / 24 | `S24_PACKED` **+ mask** | 4 of 4 | 384 frames, 2.00 ms |

- **Always needs the channel mask.** The plain form is refused at every width.
- **24-bit means three bytes.** `S24_IN_32` is refused with and without a mask.
- Played 12 s at 192000/24 through the repack path with zero underruns, and 12 s at
  44100/16 through the memcpy path with zero underruns, both at the minimum period.

## FiiO KA5 (USB DAC)

Configured: 384000 Hz, 32-bit, `nBlockAlign = 8`.

Every combination of {44100, 48000, 96000, 192000, 384000, 705600, 768000} Hz and
{16, 24, 32} bits was accepted. The minimum period is 3.00 ms at every rate.

| Asked for | Accepted as | Candidate |
|---|---|---|
| any rate / 16 | `S16`, no mask | 1 |
| any rate / 24 | `S24_PACKED`, no mask | 3 |
| any rate / 32 | `S32`, no mask | 1 |

- **768 kHz works**, and so does 705600. The shared-mode dropdown stops at 384000, which is
  a reminder that the dropdown is a setting and not a capability list.
- **Needs no channel mask**, at any width — the opposite of the virtual cable.
- **24-bit is packed here too.** Candidates 1 and 2 (`S24_IN_32`, with and without a mask)
  are refused; candidate 3 is accepted. Two independent devices, same answer.
- Played 8 s at 768000/32 — 6,179,328 frames, 49 MB/s through the ring — with zero
  underruns under MMCSS `Pro Audio`, on the memcpy path.

## Realtek onboard — "スピーカー"

Configured: 48000 Hz, 32-bit container with `wValidBitsPerSample = 24`, `nBlockAlign = 8`.
So this one wants `S24_IN_32` where the other two want `S24_PACKED`.

Not yet negotiated: it is the machine's speakers, and taking it in exclusive mode
interrupts everything.

## Volume

All four endpoints on this machine report `ENDPOINT_HARDWARE_SUPPORT_VOLUME`, including both
halves of a virtual cable. That flag means the control is not implemented by the Windows
audio engine; it does not mean the volume is applied after the converter, and a virtual cable
demonstrates the difference by claiming it while having no converter. `mediaperch-probe
devices` therefore labels it `[endpoint volume]` and not `[hardware volume]`.

Establishing whether a given device's volume is actually free would take a loopback capture
at two settings and a comparison — which this machine can do, because the virtual cable is a
loopback. Not done yet.

## The loopback is not an instrument

VB-Cable's two endpoints look like a perfect way to prove bit-exactness: play into the
render half, record from the capture half, compare. It does not work, and the way it fails
is worth recording because it looks like a bug in the player.

Both endpoints taken in exclusive mode, at the cable's own configured format
(192000 Hz / 24-bit packed), both endpoint volumes reading exactly 1.0000, zero
discontinuities reported by the capture:

| | |
|---|---|
| correlation of the played block with the recording | 0.87 at 44100/16, 0.51 at 192000/24 |
| samples identical at the best alignment | **0 of 32** |
| longest prefix of the played stream found verbatim | **none, down to six bytes** |
| what the recording looks like | samples repeated in runs, low bits cleared |

Exclusive mode guarantees that *Windows* does not touch the samples. It cannot guarantee
what a driver does, and a virtual cable's "hardware" is more software. So this measures the
cable, not the player.

`mediaperch-probe verify` therefore decides on the **tee**: every buffer handed to
`IAudioRenderClient::ReleaseBuffer` is copied and hashed. Measured on this cable:

| File | Wire format | Result |
|---|---|---|
| 16-bit FLAC, 44100 | `S16` + mask | bit-exact, 1,411,200 bytes |
| 24-bit FLAC, 44100 | `S24_PACKED` + mask | bit-exact, 2,116,800 bytes |
| 24-bit FLAC, 192000 | `S24_PACKED` + mask | bit-exact, 4,652,772 bytes |

In each case the SHA-256 of the decoder's output, of FFmpeg's decode of the same file, and
of the bytes handed to the device are the same value. Proving anything past `ReleaseBuffer`
needs a second audio interface recording a digital output, not more software.

## Still unmeasured

- **`AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` has not been seen on any device.** The realign path
  is implemented and reviewed; it has never run. Until some hardware produces it, that code
  is untested, and saying otherwise would be pretending. HDMI to a receiver is the usual
  suspect.
- **No run longer than twelve seconds.** M1's criterion is an hour at the minimum period
  with zero underruns, and that has not been done.
- HDMI, Bluetooth, and anything that is not this machine.
