// SPDX-License-Identifier: GPL-3.0-or-later
//
// What §9 decides, as arithmetic, so that it can be checked without a display.
//
// **This is the whole of the HDR argument and none of the Direct3D.** Which
// swap chain format, which colour space, which tone mapper, and how much to
// scale SDR content by are four answers that follow from two facts -- what the
// container said the stream is, and what the display turned out to be -- and
// none of them needs a device to work out. Separating them is what lets the
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

/// §9.5's two.
enum class SwapFormat : std::uint32_t {
    /// `DXGI_FORMAT_R16G16B16A16_FLOAT` in scRGB. Works on every display kind
    /// and blends with an OSD, at 64 bits a pixel.
    fp16_scrgb,
    /// `DXGI_FORMAT_R10G10B10A2_UNORM` with the PQ colour space. Half the
    /// bandwidth and no alpha blending, so only when nothing is composited
    /// over the video.
    rgb10_hdr10,
};

struct Plan {
    SwapFormat format = SwapFormat::fp16_scrgb;
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
/// OSD. It costs the HDR10 swap chain, which cannot blend.
[[nodiscard]] constexpr Plan plan_for(const Stream& stream, const Display& display,
                                      ToneMap preferred = ToneMap::driver,
                                      bool composited = true) noexcept
{
    Plan plan{};
    const bool content_is_hdr = is_hdr_transfer(assumed_transfer(stream));

    if (!content_is_hdr) {
        // **Nothing to map.** SDR content on any display is SDR content; the
        // question §9 asks does not arise, and mapping it anyway is how a
        // player makes a correct picture worse.
        plan.tone_map = ToneMap::none;
        plan.tone_mapping = false;
        plan.format = SwapFormat::fp16_scrgb;
    } else if (display.hdr) {
        // The display can show it. Pass PQ through, and take the cheaper swap
        // chain when there is nothing to blend over it.
        plan.tone_map = ToneMap::none;
        plan.tone_mapping = false;
        plan.format = composited ? SwapFormat::fp16_scrgb : SwapFormat::rgb10_hdr10;
    } else {
        // HDR content, SDR display: §9.1's whole point. Something must map it,
        // and DWM composition will not -- it clips, silently, and everything
        // outside [0, 1] is gone.
        plan.tone_map = preferred == ToneMap::none ? ToneMap::driver : preferred;
        plan.tone_mapping = true;
        plan.format = SwapFormat::fp16_scrgb;
    }

    // §9.6, and it applies whenever the swap chain is scRGB on an HDR display
    // -- including the case where the video needs no mapping at all, because
    // the subtitles still do.
    plan.sdr_scale = display.hdr && plan.format == SwapFormat::fp16_scrgb
                         ? (display.sdr_white_nits > 0.0f ? display.sdr_white_nits / 80.0f
                                                          : 1.0f)
                         : 1.0f;
    return plan;
}

/// The name a person types and reads back.
[[nodiscard]] const char* name_of(ToneMap m) noexcept;
[[nodiscard]] const char* name_of(SwapFormat f) noexcept;
[[nodiscard]] bool tone_map_from_name(const char* name, ToneMap& out) noexcept;

} // namespace mp::video

#endif // MEDIAPERCH_VIDEO_COLOUR_PLAN_HPP
