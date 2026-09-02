// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/ipc_server.hpp"

#include "mediaperch/win_headers.hpp"

#include <algorithm>
#include <exception>
#include <chrono>
#include <utility>

namespace mp::win {
namespace {

/// Messages a shell may be behind by before it is dropped. A transport tick is
/// four a second; two hundred and fifty of them is a shell that stopped reading
/// a minute ago and is not coming back.
constexpr std::size_t k_queue_limit = 256;

/// How often a subscribed shell is told where the audio is. Four times a second
/// is a transport bar that looks alive and a cost nobody can measure.
constexpr auto k_tick = std::chrono::milliseconds{250};

/// Bytes a client may keep the engine reading before it is a client sending
/// nonsense. The protocol's own limit; this is where it is enforced.
constexpr std::uint32_t k_max_payload = ipc::k_max_payload;

/// One overlapped operation, with its event, on the stack of whoever is
/// waiting for it.
///
/// **Both halves of this matter and they pull in opposite directions.** The
/// handle has to be overlapped, because a synchronous handle serialises every
/// operation on it -- the reader blocked on a request would hold up the write
/// that answers it, and a duplex pipe with a reader thread and a writer thread
/// would deadlock on the first message. And an overlapped handle has to be
/// given a real `OVERLAPPED`, because passing a null one is a promise to the
/// kernel that the buffer and the byte count will still be there when the I/O
/// finishes. Getting the first right and the second wrong cost a day: a crash
/// that only appeared in Release, only once data had flowed, and that moved
/// whenever anything was added to look at it, because what the kernel was
/// writing into was a stack frame that had already gone.
///
/// So: a real OVERLAPPED, and a wait that does not return until the kernel has
/// finished with it.
class Operation {
public:
    Operation() { overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~Operation()
    {
        if (overlapped_.hEvent != nullptr) {
            CloseHandle(overlapped_.hEvent);
        }
    }
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    Operation(Operation&&) = delete;
    Operation& operator=(Operation&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return overlapped_.hEvent != nullptr; }
    [[nodiscard]] OVERLAPPED* get() noexcept { return &overlapped_; }

    /// Waits for it, whether it completed immediately or is still going.
    [[nodiscard]] bool finish(HANDLE pipe, BOOL immediate, DWORD& transferred)
    {
        if (immediate != FALSE) {
            return true;
        }
        if (GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        return GetOverlappedResult(pipe, &overlapped_, &transferred, TRUE) != FALSE;
    }

private:
    OVERLAPPED overlapped_{};
};

bool read_exactly(HANDLE pipe, std::uint8_t* into, std::size_t bytes)
{
    std::size_t got = 0;
    while (got < bytes) {
        Operation op;
        if (!op.valid()) {
            return false;
        }
        DWORD read = 0;
        const BOOL immediate =
            ReadFile(pipe, into + got, static_cast<DWORD>(bytes - got), &read, op.get());
        if (!op.finish(pipe, immediate, read) || read == 0) {
            return false;
        }
        got += read;
    }
    return true;
}

bool write_exactly(HANDLE pipe, const std::uint8_t* from, std::size_t bytes)
{
    std::size_t sent = 0;
    while (sent < bytes) {
        Operation op;
        if (!op.valid()) {
            return false;
        }
        DWORD wrote = 0;
        const BOOL immediate = WriteFile(pipe, from + sent,
                                         static_cast<DWORD>(bytes - sent), &wrote, op.get());
        if (!op.finish(pipe, immediate, wrote) || wrote == 0) {
            return false;
        }
        sent += wrote;
    }
    return true;
}

std::vector<std::uint8_t> error_message(std::uint32_t id, const std::string& why)
{
    ipc::Writer w;
    w.str(why);
    return ipc::frame(ipc::Kind::error, id, w);
}

} // namespace

struct IpcServer::Client {
    HANDLE pipe = INVALID_HANDLE_VALUE;

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::vector<std::uint8_t>> out;
    bool closing = false;
    bool subscribed = false;
    int log_token = 0;

    std::thread reader;
    std::thread writer;
    std::atomic<bool> done{false};

    /// Queues one message. False when the shell is too far behind to keep.
    bool send(std::vector<std::uint8_t> message)
    {
        {
            const std::lock_guard lock{mutex};
            if (closing) {
                return false;
            }
            if (out.size() >= k_queue_limit) {
                // Dropped rather than waited for. A shell that has stopped
                // reading its pipe must not be able to stop the engine, and
                // that is the entire reason this queue is bounded.
                closing = true;
                wake.notify_all();
                return false;
            }
            out.push_back(std::move(message));
        }
        wake.notify_all();
        return true;
    }

