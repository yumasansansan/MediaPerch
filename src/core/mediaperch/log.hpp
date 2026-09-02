// SPDX-License-Identifier: GPL-3.0-or-later
//
// The last few hundred things the engine said.
//
// A headless process with nowhere to print to is a process nobody can ask what
// happened. This is the answer: a bounded ring that costs nothing to keep, that
// a shell can read the tail of, and that a shell can subscribe to and watch.
//
// **Bounded on purpose.** An engine that runs for a week must not grow a log in
// memory, and a file is the head's business rather than this one's -- what a
// shell wants is the last screenful, and that is what this is.

#ifndef MEDIAPERCH_LOG_HPP
#define MEDIAPERCH_LOG_HPP

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace mp {

class LogRing {
public:
    using Listener = std::function<void(const std::string&)>;

    explicit LogRing(std::size_t capacity = 256) : capacity_(capacity) {}

    /// Adds a line, stamped with how long the engine has been running. Called
    /// from any thread, including the engine's -- never from a render thread,
    /// which does not log at all.
    void add(const std::string& line);

    /// The last `count` lines, oldest first. All of them when `count` is zero.
    [[nodiscard]] std::vector<std::string> tail(std::size_t count = 0) const;

    /// Watches every line from now on. The listener is called on whichever
    /// thread logged, so it must not block -- a shell that has stopped reading
    /// its pipe must not be able to stop the engine.
    [[nodiscard]] int listen(Listener listener);
    void forget(int token);

private:
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    std::vector<std::pair<int, Listener>> listeners_;
    std::size_t capacity_;
    int next_token_ = 1;
};

} // namespace mp

#endif // MEDIAPERCH_LOG_HPP
