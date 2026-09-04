// SPDX-License-Identifier: GPL-3.0-or-later
#include "vst3_host.hpp"

#include "vst3_hostapp.hpp"

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/vstspeaker.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <iterator>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace mp::vst3 {

using namespace Steinberg;

namespace {

/// The host objects, one set for the process. VST3 hands these to every plugin
/// and expects them to outlive it; a static is the honest way to say so, and
/// they hold no per-plugin state.
HostApplication& host_application()
{
    static HostApplication app;
    return app;
}

ComponentHandler& component_handler()
{
    static ComponentHandler handler;
    return handler;
}

std::string narrow(const Vst::TChar* utf16)
{
    if (utf16 == nullptr) {
        return {};
    }
    int chars = 0;
    while (utf16[chars] != 0 && chars < 4096) {
        ++chars;
    }
    if (chars == 0) {
        return {};
    }
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(utf16),
                                            chars, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(utf16), chars, out.data(),
                          bytes, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int chars = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                            static_cast<int>(utf8.size()), nullptr, 0);
    if (chars <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(),
                          chars);
    return out;
}

/// Case-insensitive substring, for matching a class or a parameter by a piece
/// of its name. ASCII only, deliberately: a plugin's title may be anything and
/// a fold that guessed at a locale would match differently on two machines.
bool contains_fold(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) {
        return true;
    }
    const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string a;
    a.reserve(haystack.size());
    std::transform(haystack.begin(), haystack.end(), std::back_inserter(a), lower);
    std::string b;
    b.reserve(needle.size());
    std::transform(needle.begin(), needle.end(), std::back_inserter(b), lower);
    return a.find(b) != std::string::npos;
}

/// A read-only `IBStream` over bytes somebody already has, for `setState`.
class MemoryStream final : public U::Implements<U::Directly<IBStream>> {
public:
    explicit MemoryStream(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override
    {
        const auto left = static_cast<int32>(bytes_.size() - at_);
        const int32 n = std::min(numBytes, left);
        if (n > 0) {
            std::memcpy(buffer, bytes_.data() + at_, static_cast<std::size_t>(n));
            at_ += static_cast<std::size_t>(n);
        }
        if (numBytesRead != nullptr) {
            *numBytesRead = n;
        }
        return kResultOk;
    }
    tresult PLUGIN_API write(void*, int32, int32*) override { return kNotImplemented; }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override
    {
        int64 to = pos;
        if (mode == kIBSeekCur) {
            to += static_cast<int64>(at_);
        } else if (mode == kIBSeekEnd) {
            to += static_cast<int64>(bytes_.size());
        }
        if (to < 0 || to > static_cast<int64>(bytes_.size())) {
            return kResultFalse;
        }
        at_ = static_cast<std::size_t>(to);
        if (result != nullptr) {
            *result = to;
        }
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* pos) override
    {
        if (pos == nullptr) {
            return kInvalidArgument;
        }
        *pos = static_cast<int64>(at_);
        return kResultOk;
    }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t at_ = 0;
};

} // namespace

// --------------------------------------------------------------------------
// The two decisions that are ordinary logic
// --------------------------------------------------------------------------

std::wstring binary_in_bundle(const std::wstring& path)
{
    if (path.empty()) {
        return path;
    }
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return path; // a plain DLL, or something that does not exist yet
    }

    // A bundle. The binary inside has the bundle's own name, and lives under
    // the architecture folder -- `x86_64-win` for 64-bit, which is the only one
    // this program is built as.
    std::wstring trimmed = path;
    while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) {
        trimmed.pop_back();
    }
    const std::size_t slash = trimmed.find_last_of(L"\\/");
    const std::wstring leaf =
        slash == std::wstring::npos ? trimmed : trimmed.substr(slash + 1);
    return trimmed + L"\\Contents\\x86_64-win\\" + leaf;
}

