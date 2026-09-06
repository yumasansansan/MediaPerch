// SPDX-License-Identifier: GPL-3.0-or-later
//
// The boundary an exception must not cross, and what it becomes instead.
//
// **Every function in a module's vtable is `noexcept`, and it has to be.** The
// host may be another compiler's C++, or Rust, or C, and an exception that left
// this DLL would be undefined behaviour rather than an error. What `noexcept`
// does about that is call `std::terminate`, which is right for the ABI and
// wrong for the program: one stage that cannot allocate takes the daemon down
// and every other track with it.
//
// So the boundary catches, and the shape is a function-try-block on each entry
// point -- the same one `demux_wav` and fifteen other module files already use.
// `std::bad_alloc` becomes MP_ERR_NO_MEMORY, a code the ABI has had since v1
// and which no DSP module has ever returned; anything else becomes
// MP_ERR_INTERNAL, because a module that throws something else has a bug, and
// the host's business is to stop trusting the stage rather than to guess.
//
// **Only at the boundary.** A catch inside an algorithm hides the bug it
// caught; a catch at the vtable turns a failure into a result code, which is
// what a result code is for. The Rust modules draw the same fence with
// `catch_unwind` -- modules/shared/mp-abi says so in its own words -- and this
// is that fence in the other language.
//
// **What it cannot do**, and the Rust side has the same hole from the other
// direction: it converts a failure that is *reported*. A vendored library that
// calls `abort`, or a Rust allocation failure -- which reaches
// `handle_alloc_error` and aborts rather than unwinding -- is past any fence
// either language knows how to build. C++ is the easier of the two here, and
// only here: `new` throws, and a throw can be turned into a number.

#ifndef MEDIAPERCH_ABI_GUARD_HPP
#define MEDIAPERCH_ABI_GUARD_HPP

#include <mediaperch/module.h>

#include <new>

/// The catch half of an entry point's function-try-block.
///
/// A macro rather than a wrapper, because the alternative is a lambda around
/// every body and this leaves the bodies exactly as they were -- which is what
/// makes the change reviewable: the diff is two lines a function and no
/// statement moved.
#define MEDIAPERCH_ABI_GUARD_CATCH                                                       \
    catch (const std::bad_alloc&)                                                        \
    {                                                                                    \
        return MP_ERR_NO_MEMORY;                                                         \
    }                                                                                    \
    catch (...)                                                                          \
    {                                                                                    \
        return MP_ERR_INTERNAL;                                                          \
    }

#endif // MEDIAPERCH_ABI_GUARD_HPP
