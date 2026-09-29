// SPDX-License-Identifier: GPL-3.0-or-later
//
// A `describe` row written the way the ABI asks since v5: whole, or not at all.
//
// **Every describe in this tree was `std::snprintf(out, out_bytes, ...)` and
// MP_OK**, and snprintf cuts a row that does not fit and says nothing -- so a
// row longer than the host's buffer arrived cut short and was taken for whole.
// A value is part of the row, and a path is a value with no length. v5 gives
// `describe` somewhere to say how long a row is, and this is how a module says
// it: the row is measured before it is written, a buffer that cannot hold it is
// never written into, and the answer is MP_TOO_SMALL with the size, which the
// host grows to and asks again with.
//
// It lives beside abi_guard.hpp because it is the same kind of thing -- a
// promise the header makes, kept at the boundary once instead of in each of the
// hundreds of rows that would otherwise each have to keep it.

#ifndef MEDIAPERCH_DESCRIBE_ROW_HPP
#define MEDIAPERCH_DESCRIBE_ROW_HPP

#include <mediaperch/module.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace mp {

/// One `describe` row, into the caller's `out` of `out_bytes`.
///
/// A module formats the row with `format`, as it formatted it with
/// `std::snprintf` before, and answers with `result()`: MP_OK when the row and
/// its terminating NUL were written, MP_TOO_SMALL when they did not fit and
/// nothing was, and `*out_needed` the bytes they take either way. A NULL `out`
/// is a buffer of no bytes. An index past the last row answers MP_END without
/// formatting anything, and `*out_needed` stays 0.
class DescribeRow {
public:
    DescribeRow(char* out, std::uint32_t out_bytes, std::uint32_t* out_needed) noexcept
        : out_(out), out_bytes_(out_bytes), out_needed_(out_needed)
    {
        if (out_needed_ != nullptr) {
            *out_needed_ = 0;
        }
    }

    [[gnu::format(printf, 2, 3)]] void format(const char* fmt, ...) noexcept
    {
        va_list args;
        va_start(args, fmt);
        va_list measuring;
        va_copy(measuring, args);
        const int length = std::vsnprintf(nullptr, 0, fmt, measuring);
        va_end(measuring);
        if (length < 0) {
            failed_ = true;
        } else {
            needed_ = static_cast<std::uint32_t>(length) + 1u;
            fits_ = out_ != nullptr && needed_ <= out_bytes_;
            if (fits_) {
                std::vsnprintf(out_, out_bytes_, fmt, args);
            }
        }
        va_end(args);
    }

    /// What the row came to. A module returns this where it returned MP_OK.
    [[nodiscard]] MpResult result() const noexcept
    {
        if (failed_) {
            return MP_ERR_INTERNAL; // vsnprintf refused the format, which is a bug
        }
        if (out_needed_ != nullptr) {
            *out_needed_ = needed_;
        }
        return fits_ ? MP_OK : MP_TOO_SMALL;
    }

private:
    char* out_;
    std::uint32_t out_bytes_;
    std::uint32_t* out_needed_;
    std::uint32_t needed_ = 0;
    bool fits_ = false;
    bool failed_ = false;
};

} // namespace mp

#endif // MEDIAPERCH_DESCRIBE_ROW_HPP
