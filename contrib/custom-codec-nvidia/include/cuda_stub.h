// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CUDA_STUB_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CUDA_STUB_H

#ifdef _WIN32
#define CUDAAPI __stdcall
#else
#define CUDAAPI
#endif

typedef enum cudaError_enum
{
    CUDA_SUCCESS = 0,
} CUresult;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;

CUresult CUDAAPI cuGetErrorString(CUresult error, const char **pStr);
CUresult CUDAAPI cuGetErrorName(CUresult error, const char **pStr);
CUresult CUDAAPI cuInit(unsigned int Flags);
CUresult CUDAAPI cuDeviceGet(CUdevice *device, int ordinal);
CUresult CUDAAPI cuDevicePrimaryCtxRetain(CUcontext *pctx, CUdevice dev);
CUresult CUDAAPI cuDevicePrimaryCtxRelease_v2(CUdevice dev);

#endif //AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CUDA_STUB_H
