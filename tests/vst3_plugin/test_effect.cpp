// SPDX-License-Identifier: GPL-3.0-or-later
//
// A VST3 plugin, written here, so the host can be tested without one.
//
// **The host is the thing under test and a plugin is the only way to run it.**
// Every VST3 on this machine is an instrument -- Kontakt, Dexed, Vital -- and a
// host for a chain needs an effect; and even with an effect to hand, a test
// that depended on somebody's installed plugin would pass or fail depending on
// what was installed, which is not a test. So this is the fake, in the same
// spirit as the fake DSP stages in dsp_test.cpp and the fake sink in
// fake_sink.hpp: the smallest thing that is really a VST3.
//
// It is a gain with one parameter, and it is deliberately awkward in the three
// ways a real plugin is:
//
//   * **two objects**, a component and a controller, which the host has to
//     create separately and introduce to each other -- the documented shape,
//     and the one that exercises `IConnectionPoint` and the message allocation
//     that a host without an `IHostApplication` cannot do;
//   * **f32 only**, so the conversion path is what gets exercised by default,
//     with `MEDIAPERCH_TEST_EFFECT_F64` turning on the other one;
//   * **a latency and a tail**, both non-zero, so the numbers the host reports
//     have somewhere to come from.
//
// It is not a demonstration of how to write a plugin. There is no editor, no
// state versioning, no bus flexibility and no unit hierarchy.

#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/vstspeaker.h>

#include <atomic>
#include <cstring>

using namespace Steinberg;

namespace {

/// Made up once and never again; a VST3 is identified by these and nothing else.
static const FUID k_component_cid{0x4D50546Fu, 0x73744566u, 0x66656374u, 0x436D7000u};
static const FUID k_controller_cid{0x4D50546Fu, 0x73744566u, 0x66656374u, 0x43746C00u};

constexpr Vst::ParamID k_gain_id = 42;
constexpr uint32 k_latency = 64;
constexpr uint32 k_tail = 128;

/// The gain, shared between the two halves because this plugin does not bother
/// with `IConnectionPoint` messages for it -- the host is what is being tested,
/// and a plugin whose halves agree by cheating still makes the host do its part.
std::atomic<double> g_gain{1.0};

void copy_utf16(Vst::String128 dst, const char16_t* src)
{
    int i = 0;
    for (; src[i] != 0 && i < 127; ++i) {
        dst[i] = static_cast<Vst::TChar>(src[i]);
    }
    dst[i] = 0;
}

// --------------------------------------------------------------------------
// The processing half
// --------------------------------------------------------------------------

class TestComponent final
    : public U::Implements<U::Directly<Vst::IComponent, Vst::IAudioProcessor,
                                       Vst::IConnectionPoint>> {
public:
    // -- IPluginBase --------------------------------------------------------
    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        // A real plugin keeps this to allocate messages with. Holding it is
        // what makes the host's IHostApplication matter, so it is held.
        host_ = context;
        return kResultOk;
    }
    tresult PLUGIN_API terminate() override
    {
        host_ = nullptr;
        return kResultOk;
    }

