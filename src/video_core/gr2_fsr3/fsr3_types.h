// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Gr2Fsr3 {

/// Program hashes (Shader::Info::pgm_hash) of the Gravity Rush 2 passes the module recognizes.
namespace Hash {
constexpr u64 Antialias = 0x6f705679;
constexpr u64 Velocity = 0xaafa61e8;
constexpr u64 CbResolve = 0x8c5d4362;
constexpr u64 MotionBlurPrepare = 0x2b25bef7;
constexpr u64 Ssao0 = 0x2a8c3ba9;
constexpr u64 Ssao1 = 0x4c9725fc;
constexpr u64 Contour = 0x8503bcb7;
constexpr u64 Ibl0 = 0x93a1bf1d;
constexpr u64 Ibl1 = 0x3615273f;
constexpr u64 FullscreenVs = 0xe3c2cc19;
constexpr u64 CompositePs = 0x63d65960;
constexpr u64 CompositeLensPs = 0xac19a466;
} // namespace Hash

/// Bits of the fsr3Test key.
namespace TestBit {
constexpr u32 JitterOff = 1u << 0;
constexpr u32 JitterSignFlip = 1u << 1;
constexpr u32 MotionVectorYFlip = 1u << 2;
constexpr u32 CoverageJitter = 1u << 3;
constexpr u32 CutDetectionOff = 1u << 6;
constexpr u32 RepeatZeroingOff = 1u << 7;
constexpr u32 CharacterMaskHistory = 1u << 8;
constexpr u32 CharacterCameraMotion = 1u << 9;
constexpr u32 LodBiasExtraHalf = 1u << 10;
constexpr u32 LodBiasExtraOne = 1u << 11;
constexpr u32 Fsr4DepthInverted = 1u << 12;
constexpr u32 Fsr4DepthLinear = 1u << 13;
} // namespace TestBit

/// Encoding of the colour input as FSR reads it.
enum class ColorSpace : u8 {
    Linear, ///< Read through an sRGB view: FSR works on linear values.
    Gamma,  ///< Read through a UNORM view: FSR works on sRGB-encoded values.
};

/// Depth image handed to FSR.
enum class DepthSource : u8 {
    Copy, ///< Mip 0 of the R32F device-depth copy that the Velocity pass reads.
    Main, ///< Depth aspect of the scene depth-stencil image.
};

/// Debug output written in place of the Antialias result. The values are the fsr3Debug values.
enum class DebugView : u8 {
    Off,
    MotionVectors,
    Stencil,
    Depth,
    Masks,
    Reprojection,
    Bypass,
};

/// How one Antialias dispatch is handled.
enum class Mode : u8 {
    Stock,    ///< The guest dispatch runs.
    Fsr,      ///< FSR 3.1 followed by the store pass.
    Fallback, ///< The fallback pass scales the colour input into the history image.
    Debug,    ///< The debug pass writes the history image.
    Fsr4,     ///< FSR 4 (INT8, experimental) followed by the store pass.
};

/// Model set of FSR 4. The values are the fsr4Model names; Auto selects by the upscale ratio.
enum class Fsr4Model : u8 {
    Auto,
    Native,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
    Drs,
};

/// Extent of the Antialias output relative to its colour input.
enum class Kind : u8 {
    Native,  ///< Equal extents.
    Upscale, ///< The output is at least as large in both dimensions and larger in one.
    Other,
};

/// Lifecycle of an upscaler context.
enum class ContextState : u8 {
    Unsupported,
    Idle,
    Creating,
    Ready,
    Failed,
    Loading, ///< FSR 4 reads and checks its shader and model files.
};

/// Camera parameters for FSR. The defaults are the game's own camera defaults.
struct CameraParams {
    float near_plane{1.0f};
    float far_plane{10000.0f};
    float vertical_fov{0.8726646f};
    bool valid{};
};

} // namespace Gr2Fsr3
