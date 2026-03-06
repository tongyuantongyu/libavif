// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_APPS_H
#define AVIF_APPS_H

#include "avif/avif.h" // IWYU pragma: export

#ifdef __cplusplus
extern "C" {
#endif

// Custom codec can export these symbols for avifenc/avifdec to load them.
#define AVIF_APPS_CUSTOM_CODEC_SETUP_SYMBOL "avifAppsCustomCodecSetup"
#define AVIF_APPS_CUSTOM_CODEC_SHUTDOWN_SYMBOL "avifAppsCustomCodecShutdown"

typedef avifResult (*avifAppsCustomCodecSetupFunc)(avifDiagnostics * diag);
typedef avifResult (*avifAppsCustomCodecShutdownFunc)(avifDiagnostics * diag);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // AVIF_APPS_H