    // -- IComponent ---------------------------------------------------------
    tresult PLUGIN_API getControllerClassId(TUID classId) override
    {
        std::memcpy(classId, k_controller_cid.toTUID(), sizeof(TUID));
        return kResultOk;
    }
    tresult PLUGIN_API setIoMode(Vst::IoMode) override { return kNotImplemented; }
    int32 PLUGIN_API getBusCount(Vst::MediaType type, Vst::BusDirection) override
    {
        return type == Vst::kAudio ? 1 : 0;
    }
    tresult PLUGIN_API getBusInfo(Vst::MediaType type, Vst::BusDirection dir, int32 index,
                                  Vst::BusInfo& info) override
    {
        if (type != Vst::kAudio || index != 0) {
            return kInvalidArgument;
        }
        info.mediaType = Vst::kAudio;
        info.direction = dir;
        info.channelCount = static_cast<int32>(channels_);
        copy_utf16(info.name, dir == Vst::kInput ? u"In" : u"Out");
        info.busType = Vst::kMain;
        info.flags = Vst::BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(Vst::RoutingInfo&, Vst::RoutingInfo&) override
    {
        return kNotImplemented;
    }
    tresult PLUGIN_API activateBus(Vst::MediaType, Vst::BusDirection, int32, TBool) override
    {
        return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool state) override
    {
        active_ = state != 0;
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream* state) override
    {
        // One double, which is enough to prove the bytes arrived.
        double value = 1.0;
        int32 got = 0;
        if (state == nullptr || state->read(&value, sizeof(value), &got) != kResultOk ||
            got != static_cast<int32>(sizeof(value))) {
            return kResultFalse;
        }
        g_gain.store(value);
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* state) override
    {
        const double value = g_gain.load();
        int32 wrote = 0;
        return state != nullptr &&
                       state->write(const_cast<double*>(&value), sizeof(value), &wrote) ==
                           kResultOk
                   ? kResultOk
                   : kResultFalse;
    }

    // -- IAudioProcessor ----------------------------------------------------
    tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement* inputs, int32 numIns,
                                          Vst::SpeakerArrangement* outputs,
                                          int32 numOuts) override
    {
        if (numIns != 1 || numOuts != 1 || inputs[0] != outputs[0]) {
            return kResultFalse;
        }
        const auto count = Vst::SpeakerArr::getChannelCount(inputs[0]);
        if (count < 1 || count > 8) {
            return kResultFalse;
        }
        arrangement_ = inputs[0];
        channels_ = static_cast<uint32>(count);
        return kResultOk;
    }
    tresult PLUGIN_API getBusArrangement(Vst::BusDirection, int32 index,
                                         Vst::SpeakerArrangement& arr) override
    {
        if (index != 0) {
            return kInvalidArgument;
        }
        arr = arrangement_;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override
    {
#if MEDIAPERCH_TEST_EFFECT_F64
        return symbolicSampleSize == Vst::kSample32 || symbolicSampleSize == Vst::kSample64
                   ? kResultTrue
                   : kResultFalse;
#else
        return symbolicSampleSize == Vst::kSample32 ? kResultTrue : kResultFalse;
#endif
    }
    uint32 PLUGIN_API getLatencySamples() override { return k_latency; }
    tresult PLUGIN_API setupProcessing(Vst::ProcessSetup& setup) override
    {
        if (setup.maxSamplesPerBlock <= 0 || setup.sampleRate <= 0.0) {
            return kResultFalse;
        }
        sample_size_ = setup.symbolicSampleSize;
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool state) override
    {
        processing_ = state != 0;
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return k_tail; }

    tresult PLUGIN_API process(Vst::ProcessData& data) override
    {
        // Parameter changes first, which is what a host's queue is for.
        if (data.inputParameterChanges != nullptr) {
            const int32 n = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < n; ++i) {
                Vst::IParamValueQueue* q = data.inputParameterChanges->getParameterData(i);
                if (q == nullptr || q->getParameterId() != k_gain_id) {
                    continue;
                }
                int32 offset = 0;
                Vst::ParamValue value = 0.0;
                if (q->getPoint(q->getPointCount() - 1, offset, value) == kResultOk) {
                    g_gain.store(value);
                }
            }
        }
        if (!active_ || !processing_ || data.numSamples <= 0 || data.numInputs < 1 ||
            data.numOutputs < 1) {
            return kResultOk;
        }

        const double gain = g_gain.load();
        const int32 frames = data.numSamples;
        const int32 channels = data.outputs[0].numChannels;
        if (data.symbolicSampleSize == Vst::kSample64) {
            for (int32 c = 0; c < channels; ++c) {
                const Vst::Sample64* src = data.inputs[0].channelBuffers64[c];
                Vst::Sample64* dst = data.outputs[0].channelBuffers64[c];
                for (int32 n = 0; n < frames; ++n) {
                    dst[n] = src[n] * gain;
                }
            }
        } else {
            for (int32 c = 0; c < channels; ++c) {
                const Vst::Sample32* src = data.inputs[0].channelBuffers32[c];
                Vst::Sample32* dst = data.outputs[0].channelBuffers32[c];
                for (int32 n = 0; n < frames; ++n) {
                    dst[n] = static_cast<Vst::Sample32>(src[n] * gain);
                }
            }
        }
        return kResultOk;
    }

