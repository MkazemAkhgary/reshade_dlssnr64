#pragma once

// Root signature (ComputeEngine::CreateRootSignature):
//
//   root parameter 0: descriptor table with three ranges
//     t0..t3   SRV, 4 descriptors, register space 0
//     u0..u1   UAV, 2 descriptors, register space 0
//     b0       CBV, 1 descriptor,  register space 0
//
//   static sampler at s0: LINEAR + CLAMP, comparison NEVER,
//   transparent-black border, min LOD 0, max LOD FLT_MAX.
//
// Every Dispatch() call provides exactly kSrvCount SRVs and kUavCount UAV slots.
// Unused SRV slots repeat a valid resource; null unused UAV slots repeat u0.
// The CPU-side counts live in gpu/compute_engine.h; this file is documentation.
//
// The CBV is ComputeConstants from common/params.h and is always a 256-byte
// CBV slot. The static sampler is the only sampler bound by the root signature.
