# SPDX-License-Identifier: GPL-3.0-or-later
#
# The first MP_KIND_VDSP module, and the reason the kind exists.
#
# §9.8.3 held the interface back until something wanted it -- a kind number with
# no vtable and no module is the mistake §4 records making once. This is what
# wanted it: colour grading is what the video engine is meant to carry, and a
# cube LUT is what grading ships.
#
# Windows-only because the stage runs on the presenter's device (§9.8.1) and
# this tree's presenter is Direct3D 11. The half that is not -- reading the
# file -- is `modules/shared/cube`, which a Metal or a Vulkan stage takes
# unchanged.

mediaperch_add_module(mp_vdsp_lut
    KIND vdsp
    SOURCES vdsp_lut.cpp
    LINK mediaperch_cube mediaperch_module_log)
