// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every module this tree builds answers at the ABI this tree is on.
//
// **Five modules were dead for a day and nothing said so.** ABI v4 changed the
// video half -- MpPixelFormat became MpPixelLayout, MpVideoFrame grew -- and
// `modules/shared/mp-abi`, which is the Rust mirror of the header, carries no
// video structure at all. So there was nothing in it to update, no compile
// error, and its `ABI_VERSION` stayed at 3 while the header went to 4.
// `mp_module_entry` answers null to a host on a different version, which is
// exactly what a version bump is for -- so codec_aac, codec_alac, codec_dsd,
// demux_adts and demux_dsd stopped loading. AAC and ALAC became FFmpeg's
// quietly, and DSD lost its bit-exact DoP path and came out as F32.
//
// Nothing caught it. The C++ tests load the modules they name and none of them
// names those five; `rust_modules` builds the crates and runs their own tests,
// which know nothing about a host; and `format_matrix`, which would have shown
// it in one line, needs FFmpeg and a corpus and so skips in every CI leg that
// builds -- the one job that has FFmpeg runs `decode_quality` alone.
//
// So this walks the module directory instead of naming anything. A module is a
// DLL exporting `mp_module_entry`; what it must do is answer the version this
// tree is compiled for. Anything that does not is a module the player will not
// load, and a silent absence is the one thing §7 says this tree does not do.

#include "mediaperch/result.hpp"

#include "temp_path.hpp"

#include <mediaperch/module.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

/// Every DLL under the directory the modules are built into, whatever built
/// them. Naming them would defeat the point: the module that falls behind is
/// the one nobody remembered to name.
std::vector<std::filesystem::path> modules()
{
    std::vector<std::filesystem::path> found;
    const std::filesystem::path root{MEDIAPERCH_MODULE_DIR};
    std::error_code trouble;
    if (!std::filesystem::is_directory(root, trouble)) {
        return found;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, trouble)) {
        if (entry.is_regular_file(trouble) && entry.path().extension() == ".dll") {
            found.push_back(entry.path());
        }
    }
    return found;
}

/// One name per line, so a failure reads as a list rather than as a sentence
/// with commas in it.
std::string listed(const std::vector<std::string>& names)
{
    std::string out;
    for (const std::string& name : names) {
        out += "\n  ";
        out += name;
    }
    return out;
}

} // namespace

TEST_CASE("every module built here loads at this tree's ABI version", "[abi][modules]")
{
    const auto found = modules();
    // A build that produced no modules would pass every assertion below by
    // having none to make, which is the failure mode a sweep like this has.
    REQUIRE(found.size() >= 10u);

    // **Collected rather than asserted where they are found.** A REQUIRE inside
    // the loop stops at the first module and says nothing about the rest, which
    // would have reported one of the five the Rust mirror took down and left
    // the other four to be found one rebuild at a time.
    std::vector<std::string> refused;
    std::vector<std::string> unusable;

    for (const auto& path : found) {
        const std::string name = path.filename().string();
        INFO(name);

        auto* dll = ::LoadLibraryW(path.c_str());
        if (dll == nullptr) {
            unusable.push_back(name + " would not load at all");
            continue;
        }

        using Entry = const MpModuleDesc*(MP_CALL*)(std::uint32_t);
        auto* entry = reinterpret_cast<Entry>(
            reinterpret_cast<void*>(::GetProcAddress(dll, "mp_module_entry")));
        // Every DLL in this directory is a module, so one without the export is
        // something that should not have been put here.
        if (entry == nullptr) {
            unusable.push_back(name + " exports no mp_module_entry");
            ::FreeLibrary(dll);
            continue;
        }

        const MpModuleDesc* desc = entry(MP_ABI_VERSION);
        // **The line the Rust mirror would have failed.** A module that answers
        // null here is one the player silently does without.
        if (desc == nullptr) {
            refused.push_back(name);
            ::FreeLibrary(dll);
            continue;
        }
        CHECK(desc->abi_version == MP_ABI_VERSION);
        CHECK(desc->size == sizeof(MpModuleDesc));
        CHECK(desc->id != nullptr);
        CHECK(desc->vtbl != nullptr);
        // §4 rule 6: what a module can do is data, readable without running it.
        // A module that reports codecs must list them, and one that lists none
        // must say zero rather than pointing somewhere.
        CHECK((desc->codec_count == 0u) == (desc->codecs == nullptr));
        // A codec that declares no codecs is invisible to a registry reading
        // declarations -- which codec_mft was, for as long as every test
        // reached it by naming its DLL.
        if (desc->kind == MP_KIND_CODEC || desc->kind == MP_KIND_VCODEC) {
            CHECK(desc->codec_count != 0u);
        }

        // And a version this tree is not on gets nothing, which is the other
        // half of the same promise.
        CHECK(entry(MP_ABI_VERSION + 1u) == nullptr);
        CHECK(entry(MP_ABI_VERSION - 1u) == nullptr);

        if (desc->shutdown != nullptr) {
            desc->shutdown();
        }
        ::FreeLibrary(dll);
    }

    {
        INFO("modules that answered null at MP_ABI_VERSION " << MP_ABI_VERSION << ":"
                                                             << listed(refused));
        REQUIRE(refused.empty());
    }
    {
        INFO("DLLs in the module directory that are not modules:" << listed(unusable));
        REQUIRE(unusable.empty());
    }
}

