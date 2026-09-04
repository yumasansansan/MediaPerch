// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace mp {
namespace {

/// Since the first line rather than since the epoch: what a person reading a
/// log wants to know is how long after the last thing something happened, and a
/// wall clock in a headless process is a formatting problem for no gain.
std::string stamp()
{
    static const auto began = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - began).count();
    char out[32];
    std::snprintf(out, sizeof(out), "[%4lld.%03lld] ", static_cast<long long>(ms / 1000),
                  static_cast<long long>(ms % 1000));
    return out;
}

} // namespace

void LogRing::add(const std::string& line)
{
    const std::string stamped = stamp() + line;
    std::vector<std::pair<int, Listener>> listeners;
    {
        const std::lock_guard lock{mutex_};
        lines_.push_back(stamped);
        while (lines_.size() > capacity_) {
            lines_.pop_front();
        }
        listeners = listeners_;
    }
    // Called outside the lock: a listener that logs would otherwise deadlock,
    // and a listener that is slow would hold up everybody else's line.
    for (const auto& [token, listener] : listeners) {
        (void)token;
        if (listener) {
            listener(stamped);
        }
    }
}

std::vector<std::string> LogRing::tail(std::size_t count) const
{
    const std::lock_guard lock{mutex_};
    const std::size_t take = count == 0 ? lines_.size() : std::min(count, lines_.size());
    return std::vector<std::string>{lines_.end() - static_cast<std::ptrdiff_t>(take),
                                    lines_.end()};
}

int LogRing::listen(Listener listener)
{
    const std::lock_guard lock{mutex_};
    const int token = next_token_++;
    listeners_.emplace_back(token, std::move(listener));
    return token;
}

void LogRing::forget(int token)
{
    const std::lock_guard lock{mutex_};
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                    [token](const auto& e) { return e.first == token; }),
                     listeners_.end());
}

} // namespace mp