    void close()
    {
        {
            const std::lock_guard lock{mutex};
            closing = true;
        }
        wake.notify_all();
        if (pipe != INVALID_HANDLE_VALUE) {
            // Wakes the reader out of a blocking ReadFile. Closing the handle
            // underneath it instead would be a race with the read itself.
            CancelIoEx(pipe, nullptr);
        }
    }
};

IpcServer::IpcServer(Player& player, LogRing& log, std::string pipe_name)
    : player_(&player), log_(&log), name_(std::move(pipe_name))
{
}

IpcServer::~IpcServer()
{
    stop();
}

bool IpcServer::start(std::string& why)
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    // Somebody else may already be the engine. Finding out by connecting is
    // more honest than finding out by creating a second instance of the pipe
    // and taking half the clients.
    const HANDLE probe = CreateFileA(name_.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                     0, nullptr);
    const DWORD probed = GetLastError();
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        why = "another engine is already listening on " + name_;
        return false;
    }
    if (probed == ERROR_PIPE_BUSY) {
        // The name exists and every instance of it is talking to somebody. That
        // is an engine with a full house, not an absent one.
        why = "another engine is already listening on " + name_;
        return false;
    }

    pending_ = make_instance();
    if (pending_ == INVALID_HANDLE_VALUE) {
        why = "could not create the pipe " + name_;
        return false;
    }
    running_.store(true, std::memory_order_release);
    listening_.store(true, std::memory_order_release);
    listener_ = std::thread{[this] {
        listen();
        listening_.store(false, std::memory_order_release);
    }};
    notifier_ = std::thread{[this] { notify(); }};
    return true;
}

void IpcServer::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // The listener is inside a blocking `ConnectNamedPipe`, and the only way to
    // end one of those is to connect. Repeatedly, because a single knock can
    // arrive while it is between instances and find nothing to wake.
    for (int attempt = 0; attempt < 400 && listening_.load(std::memory_order_acquire);
         ++attempt) {
        const HANDLE knock = CreateFileA(name_.c_str(), GENERIC_READ, 0, nullptr,
                                         OPEN_EXISTING, 0, nullptr);
        if (knock != INVALID_HANDLE_VALUE) {
            CloseHandle(knock);
        }
        Sleep(5);
    }
    if (notifier_.joinable()) {
        notifier_.join();
    }
    if (listener_.joinable()) {
        listener_.join();
    }

    std::vector<std::shared_ptr<Client>> clients;
    {
        const std::lock_guard lock{mutex_};
        clients.swap(clients_);
    }
    for (const auto& client : clients) {
        client->close();
        if (client->reader.joinable()) {
            client->reader.join();
        }
        if (client->writer.joinable()) {
            client->writer.join();
        }
        if (client->log_token != 0) {
            log_->forget(client->log_token);
        }
        if (client->pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(client->pipe);
        }
    }
}

std::size_t IpcServer::clients() const
{
    const std::lock_guard lock{mutex_};
    return std::count_if(clients_.begin(), clients_.end(), [](const auto& c) {
        return !c->done.load(std::memory_order_acquire);
    });
}

void IpcServer::sweep()
{
    std::vector<std::shared_ptr<Client>> finished;
    {
        const std::lock_guard lock{mutex_};
        const auto split = std::stable_partition(
            clients_.begin(), clients_.end(),
            [](const auto& c) { return !c->done.load(std::memory_order_acquire); });
        finished.assign(std::make_move_iterator(split),
                        std::make_move_iterator(clients_.end()));
        clients_.erase(split, clients_.end());
    }
    for (const auto& client : finished) {
        if (client->reader.joinable()) {
            client->reader.join();
        }
        if (client->writer.joinable()) {
            client->writer.join();
        }
        if (client->log_token != 0) {
            log_->forget(client->log_token);
        }
        if (client->pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(client->pipe);
        }
    }
}