namespace {

/// What every module is asked, and what it answered wrongly. A call that hands
/// a module nonsense -- no object where one is needed, nowhere to put an
/// answer, a file that is not there, a codec nobody has -- has one right
/// answer, which is not MP_OK: MP_ERR_INVALID is "a caller passed nonsense".
/// The other wrong answer is a crash, and that one this process reports by
/// dying, with the module's name in the log above it.
struct Nonsense {
    std::string module;
    std::vector<std::string> accepted;

    void refused(MpResult r, const char* call)
    {
        if (r == MP_OK) {
            accepted.push_back(module + ": " + call);
        }
    }
};

/// A host that says nothing and allocates as the C library does: what a module
/// is initialised with before it is asked anything.
const MpHost& quiet_host()
{
    static const MpHost host = {
        sizeof(MpHost), 0, nullptr, [](void*, MpLogLevel, const char*) {},
        [](void*, std::size_t bytes) { return std::malloc(bytes); },
        [](void*, void* p) { std::free(p); }};
    return host;
}

/// Buffers a module may write into when it is wrongly willing to.
struct Scratch {
    std::uint8_t bytes[256] = {};
    double samples[64] = {};
    char text[64] = {};
};

void ask_demuxer(Nonsense& n, const MpDemuxVtbl& v)
{
    Scratch s;
    std::uint32_t score = 0;
    n.refused(v.probe(nullptr, nullptr, 0, nullptr), "probe, with nowhere to put the score");
    (void)v.probe(nullptr, nullptr, 0, &score); // neither path nor bytes: an answer, whatever it is
    const std::string missing = mp::test::temp_path("no-such-file").string();
    MpDemux* d = nullptr;
    n.refused(v.open(nullptr, &d), "open, of no path");
    n.refused(v.open(missing.c_str(), &d), "open, of a file that is not there");
    n.refused(v.open(missing.c_str(), nullptr), "open, with nowhere to put the demuxer");
    if (d != nullptr) {
        v.close(d);
    }
    std::uint32_t count = 0;
    n.refused(v.stream_count(nullptr, &count), "stream_count, of no demuxer");
    MpStreamInfo info{};
    info.size = sizeof(info);
    n.refused(v.stream_info(nullptr, 0, &info), "stream_info, of no demuxer");
    std::uint32_t needed = 0;
    n.refused(v.stream_config(nullptr, 0, s.bytes, sizeof(s.bytes), &needed),
              "stream_config, of no demuxer");
    const std::uint32_t first = 0;
    n.refused(v.select_streams(nullptr, &first, 1), "select_streams, of no demuxer");
    MpPacket packet{};
    packet.size = sizeof(packet);
    n.refused(v.read_packet(nullptr, s.bytes, sizeof(s.bytes), &packet),
              "read_packet, of no demuxer");
    n.refused(v.seek(nullptr, 0, 0), "seek, of no demuxer");
    if (v.read_frames != nullptr) {
        std::size_t got = 0;
        n.refused(v.read_frames(nullptr, s.bytes, sizeof(s.bytes), &got),
                  "read_frames, of no demuxer");
    }
    if (v.size >= sizeof(MpDemuxVtbl) && v.stream_video_info != nullptr) {
        MpVideoInfo video{};
        video.size = sizeof(video);
        n.refused(v.stream_video_info(nullptr, 0, &video), "stream_video_info, of no demuxer");
    }
    v.close(nullptr);
}

void ask_codec(Nonsense& n, const MpCodecVtbl& v, MpCodec declared)
{
    Scratch s;
    const auto nobodys = static_cast<MpCodec>(0xFFFFFFFFu);
    std::uint32_t score = 0;
    n.refused(v.probe(declared, nullptr, 0, nullptr), "probe, with nowhere to put the score");
    if (v.probe(nobodys, nullptr, 0, &score) == MP_OK && score != 0) {
        n.accepted.push_back(n.module + ": probe, scoring a codec nobody has");
    }
    MpCodecInstance* c = nullptr;
    n.refused(v.open(declared, nullptr, 0, nullptr), "open, with nowhere to put the decoder");
    n.refused(v.open(nobodys, nullptr, 0, &c), "open, of a codec nobody has");
    if (c != nullptr) {
        v.close(c);
    }
    MpFormat format{};
    n.refused(v.get_format(nullptr, &format), "get_format, of no decoder");
    std::size_t out = 0;
    n.refused(v.decode(nullptr, s.bytes, 16, s.bytes + 16, 64, &out), "decode, by no decoder");
    n.refused(v.flush(nullptr, s.bytes, sizeof(s.bytes), &out), "flush, of no decoder");
    n.refused(v.reset(nullptr), "reset, of no decoder");
    v.close(nullptr);
}

void ask_video_codec(Nonsense& n, const MpVideoCodecVtbl& v, MpCodec declared)
{
    Scratch s;
    const auto nobodys = static_cast<MpCodec>(0xFFFFFFFFu);
    n.refused(v.probe(declared, MP_GRAPHICS_NONE, nullptr, 0, nullptr),
              "probe, with nowhere to put the score");
    MpVideoCodec* c = nullptr;
    n.refused(v.open(declared, nullptr, nullptr, 0, nullptr),
              "open, with nowhere to put the decoder");
    n.refused(v.open(nobodys, nullptr, nullptr, 0, &c), "open, of a codec nobody has");
    if (c != nullptr) {
        v.close(c);
    }
    MpVideoInfo info{};
    info.size = sizeof(info);
    n.refused(v.get_format(nullptr, &info), "get_format, of no decoder");
    n.refused(v.decode(nullptr, s.bytes, 16, 0), "decode, by no decoder");
    MpVideoFrame frame{};
    frame.size = sizeof(frame);
    n.refused(v.next_frame(nullptr, &frame), "next_frame, of no decoder");
    n.refused(v.flush(nullptr), "flush, of no decoder");
    n.refused(v.reset(nullptr), "reset, of no decoder");
    if (v.set != nullptr) {
        n.refused(v.set(nullptr, "threads", "1"), "set, on no decoder");
    }
    v.close(nullptr);
}

void ask_dsp(Nonsense& n, const MpDspVtbl& v)
{
    Scratch s;
    n.refused(v.open(nullptr), "open, with nowhere to put the stage");
    MpFormat in{};
    in.sample_rate = 48000;
    in.channels = 2;
    MpFormat out{};
    std::uint32_t frames = 0;
    double* lanes[2] = {s.samples, s.samples + 32};
    const double* const* ins = lanes;
    n.refused(v.configure(nullptr, &in, 32, &out, &frames), "configure, of no stage");
    n.refused(v.process(nullptr, ins, 32, lanes, 32, &frames), "process, by no stage");
    n.refused(v.flush(nullptr, lanes, 32, &frames), "flush, of no stage");
    if (v.set != nullptr) {
        n.refused(v.set(nullptr, "gain", "0"), "set, on no stage");
    }
    if (v.describe != nullptr) {
        n.refused(v.describe(nullptr, 0, s.text, sizeof(s.text)), "describe, of no stage");
    }
    n.refused(v.reset(nullptr), "reset, of no stage");
    if (v.get_latency != nullptr) {
        n.refused(v.get_latency(nullptr, &frames), "get_latency, of no stage");
    }
    v.close(nullptr);

    // And a stage that exists, handed nonsense: a stage is arithmetic, so one
    // can be made here without touching anything outside this process.
    MpDsp* d = nullptr;
    if (v.open(&d) != MP_OK || d == nullptr) {
        return;
    }
    n.refused(v.configure(d, nullptr, 32, &out, &frames), "configure, with no format");
    n.refused(v.process(d, ins, 32, lanes, 32, nullptr),
              "process, with nowhere to say how much it wrote");
    if (v.set != nullptr) {
        n.refused(v.set(d, nullptr, nullptr), "set, of no key");
    }
    if (v.describe != nullptr) {
        n.refused(v.describe(d, 0, nullptr, sizeof(s.text)),
                  "describe, into no buffer said to hold 64 bytes");
    }
    if (v.get_latency != nullptr) {
        n.refused(v.get_latency(d, nullptr), "get_latency, with nowhere to put it");
    }
    v.close(d);
}

void ask_video_dsp(Nonsense& n, const MpVideoDspVtbl& v)
{
    Scratch s;
    n.refused(v.probe(MP_GRAPHICS_D3D11, nullptr), "probe, with nowhere to put the score");
    MpVideoDsp* d = nullptr;
    n.refused(v.open(nullptr, &d), "open, on no device");
    n.refused(v.open(nullptr, nullptr), "open, with nowhere to put the stage");
    if (d != nullptr) {
        v.close(d);
    }
    MpVideoInfo in{};
    in.size = sizeof(in);
    MpVideoInfo out{};
    out.size = sizeof(out);
    n.refused(v.configure(nullptr, &in, &out), "configure, of no stage");
    MpVideoFrame a{};
    a.size = sizeof(a);
    MpVideoFrame b{};
    b.size = sizeof(b);
    n.refused(v.process(nullptr, &a, &b), "process, by no stage");
    n.refused(v.reset(nullptr), "reset, of no stage");
    if (v.set != nullptr) {
        n.refused(v.set(nullptr, "size", "640x360"), "set, on no stage");
    }
    if (v.describe != nullptr) {
        n.refused(v.describe(nullptr, 0, s.text, sizeof(s.text)), "describe, of no stage");
    }
    v.close(nullptr);
}

/// A presenter is asked nothing that would make one: that is a window, or a
/// device, and this test opens neither.
void ask_presenter(Nonsense& n, const MpVideoVtbl& v)
{
    Scratch s;
    n.refused(v.open(nullptr, nullptr), "open, with nowhere to put the presenter");
    MpVideoInfo info{};
    info.size = sizeof(info);
    n.refused(v.configure(nullptr, &info), "configure, of no presenter");
    MpVideoFrame frame{};
    frame.size = sizeof(frame);
    n.refused(v.present(nullptr, &frame), "present, by no presenter");
    if (v.set != nullptr) {
        n.refused(v.set(nullptr, "size", "640x360"), "set, on no presenter");
    }
    if (v.describe != nullptr) {
        n.refused(v.describe(nullptr, 0, s.text, sizeof(s.text)), "describe, of no presenter");
    }
    if (v.get_device != nullptr) {
        MpGraphicsDevice device{};
        device.size = sizeof(device);
        n.refused(v.get_device(nullptr, &device), "get_device, of no presenter");
    }
    if (v.read_back != nullptr) {
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        MpPixelLayout layout{};
        n.refused(v.read_back(nullptr, s.bytes, sizeof(s.bytes), &w, &h, &layout),
                  "read_back, of no presenter");
    }
    if (v.stages != nullptr) {
        n.refused(v.stages(nullptr, nullptr, 0), "stages, of no presenter");
    }
    v.close(nullptr);
}

/// A sink is asked nothing that would open an endpoint: an exclusive one
/// silences everything else on it.
void ask_sink(Nonsense& n, const MpSinkVtbl& v)
{
    n.refused(v.enumerate(0, nullptr), "enumerate, with nowhere to put the device");
    n.refused(v.open(nullptr, MP_SHARE_SHARED, nullptr), "open, with nowhere to put the sink");
    MpFormat want{};
    want.sample_rate = 48000;
    want.channels = 2;
    MpFormat accepted{};
    n.refused(v.negotiate(nullptr, &want, &accepted), "negotiate, with no sink");
    std::uint32_t frames = 0;
    n.refused(v.get_period(nullptr, &frames), "get_period, of no sink");
    n.refused(v.start(nullptr), "start, of no sink");
    n.refused(v.stop(nullptr), "stop, of no sink");
    n.refused(v.wait(nullptr, 0), "wait, on no sink");
    void* where = nullptr;
    n.refused(v.acquire(nullptr, &where, &frames), "acquire, from no sink");
    n.refused(v.commit(nullptr, 0, 0), "commit, to no sink");
    if (v.get_position != nullptr) {
        std::uint64_t played = 0;
        std::uint64_t ticks = 0;
        n.refused(v.get_position(nullptr, &played, &ticks), "get_position, of no sink");
    }
    v.close(nullptr);
}

} // namespace