Vst::SpeakerArrangement arrangement_for(std::uint32_t channels, std::uint32_t wave_mask)
{
    // A mask that says what it holds is the answer already: the two
    // vocabularies are the same bits. Checked against the channel count rather
    // than trusted, because a mask and a count that disagree is a stream
    // somebody built by hand, and the count is the one the buffers are sized by.
    if (wave_mask != 0 &&
        static_cast<std::uint32_t>(std::popcount(wave_mask)) == channels) {
        return static_cast<Vst::SpeakerArrangement>(wave_mask);
    }

    // No mask, which is the non-extensible form and most of what arrives here.
    // The conventional layout for that many channels, which is the same guess
    // every host makes and the same one WAVEFORMATEX documents.
    switch (channels) {
    case 1:
        return Vst::SpeakerArr::kMono;
    case 2:
        return Vst::SpeakerArr::kStereo;
    case 3:
        return Vst::SpeakerArr::k30Cine; // L R C
    case 4:
        return Vst::SpeakerArr::k40Music; // L R Ls Rs, the quadraphonic one
    case 5:
        return Vst::SpeakerArr::k50;
    case 6:
        return Vst::SpeakerArr::k51;
    case 7:
        return Vst::SpeakerArr::k61Cine;
    case 8:
        return Vst::SpeakerArr::k71Music; // L R C Lfe Ls Rs Sl Sr
    default:
        break;
    }
    // Nothing named. The lowest `channels` bits is not right, but it is a mask
    // with the right number of speakers in it, which is what the plugin checks.
    // Nine channels or more, with no mask, is a stream this tree has never
    // produced.
    Vst::SpeakerArrangement out = 0;
    for (std::uint32_t c = 0; c < channels && c < 64; ++c) {
        out |= Vst::SpeakerArrangement{1} << c;
    }
    return out;
}

std::uint32_t channels_in(Vst::SpeakerArrangement arrangement)
{
    return static_cast<std::uint32_t>(std::popcount(static_cast<std::uint64_t>(arrangement)));
}

// --------------------------------------------------------------------------
// Host
// --------------------------------------------------------------------------

Host::Host() = default;

Host::~Host()
{
    unload();
}

