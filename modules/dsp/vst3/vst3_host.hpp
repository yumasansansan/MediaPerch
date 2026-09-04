// SPDX-License-Identifier: GPL-3.0-or-later
//
// A VST3 host, as much of one as an audio effect needs.
//
// **This is the first module in the tree that runs somebody else's code.**
// Everything else here is a decoder or a filter this project wrote or vendored;
// a VST3 is an arbitrary DLL, written against an interface Steinberg defines,
// doing whatever it likes to the samples. That difference is the reason the
// hosting is a class of its own rather than living in the vtable file: the
// parts that decide *what to load* and *what to tell the plugin about the
// stream* are ordinary logic and are tested as such, and the part that calls
// into the plugin is small enough to read in one sitting.
//
// The SDK is `pluginterfaces` only -- four .cpp files and 690 KB of headers,
// MIT-licensed since October 2025, which is what makes it usable from a GPLv3
// program at all. `public.sdk`, which is where a normal host gets its module
// loader and its `HostApplication`, is not here: it is another 40,000 lines to
// do things this module does in four hundred, and it drags in a build system
// with opinions.
//
// **The bus is f64 and so is VST3's, which is the happy part.** A VST3 declares
// whether it can process `kSample64`, and a plugin that can gets this tree's
// deinterleaved doubles handed to it with no conversion whatsoever. One that
// cannot gets f32 scratch buffers, and `describe` says which of the two
// happened, because it is the difference between a stage that is exact and one
// that is not.

