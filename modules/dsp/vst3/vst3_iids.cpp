// SPDX-License-Identifier: GPL-3.0-or-later
//
// The interface identifiers, defined once.
//
// A VST3 interface header *declares* `static const FUID iid` and something has
// to define it. In the SDK that something is `public.sdk/source/vst/vstinitiids.cpp`,
// which is one file in forty thousand lines of hosting code this module does
// not otherwise want -- so it is here instead, and it is the whole of what was
// borrowed conceptually rather than linked.
//
// **Only the interfaces this host actually uses.** A missing one is a link
// error naming exactly what to add, which is a better failure than carrying
// two hundred symbols for the seventeen that are called.

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstpluginterfacesupport.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>

namespace Steinberg {
namespace Vst {

DEF_CLASS_IID(IComponent)
DEF_CLASS_IID(IAudioProcessor)
DEF_CLASS_IID(IEditController)
DEF_CLASS_IID(IEditController2)
DEF_CLASS_IID(IComponentHandler)
DEF_CLASS_IID(IConnectionPoint)
DEF_CLASS_IID(IMessage)
DEF_CLASS_IID(IAttributeList)
DEF_CLASS_IID(IHostApplication)
DEF_CLASS_IID(IPlugInterfaceSupport)
DEF_CLASS_IID(IParameterChanges)
DEF_CLASS_IID(IParamValueQueue)

} // namespace Vst
} // namespace Steinberg
