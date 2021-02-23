// Copyright 2021 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef LIBAVIF_APPS_SHARED_AVIFWIC_H
#define LIBAVIF_APPS_SHARED_AVIFWIC_H

#include "avifutil.h"

// if (requestedDepth == 0), do best-fit
avifBool avifWICRead(const char * inputFilename, avifImage * avif, avifAppReadOptions options, uint32_t * outDepth);

#endif //LIBAVIF_APPS_SHARED_AVIFWIC_H