#ifndef MEDIAPERCH_DSP_VST3_HOST_HPP
#define MEDIAPERCH_DSP_VST3_HOST_HPP

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mp::vst3 {

class ParameterChanges;

// --------------------------------------------------------------------------
// The two decisions that are ordinary logic, and are tested as such
// --------------------------------------------------------------------------

/// The DLL inside what a person typed.
///
/// A `.vst3` on Windows is one of two things and they look alike from the
/// outside: a plain DLL that happens to end in `.vst3`, which is how they were
/// shipped before 3.6.10, or a **bundle** -- a *directory* ending in `.vst3`
/// with the binary at `Contents/x86_64-win/<name>.vst3` inside it. Every plugin
/// installed in the last decade is the second kind, and `LoadLibrary` on the
/// directory fails with a message about a bad image.
///
/// Returns the path to hand `LoadLibrary`, which for a plain DLL is the input
/// unchanged. Says nothing about whether it exists: that is the caller's error
/// to report, with the name the person typed in it.
[[nodiscard]] std::wstring binary_in_bundle(const std::wstring& path);

/// The speaker arrangement for a stream this tree is carrying.
///
/// **A `SpeakerArrangement` is a `WAVEFORMATEXTENSIBLE` channel mask.** Not
/// merely similar: all eighteen standard positions are the same bit in the same
/// place, from `kSpeakerL` at bit 0 through `kSpeakerTrr` at bit 17, because
/// VST3 took the layout Microsoft had already published. A mask therefore
/// passes straight through, and the first version of this function was a table
/// that translated it into itself -- a test comparing `k71Music` against 0x63F
/// is what said so.
///
/// The work is the other case, and it is the common one here: `Format` allows
/// `channel_mask == 0`, meaning the non-extensible form, which is what a WAV
/// header without an extension chunk and most decoders give. A plugin still has
/// to be told *something*, so the conventional layout for that many channels is
/// named -- mono, stereo, 5.1, 7.1 -- and past eight the lowest `channels` bits
/// stand in, which is at least a mask with the right number of speakers in it.
[[nodiscard]] Steinberg::Vst::SpeakerArrangement arrangement_for(std::uint32_t channels,
                                                                std::uint32_t wave_mask);

/// How many channels an arrangement has. `std::popcount`, named.
[[nodiscard]] std::uint32_t channels_in(Steinberg::Vst::SpeakerArrangement arrangement);

// --------------------------------------------------------------------------
// The part that talks to the plugin
// --------------------------------------------------------------------------

/// One loaded plugin, from `LoadLibrary` to `process`.
///
/// Not copyable and not movable: it owns a module handle, two reference-counted
/// COM-ish objects and the connection between them, and the order they are let
/// go in is part of the VST3 contract rather than a detail.
class Host {
public:
    /// Both out of line, because `changes_` points at a type this header only
    /// forward-declares: an inline constructor makes the compiler instantiate
    /// the destructor at every construction site, where the type is still
    /// incomplete.
    Host();
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    /// Loads the binary and finds an audio effect in it.
    ///
    /// `which` picks between several: a class index, or a substring of the
    /// class name matched case-insensitively. Empty takes the first audio
    /// effect, which is what nearly every plugin has exactly one of.
    [[nodiscard]] bool load(const std::string& utf8_path, const std::string& which,
                            std::string& why);

    /// Tells the plugin about the stream and starts it processing.
    ///
    /// Idempotent in the sense that calling it again reconfigures: the graph
    /// calls `configure` twice, once to find out what comes out and once with
    /// the block the device turned out to want.
    [[nodiscard]] bool configure(std::uint32_t channels, std::uint32_t wave_mask,
                                 double sample_rate, std::uint32_t max_frames,
                                 std::string& why);

    /// One block, deinterleaved, in this tree's f64. `in` and `out` may be the
    /// same pointers; VST3 permits in-place and this tree does not use it.
    [[nodiscard]] bool process(const double* const* in, std::uint32_t frames,
                               double* const* out);

    /// A block of silence, for draining a tail. Same shape as `process`.
    [[nodiscard]] bool process_silence(std::uint32_t frames, double* const* out);

    /// A parameter, by id or by a substring of its title, normalised 0..1.
    [[nodiscard]] bool set_parameter(const std::string& which, double normalised,
                                     std::string& why);

    /// The plugin's own state, as `IComponent::setState` takes it. This is what
    /// a `.vstpreset` file's `Comp` chunk holds, and the only way to reach a
    /// setting that is not an automatable parameter.
    [[nodiscard]] bool set_state(const std::vector<std::uint8_t>& bytes, std::string& why);

    /// Stops processing and deactivates, keeping the plugin loaded. `configure`
    /// puts it back.
    void deactivate();

    /// Forgets what the stream was, for a seek. VST3 has no reset: the
    /// documented way is to deactivate and reactivate, which is what this does.
    [[nodiscard]] bool reset();

    // -- what it turned out to be, for `describe` ---------------------------
    [[nodiscard]] bool loaded() const noexcept { return processor_ != nullptr; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& vendor() const noexcept { return vendor_; }
    [[nodiscard]] const std::string& version() const noexcept { return version_; }
    [[nodiscard]] const std::string& subcategories() const noexcept { return categories_; }
    [[nodiscard]] bool native_f64() const noexcept { return sample_size64_; }
    [[nodiscard]] std::uint32_t latency_frames() const noexcept { return latency_; }
    /// Frames the plugin keeps producing after the input stops. `0` means none;
    /// `k_infinite_tail` means it never stops, and something has to choose.
    [[nodiscard]] std::uint32_t tail_frames() const noexcept { return tail_; }
    /// How many parameters, and one of them by index.
    [[nodiscard]] std::uint32_t parameter_count() const noexcept;
    /// `title`, its current normalised value, and the plugin's own text for it.
    [[nodiscard]] bool parameter(std::uint32_t index, std::string& title, double& value,
                                 std::string& shown) const;

    static constexpr std::uint32_t k_infinite_tail = 0xFFFFFFFFu;

private:
    void unload();

    void* library_ = nullptr; ///< HMODULE, kept opaque so the header is portable
    Steinberg::IPluginFactory* factory_ = nullptr;
    Steinberg::Vst::IComponent* component_ = nullptr;
    Steinberg::Vst::IAudioProcessor* processor_ = nullptr;
    Steinberg::Vst::IEditController* controller_ = nullptr;
    /// True when the controller is the component wearing another hat, in which
    /// case it is not separately released and not connected to itself.
    bool single_component_ = false;

    bool active_ = false;
    bool sample_size64_ = false;
    std::uint32_t channels_ = 0;
    std::uint32_t max_frames_ = 0;
    double sample_rate_ = 0.0;
    std::uint32_t latency_ = 0;
    std::uint32_t tail_ = 0;

    std::string name_;
    std::string vendor_;
    std::string version_;
    std::string categories_;

    /// f32 scratch, only when the plugin cannot take doubles. One block per
    /// channel each way, allocated in `configure` and never after.
    std::vector<float> in32_;
    std::vector<float> out32_;
    std::vector<float*> in32_ptr_;
    std::vector<float*> out32_ptr_;
    /// f64 output, because a plugin may write in place and this tree's caller
    /// hands out and in as separate arrays it does not expect to be swapped.
    std::vector<double> out64_;
    std::vector<double*> out64_ptr_;
    /// Non-const because `AudioBusBuffers` has no const form; see `process`.
    std::vector<double*> in64_ptr_;
    /// Silence for the tail, one block, all zeros.
    std::vector<double> silence_;
    std::vector<const double*> silence_ptr_;

    /// Parameters set since the last block: id and normalised value.
    std::vector<std::pair<std::uint32_t, double>> pending_;
    /// The queues handed to the plugin each block. A member and not a static:
    /// two VST3 stages in one chain is the ordinary case, and a shared one
    /// would give each of them the other's parameter changes.
    std::unique_ptr<ParameterChanges> changes_;
};

} // namespace mp::vst3

#endif // MEDIAPERCH_DSP_VST3_HOST_HPP
