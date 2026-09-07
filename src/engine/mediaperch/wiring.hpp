// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One route from a source format to a running device, for every caller.
//
// **Four callers had written this out and they had drifted.** `play`, `decode`,
// `show` and `mp::Player` each ask the same five questions in the same order --
// what does the chain turn the offer into, what will the device take, how big
// is a period, what block is the chain then sized for, and is this Path B --
// and each had its own copy of the answer. `show`'s copy did not exist at all,
// so `--dsp` was parsed, accepted, and silently dropped: a mono file on a
// stereo-only endpoint refused, with the fix in the user's hand and no way to
// reach it. That is not a bug that gets fixed once. It gets fixed once *per
// copy*, which is the same thing as never.
//
// So the order lives here and nowhere else. What stays with the caller is
// everything that is genuinely its own: which stages to build (a registry or
// an `IEngineHost`), which device to open, and how to report a refusal -- the
// probe prints paragraphs to stderr and the engine puts one line in a status,
// and neither is this file's business.
//
// **It does not decide anything the user did not.** A chain forces Path B
// because a stage exists in order to change the samples and a bit-exact claim
// over filtered audio would be a lie; nothing else here overrides a choice.
// Notably it does not resample and does not remix to make a device accept a
// file. Those are stages, they are named on the command line, and a device
// that refuses what it was offered is a refusal the person gets to see.

#ifndef MEDIAPERCH_WIRING_HPP
#define MEDIAPERCH_WIRING_HPP

#include "mediaperch/dsp.hpp"
#include "mediaperch/negotiation.hpp"
#include "mediaperch/sink.hpp"

#include <cstdint>
#include <string>

namespace mp {

/// Everything between a source format and a device that will take it.
struct Wired {
    /// What the device answered, and to what. `ok` false means it took none of
    /// them; `tried` and `last_error` are what a caller reports.
    Negotiated negotiated;
    /// Frames in one device period. Zero until the device has said.
    std::uint32_t period_frames = 0;
    /// The policy as it ended up. A chain forces `processed`; without one it is
    /// exactly what was asked for.
    PathPolicy policy = PathPolicy::bit_exact;
    /// Whether Path B is the graph -- `use_processed`, with the chain counted.
    bool processed = false;
    /// What was actually offered to the device: the source, with the chain's
    /// rate, channels and mask where there is a chain. Kept because a report
    /// that shows the source and the accepted format and not the offer cannot
    /// explain a refusal.
    Format offered{};
};

/// Configures `chain`, negotiates, and sizes the chain to the device's period.
///
/// `gain_changes` is the caller's other reason to be on Path B: a gain changes
/// the samples whatever the formats say, and no format comparison can see it.
///
/// False when the device took nothing or would not say how big a period is.
/// **`out.negotiated` is filled either way**, because the refusal is the thing
/// the caller has to report and it is not this function's to phrase. `why` is
/// set only for the failures that are not a refusal -- a stage that would not
/// configure, a device with no period -- and is left alone otherwise.
[[nodiscard]] bool wire_up(Sink& sink, const Format& source, DspChain& chain,
                           PathPolicy policy, bool gain_changes, Wired& out,
                           std::string& why);

} // namespace mp

#endif // MEDIAPERCH_WIRING_HPP
