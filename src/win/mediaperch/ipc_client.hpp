// SPDX-License-Identifier: GPL-3.0-or-later
//
// The other end of the pipe: what a shell uses to talk to the engine.
//
// Small enough to read in one sitting, which is the point. A third-party shell
// -- a web front end, a hardware remote, somebody's Qt window -- should be able
// to reimplement this from `protocol.hpp` in an afternoon, and the way to keep
// that true is for the reference client to have nothing clever in it.

#ifndef MEDIAPERCH_WIN_IPC_CLIENT_HPP
#define MEDIAPERCH_WIN_IPC_CLIENT_HPP

#include "mediaperch/protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mp::win {

/// Where an engine listens unless it was told otherwise: the pipe called
/// `mediaperch`, spelt the way `CreateFile` wants a pipe spelt.
/// Declared with the client because the client is who has to find it.
[[nodiscard]] const char* default_pipe_name() noexcept;

class IpcClient {
public:
    IpcClient() = default;
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;
    IpcClient(IpcClient&&) = delete;
    IpcClient& operator=(IpcClient&&) = delete;

    /// Connects, waiting up to `timeout_ms` for an engine that is busy with
    /// other shells. False and a reason when there is no engine there.
    [[nodiscard]] bool connect(const std::string& name, unsigned timeout_ms,
                               std::string& why);
    void close();
    [[nodiscard]] bool connected() const noexcept;

    /// Sends a message. The id is chosen here and returned.
    [[nodiscard]] std::uint32_t send(ipc::Kind kind, const ipc::Writer& payload);
    [[nodiscard]] std::uint32_t send(ipc::Kind kind);

    /// Reads the next message, whatever it is. False when the engine went away.
    [[nodiscard]] bool receive(ipc::Header& header, std::vector<std::uint8_t>& body);

    /// Sends and waits for the reply to *that* message, handing anything that
    /// arrives first back through `receive`'s door -- an engine may be talking
    /// about something else when a request goes out, and a shell that assumed
    /// the next message was its answer would be wrong exactly when it is busy.
    [[nodiscard]] bool call(ipc::Kind kind, const ipc::Writer& payload,
                            ipc::Header& reply, std::vector<std::uint8_t>& body,
                            std::string& why);
    [[nodiscard]] bool call(ipc::Kind kind, ipc::Header& reply,
                            std::vector<std::uint8_t>& body, std::string& why);

private:
    void* pipe_ = nullptr; // HANDLE
    std::uint32_t next_id_ = 1;
};

/// The text of an `error` reply, or a general description when it is not one.
[[nodiscard]] std::string error_text(const ipc::Header& header,
                                     const std::vector<std::uint8_t>& body);

} // namespace mp::win

#endif // MEDIAPERCH_WIN_IPC_CLIENT_HPP
