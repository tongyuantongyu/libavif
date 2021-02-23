// Copyright 2019 Joe Drago. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef LIBAVIF_APPS_SHARED_AVIFJPEG_H
#define LIBAVIF_APPS_SHARED_AVIFJPEG_H

#include "avifutil.h"

#ifdef __cplusplus
extern "C" {
#endif

avifBool avifJPEGRead(const char * inputFilename, avifImage * avif, avifAppReadOptions options);
avifBool avifJPEGWrite(const char * outputFilename, const avifImage * avif, avifAppWriteOptions options);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // ifndef LIBAVIF_APPS_SHARED_AVIFJPEG_H
