// SPDX-License-Identifier: GPL-3.0-or-later

#include "transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mp::transform {
namespace {

constexpr double k_pi = 3.14159265358979323846;

} // namespace

std::size_t next_power_of_two(std::size_t n) noexcept
{
    std::size_t k = 1;
    while (k < n) {
        k <<= 1;
    }
    return k;
}

void fft(std::vector<std::complex<double>>& a, bool inverse)
{
    const std::size_t n = a.size();
    if (n < 2) {
        return;
    }
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * k_pi / static_cast<double>(len);
        const std::complex<double> step{std::cos(angle), std::sin(angle)};
        for (std::size_t at = 0; at < n; at += len) {
            std::complex<double> w{1.0, 0.0};
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[at + k];
                const std::complex<double> v = a[at + k + len / 2] * w;
                a[at + k] = u + v;
                a[at + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
    if (inverse) {
        for (auto& value : a) {
            value /= static_cast<double>(n);
        }
    }
}

void dft_any(std::vector<std::complex<double>>& a)
{
    const std::size_t n = a.size();
    if (n < 2) {
        return;
    }
    if ((n & (n - 1)) == 0) {
        fft(a, false);
        return;
    }

    // Bluestein: n*k = (n^2 + k^2 - (k-n)^2) / 2 turns a transform of any length
    // into a convolution, which a power-of-two FFT can do.
    const std::size_t m = next_power_of_two(2 * n - 1);
    std::vector<std::complex<double>> x(m, {0.0, 0.0});
    std::vector<std::complex<double>> y(m, {0.0, 0.0});

    const auto chirp = [n](std::size_t i) {
        // fmod keeps the argument small: i*i overflows the exactly-representable
        // range of a double long before n does.
        const double phase =
            k_pi * std::fmod(static_cast<double>(i) * static_cast<double>(i),
                             2.0 * static_cast<double>(n)) /
            static_cast<double>(n);
        return std::complex<double>{std::cos(phase), std::sin(phase)};
    };

    for (std::size_t i = 0; i < n; ++i) {
        x[i] = a[i] * std::conj(chirp(i));
        y[i] = chirp(i);
        if (i != 0) {
            y[m - i] = chirp(i);
        }
    }
    fft(x, false);
    fft(y, false);
    for (std::size_t i = 0; i < m; ++i) {
        x[i] *= y[i];
    }
    fft(x, true);
    for (std::size_t i = 0; i < n; ++i) {
        a[i] = x[i] * std::conj(chirp(i));
    }
}

void to_minimum_phase(std::vector<double>& h, double floor_db, unsigned oversample)
{
    const std::size_t length = h.size();
    if (length < 2) {
        return;
    }
    // Room for the cepstrum to decay in. Too little and it wraps, which shows
    // up as a filter whose magnitude is not the one it was given.
    const std::size_t factor = std::max<std::size_t>(oversample, 2);
    const std::size_t n =
        std::min<std::size_t>(next_power_of_two(length * factor), std::size_t{1} << 22);

    std::vector<std::complex<double>> spectrum(n, {0.0, 0.0});
    for (std::size_t i = 0; i < length; ++i) {
        spectrum[i] = {h[i], 0.0};
    }
    fft(spectrum, false);

    double peak = 0.0;
    for (const auto& value : spectrum) {
        peak = std::max(peak, std::abs(value));
    }
    const double floor = peak * std::pow(10.0, floor_db / 20.0);
    for (auto& value : spectrum) {
        value = {std::log(std::max(std::abs(value), floor)), 0.0};
    }

    // The real cepstrum, folded onto its causal half. Everything a minimum
    // phase filter is follows from that fold.
    fft(spectrum, true);
    for (std::size_t k = 1; k < n / 2; ++k) {
        spectrum[k] *= 2.0;
        spectrum[n - k] = {0.0, 0.0};
    }
    fft(spectrum, false);
    for (auto& value : spectrum) {
        value = std::exp(value);
    }
    fft(spectrum, true);

    for (std::size_t i = 0; i < length; ++i) {
        h[i] = spectrum[i].real();
    }
}

} // namespace mp::transform