bool Host::load(const std::string& utf8_path, const std::string& which, std::string& why)
{
    unload();

    const std::wstring binary = binary_in_bundle(widen(utf8_path));
    if (binary.empty()) {
        why = "no plugin path was given";
        return false;
    }

    // The plugin's own directory first: a VST3 that ships helper DLLs beside
    // itself finds them, and nothing else on the search path is consulted --
    // which is also what keeps a plugin from picking up a stray DLL from the
    // working directory.
    auto* module = static_cast<HMODULE>(
        ::LoadLibraryExW(binary.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
    if (module == nullptr) {
        why = "cannot load " + utf8_path + " (error " +
              std::to_string(static_cast<unsigned long>(::GetLastError())) +
              "); a .vst3 is either a DLL or a bundle directory with the DLL "
              "inside it, and neither was there";
        return false;
    }
    library_ = module;

    // `InitDll` is optional and predates the bundle format. A plugin that has
    // one and is not given it may fail later rather than here.
    using InitFn = bool(PLUGIN_API*)();
    if (auto* init = reinterpret_cast<InitFn>(
            reinterpret_cast<void*>(::GetProcAddress(module, "InitDll")));
        init != nullptr && !init()) {
        why = utf8_path + ": InitDll said no";
        unload();
        return false;
    }

    using FactoryFn = IPluginFactory*(PLUGIN_API*)();
    auto* get_factory = reinterpret_cast<FactoryFn>(
        reinterpret_cast<void*>(::GetProcAddress(module, "GetPluginFactory")));
    if (get_factory == nullptr) {
        why = utf8_path + " exports no GetPluginFactory, so it is a DLL but not a VST3";
        unload();
        return false;
    }
    factory_ = get_factory();
    if (factory_ == nullptr) {
        why = utf8_path + " gave back no plugin factory";
        unload();
        return false;
    }

    // A factory that can be told about the host is, before anything is created
    // from it: some plugins read the host's name during class construction.
    if (IPluginFactory3* f3 = nullptr;
        factory_->queryInterface(IPluginFactory3::iid, reinterpret_cast<void**>(&f3)) ==
            kResultOk &&
        f3 != nullptr) {
        f3->setHostContext(host_application().unknownCast());
        f3->release();
    }

    // Find an audio effect. `which` is an index or a piece of a name; empty
    // takes the first, which is what a plugin with one class has.
    const int32 count = factory_->countClasses();
    int32 chosen = -1;
    PClassInfo info{};
    char* end = nullptr;
    const long asked = which.empty() ? -1 : std::strtol(which.c_str(), &end, 10);
    const bool by_index = !which.empty() && end != nullptr && *end == '\0';

    int32 effects_seen = 0;
    std::string available;
    for (int32 i = 0; i < count; ++i) {
        PClassInfo probe{};
        if (factory_->getClassInfo(i, &probe) != kResultOk ||
            std::strcmp(probe.category, kVstAudioEffectClass) != 0) {
            continue;
        }
        if (!available.empty()) {
            available += ", ";
        }
        available += std::to_string(effects_seen) + "=" + probe.name;
        const bool match = by_index ? effects_seen == static_cast<int32>(asked)
                                    : contains_fold(probe.name, which);
        if (chosen < 0 && match) {
            chosen = i;
            info = probe;
        }
        ++effects_seen;
    }
    if (chosen < 0) {
        why = effects_seen == 0
                  ? utf8_path + " has a plugin factory but no audio effect in it"
                  : utf8_path + ": no audio effect matches `" + which + "`; it has " +
                        available;
        unload();
        return false;
    }

    if (factory_->createInstance(info.cid, Vst::IComponent::iid,
                                 reinterpret_cast<void**>(&component_)) != kResultOk ||
        component_ == nullptr) {
        why = std::string{info.name} + " would not be created";
        unload();
        return false;
    }
    if (component_->initialize(host_application().unknownCast()) != kResultOk) {
        why = std::string{info.name} + " refused to initialise";
        unload();
        return false;
    }
    if (component_->queryInterface(Vst::IAudioProcessor::iid,
                                   reinterpret_cast<void**>(&processor_)) != kResultOk ||
        processor_ == nullptr) {
        why = std::string{info.name} + " is an audio effect with no IAudioProcessor";
        unload();
        return false;
    }

    // The controller. Two shapes: one object wearing both hats, or two objects
    // the host has to introduce to each other. The second is the documented
    // one and the first is what most plugins actually do.
    if (component_->queryInterface(Vst::IEditController::iid,
                                   reinterpret_cast<void**>(&controller_)) == kResultOk &&
        controller_ != nullptr) {
        single_component_ = true;
    } else {
        TUID controller_cid{};
        if (component_->getControllerClassId(controller_cid) == kResultOk &&
            factory_->createInstance(controller_cid, Vst::IEditController::iid,
                                     reinterpret_cast<void**>(&controller_)) == kResultOk &&
            controller_ != nullptr) {
            if (controller_->initialize(host_application().unknownCast()) != kResultOk) {
                controller_->release();
                controller_ = nullptr;
            }
        }
    }
    if (controller_ != nullptr) {
        controller_->setComponentHandler(&component_handler());
        if (!single_component_) {
            // Two halves that cannot talk to each other is a plugin whose
            // editor and processor disagree about everything.
            Vst::IConnectionPoint* from = nullptr;
            Vst::IConnectionPoint* to = nullptr;
            if (component_->queryInterface(Vst::IConnectionPoint::iid,
                                           reinterpret_cast<void**>(&from)) == kResultOk &&
                controller_->queryInterface(Vst::IConnectionPoint::iid,
                                            reinterpret_cast<void**>(&to)) == kResultOk) {
                from->connect(to);
                to->connect(from);
            }
            if (from != nullptr) {
                from->release();
            }
            if (to != nullptr) {
                to->release();
            }
        }
    }

    name_ = info.name;
    if (PClassInfo2 info2{}; factory_ != nullptr) {
        IPluginFactory2* f2 = nullptr;
        if (factory_->queryInterface(IPluginFactory2::iid, reinterpret_cast<void**>(&f2)) ==
                kResultOk &&
            f2 != nullptr) {
            if (f2->getClassInfo2(chosen, &info2) == kResultOk) {
                vendor_ = info2.vendor;
                version_ = info2.version;
                categories_ = info2.subCategories;
            }
            f2->release();
        }
    }
    return true;
}

bool Host::configure(std::uint32_t channels, std::uint32_t wave_mask, double sample_rate,
                     std::uint32_t max_frames, std::string& why)
{
    if (processor_ == nullptr || component_ == nullptr) {
        why = "no plugin is loaded";
        return false;
    }
    deactivate();

    // The buses. An effect has one audio input and one audio output; a synth
    // has no input and this host has nothing to feed one, so it says so.
    const int32 ins = component_->getBusCount(Vst::kAudio, Vst::kInput);
    const int32 outs = component_->getBusCount(Vst::kAudio, Vst::kOutput);
    if (ins < 1 || outs < 1) {
        why = name_ + " has " + std::to_string(ins) + " audio inputs and " +
              std::to_string(outs) +
              " outputs; this is a stage in a chain, so it needs one of each";
        return false;
    }

    const Vst::SpeakerArrangement arrangement = arrangement_for(channels, wave_mask);
    Vst::SpeakerArrangement in_arr = arrangement;
    Vst::SpeakerArrangement out_arr = arrangement;
    if (processor_->setBusArrangements(&in_arr, 1, &out_arr, 1) != kResultOk) {
        // Not fatal on its own: the plugin may have kept an arrangement it
        // prefers, and what matters is whether it is the one being handed over.
        Vst::SpeakerArrangement got_in = 0;
        Vst::SpeakerArrangement got_out = 0;
        processor_->getBusArrangement(Vst::kInput, 0, got_in);
        processor_->getBusArrangement(Vst::kOutput, 0, got_out);
        if (channels_in(got_in) != channels || channels_in(got_out) != channels) {
            why = name_ + " will not take " + std::to_string(channels) +
                  " channels in and out; it wants " + std::to_string(channels_in(got_in)) +
                  " in and " + std::to_string(channels_in(got_out)) + " out";
            return false;
        }
    }

    // Every bus on, including any the plugin has beyond the first: a side-chain
    // left inactive is the documented way to say it is not connected, and a
    // plugin reading an inactive bus reads the silence this host provides.
    for (int32 i = 0; i < ins; ++i) {
        component_->activateBus(Vst::kAudio, Vst::kInput, i, i == 0 ? 1 : 0);
    }
    for (int32 i = 0; i < outs; ++i) {
        component_->activateBus(Vst::kAudio, Vst::kOutput, i, i == 0 ? 1 : 0);
    }
    for (int32 i = 0, n = component_->getBusCount(Vst::kEvent, Vst::kInput); i < n; ++i) {
        component_->activateBus(Vst::kEvent, Vst::kInput, i, 0);
    }

    // f64 if the plugin can, which is this tree's bus exactly and no conversion
    // at all. Most cannot, and those get f32 with the loss that implies said
    // out loud in `describe` rather than left to be discovered.
    sample_size64_ =
        processor_->canProcessSampleSize(Vst::kSample64) == kResultTrue;

    Vst::ProcessSetup setup{};
    // kOffline rather than kRealtime: this host renders faster than real time
    // when it can, and a plugin that has a better-sounding offline path should
    // take it. Nothing here has a deadline the way a device callback does.
    setup.processMode = Vst::kOffline;
    setup.symbolicSampleSize = sample_size64_ ? Vst::kSample64 : Vst::kSample32;
    setup.maxSamplesPerBlock = static_cast<int32>(max_frames);
    setup.sampleRate = sample_rate;
    if (processor_->setupProcessing(setup) != kResultOk) {
        why = name_ + " refused " + std::to_string(sample_rate) + " Hz in blocks of " +
              std::to_string(max_frames);
        return false;
    }

    channels_ = channels;
    max_frames_ = max_frames;
    sample_rate_ = sample_rate;

    // Room for every parameter the plugin has, taken now so that the audio path
    // never allocates. 64 is a floor rather than a guess: a plugin with two
    // knobs still gets a queue, and one with three hundred gets three hundred.
    if (changes_ == nullptr) {
        changes_ = std::make_unique<ParameterChanges>();
    }
    changes_->reserve(std::max<std::size_t>(64, parameter_count()));

    const std::size_t block = static_cast<std::size_t>(channels) * max_frames;
    out64_.assign(block, 0.0);
    out64_ptr_.resize(channels);
    in64_ptr_.resize(channels);
    silence_.assign(block, 0.0);
    silence_ptr_.resize(channels);
    for (std::uint32_t c = 0; c < channels; ++c) {
        out64_ptr_[c] = out64_.data() + static_cast<std::size_t>(c) * max_frames;
        silence_ptr_[c] = silence_.data() + static_cast<std::size_t>(c) * max_frames;
    }
    if (!sample_size64_) {
        in32_.assign(block, 0.0F);
        out32_.assign(block, 0.0F);
        in32_ptr_.resize(channels);
        out32_ptr_.resize(channels);
        for (std::uint32_t c = 0; c < channels; ++c) {
            in32_ptr_[c] = in32_.data() + static_cast<std::size_t>(c) * max_frames;
            out32_ptr_[c] = out32_.data() + static_cast<std::size_t>(c) * max_frames;
        }
    }

    if (component_->setActive(true) != kResultOk) {
        why = name_ + " would not activate";
        return false;
    }
    processor_->setProcessing(true);
    active_ = true;

    // Latency and tail are only meaningful once it is set up, and the ABI has
    // somewhere to put the first: `get_latency` is what a second chain would
    // need to line this one up against.
    latency_ = static_cast<std::uint32_t>(processor_->getLatencySamples());
    const uint32 tail = processor_->getTailSamples();
    tail_ = tail == Vst::kInfiniteTail ? k_infinite_tail
            : tail == Vst::kNoTail     ? 0u
                                       : static_cast<std::uint32_t>(tail);
    return true;
}

bool Host::process(const double* const* in, std::uint32_t frames, double* const* out)
{
    if (!active_ || processor_ == nullptr || frames == 0 || frames > max_frames_) {
        return false;
    }

    // A plugin is told twice, and it has to be: `set_parameter` moves the
    // controller, which is what an editor would see, and this queue moves the
    // processor, which is what the samples go through. A host that did only the
    // first has a plugin whose displayed value and its audio disagree.
    changes_->clear();
    for (const auto& [id, value] : pending_) {
        changes_->add(id, value);
    }
    pending_.clear();

    Vst::AudioBusBuffers input{};
    Vst::AudioBusBuffers output{};
    input.numChannels = static_cast<int32>(channels_);
    output.numChannels = static_cast<int32>(channels_);

    if (sample_size64_) {
        // const_cast: VST3's `AudioBusBuffers` has no const input variant, and
        // a plugin that writes into its input is one that asked to process in
        // place. This host never hands it the same pointers, so it cannot.
        for (std::uint32_t c = 0; c < channels_; ++c) {
            in64_ptr_[c] = const_cast<double*>(in[c]);
        }
        input.channelBuffers64 = in64_ptr_.data();
        output.channelBuffers64 = out64_ptr_.data();
    } else {
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const double* src = in[c];
            float* dst = in32_ptr_[c];
            for (std::uint32_t n = 0; n < frames; ++n) {
                dst[n] = static_cast<float>(src[n]);
            }
        }
        input.channelBuffers32 = in32_ptr_.data();
        output.channelBuffers32 = out32_ptr_.data();
    }

    Vst::ProcessData data{};
    data.processMode = Vst::kOffline;
    data.symbolicSampleSize = sample_size64_ ? Vst::kSample64 : Vst::kSample32;
    data.numSamples = static_cast<int32>(frames);
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;
    data.inputParameterChanges = changes_.get();

    if (processor_->process(data) != kResultOk) {
        return false;
    }

    if (sample_size64_) {
        for (std::uint32_t c = 0; c < channels_; ++c) {
            std::memcpy(out[c], out64_ptr_[c], static_cast<std::size_t>(frames) * sizeof(double));
        }
    } else {
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const float* src = out32_ptr_[c];
            double* dst = out[c];
            for (std::uint32_t n = 0; n < frames; ++n) {
                dst[n] = static_cast<double>(src[n]);
            }
        }
    }
    return true;
}