void* IpcServer::make_instance() const
{
    return CreateNamedPipeA(
        name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
}

void IpcServer::listen()
{
    while (running_.load(std::memory_order_acquire)) {
        sweep();

        // The first turn uses the instance `start` opened; every turn after it
        // opens its own, so that there is always exactly one waiting.
        HANDLE pipe = static_cast<HANDLE>(pending_);
        pending_ = nullptr;
        if (pipe == nullptr) {
            pipe = static_cast<HANDLE>(make_instance());
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            log_->add("the engine could not create its pipe");
            return;
        }

        Operation connect;
        if (!connect.valid()) {
            CloseHandle(pipe);
            return;
        }
        DWORD ignored = 0;
        const BOOL immediate = ConnectNamedPipe(pipe, connect.get());
        // A client that connected before `ConnectNamedPipe` ran is already
        // there, which Windows reports as a failure and is not one.
        const bool have_client = connect.finish(pipe, immediate, ignored) ||
                                 GetLastError() == ERROR_PIPE_CONNECTED;
        if (!have_client) {
            CloseHandle(pipe);
            continue;
        }
        // `stop` knocks on this door to end the wait above. What arrived is a
        // knock and not a shell.
        if (!running_.load(std::memory_order_acquire)) {
            CloseHandle(pipe);
            return;
        }
        if (!running_.load(std::memory_order_acquire)) {
            CloseHandle(pipe);
            return;
        }

        auto client = std::make_shared<Client>();
        client->pipe = pipe;
        // Guarded, because a thread that dies of an exception takes the process
        // with it, and this process is somebody's music. A shell is not allowed
        // to be the reason the audio stops.
        client->reader = std::thread{[this, client] {
            try {
                read_from(client);
            } catch (const std::exception& e) {
                log_->add(std::string{"a shell's reader failed: "} + e.what());
            } catch (...) {
                log_->add("a shell's reader failed");
            }
            client->done.store(true, std::memory_order_release);
        }};
        client->writer = std::thread{[this, client] {
            try {
                write_to(client);
            } catch (const std::exception& e) {
                log_->add(std::string{"a shell's writer failed: "} + e.what());
            } catch (...) {
                log_->add("a shell's writer failed");
            }
        }};
        {
            const std::lock_guard lock{mutex_};
            clients_.push_back(client);
        }
    }
}

void IpcServer::write_to(const std::shared_ptr<Client>& client)
{
    for (;;) {
        std::vector<std::uint8_t> message;
        {
            std::unique_lock lock{client->mutex};
            client->wake.wait(lock,
                              [&] { return client->closing || !client->out.empty(); });
            if (client->out.empty()) {
                return; // closing, and nothing left to say
            }
            message = std::move(client->out.front());
            client->out.pop_front();
        }
        if (!write_exactly(client->pipe, message.data(), message.size())) {
            client->close();
            return;
        }
    }
}

void IpcServer::read_from(const std::shared_ptr<Client>& client)
{
    std::uint8_t head[ipc::k_header_bytes]{};
    std::vector<std::uint8_t> payload;
    while (running_.load(std::memory_order_acquire)) {
        if (!read_exactly(client->pipe, head, sizeof(head))) {
            break;
        }
        ipc::Header header{};
        if (!ipc::parse_header(head, sizeof(head), header)) {
            // Not one of ours, or from a version this build cannot read. There
            // is no recovering a byte stream whose framing is wrong, so the
            // connection ends rather than being resynchronised.
            (void)client->send(error_message(0, "that is not this protocol"));
            break;
        }
        payload.assign(header.payload, 0u);
        if (header.payload > k_max_payload ||
            (header.payload != 0 &&
             !read_exactly(client->pipe, payload.data(), payload.size()))) {
            break;
        }
        if (!handle(client, header, payload)) {
            break;
        }
    }
    client->close();
    client->done.store(true, std::memory_order_release);
}

bool IpcServer::handle(const std::shared_ptr<Client>& client, const ipc::Header& header,
                       const std::vector<std::uint8_t>& payload)
{
    const auto kind = static_cast<ipc::Kind>(header.kind);
    const std::uint32_t id = header.id;
    ipc::Reader r{payload.data(), payload.size()};
    ipc::Writer w;

    const auto ok = [&] { return client->send(ipc::frame(ipc::Kind::ok, id)); };
    const auto fail = [&](const std::string& why) {
        return client->send(error_message(id, why));
    };
    const auto malformed = [&] { return fail("that message is not well formed"); };

    switch (kind) {
    case ipc::Kind::hello: {
        (void)r.u32();
        const std::string who = r.str();
        if (!r.complete()) {
            return malformed();
        }
        log_->add("a shell attached: " + (who.empty() ? std::string{"(unnamed)"} : who));
        w.u32(ipc::k_version);
        w.str("mediaperchd");
        return client->send(ipc::frame(ipc::Kind::hello_reply, id, w));
    }
    case ipc::Kind::status: {
        if (!r.complete()) {
            return malformed();
        }
        write(w, player_->status());
        return client->send(ipc::frame(ipc::Kind::status_reply, id, w));
    }
    case ipc::Kind::play:
    case ipc::Kind::enqueue: {
        std::vector<std::string> files;
        if (!ipc::read_strings(r, files) || !r.complete()) {
            return malformed();
        }
        if (files.empty()) {
            return fail("nothing to play");
        }
        if (kind == ipc::Kind::play) {
            player_->play(std::move(files));
        } else {
            player_->enqueue(files);
        }
        return ok();
    }
    case ipc::Kind::clear:
        player_->clear();
        return ok();
    case ipc::Kind::pause:
        player_->pause();
        return ok();
    case ipc::Kind::resume:
        player_->resume();
        return ok();
    case ipc::Kind::stop:
        player_->stop();
        return ok();
    case ipc::Kind::next:
        player_->next();
        return ok();
    case ipc::Kind::previous:
        player_->previous();
        return ok();
    case ipc::Kind::seek: {
        const std::uint8_t relative = r.u8();
        const std::int64_t frames = r.i64();
        if (!r.complete()) {
            return malformed();
        }
        if (!player_->seek(frames, relative != 0)) {
            return fail("nothing is playing, or this source cannot seek");
        }
        return ok();
    }
    case ipc::Kind::playlist: {
        if (!r.complete()) {
            return malformed();
        }
        ipc::write_strings(w, player_->playlist());
        w.u32(player_->status().index);
        return client->send(ipc::frame(ipc::Kind::playlist_reply, id, w));
    }
    case ipc::Kind::settings: {
        if (!r.complete()) {
            return malformed();
        }
        write(w, player_->settings());
        return client->send(ipc::frame(ipc::Kind::settings_reply, id, w));
    }
    case ipc::Kind::setting_set: {
        const std::string key = r.str();
        const std::string value = r.str();
        if (!r.complete()) {
            return malformed();
        }
        std::string why;
        if (!player_->set(key, value, why)) {
            return fail(why);
        }
        log_->add("set " + key + " = " + value);
        return ok();
    }
    case ipc::Kind::log: {
        const std::uint32_t count = r.u32();
        if (!r.complete()) {
            return malformed();
        }
        ipc::write_strings(w, log_->tail(count));
        return client->send(ipc::frame(ipc::Kind::log_reply, id, w));
    }
    case ipc::Kind::subscribe: {
        if (!r.complete()) {
            return malformed();
        }
        {
            // Scoped, and it matters: `send` takes this same mutex, and a
            // `std::mutex` relocked by the thread that holds it is a deadlock
            // -- which the standard library was decent enough to turn into an
            // exception rather than a hang.
            const std::lock_guard lock{client->mutex};
            if (!client->subscribed) {
                client->subscribed = true;
                // Weak, because the client may be gone by the time a line is
                // logged and the log must not be what keeps it alive.
                std::weak_ptr<Client> weak = client;
                client->log_token = log_->listen([weak](const std::string& line) {
                    if (const auto strong = weak.lock()) {
                        ipc::Writer event;
                        event.str(line);
                        (void)strong->send(ipc::frame(ipc::Kind::event_log, 0, event));
                    }
                });
            }
        }
        return client->send(ipc::frame(ipc::Kind::ok, id));
    }
    case ipc::Kind::quit:
        log_->add("a shell asked the engine to quit");
        quit_.store(true, std::memory_order_release);
        return ok();
    default:
        return fail(std::string{"the engine does not know `"} + ipc::kind_name(kind) + "`");
    }
}

void IpcServer::notify()
{
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(k_tick);
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }

        std::vector<std::shared_ptr<Client>> subscribed;
        {
            const std::lock_guard lock{mutex_};
            for (const auto& client : clients_) {
                const std::lock_guard inner{client->mutex};
                if (client->subscribed && !client->closing) {
                    subscribed.push_back(client);
                }
            }
        }
        if (subscribed.empty()) {
            continue;
        }

        const ipc::Status status = player_->status();
        ipc::Writer w;
        write(w, status);
        const auto message = ipc::frame(ipc::Kind::event_state, 0, w);
        for (const auto& client : subscribed) {
            // Queued, never written from here: a shell that has stopped reading
            // is dropped by its own queue and this thread does not wait for it.
            (void)client->send(message);
        }
    }
}

} // namespace mp::win