TEST_CASE("every module refuses nonsense, and lives through it", "[abi][modules]")
{
    // **A careless host is a host**, and the modules are where a careless
    // call ends up: a null where the object should be, nowhere to write an
    // answer, a path to nothing. The demuxer fuzzers make those calls on
    // objects that exist; this makes them on none, for every module of every
    // kind this tree builds, found rather than named, as the test above finds
    // them. Nothing here opens a device, a window or an endpoint.
    const auto found = modules();
    REQUIRE(found.size() >= 10u);

    std::vector<std::string> accepted;
    std::size_t asked = 0;
    for (const auto& path : found) {
        const std::string name = path.filename().string();
        INFO(name);
        auto* dll = ::LoadLibraryW(path.c_str());
        if (dll == nullptr) {
            continue; // the test above says which, and why
        }
        using Entry = const MpModuleDesc*(MP_CALL*)(std::uint32_t);
        auto* entry = reinterpret_cast<Entry>(
            reinterpret_cast<void*>(::GetProcAddress(dll, "mp_module_entry")));
        const MpModuleDesc* desc = entry != nullptr ? entry(MP_ABI_VERSION) : nullptr;
        if (desc == nullptr || desc->vtbl == nullptr ||
            (desc->init != nullptr && desc->init(&quiet_host()) != MP_OK)) {
            ::FreeLibrary(dll);
            continue;
        }
        Nonsense n{name, {}};
        const MpCodec declared = desc->codec_count != 0 ? desc->codecs[0] : MP_CODEC_PCM;
        switch (desc->kind) {
        case MP_KIND_DEMUX:
            ask_demuxer(n, *static_cast<const MpDemuxVtbl*>(desc->vtbl));
            break;
        case MP_KIND_CODEC:
            ask_codec(n, *static_cast<const MpCodecVtbl*>(desc->vtbl), declared);
            break;
        case MP_KIND_VCODEC:
            ask_video_codec(n, *static_cast<const MpVideoCodecVtbl*>(desc->vtbl), declared);
            break;
        case MP_KIND_DSP:
            ask_dsp(n, *static_cast<const MpDspVtbl*>(desc->vtbl));
            break;
        case MP_KIND_VDSP:
            ask_video_dsp(n, *static_cast<const MpVideoDspVtbl*>(desc->vtbl));
            break;
        case MP_KIND_VIDEO:
            ask_presenter(n, *static_cast<const MpVideoVtbl*>(desc->vtbl));
            break;
        case MP_KIND_SINK:
            ask_sink(n, *static_cast<const MpSinkVtbl*>(desc->vtbl));
            break;
        default:
            break;
        }
        ++asked;
        accepted.insert(accepted.end(), n.accepted.begin(), n.accepted.end());
        if (desc->shutdown != nullptr) {
            desc->shutdown();
        }
        ::FreeLibrary(dll);
    }
    CHECK(asked >= 10u);
    INFO("calls a module answered MP_OK when it was handed nonsense:" << listed(accepted));
    CHECK(accepted.empty());
}

