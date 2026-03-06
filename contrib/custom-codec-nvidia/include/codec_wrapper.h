// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CODEC_WRAPPER_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CODEC_WRAPPER_H

#include "avif/codec.h"

namespace avif_nvenc
{

const char * codec_version();
avifCodec * create_codec();

} // namespace avif_nvenc

#endif // AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CODEC_WRAPPER_H