bool Host::process_silence(std::uint32_t frames, double* const* out)
{
    if (frames > max_frames_) {
        return false;
    }
    return process(silence_ptr_.data(), frames, out);
}

bool Host::set_parameter(const std::string& which, double normalised, std::string& why)
{
    if (controller_ == nullptr) {
        why = name_ + " has no controller, so its parameters cannot be reached by name";
        return false;
    }
    if (!(normalised >= 0.0 && normalised <= 1.0)) {
        why = "a VST3 parameter is normalised: 0 to 1, not " + std::to_string(normalised);
        return false;
    }

    char* end = nullptr;
    const unsigned long asked = std::strtoul(which.c_str(), &end, 10);
    const bool by_id = !which.empty() && end != nullptr && *end == '\0';

    const int32 count = controller_->getParameterCount();
    std::string available;
    for (int32 i = 0; i < count; ++i) {
        Vst::ParameterInfo info{};
        if (controller_->getParameterInfo(i, info) != kResultOk) {
            continue;
        }
        const std::string title = narrow(info.title);
        const bool match = by_id ? info.id == static_cast<Vst::ParamID>(asked)
                                 : contains_fold(title, which);
        if (match) {
            controller_->setParamNormalized(info.id, normalised);
            pending_.emplace_back(static_cast<std::uint32_t>(info.id), normalised);
            return true;
        }
        if (available.size() < 400) {
            if (!available.empty()) {
                available += ", ";
            }
            available += title;
        }
    }
    why = name_ + " has no parameter matching `" + which + "`; it has " + available;
    return false;
}

