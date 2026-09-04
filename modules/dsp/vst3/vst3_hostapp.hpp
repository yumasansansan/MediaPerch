// SPDX-License-Identifier: GPL-3.0-or-later
//
// The objects a VST3 is allowed to call back into.
//
// **A VST3 does not only get called; it calls.** It asks the host its name, it
// asks the host to allocate messages so that its processor half and its
// controller half can talk to each other, and it tells the host that a
// parameter moved. A host that provides none of that loads about half the
// plugins in the world and crashes on some of the rest, because the interfaces
// are documented as always present and plugins are written against the
// documentation.
//
// The SDK's own implementations of these live in `public.sdk`, which is 40,000
// lines and a build system with opinions. What a host actually needs is here,
// and it is four small classes: an attribute list, a message that carries one,
// an application that hands them out, and a parameter queue.
//
// `U::Implements` comes from the SDK's `funknownimpl.h` and is the one piece of
// cleverness borrowed rather than written: it generates `queryInterface`,
// `addRef` and `release` from the list of interfaces a class claims, which is
// the boilerplate that makes hand-written COM wrong.

#ifndef MEDIAPERCH_DSP_VST3_HOSTAPP_HPP
#define MEDIAPERCH_DSP_VST3_HOSTAPP_HPP

#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstpluginterfacesupport.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mp::vst3 {

using namespace Steinberg;