    // -- IConnectionPoint ---------------------------------------------------
    tresult PLUGIN_API connect(Vst::IConnectionPoint* other) override
    {
        peer_ = other;
        // The call a host with no IHostApplication cannot serve. Sending it
        // here is the point: it is how the test knows the host allocated one.
        if (host_ != nullptr) {
            Vst::IHostApplication* app = nullptr;
            if (host_->queryInterface(Vst::IHostApplication::iid,
                                      reinterpret_cast<void**>(&app)) == kResultOk &&
                app != nullptr) {
                Vst::IMessage* message = nullptr;
                TUID message_iid{};
                std::memcpy(message_iid, Vst::IMessage::iid.toTUID(), sizeof(TUID));
                if (app->createInstance(message_iid, message_iid,
                                        reinterpret_cast<void**>(&message)) == kResultOk &&
                    message != nullptr) {
                    message->setMessageID("hello");
                    if (peer_ != nullptr) {
                        peer_->notify(message);
                    }
                    message->release();
                }
                app->release();
            }
        }
        return kResultOk;
    }
    tresult PLUGIN_API disconnect(Vst::IConnectionPoint*) override
    {
        peer_ = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API notify(Vst::IMessage*) override { return kResultOk; }

private:
    FUnknown* host_ = nullptr;
    Vst::IConnectionPoint* peer_ = nullptr;
    Vst::SpeakerArrangement arrangement_ = Vst::SpeakerArr::kStereo;
    uint32 channels_ = 2;
    int32 sample_size_ = Vst::kSample32;
    bool active_ = false;
    bool processing_ = false;
};

// --------------------------------------------------------------------------
// The controlling half
// --------------------------------------------------------------------------

class TestController final
    : public U::Implements<U::Directly<Vst::IEditController, Vst::IConnectionPoint>> {
public:
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }

    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo(int32 index, Vst::ParameterInfo& info) override
    {
        if (index != 0) {
            return kInvalidArgument;
        }
        info = Vst::ParameterInfo{};
        info.id = k_gain_id;
        copy_utf16(info.title, u"Gain");
        copy_utf16(info.shortTitle, u"Gain");
        copy_utf16(info.units, u"x");
        info.stepCount = 0;
        info.defaultNormalizedValue = 1.0;
        info.unitId = 0; // the root unit, without pulling in ivstunits.h
        info.flags = Vst::ParameterInfo::kCanAutomate;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(Vst::ParamID id, Vst::ParamValue value,
                                             Vst::String128 string) override
    {
        if (id != k_gain_id) {
            return kInvalidArgument;
        }
        // Two decimals, spelled out by hand: this plugin does not link a CRT
        // formatter for one string.
        const int hundredths = static_cast<int>(value * 100.0 + 0.5);
        const char16_t digits[] = {u'0' + static_cast<char16_t>(hundredths / 100), u'.',
                                   u'0' + static_cast<char16_t>((hundredths / 10) % 10),
                                   u'0' + static_cast<char16_t>(hundredths % 10), 0};
        copy_utf16(string, digits);
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(Vst::ParamID, Vst::TChar*,
                                             Vst::ParamValue&) override
    {
        return kNotImplemented;
    }
    Vst::ParamValue PLUGIN_API normalizedParamToPlain(Vst::ParamID, Vst::ParamValue v) override
    {
        return v;
    }
    Vst::ParamValue PLUGIN_API plainParamToNormalized(Vst::ParamID, Vst::ParamValue v) override
    {
        return v;
    }
    Vst::ParamValue PLUGIN_API getParamNormalized(Vst::ParamID id) override
    {
        return id == k_gain_id ? g_gain.load() : 0.0;
    }
    tresult PLUGIN_API setParamNormalized(Vst::ParamID id, Vst::ParamValue value) override
    {
        if (id != k_gain_id) {
            return kInvalidArgument;
        }
        g_gain.store(value);
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(Vst::IComponentHandler* handler) override
    {
        handler_ = handler;
        return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

    tresult PLUGIN_API connect(Vst::IConnectionPoint* other) override
    {
        peer_ = other;
        return kResultOk;
    }
    tresult PLUGIN_API disconnect(Vst::IConnectionPoint*) override
    {
        peer_ = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API notify(Vst::IMessage* message) override
    {
        // Proof that the host's message allocation worked: the id came from an
        // attribute list this process did not write.
        if (message != nullptr && message->getMessageID() != nullptr &&
            std::strcmp(message->getMessageID(), "hello") == 0) {
            greeted_ = true;
        }
        return kResultOk;
    }

    [[nodiscard]] bool greeted() const noexcept { return greeted_; }

private:
    Vst::IComponentHandler* handler_ = nullptr;
    Vst::IConnectionPoint* peer_ = nullptr;
    bool greeted_ = false;
};

// --------------------------------------------------------------------------
// The factory
// --------------------------------------------------------------------------

class TestFactory final : public U::Implements<U::Directly<IPluginFactory2>> {
public:
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override
    {
        if (info == nullptr) {
            return kInvalidArgument;
        }
        *info = PFactoryInfo{};
        std::strcpy(info->vendor, "MediaPerch");
        std::strcpy(info->url, "https://github.com/yumasansansan/MediaPerch");
        std::strcpy(info->email, "");
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 2; }

    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override
    {
        if (info == nullptr) {
            return kInvalidArgument;
        }
        *info = PClassInfo{};
        if (index == 0) {
            std::memcpy(info->cid, k_component_cid.toTUID(), sizeof(TUID));
            info->cardinality = PClassInfo::kManyInstances;
            std::strcpy(info->category, kVstAudioEffectClass);
            std::strcpy(info->name, "MediaPerch Test Effect");
            return kResultOk;
        }
        if (index == 1) {
            std::memcpy(info->cid, k_controller_cid.toTUID(), sizeof(TUID));
            info->cardinality = PClassInfo::kManyInstances;
            std::strcpy(info->category, kVstComponentControllerClass);
            std::strcpy(info->name, "MediaPerch Test Effect Controller");
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override
    {
        if (info == nullptr) {
            return kInvalidArgument;
        }
        PClassInfo one{};
        if (getClassInfo(index, &one) != kResultOk) {
            return kInvalidArgument;
        }
        *info = PClassInfo2{};
        std::memcpy(info->cid, one.cid, sizeof(TUID));
        info->cardinality = one.cardinality;
        std::strcpy(info->category, one.category);
        std::strcpy(info->name, one.name);
        std::strcpy(info->vendor, "MediaPerch");
        std::strcpy(info->version, "0.1.0");
        std::strcpy(info->sdkVersion, kVstVersionString);
        if (index == 0) {
            std::strcpy(info->subCategories, "Fx|Tools");
        }
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(FIDString cid, FIDString _iid, void** obj) override
    {
        if (obj == nullptr) {
            return kInvalidArgument;
        }
        *obj = nullptr;
        const FUID wanted{FUID::fromTUID(cid)};
        FUnknown* made = nullptr;
        if (wanted == k_component_cid) {
            made = static_cast<Vst::IComponent*>(new TestComponent());
        } else if (wanted == k_controller_cid) {
            made = static_cast<Vst::IEditController*>(new TestController());
        }
        if (made == nullptr) {
            return kResultFalse;
        }
        const tresult r = made->queryInterface(_iid, obj);
        made->release(); // queryInterface took its own reference
        return r;
    }

private:
};

} // namespace

extern "C" {

SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory()
{
    static TestFactory factory;
    factory.addRef(); // the host releases what it is given
    return &factory;
}

SMTG_EXPORT_SYMBOL bool InitDll()
{
    return true;
}

SMTG_EXPORT_SYMBOL bool ExitDll()
{
    return true;
}

} // extern "C"
