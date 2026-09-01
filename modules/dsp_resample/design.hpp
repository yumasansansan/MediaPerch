// SPDX-License-Identifier: GPL-3.0-or-later
//
// Designing the prototype, and then checking that the design is what was asked
// for.
//
// Three methods live here, and the difference between them is not a preference.
//
// **The window method** multiplies the ideal lowpass -- a sinc -- by a window.
// It is closed-form, unconditionally stable, and works at any length. Its limit
// is structural: the passband ripple and the stopband ripple come from the same
// convolution with the window's own spectrum, so they are forced to be roughly
// equal. A resampler wants them wildly unequal (a thousandth of a decibel in
// the passband, a hundred and sixty in the stopband) and this method cannot say
// so. Kaiser's window is itself a closed-form approximation to the Slepian
// (DPSS) window, which is the optimum of this family -- the gap is a fraction of
// a decibel, which is why there is no `window=dpss` here and why the effort went
// into the other method instead.
//
// **Parks-McClellan** minimises the *weighted* maximum error over the whole
// filter, and the alternation theorem says the answer is the best any filter of
// that length can do. That buys two things the window method has no way to
// offer: independent passband and stopband weights, and 20 to 30 per cent fewer
// taps for the same specification. It has one real limitation and this file
// does not hide it -- the Lagrange interpolation at the heart of the exchange
// loses conditioning somewhere past a thousand extremal points, so a prototype
// of 25,280 taps (which is what 44100 -> 48000 asks for) is out of reach and is
// refused rather than attempted.
//
// **Alternating projection** is what is left when Parks-McClellan will not fit.
// Clip the response to the mask, put the filter back to its own length, repeat:
// two constraints, projected onto in turn, converging towards the same
// equiripple answer at two FFTs a round. It is not proved optimal the way
// Parks-McClellan is, and it does not need to be -- it is measured, and what it
// buys over a Kaiser window at the same length is a number rather than a claim.
//
// Deliberately not here, and why. **DPSS** (the Slepian window Kaiser
// approximates) is the window family's true optimum, and the gap to Kaiser is a
// fraction of a decibel -- the effort went to changing method instead, which is
// worth tens. **Multistage decomposition** would cut 160/147 into cascaded
// stages and save most of the multiplies; that is an efficiency question rather
// than a fidelity one, and this filter already runs 45 seconds of 768 kHz
// without an underrun. **Minimum phase** would halve the taps for the same
// magnitude and remove the pre-ringing, at the cost of the property the graph
// currently relies on -- that output frame k lines up with input frame
// k * down / up -- so it is a change to the alignment contract and not a flag.
//
// Every design is measured against its own specification before it is returned.
// An iterative algorithm that quietly converges to the wrong answer is the exact
// failure this is for.