bool Host::set_state(const std::vector<std::uint8_t>& bytes, std::string& why)
{
    if (component_ == nullptr) {
        why = "no plugin is loaded";
        return false;
    }
    MemoryStream stream{bytes};
    if (component_->setState(&stream) != kResultOk) {
        why = name_ + " would not take that state";
        return false;
    }
    // The controller is told separately and from the start of the same bytes:
    // the two halves each read what they understand out of one blob.
    if (controller_ != nullptr) {
        MemoryStream again{bytes};
        controller_->setComponentState(&again);
    }
    return true;
}

void Host::deactivate()
{
    if (!active_) {
        return;
    }
    if (processor_ != nullptr) {
        processor_->setProcessing(false);
    }
    if (component_ != nullptr) {
        component_->setActive(false);
    }
    active_ = false;
}

bool Host::reset()
{
    if (component_ == nullptr || !active_) {
        return true;
    }
    // VST3 has no `reset`. Deactivating and reactivating is what the SDK's own
    // documentation says to do, and is what every host does at a locate.
    processor_->setProcessing(false);
    component_->setActive(false);
    if (component_->setActive(true) != kResultOk) {
        active_ = false;
        return false;
    }
    processor_->setProcessing(true);
    return true;
}

std::uint32_t Host::parameter_count() const noexcept
{
    return controller_ == nullptr
               ? 0u
               : static_cast<std::uint32_t>(std::max<int32>(0, controller_->getParameterCount()));
}

