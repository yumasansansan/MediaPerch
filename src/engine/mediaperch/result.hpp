// SPDX-License-Identifier: GPL-3.0-or-later
//
// What an `MpResult` is called, in words a person can act on.
//
// In the core because more than one head needs it now: the probe prints these,
// the engine puts them in its log, and a shell shows them to somebody who is
// not going to look up a number. One table, so the three cannot disagree.

#ifndef MEDIAPERCH_RESULT_HPP
#define MEDIAPERCH_RESULT_HPP

#include <mediaperch/module.h>

namespace mp {

/// Never null, and never a bare code: "device in use by another application"
/// tells somebody what to do and `MP_ERR_BUSY` does not.
[[nodiscard]] const char* result_name(MpResult r) noexcept;

/// What a codec id is called. Never null: an id nobody here knows comes back as
/// "codec 0x…", which is still something a person can look up.
///
/// Here rather than in the probe for the same reason `result_name` is: a
/// container now names the codec of every stream, so the probe prints it, the
/// engine logs it, and a shell will want to show it. One table, so the three
/// cannot disagree about what 37 means.
[[nodiscard]] const char* codec_name(MpCodec codec) noexcept;

/// Audio, video, subtitle. A file is not one stream, and this is what says so
/// in a listing.
[[nodiscard]] const char* stream_kind_name(MpStreamKind kind) noexcept;

/// What a module kind is called: demux, codec, dsp. Never null.
///
/// There are seven of them now and two were added this year, so a listing that
/// prints the number makes a reader go and look it up.
[[nodiscard]] const char* module_kind_name(MpKind kind) noexcept;

} // namespace mp

#endif // MEDIAPERCH_RESULT_HPP
