// SPDX-License-Identifier: GPL-3.0-or-later
//
// Interfaces out of the COM calls that write one through a `void**`, without a
// pointer ever being written as another type than it is.
//
// Activate, GetService and CoCreateInstance take the place to write the
// interface they were asked for as a `void**`, and the usual ways to hand them
// one -- the address of a `T*` or of a ComPtr's pointer, cast, or IID_PPV_ARGS,
// which casts inside -- have the call write a `T*` as a `void*`. Here the call
// writes a `void*` of its own, and the pointer it wrote goes into the ComPtr,
// or the plain pointer, as the reference it already is. The sink and the
// engine's verifier both open devices this way, so the one set of them is
// here, beside the formats they share.

#pragma once

#include "win_headers.hpp"

namespace mp::wasapi {

/// `call` given the IID of `T` and a `void*` to write into, and what it wrote
/// given to `to`: a ComPtr takes it without counting it again.
template <typename T, typename Call>
HRESULT into(ComPtr<T>& to, Call&& call)
{
    void* raw = nullptr;
    const HRESULT hr = call(__uuidof(T), &raw);
    to.Attach(static_cast<T*>(raw));
    return hr;
}

/// The same into a plain pointer, whose owner releases it.
template <typename T, typename Call>
HRESULT into(T*& to, Call&& call)
{
    void* raw = nullptr;
    const HRESULT hr = call(__uuidof(T), &raw);
    to = static_cast<T*>(raw);
    return hr;
}

/// The interface `to` holds, activated on `device`.
template <typename Target>
HRESULT activate(IMMDevice* device, Target& to)
{
    return into(to, [device](REFIID iid, void** out) {
        return device->Activate(iid, CLSCTX_ALL, nullptr, out);
    });
}

/// The service `to` holds, from `client`.
template <typename Target>
HRESULT service(IAudioClient* client, Target& to)
{
    return into(to, [client](REFIID iid, void** out) { return client->GetService(iid, out); });
}

/// The system's device enumerator.
inline HRESULT make_enumerator(ComPtr<IMMDeviceEnumerator>& to)
{
    return into(to, [](REFIID iid, void** out) {
        return ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, iid, out);
    });
}

} // namespace mp::wasapi
