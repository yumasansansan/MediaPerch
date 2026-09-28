// SPDX-License-Identifier: GPL-3.0-or-later
//
// The module ABI's handles, completed once for the test program.
//
// A handle is an opaque pointer that a module hands out and is handed back,
// and every module completes its type with what it keeps. The fakes here are
// modules of a kind, and they handed out pointers to other things -- their own
// state, a log, an int -- as pointers to the handle's type, and turned them
// back on the way in: a pointer to one type standing for another, which a
// test has no more business doing than a module. A type is completed once in a
// program, and several fakes in this one hand out the same kind of handle, so
// each type is completed here, for all of them; mediaperch_tests loads the
// real modules as libraries, and none of their sources is compiled into it.

#ifndef MEDIAPERCH_TESTS_FAKE_HANDLES_HPP
#define MEDIAPERCH_TESTS_FAKE_HANDLES_HPP

#include <mediaperch/module.h>

#include <cstddef>
#include <cstdint>
#include <vector>

/// A DSP stage of this program: dsp_test.cpp's stages, and fake_dsp.hpp's one,
/// each with the fields that are its own.
struct MpDsp {
    // dsp_test.cpp: a stage that does real work of one of these kinds.
    enum class Kind { gain, upsample2, delay, refuse };

    Kind kind = Kind::gain;
    double gain = 0.5;
    std::uint32_t channels = 0;
    std::uint32_t capacity = 0;
    /// `delay` only: one line per channel, and where in it we are.
    std::vector<std::vector<double>> line;
    std::size_t at = 0;
    std::uint32_t held = 0;

    // fake_dsp.hpp: a stage that only multiplies, and what it was configured
    // with. Its own object for each stage opened, which is what makes two
    // stages in one chain two things rather than one.
    double amount = 1.0;
    MpFormat format{};
};

/// Handles that the fakes only hand out and take back. A pointer to one of
/// these is distinct from null, which is all the ABI promises about a handle,
/// and nothing looks through it: what the fakes remember is in their logs.
struct MpVideo {};
struct MpVideoCodec {};
struct MpVideoDsp {};
struct MpDemux {};
struct MpCodecInstance {};

#endif // MEDIAPERCH_TESTS_FAKE_HANDLES_HPP
