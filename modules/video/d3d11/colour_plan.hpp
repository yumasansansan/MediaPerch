// SPDX-License-Identifier: GPL-3.0-or-later
//
// What §9 decides, as arithmetic, so that it can be checked without a display.
//
// **This is the whole of the HDR argument and none of the Direct3D.** What the
// presentation buffer must hold, whether it has to blend, which tone mapper,
// and how much to scale SDR content by are four answers that follow from two
// facts -- what the container said the stream is, and what the display turned
// out to be -- and none of them needs a device to work out.
//
// It is also free of any one platform's format list, which took a correction:
// it used to decide between `fp16_scrgb` and `rgb10_hdr10`, which is DXGI's
// vocabulary and would have made a Wayland presenter round to Windows'
// ceiling. Linux is exactly where that matters -- DRM has `ABGR16161616` and
// DXGI has nothing like it (§9.10). Separating them is what lets the
// part that is easy to get subtly wrong be tested at all: a renderer judged by
// whether it looks plausible is how the OS tone mapper's 2.4 gamma survived for
// years with a vendor bug report open against it (§9.2).
//
// The code points are ISO/IEC 23091-2, the same ones `MpVideoInfo` carries and
// the same numbers H.273, HEVC and AV1 use.

#ifndef MEDIAPERCH_VIDEO_COLOUR_PLAN_HPP
#define MEDIAPERCH_VIDEO_COLOUR_PLAN_HPP

#include <cstdint>