#ifndef MEDIAPERCH_RESAMPLE_DESIGN_HPP
#define MEDIAPERCH_RESAMPLE_DESIGN_HPP

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace mp::resample {

enum class Window : std::uint32_t {
    /// The default. A closed-form near-optimum, stable at any length.
    kaiser,
    /// Every sidelobe at exactly the attenuation asked for, and the narrowest
    /// mainlobe that allows. The sidelobes do not decay, so the far stopband
    /// stops improving -- and the window has spikes at both ends, which is a
    /// real effect and is why this is measured rather than assumed.
    dolph,
};

enum class Method : std::uint32_t {
    /// sinc times a window.
    window,
    /// Parks-McClellan. Optimal, and refused where it cannot be trusted.
    remez,
    /// Alternating projection, starting from the window design: clip the
    /// response to a mask, put the filter back to its own length, repeat. It
    /// converges towards the same equiripple answer Parks-McClellan computes
    /// directly, costs two FFTs a round instead of an interpolation over ten
    /// thousand nodes, and is therefore the only method here that can improve
    /// on a window at the length 44100 -> 48000 actually needs.
    refine,
};

/// What the filter has to do. Everything a caller can ask for is here.
struct Design {
    Method method = Method::window;
    Window window = Window::kaiser;

    /// Stopband attenuation, in dB. Also the aliasing floor.
    double attenuation_db = 120.0;
    /// Passband ripple, in dB, as a peak deviation from unity. Zero means "the
    /// same as the stopband", which is all the window method can do anyway.
    /// Parks-McClellan is the only method here that can spend the difference.
    double passband_ripple_db = 0.0;
    /// Where the passband ends, as a fraction of the lower of the two Nyquist
    /// frequencies. The rest is the transition band.
    double bandwidth = 0.95;

    /// Taps per phase, or 0 to derive it from the specification above. This is
    /// the brute-force axis: fix the attenuation, spend more multiplies, and
    /// the transition band narrows to match.
    std::uint32_t taps = 0;
    /// The prototype's ceiling, in coefficients.
    std::uint32_t max_taps = 1u << 22;
    /// Refuse a design that misses its own specification, buying the shortfall
    /// in taps first where the length was left open. Parks-McClellan is always
    /// checked against its own deviation whatever this says: an exchange that
    /// converged to one answer and a recovery that built a different filter are
    /// two failures that look identical from outside.
    bool verify = false;

    friend bool operator==(const Design&, const Design&) = default;
};

[[nodiscard]] bool method_from_name(const std::string& name, Method& out);
[[nodiscard]] bool window_from_name(const std::string& name, Window& out);
[[nodiscard]] const char* method_name(Method m) noexcept;
[[nodiscard]] const char* window_name(Window w) noexcept;

/// What a filter actually does, read off its response rather than assumed.
struct Response {
    /// Peak deviation from unity in the passband, in dB. Positive.
    double passband_ripple_db = 0.0;
    /// The worst thing left in the stopband, in dB. Negative.
    double stopband_db = 0.0;
    /// How many points the response was sampled at.
    std::size_t points = 0;
};

/// Samples |H| on a grid and reports the two numbers a specification is made of.
///
/// `gain` is what the passband is compared against -- `up` for a polyphase
/// prototype, whose DC gain is the interpolation factor.
[[nodiscard]] Response measure(const std::vector<double>& h, double passband_edge,
                               double stopband_edge, double gain);

/// The prototype for a polyphase resampler.
///
/// Length is `taps * up + 1`, odd, symmetric about `taps * up / 2` -- an integer
/// centre, which is what makes a 1:1 ratio come out as a unit impulse. The extra
/// tap belongs to phase 0; every other phase has one fewer and is padded, which
/// costs one multiply per output sample and keeps every phase the same length.
///
/// Returns false with `why` filled in when the design cannot be done, or when it
/// was done and missed its specification.
[[nodiscard]] bool design_prototype(const Design& design, std::uint32_t up,
                                    std::uint32_t down, std::vector<double>& out,
                                    std::uint32_t& taps, Response& achieved,
                                    std::string& why);

// --- the pieces, exposed because the tests measure them separately ----------

/// In-place radix-2 FFT. `a.size()` must be a power of two.
void fft(std::vector<std::complex<double>>& a, bool inverse);

/// The DFT of any length, via Bluestein's chirp-z. Needed because a Dolph
/// window is as long as the filter and filter lengths are not powers of two.
void dft_any(std::vector<std::complex<double>>& a);

/// A Kaiser window of `length` points for the given stopband attenuation.
[[nodiscard]] std::vector<double> kaiser_window(std::size_t length, double attenuation_db);
/// A Dolph-Chebyshev window: every sidelobe at exactly `attenuation_db`.
[[nodiscard]] std::vector<double> dolph_window(std::size_t length, double attenuation_db);

/// Parks-McClellan for a Type I (odd length, symmetric) lowpass.
///
/// `weight_pass` and `weight_stop` are the reciprocals of the ripples allowed
/// in each band, so a stopband a hundred times tighter than the passband gets a
/// weight a hundred times larger. `deviation` comes back as the weighted error
/// the exchange settled on.
[[nodiscard]] bool remez_lowpass(std::size_t length, double passband_edge,
                                 double stopband_edge, double weight_pass,
                                 double weight_stop, std::vector<double>& h,
                                 double& deviation, std::string& why);

} // namespace mp::resample

#endif // MEDIAPERCH_RESAMPLE_DESIGN_HPP