TEST_CASE("every codec the ABI declares has a name", "[abi]")
{
    // **Six of them did not, for two ABI versions.** `codec_name` was written
    // when this tree decoded audio, and the video codecs arrived in v3 without
    // it -- so `claims` printed `codec 0x00000041` for an HEVC stream and
    // `0x00000042` for an AV1 one, which is the id in hex and tells a reader
    // nothing. Nothing failed, because a fallback that formats the number is
    // indistinguishable from a name until somebody reads the output.
    //
    // The list here is the ABI's, so the next codec added to the header fails
    // this until it is named. That is the whole point: the fallback exists for
    // an id from a *newer* module than this build, and it should never be
    // reached for one this build compiled against.
    struct Known {
        MpCodec codec;
        const char* what;
    };
    const Known every[] = {
        {MP_CODEC_PCM, "PCM"},         {MP_CODEC_DSD, "DSD"},
        {MP_CODEC_FLAC, "FLAC"},       {MP_CODEC_ALAC, "ALAC"},
        {MP_CODEC_WAVPACK, "WavPack"}, {MP_CODEC_APE, "Monkey's Audio"},
        {MP_CODEC_TTA, "TTA"},         {MP_CODEC_MP1, "MPEG-1 layer I"},
        {MP_CODEC_MP2, "MPEG-1 layer II"}, {MP_CODEC_MP3, "MP3"},
        {MP_CODEC_AAC_LC, "AAC-LC"},   {MP_CODEC_HE_AAC, "HE-AAC"},
        {MP_CODEC_VORBIS, "Vorbis"},   {MP_CODEC_OPUS, "Opus"},
        {MP_CODEC_SPEEX, "Speex"},     {MP_CODEC_WMA, "WMA"},
        {MP_CODEC_AC3, "AC-3"},        {MP_CODEC_EAC3, "E-AC-3"},
        {MP_CODEC_DTS, "DTS"},
        {MP_CODEC_H264, "H.264"},      {MP_CODEC_HEVC, "HEVC"},
        {MP_CODEC_AV1, "AV1"},         {MP_CODEC_VP8, "VP8"},
        {MP_CODEC_VP9, "VP9"},         {MP_CODEC_AV2, "AV2"},
        {MP_CODEC_INTERNAL, "internal"}, {MP_CODEC_UNKNOWN, "unknown"},
    };

    for (const Known& one : every) {
        const std::string got = mp::codec_name(one.codec);
        INFO("codec " << static_cast<unsigned>(one.codec) << " came back as " << got);
        // Not the hex fallback, which is what an unnamed id produces.
        CHECK(got.find("0x") == std::string::npos);
        CHECK(got == one.what);
    }

    // And an id this build has never heard of still says something rather than
    // nothing, because a module newer than the host is allowed to exist.
    const std::string future = mp::codec_name(static_cast<MpCodec>(4242u));
    INFO(future);
    CHECK_FALSE(future.empty());
}