bool Host::parameter(std::uint32_t index, std::string& title, double& value,
                     std::string& shown) const
{
    if (controller_ == nullptr) {
        return false;
    }
    Vst::ParameterInfo info{};
    if (controller_->getParameterInfo(static_cast<int32>(index), info) != kResultOk) {
        return false;
    }
    title = narrow(info.title);
    value = controller_->getParamNormalized(info.id);
    Vst::String128 text{};
    if (controller_->getParamStringByValue(info.id, value, text) == kResultOk) {
        shown = narrow(text);
    } else {
        shown.clear();
    }
    return true;
}

void Host::unload()
{
    deactivate();
    if (controller_ != nullptr) {
        controller_->setComponentHandler(nullptr);
        if (!single_component_) {
            controller_->terminate();
        }
        controller_->release();
        controller_ = nullptr;
    }
    single_component_ = false;
    if (processor_ != nullptr) {
        processor_->release();
        processor_ = nullptr;
    }
    if (component_ != nullptr) {
        component_->terminate();
        component_->release();
        component_ = nullptr;
    }
    if (factory_ != nullptr) {
        factory_->release();
        factory_ = nullptr;
    }
    if (library_ != nullptr) {
        auto* module = static_cast<HMODULE>(library_);
        using ExitFn = bool(PLUGIN_API*)();
        if (auto* exit_dll = reinterpret_cast<ExitFn>(
                reinterpret_cast<void*>(::GetProcAddress(module, "ExitDll")));
            exit_dll != nullptr) {
            exit_dll();
        }
        ::FreeLibrary(module);
        library_ = nullptr;
    }
    name_.clear();
    vendor_.clear();
    version_.clear();
    categories_.clear();
    channels_ = 0;
    max_frames_ = 0;
    latency_ = 0;
    tail_ = 0;
    sample_size64_ = false;
    pending_.clear();
}

} // namespace mp::vst3