namespace mp::video {

/// ISO/IEC 23091-2 transfer characteristics, for the two that mean HDR.
enum : std::uint32_t {
    k_transfer_unspecified = 2u,
    k_transfer_bt709 = 1u,
    k_transfer_srgb = 13u,
    /// SMPTE ST.2084, which everybody calls PQ.
    k_transfer_pq = 16u,
    /// ARIB STD-B67, which everybody calls HLG.
    k_transfer_hlg = 18u,
};

/// And the two primaries that matter to the decision.
enum : std::uint32_t {
    k_primaries_unspecified = 2u,
    k_primaries_bt709 = 1u,
    k_primaries_bt2020 = 9u,
};

/// What the container said, out of `MpVideoInfo`.
struct Stream {
    std::uint32_t primaries = k_primaries_unspecified;
    std::uint32_t transfer = k_transfer_unspecified;
    std::uint32_t matrix = k_primaries_unspecified;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// What the display turned out to be, out of §9.4's three calls.
struct Display {
    /// The active colour mode is HDR: `DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR`,
    /// or `IDXGIOutput6` reporting an HDR colour space on Windows 10.
    bool hdr = false;
    /// Advanced Color is on but the mode is SDR -- a wide-gamut display doing
    /// its own colour management. **`IDXGIOutput6` cannot tell this from a
    /// plain SDR display** (§9.4), so on Windows 10 it is always false and the
    /// difference only exists from 11 24H2.
    bool wide = false;
    /// `DISPLAYCONFIG_SDR_WHITE_LEVEL`, in nits. 80 is the scRGB reference and
    /// what a display that does not say is assumed to use.
    float sdr_white_nits = 80.0f;
    /// The brightest the display claims, from `DXGI_OUTPUT_DESC1::MaxLuminance`
    /// or its equivalent. **HLG needs it and PQ does not**: PQ states absolute
    /// nits, while HLG is scene-referred and its OOTF has a system gamma that
    /// is a function of the display's peak. 1000 is the reference HLG display
    /// and the assumption BT.2100 makes when nothing says otherwise.
    float peak_nits = 1000.0f;
};

/// §9.3's four, in the order §9.7 wants them tried.
enum class ToneMap : std::uint32_t {
    /// The display is already HDR: pass PQ through and map nothing.
    none,
    /// The GPU's fixed-function video processor, using the stream's metadata.
    /// **The default**: cheapest, and the one the OS's own player path uses.
    driver,
    /// Direct2D's HDR tone map effect -- the same mapper Windows ships.
    d2d,
    /// Ours, a BT.2390 EETF to sRGB. The escape hatch when a driver's is bad,
    /// and the one §9.2 says is *correct* rather than merely conventional.
    shader,
};

/// What the presentation buffer *holds*. **Not which format it is in.**
///
/// This header decides for every platform and knows the format list of none of
/// them, which is the difference between a portable decision and a Windows one
/// wearing a portable name. An earlier version of this enum was
/// `{ fp16_scrgb, rgb10_hdr10 }` -- DXGI's own four formats, minus two, baked
/// into logic that a Wayland presenter was supposed to share. It would have
/// made every platform round to half whether or not half was the widest thing
/// it had, and Linux is precisely the platform where it is not: DRM defines
/// `ABGR16161616`, sixteen bits of integer per channel, and hardware planes
/// that scan it out.
///
/// So the decision is the encoding, and the format is the presenter's -- the
/// widest its platform offers that carries this encoding and blends if
/// `Plan::needs_blending` says it must.
enum class Encoding : std::uint32_t {
    /// Linear light, BT.709 primaries: scRGB. Works on every display kind, and
    /// is what an OSD can be blended into.
    linear,
    /// PQ-encoded, BT.2020 primaries: HDR10 as a display takes it. Cheaper on
    /// every platform that has a packed format for it, and on none of them can
    /// it be alpha-blended, so it is only offered when nothing is composited
    /// over the video.
    pq,
};

/// What has to happen to the source's transfer function on the way in.
///
/// **Separate from tone mapping, which conflating them got wrong.** Tone
/// mapping reduces dynamic range because a display cannot show it all. A
/// transfer conversion changes how the numbers are coded and reduces nothing --
/// and HLG needs one even on a display that can show every stop of it, because
/// no platform presents HLG directly.
enum class Convert : std::uint32_t {
    /// The buffer already holds what the source coded. Only PQ into a PQ
    /// buffer gets this.
    none,
    /// Undo the source's transfer into linear light. sRGB, BT.709 and PQ all
    /// take it, and it is what a linear buffer always needs.
    to_linear,
    /// Undo HLG's inverse-OETF *and* its OOTF, which is the step that makes
    /// HLG different: the OOTF is a system gamma derived from the display's
    /// peak luminance, so the same signal is a different picture on two
    /// displays and that is by design.
    hlg_to_linear,
    /// Linearise, then re-encode as PQ. What a source that is not PQ needs to
    /// reach a PQ buffer -- and the reason this plan does not send one there.
    to_pq,
};

struct Plan {
    Encoding encoding = Encoding::linear;
    /// What the source's transfer needs on the way into that buffer.
    Convert convert = Convert::to_linear;
    /// The peak the HLG OOTF was derived for, in nits. Meaningless unless
    /// `convert` is `hlg_to_linear`, and carried here so the shader does not
    /// have to ask a display anything.
    float hlg_peak_nits = 1000.0f;
    /// Whether anything is drawn over the video, which rules out a packed
    /// format that cannot blend -- and is a fact about the *player*, not about
    /// any platform's format list.
    bool needs_blending = true;
    ToneMap tone_map = ToneMap::none;
    /// **What SDR content is multiplied by, in linear space, before it is
    /// composited.** §9.6: on an HDR display scRGB 1.0 is 80 nits and is
    /// scene-referred, so an OSD drawn at 1.0 appears dim and grey next to the
    /// video -- the single most common HDR bug in players. On an SDR display
    /// 1.0 is already the display's white and this is 1.
    float sdr_scale = 1.0f;
    /// True when the stream carries more than the display can show as-is.
    bool tone_mapping = false;
};

/// Whether a transfer function means high dynamic range.
[[nodiscard]] constexpr bool is_hdr_transfer(std::uint32_t transfer) noexcept
{
    return transfer == k_transfer_pq || transfer == k_transfer_hlg;
}

/// The convention for a stream that states nothing, which is most of them.
///
/// **BT.709 above standard definition and BT.601 below it**, which is what
/// every player does and what nothing writes down. `unspecified` is code point
/// 2 in all three fields and is what a container with no `colr` box leaves; a
/// renderer still has to draw something.
[[nodiscard]] constexpr std::uint32_t assumed_primaries(const Stream& s) noexcept
{
    if (s.primaries != k_primaries_unspecified) {
        return s.primaries;
    }
    return s.height > 576 ? k_primaries_bt709 : 6u /* BT.601 525-line */;
}

[[nodiscard]] constexpr std::uint32_t assumed_transfer(const Stream& s) noexcept
{
    return s.transfer != k_transfer_unspecified ? s.transfer : k_transfer_bt709;
}

/// The four answers.
///
/// `preferred` is what a person asked for with `--dsp`-style settings, and is
/// honoured only where it can be: asking for a tone mapper on a display that
/// needs none would darken a picture that was already right.
///
/// `composited` says whether anything is drawn over the video -- subtitles, an
/// OSD. It costs the packed PQ buffer, which cannot blend.
[[nodiscard]] constexpr Plan plan_for(const Stream& stream, const Display& display,
                                      ToneMap preferred = ToneMap::driver,
                                      bool composited = true) noexcept
{
    Plan plan{};
    const std::uint32_t transfer = assumed_transfer(stream);
    const bool content_is_hdr = is_hdr_transfer(transfer);
    const bool content_is_hlg = transfer == k_transfer_hlg;

    plan.needs_blending = composited;
    plan.hlg_peak_nits = display.peak_nits;

    if (!content_is_hdr) {
        // **Nothing to map.** SDR content on any display is SDR content; the
        // question §9 asks does not arise, and mapping it anyway is how a
        // player makes a correct picture worse.
        plan.tone_map = ToneMap::none;
        plan.tone_mapping = false;
        plan.encoding = Encoding::linear;
    } else if (display.hdr) {
        // The display can show it, so nothing is tone-mapped -- but **that is
        // not the same as nothing being done**, which is what an earlier
        // version of this function assumed. PQ passes through into a PQ buffer
        // and HLG does not: it is scene-referred, its OOTF depends on the
        // display's peak, and no platform here presents it directly. DXGI has
        // no HLG swap chain colour space at all; a `CAMetalLayer` has none
        // either. So HLG is linearised like everything else, which costs the
        // packed buffer and is the honest price of the format.
        plan.tone_map = ToneMap::none;
        plan.tone_mapping = false;
        plan.encoding = composited || content_is_hlg ? Encoding::linear : Encoding::pq;
    } else {
        // HDR content, SDR display: §9.1's whole point. Something must map it,
        // and composition will not -- it clips, silently, and everything
        // outside [0, 1] is gone.
        plan.tone_map = preferred == ToneMap::none ? ToneMap::driver : preferred;
        plan.tone_mapping = true;
        plan.encoding = Encoding::linear;
    }

    // What the transfer needs, which follows from the two above and is stated
    // rather than left for a renderer to infer from `transfer` and `encoding`.
    if (plan.encoding == Encoding::pq) {
        plan.convert = Convert::none; // only PQ reaches a PQ buffer
    } else if (content_is_hlg) {
        plan.convert = Convert::hlg_to_linear;
    } else {
        plan.convert = Convert::to_linear;
    }

    // §9.6, and it applies whenever the buffer is linear on an HDR display --
    // including the case where the video needs no mapping at all, because the
    // subtitles still do. A PQ buffer states absolute nits and needs no scale.
    plan.sdr_scale = display.hdr && plan.encoding == Encoding::linear
                         ? (display.sdr_white_nits > 0.0f ? display.sdr_white_nits / 80.0f
                                                          : 1.0f)
                         : 1.0f;
    return plan;
}

/// The name a person types and reads back.
[[nodiscard]] const char* name_of(ToneMap m) noexcept;
[[nodiscard]] const char* name_of(Encoding e) noexcept;
[[nodiscard]] const char* name_of(Convert c) noexcept;
[[nodiscard]] bool tone_map_from_name(const char* name, ToneMap& out) noexcept;

} // namespace mp::video

#endif // MEDIAPERCH_VIDEO_COLOUR_PLAN_HPP
