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

## Still unmeasured

- **`AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` has not been seen on any device.** The realign path
  is implemented and reviewed; it has never run. Until some hardware produces it, that code
  is untested, and saying otherwise would be pretending. HDMI to a receiver is the usual
  suspect.
- **No run longer than twelve seconds.** M1's criterion is an hour at the minimum period
  with zero underruns, and that has not been done.
- HDMI, Bluetooth, and anything that is not this machine.
