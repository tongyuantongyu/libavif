// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_AVIF_NVENC_CODEC_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_AVIF_NVENC_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "avif/avif.h"

#if defined(AVIF_NVENC_DLL) && defined(AVIF_NVENC_USING_STATIC_LIBS)
#error "Your target is linking against avif_nvenc and custom-codec-nvenc_internal: only one should be chosen"
#endif

#if defined(AVIF_NVENC_DLL)
#if defined(AVIF_NVENC_BUILDING_SHARED_LIBS)
#define AVIF_NVENC_API AVIF_HELPER_EXPORT
#else
#define AVIF_NVENC_API AVIF_HELPER_IMPORT
#endif
#else
#define AVIF_NVENC_API
#endif

#define AVIF_NVENC_CODEC_NAME "nvenc"

// Encode-time codec options.
#define AVIF_NVENC_OPTION_POOL "pool"
#define AVIF_NVENC_OPTION_PRESET "preset"
#define AVIF_NVENC_OPTION_TUNING "tuning"
#define AVIF_NVENC_OPTION_QP "qp" // In NVENC AV1 encoder, qp range is 0-255
#define AVIF_NVENC_OPTION_AQ "aq" // Enabled by default; 0 disables spatial AQ.
// auto (default): QP-dependent policy; 0: driver selection; 1-15: fixed strength. Implies AQ.
#define AVIF_NVENC_OPTION_AQ_STRENGTH "aq-strength"
#define AVIF_NVENC_OPTION_MIN_PART_SIZE "min-part-size"
#define AVIF_NVENC_OPTION_MAX_PART_SIZE "max-part-size"
#define AVIF_NVENC_OPTION_Y_DC_QP_OFFSET "y-dc-qp-offset"
#define AVIF_NVENC_OPTION_U_DC_QP_OFFSET "u-dc-qp-offset"
#define AVIF_NVENC_OPTION_V_DC_QP_OFFSET "v-dc-qp-offset"
#define AVIF_NVENC_OPTION_CB_QP_OFFSET "cb-qp-offset"
#define AVIF_NVENC_OPTION_CR_QP_OFFSET "cr-qp-offset"
// Setting any of the five QP offsets disables automatic tuning of all five;
// unspecified offsets then retain the NVENC preset's values.
#define AVIF_NVENC_OPTION_SPLIT_ENCODE_MODE "split-encode-mode"
// Path to an AOM film grain table file. Only the entry covering time 0 is used.
#define AVIF_NVENC_OPTION_FILM_GRAIN_TABLE "film-grain-table"
// 0 (default, off) to 24. Adds a preset film grain tuned to hide the color banding.
// Cannot be combined with film-grain-table.
#define AVIF_NVENC_OPTION_FILM_GRAIN_DITHER_STRENGTH "film-grain-dither-strength"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum avifNvencPoolMode
{
    AVIF_NVENC_POOL_MODE_ON_DEMAND = 0,
    AVIF_NVENC_POOL_MODE_OMNI = 1,
    AVIF_NVENC_POOL_MODE_BUCKET = 2,
} avifNvencPoolMode;

typedef struct avifNvencImageSize
{
    uint32_t width;
    uint32_t height;
} avifNvencImageSize;

typedef struct avifNvencPoolConfig
{
    const char * id;
    int gpu;
    avifNvencPoolMode mode;

    // maxSessions limits the total number of live NVENC sessions owned by the
    // pool across all compatible session keys. If maxSessions is 0, it defaults
    // to 1.
    uint32_t maxSessions;

    // Only for AVIF_NVENC_POOL_MODE_OMNI
    avifNvencImageSize maxSize;

    // Only for AVIF_NVENC_POOL_MODE_BUCKET
    const avifNvencImageSize * bucketSizes;
    size_t bucketSizeCount;
} avifNvencPoolConfig;

// Registers the custom NVENC AV1 encoder as codec name "nvenc".
// Supports single images and pre-scaled layered images, but not animations.
// When encoding layered images, you must not expect a later started avifEncoder
// to finish encoding earlier when sharing one session pool.
AVIF_NVENC_API avifResult avifNvencRegister(void);

// Returns the registered codec choice, or AVIF_CODEC_CHOICE_AUTO if not registered.
AVIF_NVENC_API avifCodecChoice avifNvencCodecChoice(void);

// Creates a reusable NVENC pool.
AVIF_NVENC_API avifResult avifNvencCreatePool(const avifNvencPoolConfig * config, avifDiagnostics * diag);

// Shuts down a previously created NVENC pool. This waits for outstanding
// avifEncoders to be finished. Call this for every pool before process exit.
AVIF_NVENC_API avifResult avifNvencShutdownPool(const char * poolId, avifDiagnostics * diag);

// Returns the contrib module version string.
AVIF_NVENC_API const char * avifNvencVersion(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_AVIF_NVENC_CODEC_H