/// A bag of typed values, keyed by an ASCII string.
///
/// The plugin's two halves use it to send each other whatever they like; the
/// host stores it and gives it back. Nothing here interprets a single key --
/// that is the whole design of the interface, and the reason it can be
/// implemented without knowing anything about any plugin.
class AttributeList final : public U::Implements<U::Directly<Vst::IAttributeList>> {
public:
    tresult PLUGIN_API setInt(AttrID id, int64 value) override
    {
        ints_[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64& value) override
    {
        const auto at = ints_.find(id);
        if (at == ints_.end()) {
            return kResultFalse;
        }
        value = at->second;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double value) override
    {
        floats_[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getFloat(AttrID id, double& value) override
    {
        const auto at = floats_.find(id);
        if (at == floats_.end()) {
            return kResultFalse;
        }
        value = at->second;
        return kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const Vst::TChar* string) override
    {
        std::u16string held;
        for (const Vst::TChar* p = string; p != nullptr && *p != 0; ++p) {
            held.push_back(static_cast<char16_t>(*p));
        }
        strings_[id] = std::move(held);
        return kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, Vst::TChar* string, uint32 sizeInBytes) override
    {
        const auto at = strings_.find(id);
        if (at == strings_.end() || string == nullptr) {
            return kResultFalse;
        }
        // sizeInBytes counts bytes and a TChar is two of them, so the room for
        // characters is half of it, and one of those is the terminator.
        const std::size_t room = sizeInBytes / sizeof(Vst::TChar);
        if (room == 0) {
            return kResultFalse;
        }
        const std::size_t n = std::min(at->second.size(), room - 1);
        for (std::size_t i = 0; i < n; ++i) {
            string[i] = static_cast<Vst::TChar>(at->second[i]);
        }
        string[n] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 sizeInBytes) override
    {
        const auto* p = static_cast<const std::uint8_t*>(data);
        binaries_[id].assign(p, p + sizeInBytes);
        return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& sizeInBytes) override
    {
        const auto at = binaries_.find(id);
        if (at == binaries_.end()) {
            sizeInBytes = 0;
            return kResultFalse;
        }
        data = at->second.data();
        sizeInBytes = static_cast<uint32>(at->second.size());
        return kResultOk;
    }

private:
    std::map<std::string, int64> ints_;
    std::map<std::string, double> floats_;
    std::map<std::string, std::u16string> strings_;
    std::map<std::string, std::vector<std::uint8_t>> binaries_;
};

/// An id and an attribute list. That is the entire message type.
class Message final : public U::Implements<U::Directly<Vst::IMessage>> {
public:
    FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { id_ = id != nullptr ? id : ""; }
    Vst::IAttributeList* PLUGIN_API getAttributes() override { return &attributes_; }

private:
    std::string id_;
    AttributeList attributes_;
};

/// What `IComponent::initialize` is handed.
///
/// Two jobs. It names the host, which plugins put in their about box and
/// occasionally branch on -- this one gives its real name, because a host that
/// lies to work around one plugin has to keep lying. And it allocates the
/// message objects the plugin's halves pass between themselves, which is the
/// call that fails on a host that returns `kNotImplemented` here, and it fails
/// as a crash in the plugin rather than as an error in the host.
class HostApplication final
    : public U::Implements<U::Directly<Vst::IHostApplication, Vst::IPlugInterfaceSupport>> {
public:
    tresult PLUGIN_API getName(Vst::String128 name) override
    {
        static constexpr char16_t k_name[] = u"MediaPerch";
        std::size_t i = 0;
        for (; k_name[i] != 0; ++i) {
            name[i] = static_cast<Vst::TChar>(k_name[i]);
        }
        name[i] = 0;
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(TUID cid, TUID _iid, void** obj) override
    {
        // **The cast is not decoration.** These classes have several bases and
        // the interface subobject is not always at offset zero, so handing back
        // the derived pointer gives the plugin a vtable that is off by one
        // pointer -- which is a crash inside somebody else's DLL, on the first
        // call, with a stack that says nothing about this function.
        const FUID wanted{FUID::fromTUID(cid)};
        if (wanted == Vst::IMessage::iid) {
            *obj = static_cast<Vst::IMessage*>(new Message());
            return kResultOk;
        }
        if (wanted == Vst::IAttributeList::iid) {
            *obj = static_cast<Vst::IAttributeList*>(new AttributeList());
            return kResultOk;
        }
        (void)_iid;
        return kResultFalse;
    }

    /// Which optional interfaces this host answers to, asked before the plugin
    /// tries them. Saying no here is cheaper for both sides than a
    /// `queryInterface` that fails, and honest: this host has no editor, no
    /// transport and no automation, and a plugin that adapts to that is a
    /// plugin behaving better than one that assumes.
    tresult PLUGIN_API isPlugInterfaceSupported(const TUID _iid) override
    {
        const FUID asked{FUID::fromTUID(_iid)};
        return asked == Vst::IComponentHandler::iid ? kResultTrue : kResultFalse;
    }
};

/// One parameter's changes within one block.
///
/// The interface is a curve -- several points, each at a sample offset -- and
/// this host only ever sets one, at offset zero: a setting typed on a command
/// line happens at the start of the block or it does not happen. A plugin that
/// interpolates between points gets a single point and holds it, which is the
/// same thing every host does for a parameter that was not automated.
class ParamQueue final : public U::Implements<U::Directly<Vst::IParamValueQueue>> {
public:
    void set(Vst::ParamID id, Vst::ParamValue value)
    {
        id_ = id;
        value_ = value;
    }
    Vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    int32 PLUGIN_API getPointCount() override { return 1; }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset,
                                Vst::ParamValue& value) override
    {
        if (index != 0) {
            return kResultFalse;
        }
        sampleOffset = 0;
        value = value_;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32, Vst::ParamValue, int32&) override
    {
        return kNotImplemented; // the plugin does not write into the input queue
    }

private:
    Vst::ParamID id_ = 0;
    Vst::ParamValue value_ = 0.0;
};

/// The queues for one block. Fixed capacity: the caller reserves once and the
/// audio path only ever fills what is already there.
class ParameterChanges final : public U::Implements<U::Directly<Vst::IParameterChanges>> {
public:
    void clear() { used_ = 0; }
    void reserve(std::size_t n)
    {
        while (queues_.size() < n) {
            queues_.push_back(std::make_unique<ParamQueue>());
        }
    }
    /// Adds one, or does nothing if `reserve` was not generous enough -- which
    /// would be a dropped parameter change and never a reallocation on the
    /// audio path.
    void add(Vst::ParamID id, Vst::ParamValue value)
    {
        if (used_ >= queues_.size()) {
            return;
        }
        queues_[used_]->set(id, value);
        ++used_;
    }

    int32 PLUGIN_API getParameterCount() override { return static_cast<int32>(used_); }
    Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override
    {
        if (index < 0 || static_cast<std::size_t>(index) >= used_) {
            return nullptr;
        }
        return queues_[static_cast<std::size_t>(index)].get();
    }
    Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID&, int32&) override
    {
        return nullptr; // output changes are read, not written, by this host
    }

private:
    std::vector<std::unique_ptr<ParamQueue>> queues_;
    std::size_t used_ = 0;
};

/// What a plugin's controller half calls when a parameter moves on its own.
///
/// Nothing here has an editor, so nothing moves a parameter except this host --
/// but a plugin whose `IEditController` gets a null handler is a plugin that
/// may refuse to initialise. It says yes and remembers nothing, which is the
/// truthful behaviour for a host with no automation to record into.
class ComponentHandler final : public U::Implements<U::Directly<Vst::IComponentHandler>> {
public:
    tresult PLUGIN_API beginEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(Vst::ParamID, Vst::ParamValue) override
    {
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};

} // namespace mp::vst3

#endif // MEDIAPERCH_DSP_VST3_HOSTAPP_HPP
