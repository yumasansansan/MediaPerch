// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/ipc_client.hpp"

#include "mediaperch/win_headers.hpp"

namespace mp::win {
namespace {

bool read_exactly(HANDLE pipe, std::uint8_t* into, std::size_t bytes)
{
    std::size_t got = 0;
    while (got < bytes) {
        DWORD read = 0;
        if (ReadFile(pipe, into + got, static_cast<DWORD>(bytes - got), &read, nullptr) ==
                FALSE ||
            read == 0) {
            return false;
        }
        got += read;
    }
    return true;
}

} // namespace

const char* default_pipe_name() noexcept
{
    return "\\\\.\\pipe\\mediaperch";
}

IpcClient::~IpcClient()
{
    close();
}

bool IpcClient::connected() const noexcept
{
    return pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE;
}

bool IpcClient::connect(const std::string& name, unsigned timeout_ms, std::string& why)
{
    close();
    for (;;) {
        const HANDLE pipe =
            CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            pipe_ = pipe;
            return true;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY) {
            why = error == ERROR_FILE_NOT_FOUND
                      ? "no engine is running: start mediaperchd, or say `--pipe` if it "
                        "is listening somewhere else"
                      : "could not connect to " + name;
            return false;
        }
        // Every instance is talking to another shell. Waiting is right: they
        // are short conversations.
        if (WaitNamedPipeA(name.c_str(), timeout_ms) == FALSE) {
            why = "the engine is busy with other shells";
            return false;
        }
    }
}

void IpcClient::close()
{
    if (connected()) {
        CloseHandle(static_cast<HANDLE>(pipe_));
    }
    pipe_ = nullptr;
}

std::uint32_t IpcClient::send(ipc::Kind kind, const ipc::Writer& payload)
{
    const std::uint32_t id = next_id_++;
    const auto message = ipc::frame(kind, id, payload);
    if (!connected()) {
        return id;
    }
    std::size_t sent = 0;
    while (sent < message.size()) {
        DWORD wrote = 0;
        if (WriteFile(static_cast<HANDLE>(pipe_), message.data() + sent,
                      static_cast<DWORD>(message.size() - sent), &wrote, nullptr) ==
                FALSE ||
            wrote == 0) {
            close();
            return id;
        }
        sent += wrote;
    }
    return id;
}

std::uint32_t IpcClient::send(ipc::Kind kind)
{
    const ipc::Writer empty;
    return send(kind, empty);
}

bool IpcClient::receive(ipc::Header& header, std::vector<std::uint8_t>& body)
{
    if (!connected()) {
        return false;
    }
    std::uint8_t head[ipc::k_header_bytes]{};
    if (!read_exactly(static_cast<HANDLE>(pipe_), head, sizeof(head))) {
        close();
        return false;
    }
    if (!ipc::parse_header(head, sizeof(head), header)) {
        close();
        return false;
    }
    body.assign(header.payload, 0u);
    if (header.payload != 0 &&
        !read_exactly(static_cast<HANDLE>(pipe_), body.data(), body.size())) {
        close();
        return false;
    }
    return true;
}

bool IpcClient::call(ipc::Kind kind, const ipc::Writer& payload, ipc::Header& reply,
                     std::vector<std::uint8_t>& body, std::string& why)
{
    const std::uint32_t id = send(kind, payload);
    if (!connected()) {
        why = "the engine closed the connection";
        return false;
    }
    for (;;) {
        if (!receive(reply, body)) {
            why = "the engine closed the connection";
            return false;
        }
        if (reply.id == id) {
            return true;
        }
        // An event, or an answer to somebody else's question on a shared
        // connection. Not ours; keep reading.
    }
}

bool IpcClient::call(ipc::Kind kind, ipc::Header& reply, std::vector<std::uint8_t>& body,
                     std::string& why)
{
    const ipc::Writer empty;
    return call(kind, empty, reply, body, why);
}

std::string error_text(const ipc::Header& header, const std::vector<std::uint8_t>& body)
{
    if (static_cast<ipc::Kind>(header.kind) != ipc::Kind::error) {
        return std::string{"the engine answered `"} +
               ipc::kind_name(static_cast<ipc::Kind>(header.kind)) + "`";
    }
    ipc::Reader r{body.data(), body.size()};
    const std::string why = r.str();
    return r.ok() ? why : "the engine sent an error it could not spell";
}

} // namespace mp::win
