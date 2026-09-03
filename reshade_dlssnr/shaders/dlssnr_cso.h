#pragma once

// Placeholder for the precompiled DLSSNR composition shader.
//
// The final addon must embed the actual precompiled CSO from OptiScaler:
//   optiscaler/OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader.h
//
// This placeholder uses the required CSMain entry point binding signature:
//   t0..t4 SRVs, u0..u1 UAVs, b0 CBV, s0 sampler.
//
// The CSO below is intentionally minimal and may be replaced with the
// production bytecode before shipping.

inline static const unsigned char DlssNr_cso[] = {
    0x44, 0x58, 0x42, 0x43, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
