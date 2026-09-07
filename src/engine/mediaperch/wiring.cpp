// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/wiring.hpp"

namespace mp {

bool wire_up(Sink& sink, const Format& source, DspChain& chain, PathPolicy policy,
             bool gain_changes, Wired& out, std::string& why)
{
    out = Wired{};
    out.policy = policy;
    out.offered = source;

    if (!chain.empty()) {
        // **Sized provisionally.** The device has not said how big a period is
        // yet, and all that is wanted from this pass is the format at the far
        // end of the chain. It is configured again below, for the block the
        // graph will really feed it.
        if (!chain.configure(dsp_bus_format(source), 4096, why)) {
            return false;
        }
        // The rate, the channels and the layout come from the chain's output --
        // that is what has to reach the device now. **The sample type stays the
        // source's**: the f64 bus is this program's business, and offering a
        // device f64 first would say the file was something it is not.
        out.offered.sample_rate = chain.output_format().sample_rate;
        out.offered.channels = chain.output_format().channels;
        out.offered.channel_mask = chain.output_format().channel_mask;
        // A stage exists in order to change the samples. There is nothing left
        // for a policy to decide, and pretending otherwise would end in a
        // bit-exact claim over audio that had been through a filter.
        out.policy = PathPolicy::processed;
    }

    out.negotiated = negotiate_best(sink, out.offered, out.policy);
    if (!out.negotiated.ok) {
        return false; // the caller reports it; `negotiated` says what happened
    }

    if (sink.period_frames(out.period_frames) != MP_OK || out.period_frames == 0) {
        why = "the device did not report a buffer size";
        return false;
    }
    if (!chain.empty() &&
        !chain.configure(dsp_bus_format(source), out.period_frames, why)) {
        return false;
    }

    out.processed =
        use_processed(out.policy, out.negotiated.fidelity, !chain.empty() || gain_changes);
    return true;
}

} // namespace mp
