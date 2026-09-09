# SPDX-License-Identifier: GPL-3.0-or-later
#
# The scaler, as a stage. §9.11's resampling lived in the presenter's own
# passes until the HDR10 test patterns asked for a choice of kernel with every
# parameter a person can set, and a second implementation to hold the first
# to -- both of which are what a stage is for. The presenter keeps a bilinear
# fetch for a chain that has no scaler in it, and this is in the default chain.
#
# Windows-only for the reason `vdsp/lut` is: it runs on the presenter's
# Direct3D 11 device. The kernels themselves are `modules/shared/kernels`,
# which is where a Metal or a Vulkan scaler would take them from.

mediaperch_add_module(mp_vdsp_scale
    KIND vdsp
    SOURCES vdsp_scale.cpp
    LINK mediaperch_kernels mediaperch_module_log)
