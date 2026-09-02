// SPDX-License-Identifier: GPL-3.0-or-later
//
// The door a shell knocks on.
//
// A named pipe, because it is the one Windows IPC that is a byte stream with an
// access-controlled name and no network in it. `PIPE_REJECT_REMOTE_CLIENTS` is
// set for that last reason: an audio engine has no business being reachable
// from another machine, and a default that allowed it would be one nobody
// noticed until it mattered.
//
// **A shell may die at any moment, and this is where that is made true.** Every
// client gets a reader thread and a writer thread with a bounded queue between
// the engine and its pipe. A shell that stops reading fills its queue and is
// dropped; it cannot slow the engine down, and it certainly cannot stop it. The
// audio path shares nothing with any of this -- no lock, no allocation, no
// thread -- which is the whole reason the shell is a separate process.

#ifndef MEDIAPERCH_WIN_IPC_SERVER_HPP
#define MEDIAPERCH_WIN_IPC_SERVER_HPP

#include "mediaperch/log.hpp"
#include "mediaperch/player.hpp"
#include "mediaperch/protocol.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mp::win {

class IpcServer {
public:
    IpcServer(Player& player, LogRing& log, std::string pipe_name);
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) = delete;
    IpcServer& operator=(IpcServer&&) = delete;

    /// Starts listening. False and a reason when the pipe cannot be created --
    /// almost always because another engine already has it.
    [[nodiscard]] bool start(std::string& why);
    void stop();

    /// Set when a shell asked the engine to quit. The engine's main thread
    /// watches this rather than being killed from inside a callback.
    [[nodiscard]] bool quit_requested() const noexcept
    {
        return quit_.load(std::memory_order_acquire);
    }

    /// How many shells are attached. For the log, and for a test.
    [[nodiscard]] std::size_t clients() const;

private:
    struct Client;

    void listen();
    /// One waiting instance of the pipe, or an invalid handle.
    [[nodiscard]] void* make_instance() const;
    void read_from(const std::shared_ptr<Client>& client);
    void write_to(const std::shared_ptr<Client>& client);
    void notify();
    /// Answers one message. False to close the connection.
    bool handle(const std::shared_ptr<Client>& client, const ipc::Header& header,
                const std::vector<std::uint8_t>& payload);
    void sweep();

    Player* player_;
    LogRing* log_;
    std::string name_;

    /// The instance `start` opened, handed to the listener for its first turn.
    /// Created there rather than on the listener thread so that `start`
    /// returning true means the door is actually open -- a shell that connects
    /// the instant it is told the engine is up must not find nothing there.
    void* pending_ = nullptr;
    std::thread listener_;
    std::thread notifier_;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Client>> clients_;

    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    /// The listener has left. `stop` knocks on the pipe until this is true,
    /// because a single knock can arrive while the listener is between
    /// instances and find nothing to wake.
    std::atomic<bool> listening_{false};
};

} // namespace mp::win

#endif // MEDIAPERCH_WIN_IPC_SERVER_HPP
