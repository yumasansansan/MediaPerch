// SPDX-License-Identifier: GPL-3.0-or-later

#include "mix.hpp"

#include <mediaperch/module.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace mp::mix {
namespace {

/// Every position this program knows, in the order WAVE puts them in a frame --
/// which is the order of the bits themselves, and is why the mask is enough to
/// say which channel is which.
constexpr std::uint32_t k_positions[] = {
    MP_SPEAKER_FRONT_LEFT,  MP_SPEAKER_FRONT_RIGHT, MP_SPEAKER_FRONT_CENTER,
    MP_SPEAKER_LOW_FREQUENCY, MP_SPEAKER_BACK_LEFT, MP_SPEAKER_BACK_RIGHT,
    MP_SPEAKER_SIDE_LEFT,   MP_SPEAKER_SIDE_RIGHT,
};

std::vector<std::uint32_t> positions_of(std::uint32_t mask)
{
    std::vector<std::uint32_t> out;
    for (const std::uint32_t bit : k_positions) {
        if ((mask & bit) != 0) {
            out.push_back(bit);
        }
    }
    return out;
}

double from_db(double db) noexcept
{
    return db <= -400.0 ? 0.0 : std::pow(10.0, db / 20.0);
}

bool is_left(std::uint32_t speaker) noexcept
{
    return speaker == MP_SPEAKER_FRONT_LEFT || speaker == MP_SPEAKER_BACK_LEFT ||
           speaker == MP_SPEAKER_SIDE_LEFT;
}

bool is_right(std::uint32_t speaker) noexcept
{
    return speaker == MP_SPEAKER_FRONT_RIGHT || speaker == MP_SPEAKER_BACK_RIGHT ||
           speaker == MP_SPEAKER_SIDE_RIGHT;
}

bool is_surround(std::uint32_t speaker) noexcept
{
    return speaker == MP_SPEAKER_BACK_LEFT || speaker == MP_SPEAKER_BACK_RIGHT ||
           speaker == MP_SPEAKER_SIDE_LEFT || speaker == MP_SPEAKER_SIDE_RIGHT;
}

} // namespace

std::uint32_t conventional_mask(std::uint32_t channels) noexcept
{
    switch (channels) {
    case 1:
        return MP_SPEAKER_FRONT_CENTER;
    case 2:
        return MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT;
    case 4:
        return MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT | MP_SPEAKER_BACK_LEFT |
               MP_SPEAKER_BACK_RIGHT;
    case 6:
        return MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT | MP_SPEAKER_FRONT_CENTER |
               MP_SPEAKER_LOW_FREQUENCY | MP_SPEAKER_SIDE_LEFT | MP_SPEAKER_SIDE_RIGHT;
    case 8:
        return MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT | MP_SPEAKER_FRONT_CENTER |
               MP_SPEAKER_LOW_FREQUENCY | MP_SPEAKER_BACK_LEFT | MP_SPEAKER_BACK_RIGHT |
               MP_SPEAKER_SIDE_LEFT | MP_SPEAKER_SIDE_RIGHT;
    default:
        return 0;
    }
}

bool Matrix::identity() const noexcept
{
    if (inputs != outputs) {
        return false;
    }
    for (std::uint32_t o = 0; o < outputs; ++o) {
        for (std::uint32_t i = 0; i < inputs; ++i) {
            const double want = o == i ? 1.0 : 0.0;
            if (std::abs(at(o, i) * scale - want) > 1e-12) {
                return false;
            }
        }
    }
    return true;
}

std::string Matrix::text() const
{
    std::string out;
    char number[32];
    for (std::uint32_t o = 0; o < outputs; ++o) {
        if (o != 0) {
            out += ';';
        }
        for (std::uint32_t i = 0; i < inputs; ++i) {
            if (i != 0) {
                out += ',';
            }
            std::snprintf(number, sizeof(number), "%.4f", at(o, i) * scale);
            out += number;
        }
    }
    return out;
}

bool normalise_from_name(const std::string& name, Normalise& out)
{
    if (name == "none") {
        out = Normalise::none;
    } else if (name == "peak") {
        out = Normalise::peak;
    } else if (name == "energy") {
        out = Normalise::energy;
    } else {
        return false;
    }
    return true;
}

