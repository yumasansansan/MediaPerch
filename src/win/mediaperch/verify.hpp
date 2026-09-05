// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The parts of bit-exactness verification that need Windows: a hash, and a
// capture endpoint to hear what actually came out.
//
// This is test equipment, not a product path. It lives in the head rather than
// behind the module ABI because a capture interface with one implementation and
// one caller would be an interface added on speculation.

#include <mediaperch/module.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace mp::win {

/// SHA-256, from the platform's own provider. No third-party crypto to vendor,
/// audit or update.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    Sha256(Sha256&&) = delete;
    Sha256& operator=(Sha256&&) = delete;

    /// Whether there is a hash to feed. A feed that failed is remembered and
    /// makes `hex` empty rather than the digest of whatever did get in.
    [[nodiscard]] bool ok() const noexcept { return hash_ != nullptr && !failed_; }

    void update(const void* data, std::size_t bytes) noexcept;

    /// Finalises and returns the digest as lower-case hex. Further calls to
    /// `update` are undefined; make a new one.
    [[nodiscard]] std::string hex();

    /// One-shot.
    [[nodiscard]] static std::string of(const void* data, std::size_t bytes);

private:
    void* algorithm_ = nullptr;
    void* hash_ = nullptr;
    bool failed_ = false;
};

/// A WASAPI capture endpoint, taken exclusively, recording into memory.
///
/// The other end of a loopback: play into a virtual cable's render endpoint,
/// record from its capture endpoint, and the two byte streams should be equal.
/// Anything that is not equal happened inside Windows or inside the driver,
/// which is the half of the path our own tests cannot see.
class Capture {
public:
    Capture() = default;
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture(Capture&&) = delete;
    Capture& operator=(Capture&&) = delete;

    /// Enumerates capture endpoints. Returns MP_END past the last one.
    static MpResult enumerate(std::uint32_t index, MpDeviceInfo& out) noexcept;

    /// Finds the first capture endpoint whose name contains `needle` (UTF-8,
    /// case-sensitive). Empty on no match.
    [[nodiscard]] static std::string find_by_name(const std::string& needle);

    MpResult open(const std::string& device_id);

    /// Exclusive mode, event-driven, the same realign dance as the sink.
    MpResult negotiate(const MpFormat& want);

    /// Starts a thread that appends every period into memory until `stop`.
    MpResult start(std::size_t reserve_bytes);
    void stop() noexcept;

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    [[nodiscard]] std::uint64_t discontinuities() const noexcept { return discontinuities_; }
    [[nodiscard]] std::uint64_t silent_periods() const noexcept { return silent_; }
    [[nodiscard]] std::uint32_t period_frames() const noexcept { return buffer_frames_; }

private:
    void record_loop() noexcept;
    void release() noexcept;

    void* client_ = nullptr;   // IAudioClient*
    void* capture_ = nullptr;  // IAudioCaptureClient*
    void* device_ = nullptr;   // IMMDevice*
    void* event_ = nullptr;    // HANDLE
    std::uint32_t buffer_frames_ = 0;
    std::uint32_t frame_bytes_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::vector<std::uint8_t> data_;
    std::uint64_t discontinuities_ = 0;
    std::uint64_t silent_ = 0;
};

/// The endpoint's current master volume, 0..1, or -1 when it cannot be read.
///
/// Worth printing next to any loopback result: a cable whose driver applies
/// volume by scaling samples cannot be bit-exact at anything but unity, and the
/// symptom is a mismatch that looks like a bug in the player.
[[nodiscard]] float endpoint_volume(const std::string& device_id, bool capture) noexcept;

/// Where `needle` first occurs in `haystack`, or npos. Used to line the captured
/// stream up with the played one, because a capture starts whenever it starts.
[[nodiscard]] std::size_t find_bytes(const std::vector<std::uint8_t>& haystack,
                                     const std::uint8_t* needle, std::size_t needle_bytes,
                                     std::size_t from = 0) noexcept;

} // namespace mp::win
