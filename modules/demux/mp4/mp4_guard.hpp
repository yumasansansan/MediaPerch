// SPDX-License-Identifier: GPL-3.0-or-later
//
// The boxes `demux_mp4` does not let Bento4 parse, and why.
//
// It is a header rather than a private class in the module because
// `fuzz/mp4_fuzzer.cpp` has to use it too. A fuzzer that walks a different path
// from the shipped module tests a program nobody runs -- and here the difference
// would be exactly the box the fuzzer already found a denial of service in, so
// the corpus would fail on its own regression seed.

#ifndef MEDIAPERCH_MP4_GUARD_HPP
#define MEDIAPERCH_MP4_GUARD_HPP

#include <Ap4.h>

namespace mp::mp4 {

/// An atom factory that declines to *parse* three boxes this tree never reads.
///
/// **All three had the same defect, and `mp4_fuzzer` found two of them in the
/// first ten minutes it ran.** A box states an entry count; the parser loops
/// that many times; nothing checks the count against the bytes the box actually
/// has. Measured, from files kept in `fuzz/corpus/mp4/`:
///
///  * `sgpd` -- a 26-byte box declaring 67,108,865 entries of two bytes each,
///    one `AP4_DataBuffer` allocated per entry. **2 GB and no return**, out of a
///    1143-byte file. `mediaperch-probe claims` on it did not come back.
///  * `dref` -- a 28-byte box declaring 956,301,312 entries. Its inner loop
///    drains the stream on the first pass, so the remaining 956 million
///    iterations each do a `Tell`, two reads and a `Seek` against nothing.
///    **84 seconds**, out of a 1269-byte file.
///
/// `sbgp` is here because it is the other half of `sgpd` -- a sample-to-group
/// box means nothing without the descriptions it points into -- and it has the
/// same shape.
///
/// **Not parsing them is free.** Sample groups describe roll distances and
/// rate-adaptation groups, for editors and packagers; `dref` says which file the
/// media lives in, and this tree reads self-contained files. Nothing in Bento4
/// reads a parsed `dref` either: the only other mention of the class is
/// `Ap4TrakAtom.cpp` constructing one when *writing* a track.
///
/// Leaving `atom` null and returning success is Bento4's own path for a box it
/// does not recognise: the caller rewinds and keeps the bytes as an
/// `AP4_UnknownAtom`, so the box is still there, still the right size, and
/// simply not interpreted.
///
/// **This is the sharp instrument; `FileStream`'s operation budget in
/// demux_mp4.cpp is the blunt one.** The budget bounds any parse loop that
/// touches the file, which covers `sgpd` and the general class of this bug. It
/// does *not* cover `dref`, whose spin never reads a byte -- `bytes_available <
/// 8` returns before any I/O -- which is exactly why both defences are here.
///
/// **The durable fix is upstream, and it is small.** In `Ap4SgpdAtom.cpp`,
/// subtract each entry from `bytes_available` as it is read and stop after the
/// first entry when the version is 0, since a version-0 entry consumes the rest
/// of the box by definition. In `Ap4DrefAtom.cpp`, break out of the outer loop
/// when the inner one adds nothing.
class GuardedAtomFactory : public AP4_DefaultAtomFactory {
public:
    AP4_Result CreateAtomFromStream(AP4_ByteStream& stream, AP4_UI32 type,
                                    AP4_UI32 size_32, AP4_UI64 size_64,
                                    AP4_Atom*& atom) override
    {
        if (type == AP4_ATOM_TYPE_SGPD || type == AP4_ATOM_TYPE_SBGP ||
            type == AP4_ATOM_TYPE_DREF) {
            atom = nullptr;
            return AP4_SUCCESS;
        }
        return AP4_DefaultAtomFactory::CreateAtomFromStream(stream, type, size_32,
                                                           size_64, atom);
    }
};

} // namespace mp::mp4

#endif // MEDIAPERCH_MP4_GUARD_HPP