const char* normalise_name(Normalise n) noexcept
{
    switch (n) {
    case Normalise::none:
        return "none";
    case Normalise::peak:
        return "peak";
    case Normalise::energy:
        break;
    }
    return "energy";
}

bool parse_matrix(const std::string& text, std::uint32_t inputs, std::uint32_t outputs,
                  Matrix& out, std::string& why)
{
    out.inputs = inputs;
    out.outputs = outputs;
    out.scale = 1.0;
    out.coefficients.assign(static_cast<std::size_t>(inputs) * outputs, 0.0);

    std::uint32_t row = 0;
    std::uint32_t column = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t next = text.find_first_of(",;", at);
        const std::string number =
            text.substr(at, next == std::string::npos ? std::string::npos : next - at);
        const char* start = number.c_str();
        char* end = nullptr;
        const double value = std::strtod(start, &end);
        if (end == start || !std::isfinite(value)) {
            why = "`" + number + "` is not a number";
            return false;
        }
        if (row >= outputs || column >= inputs) {
            why = "the matrix has more numbers than " + std::to_string(outputs) + " by " +
                  std::to_string(inputs);
            return false;
        }
        out.coefficients[static_cast<std::size_t>(row) * inputs + column] = value;
        ++column;
        if (next == std::string::npos) {
            break;
        }
        if (text[next] == ';') {
            if (column != inputs) {
                why = "row " + std::to_string(row + 1) + " has " + std::to_string(column) +
                      " numbers where the input has " + std::to_string(inputs) +
                      " channels";
                return false;
            }
            ++row;
            column = 0;
        }
        at = next + 1;
    }
    if (row + 1 != outputs || column != inputs) {
        why = "the matrix needs " + std::to_string(outputs) + " rows of " +
              std::to_string(inputs) + ", separated by semicolons";
        return false;
    }
    return true;
}

bool build(std::uint32_t in_channels, std::uint32_t in_mask, std::uint32_t out_channels,
           std::uint32_t out_mask, const Recipe& recipe, Matrix& out, std::string& why)
{
    if (in_channels == 0 || out_channels == 0) {
        why = "a mix needs channels at both ends";
        return false;
    }
    if (in_mask == 0) {
        in_mask = conventional_mask(in_channels);
    }
    if (out_mask == 0) {
        out_mask = conventional_mask(out_channels);
    }
    // A count with no conventional layout is refused rather than guessed at:
    // three channels could be L/R/C or L/R/S, and putting the audio in the
    // wrong speaker is not better than saying so.
    if (in_mask == 0) {
        why = std::to_string(in_channels) +
              " channels have no conventional layout; give the input mask";
        return false;
    }
    if (out_mask == 0) {
        why = std::to_string(out_channels) +
              " channels have no conventional layout; give the output mask";
        return false;
    }

    const std::vector<std::uint32_t> from = positions_of(in_mask);
    const std::vector<std::uint32_t> to = positions_of(out_mask);
    if (from.size() != in_channels) {
        why = "the input mask names " + std::to_string(from.size()) + " speakers for " +
              std::to_string(in_channels) + " channels";
        return false;
    }
    if (to.size() != out_channels) {
        why = "the output mask names " + std::to_string(to.size()) + " speakers for " +
              std::to_string(out_channels) + " channels";
        return false;
    }

    const double centre = from_db(recipe.centre_db);
    const double surround = from_db(recipe.surround_db);
    const double lfe = from_db(recipe.lfe_db);

    out.inputs = in_channels;
    out.outputs = out_channels;
    out.scale = 1.0;
    out.coefficients.assign(static_cast<std::size_t>(in_channels) * out_channels, 0.0);

    // **The rule the whole matrix follows: a channel goes to its own speaker if
    // that speaker exists downstream, and is distributed only when it does
    // not.** Everything below is what "distributed" means for each position;
    // getting this backwards is what makes a 5.1 file played on 5.1 equipment
    // come out with the centre smeared into the front pair.
    const auto find = [&](std::uint32_t speaker) -> std::uint32_t {
        for (std::uint32_t o = 0; o < out_channels; ++o) {
            if (to[o] == speaker) {
                return o;
            }
        }
        return out_channels; // absent
    };
    const auto add = [&](std::uint32_t o, std::uint32_t i, double value) {
        if (o < out_channels && value != 0.0) {
            out.coefficients[static_cast<std::size_t>(o) * in_channels + i] += value;
        }
    };

    const std::uint32_t front_left = find(MP_SPEAKER_FRONT_LEFT);
    const std::uint32_t front_right = find(MP_SPEAKER_FRONT_RIGHT);
    const std::uint32_t front_centre = find(MP_SPEAKER_FRONT_CENTER);
    const bool has_front_pair = front_left < out_channels || front_right < out_channels;

    for (std::uint32_t i = 0; i < in_channels; ++i) {
        const std::uint32_t source = from[i];
        const std::uint32_t home = find(source);
        if (home < out_channels) {
            add(home, i, 1.0);
            continue;
        }

        if (source == MP_SPEAKER_LOW_FREQUENCY) {
            // Dropped unless asked for, and then only into what is in front of
            // the listener.
            if (has_front_pair) {
                add(front_left, i, lfe);
                add(front_right, i, lfe);
            } else {
                add(front_centre, i, lfe);
            }
        } else if (source == MP_SPEAKER_FRONT_CENTER) {
            if (has_front_pair) {
                add(front_left, i, centre);
                add(front_right, i, centre);
            }
            // With no front pair and no centre there is nowhere for it to go,
            // and silently inventing a destination would be worse than losing
            // it audibly.
        } else if (is_surround(source)) {
            // The nearest surround on its own side, then the front on its own
            // side, then whatever front there is.
            const std::uint32_t other = is_left(source)
                                            ? (find(MP_SPEAKER_SIDE_LEFT) < out_channels
                                                   ? find(MP_SPEAKER_SIDE_LEFT)
                                                   : find(MP_SPEAKER_BACK_LEFT))
                                            : (find(MP_SPEAKER_SIDE_RIGHT) < out_channels
                                                   ? find(MP_SPEAKER_SIDE_RIGHT)
                                                   : find(MP_SPEAKER_BACK_RIGHT));
            if (other < out_channels) {
                add(other, i, 1.0);
            } else if (has_front_pair) {
                add(is_left(source) ? front_left : front_right, i, surround);
            } else {
                add(front_centre, i, surround);
            }
        } else {
            // A front left or right with no front left or right downstream,
            // which is a fold to mono.
            add(front_centre, i, centre);
        }
    }

    if (recipe.synthesise) {
        // Only the channels nothing reached, and never the effects channel: an
        // LFE derived from full-range content is a crossover with an opinion
        // about a frequency, not a matrix.
        for (std::uint32_t o = 0; o < out_channels; ++o) {
            bool empty = true;
            for (std::uint32_t i = 0; i < in_channels; ++i) {
                empty = empty && out.at(o, i) == 0.0;
            }
            if (!empty || to[o] == MP_SPEAKER_LOW_FREQUENCY) {
                continue;
            }
            for (std::uint32_t i = 0; i < in_channels; ++i) {
                const std::uint32_t source = from[i];
                if (to[o] == MP_SPEAKER_FRONT_CENTER &&
                    (source == MP_SPEAKER_FRONT_LEFT || source == MP_SPEAKER_FRONT_RIGHT)) {
                    add(o, i, centre);
                } else if (is_surround(to[o]) &&
                           ((is_left(source) && is_left(to[o])) ||
                            (is_right(source) && is_right(to[o])))) {
                    add(o, i, surround);
                }
            }
        }
    }

    // One scale for the whole matrix. Per row would be arithmetically tidier
    // and would move the stereo image, which is worse than being quiet.
    if (recipe.normalise != Normalise::none) {
        double worst = 0.0;
        for (std::uint32_t o = 0; o < out_channels; ++o) {
            double sum = 0.0;
            for (std::uint32_t i = 0; i < in_channels; ++i) {
                const double c = out.at(o, i);
                sum += recipe.normalise == Normalise::peak ? std::abs(c) : c * c;
            }
            worst = std::max(worst, recipe.normalise == Normalise::peak ? sum
                                                                       : std::sqrt(sum));
        }
        if (worst > 1.0) {
            out.scale = 1.0 / worst;
        }
    }
    return true;
}

} // namespace mp::mix
