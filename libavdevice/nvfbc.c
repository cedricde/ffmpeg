/*
 * NVIDIA Framebuffer Capture grabber
 * Copyright (C) 2022 Cedric Delmas
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>

#include "compat/nvfbc/NvFBC.h"
#include "compat/nvfbc/dynlink_loader.h"

#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#if CONFIG_CUDA
# include "libavutil/cuda_check.h"
# include "libavutil/hwcontext.h"
# include "libavutil/hwcontext_cuda_internal.h"
#endif

#include "libavformat/avformat.h"
#include "libavformat/internal.h"

#define NVFBC_CHECK_VERSION(major, minor)  \
    (NVFBC_VERSION_MAJOR > (major) || \
     (NVFBC_VERSION_MAJOR == (major) && NVFBC_VERSION_MINOR >= (minor)))

typedef struct NvFBCContext {
    const AVClass *class;

    /// Capture region offset and dimensions
    int x, y, w, h;
    /// Output frame size
    int frame_width, frame_height;
    /// Name of the output to capture or NULL for capture box in whole X screen
    const char *output_name;
    /// Index of the output to capture (valid only if output_name is not NULL)
    uint32_t output_id;
    /// Pixel format
    enum AVPixelFormat format;

    /// Index in array nvfbc_formats of the current pixel format
    int format_idx;

    /// Name of the CUDA device to use, NULL if not requested
    const char *hwdevice_name;
    /// Reference to the hardware device (non-NULL after the creation of the
    /// capture session if CUDA is used)
    AVBufferRef *hwdevice_ref;
    /// Reference to the hardware frames
    AVBufferRef *hwframes_ref;

    /// Capture framerate
    AVRational framerate;
    /// Time base
    AVRational time_base;
    /// Frame duration (in internal timebase)
    int64_t frame_duration;
    /// Current time
    int64_t time_frame;

    /// Pointer to NvFBC entry functions
    NvfbcFunctions *dl;
    /// Pointers to NvFBC API functions
    NVFBC_API_FUNCTION_LIST funcs;

    /// NvFBC session handle
    NVFBC_SESSION_HANDLE handle;
    /// True if the handle is created, false otherwise
    int has_handle;
    /// True if the capture session is created, false otherwise
    int has_capture_session;
    /// Pointer to frame data
    uint8_t *frame_data;
} NvFBCContext;

#define OFFSET(x) offsetof(NvFBCContext, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "video_size", "set capture output size", OFFSET(frame_width), AV_OPT_TYPE_IMAGE_SIZE, { .str = NULL }, 0, 0, FLAGS },
    { "pixel_format", "set pixel format", OFFSET(format), AV_OPT_TYPE_PIXEL_FMT, { .i64 = AV_PIX_FMT_BGRA }, -1, INT_MAX, FLAGS },
    { "framerate", "set capture framerate", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, { .str = "pal" }, 0, INT_MAX, FLAGS },
#if CONFIG_CUDA
    { "device", "CUDA device to use", OFFSET(hwdevice_name), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
#endif
    { NULL },
};


static const struct {
    /// Pixel format in FFmpeg
    enum AVPixelFormat pix_fmt;
    /// Buffer format in NvFBC
    NVFBC_BUFFER_FORMAT nvfbc_fmt;
    /// Bits per pixel
    int bpp;
} nvfbc_formats[] = {
    { AV_PIX_FMT_BGRA,      NVFBC_BUFFER_FORMAT_BGRA,       32 },  // native
    { AV_PIX_FMT_BGR0,      NVFBC_BUFFER_FORMAT_BGRA,       32 },  // same but ignore alpha channel
    { AV_PIX_FMT_ARGB,      NVFBC_BUFFER_FORMAT_ARGB,       32 },
    { AV_PIX_FMT_0RGB,      NVFBC_BUFFER_FORMAT_ARGB,       32 },  // same but ignore alpha channel
    { AV_PIX_FMT_RGBA,      NVFBC_BUFFER_FORMAT_RGBA,       32 },
    { AV_PIX_FMT_RGB0,      NVFBC_BUFFER_FORMAT_RGBA,       32 },  // same but ignore alpha channel
    { AV_PIX_FMT_RGB24,     NVFBC_BUFFER_FORMAT_RGB,        24 },
    { AV_PIX_FMT_YUV444P,   NVFBC_BUFFER_FORMAT_YUV444P,    24 },
    { AV_PIX_FMT_NV12,      NVFBC_BUFFER_FORMAT_NV12,       12 },
};

static const struct {
    NVFBCSTATUS nverr;
    int         averr;
    const char *desc;
} nvfbc_errors[] = {
    { NVFBC_SUCCESS,            0,                      "success"                   },
    { NVFBC_ERR_API_VERSION,    AVERROR(EINVAL),        "incompatible version"      },
    { NVFBC_ERR_INTERNAL,       AVERROR_EXTERNAL,       "internal error"            },
    { NVFBC_ERR_INVALID_PARAM,  AVERROR(EINVAL),        "invalid param"             },
    { NVFBC_ERR_INVALID_PTR,    AVERROR(EFAULT),        "invalid pointer"           },
    { NVFBC_ERR_INVALID_HANDLE, AVERROR(EBADF),         "invalid handle"            },
    { NVFBC_ERR_MAX_CLIENTS,    AVERROR(EUSERS),        "too many clients"          },
    { NVFBC_ERR_UNSUPPORTED,    AVERROR(ENOSYS),        "not supported"             },
    { NVFBC_ERR_OUT_OF_MEMORY,  AVERROR(ENOMEM),        "out of memory"             },
    { NVFBC_ERR_BAD_REQUEST,    AVERROR(EBADR),         "bad request"               },
    { NVFBC_ERR_X,              AVERROR_EXTERNAL,       "X error"                   },
    { NVFBC_ERR_GLX,            AVERROR_EXTERNAL,       "GLX error"                 },
    { NVFBC_ERR_GL,             AVERROR_EXTERNAL,       "OpenGL error"              },
    { NVFBC_ERR_CUDA,           AVERROR_EXTERNAL,       "CUDA error"                },
    { NVFBC_ERR_ENCODER,        AVERROR_EXTERNAL,       "HW encoder error"          },
    { NVFBC_ERR_CONTEXT,        AVERROR(EBADF),         "NvFBC context error"       },
    { NVFBC_ERR_MUST_RECREATE,  AVERROR_INPUT_CHANGED,  "modeset event occurred"    },
#if NVFBC_CHECK_VERSION(1, 8)
    { NVFBC_ERR_VULKAN,         AVERROR_EXTERNAL,       "Vulkan error"              },
#endif
};

/**
 * Convert NvFBC error code to FFmpeg.
 * @param nverr NvFBC error code.
 * @param desc Optional pointer which receives the associated error message.
 * @return FFmpeg error code.
 */
static int error_nv2av(NVFBCSTATUS nverr, const char **desc)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(nvfbc_errors); i++) {
        if (nverr == nvfbc_errors[i].nverr) {
            if (desc)
                *desc = nvfbc_errors[i].desc;

            return nvfbc_errors[i].averr;
        }
    }

    if (desc)
        *desc = "unknown error";

    return AVERROR_UNKNOWN;
}

/**
 * Wait until next frame shall be captured.
 *
 * @return Current wall-clock time.
 */
static int64_t wait_frame(AVFormatContext *s, AVPacket *pkt)
{
    NvFBCContext *ctx = s->priv_data;
    int64_t curtime, delay;

    ctx->time_frame += ctx->frame_duration;

    for (;;) {
        curtime = av_gettime_relative();
        delay   = ctx->time_frame - curtime;
        if (delay <= 0)
            break;
        av_usleep(delay);
    }

    return av_gettime();
}

/** Free function which does nothing. */
static void free_noop(void *opaque, uint8_t *data)
{
}

static av_cold int nvfbc_load_libraries(AVFormatContext *s)
{
    NvFBCContext *ctx = s->priv_data;
    NVFBCSTATUS nv_res;
    const char *desc;
    int res;

    res = nvfbc_load_functions(&ctx->dl, s);
    if (res < 0)
        return res;

    av_log(s, AV_LOG_VERBOSE, "Built for NvFBC API version %u.%u.\n",
           NVFBC_VERSION_MAJOR, NVFBC_VERSION_MINOR);

    ctx->funcs.dwVersion = NVFBC_VERSION;

    nv_res = ctx->dl->NvFBCCreateInstance(&ctx->funcs);
    if (nv_res != NVFBC_SUCCESS) {
        res = error_nv2av(nv_res, &desc);
        av_log(s, AV_LOG_ERROR, "Cannot create NvFBC instance: %s.\n", desc);
        return res;
    }

    return 0;
}

static av_cold int create_capture_session_tosys(AVFormatContext *s)
{
    NvFBCContext *ctx = s->priv_data;
    NVFBC_CREATE_CAPTURE_SESSION_PARAMS nvccs_params = {
        .dwVersion                   = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER,
        .eCaptureType                = NVFBC_CAPTURE_TO_SYS,
        .bDisableAutoModesetRecovery = NVFBC_TRUE,
        .bWithCursor                 = NVFBC_TRUE,
        .eTrackingType               = ctx->output_name ? NVFBC_TRACKING_OUTPUT
                                                        : NVFBC_TRACKING_SCREEN,
        .dwOutputId                  = ctx->output_id,
        .bPushModel                  = NVFBC_FALSE,
        .dwSamplingRateMs            = av_rescale_q(ctx->frame_duration,
                                                    AV_TIME_BASE_Q,
                                                    (AVRational){ 1, 1000 }),
        .captureBox                  = {
            .x = ctx->x,
            .y = ctx->y,
            .w = ctx->w,
            .h = ctx->h,
        },
        .frameSize                   = {
            .w = ctx->frame_width,
            .h = ctx->frame_height,
        },
        .bRoundFrameSize             = NVFBC_FALSE,
    };
    NVFBC_TOSYS_SETUP_PARAMS nvtss_params = {
        .dwVersion     = NVFBC_TOSYS_SETUP_PARAMS_VER,
        .eBufferFormat = nvfbc_formats[ctx->format_idx].nvfbc_fmt,
        .ppBuffer      = (void **)&ctx->frame_data,
        .bWithDiffMap  = NVFBC_FALSE,
    };
    NVFBCSTATUS nv_res;

    nv_res = ctx->funcs.nvFBCCreateCaptureSession(ctx->handle, &nvccs_params);
    if (nv_res != NVFBC_SUCCESS) {
        av_log(s, AV_LOG_ERROR,
               "Cannot create capture to system memory session: %s.\n",
               ctx->funcs.nvFBCGetLastErrorStr(ctx->handle));
        return error_nv2av(nv_res, NULL);
    } else {
        ctx->has_capture_session = 1;
    }

    nv_res = ctx->funcs.nvFBCToSysSetUp(ctx->handle, &nvtss_params);
    if (nv_res != NVFBC_SUCCESS) {
        av_log(s, AV_LOG_ERROR,
               "Cannot set up capture to system memory: %s.\n",
               ctx->funcs.nvFBCGetLastErrorStr(ctx->handle));
        return error_nv2av(nv_res, NULL);
    }

    return 0;
}

static int nvfbc_read_packet_tosys(AVFormatContext *s, AVPacket *pkt)
{
    NvFBCContext *ctx = s->priv_data;
    NVFBC_FRAME_GRAB_INFO frame_info = {};
    NVFBC_TOSYS_GRAB_FRAME_PARAMS params = {
        .dwVersion      = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER,
        .dwFlags        = NVFBC_TOSYS_GRAB_FLAGS_NOWAIT,
        .pFrameGrabInfo = &frame_info,
        .dwTimeoutMs    = 0,
    };
    int64_t pts;
    NVFBCSTATUS nv_res;

    pts = wait_frame(s, pkt);

    nv_res = ctx->funcs.nvFBCToSysGrabFrame(ctx->handle, &params);
    if (nv_res != NVFBC_SUCCESS) {
       av_log(s, AV_LOG_ERROR,
              "Cannot grab framebuffer to system memory: %s.\n",
              ctx->funcs.nvFBCGetLastErrorStr(ctx->handle));
        return error_nv2av(nv_res, NULL);
    }

    av_log(s, AV_LOG_DEBUG, "Frame %"PRIu32": %"PRIu32"x%"PRIu32", "
                            "%"PRIu32" bytes, ts=%"PRIu64" usecs, %s\n",
           frame_info.dwCurrentFrame, frame_info.dwWidth, frame_info.dwHeight,
           frame_info.dwByteSize, frame_info.ulTimestampUs,
           frame_info.bIsNewFrame ? "new" : "old");

    pkt->buf = av_buffer_create(ctx->frame_data, frame_info.dwByteSize,
                                free_noop, s, AV_BUFFER_FLAG_READONLY);
    if (!pkt->buf)
        return AVERROR(ENOMEM);

    pkt->data = ctx->frame_data;
    pkt->size = frame_info.dwByteSize;
    pkt->pts = pts;
    pkt->duration = ctx->frame_duration;

    return 0;
}

#if CONFIG_CUDA
static void free_av_frame(void *opaque, uint8_t *data)
{
    AVFrame *frame = (AVFrame*)data;

    av_frame_free(&frame);
}

static int nvfbc_push_context(AVFormatContext *s)
{
    NvFBCContext *ctx = s->priv_data;
    AVCUDADeviceContext *hwctx = ((AVHWDeviceContext*)ctx->hwdevice_ref->data)->hwctx;
    CudaFunctions *cudl = hwctx->internal->cuda_dl;

    return FF_CUDA_CHECK_DL(s, cudl, cudl->cuCtxPushCurrent(hwctx->cuda_ctx));
}

static int nvfbc_pop_context(AVFormatContext *s)
{
    NvFBCContext *ctx = s->priv_data;
    AVCUDADeviceContext *hwctx = ((AVHWDeviceContext*)ctx->hwdevice_ref->data)->hwctx;
    CudaFunctions *cudl = hwctx->internal->cuda_dl;
    CUcontext dummy;

    return FF_CUDA_CHECK_DL(s, cudl, cudl->cuCtxPopCurrent(&dummy));
}

static av_cold int create_capture_session_tocuda(AVFormatContext *s)
{
    NvFBCContext *ctx = s->priv_data;
    AVHWFramesContext *hwframes;
    NVFBC_CREATE_CAPTURE_SESSION_PARAMS nvccs_params = {
        .dwVersion                   = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER,
        .eCaptureType                = NVFBC_CAPTURE_SHARED_CUDA,
        .bDisableAutoModesetRecovery = NVFBC_TRUE,
        .bWithCursor                 = NVFBC_TRUE,
        .eTrackingType               = ctx->output_name ? NVFBC_TRACKING_OUTPUT
                                                        : NVFBC_TRACKING_SCREEN,
        .dwOutputId                  = ctx->output_id,
        .bPushModel                  = NVFBC_FALSE,
        .dwSamplingRateMs            = av_rescale_q(ctx->frame_duration,
                                                    AV_TIME_BASE_Q,
                                                    (AVRational){ 1, 1000 }),
        .captureBox                  = {
            .x = ctx->x,
            .y = ctx->y,
            .w = ctx->w,
            .h = ctx->h,
        },
        .frameSize                   = {
            .w = ctx->frame_width,
            .h = ctx->frame_height,
        },
        .bRoundFrameSize             = NVFBC_FALSE,
    };
    NVFBC_TOCUDA_SETUP_PARAMS nvtcs_params = {
        .dwVersion     = NVFBC_TOCUDA_SETUP_PARAMS_VER,
        .eBufferFormat = nvfbc_formats[ctx->format_idx].nvfbc_fmt,
    };
    NVFBCSTATUS nv_res;
    int res;

    // create CUDA hardware contexts
    res = av_hwdevice_ctx_create(&ctx->hwdevice_ref, AV_HWDEVICE_TYPE_CUDA,
                                 ctx->hwdevice_name, NULL, 0);
    if (res < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to open CUDA device.\n");
        return res;
    }

    ctx->hwframes_ref = av_hwframe_ctx_alloc(ctx->hwdevice_ref);
    if (!ctx->hwframes_ref)
        return AVERROR(ENOMEM);

    hwframes = (AVHWFramesContext*)ctx->hwframes_ref->data;
    hwframes->format    = AV_PIX_FMT_CUDA;
    hwframes->sw_format = ctx->format;
    hwframes->width     = ctx->frame_width;
    hwframes->height    = ctx->frame_height;

    res = av_hwframe_ctx_init(ctx->hwframes_ref);
    if (res < 0) {
        av_log(s, AV_LOG_ERROR,
               "Failed to initialize hardware frames context.\n");
        return res;
    }

    res = nvfbc_push_context(s);
    if (res < 0)
        return res;

    nv_res = ctx->funcs.nvFBCCreateCaptureSession(ctx->handle, &nvccs_params);
    if (nv_res != NVFBC_SUCCESS) {
        av_log(s, AV_LOG_ERROR,
               "Cannot create capture to video memory session: %s.\n",
               ctx->funcs.nvFBCGetLastErrorStr(ctx->handle));
        res = error_nv2av(nv_res, NULL);
        goto error_ctx;
    } else {
        ctx->has_capture_session = 1;
    }

    nv_res = ctx->funcs.nvFBCToCudaSetUp(ctx->handle, &nvtcs_params);
    if (nv_res != NVFBC_SUCCESS) {
        av_log(s, AV_LOG_ERROR,
               "Cannot set up capture to video memory: %s.\n",
               ctx->funcs.nvFBCGetLastErrorStr(ctx->handle));
        res = error_nv2av(nv_res, NULL);
        goto error_ctx;
    }

    res = 0;

error_ctx:
    nvfbc_pop_context(s);

    return res;
}

static int nvfbc_read_packet_tocuda(AVFormatContext *s, AVPacket *pkt)
{
    NvFBCContext *ctx = s->priv_data;
    AVHWFramesContext *hwframes = (AVHWFramesContext*)ctx->hwframes_ref->data;
    AVFrame *frame = NULL;
    CUdeviceptr dev_ptr;
    NVFBC_FRAME_GRAB_INFO frame_info = {};
    NVFBC_TOCUDA_GRAB_FRAME_PARAMS params = {
        .dwVersion         = NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER,
        .dwFlags           = NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT,
        .pCUDADeviceBuffer = &dev_ptr,
        .pFrameGrabInfo    = &frame_info,
        .dwTimeoutMs       = 0,
    };
    int64_t pts;
    NVFBCSTATUS nv_res;
    int res;

    pts = wait_frame(s, pkt);

    res = nvfbc_push_context(s);
    if (res < 0)
        return res;

    nv_res = ctx->funcs.nvFBCToCudaGrabFrame(ctx->handle, &params);

    nvfbc_pop_context(s);

    if (nv_res != NVFBC_SUCCESS) {
       av_log(s, AV_LOG_ERROR,
              "Cannot grab framebuffer to video memory: %s.\n",
              ctx->funcs.nvFBCGetLastErrorStr(ctx->handle));
        res = error_nv2av(nv_res, NULL);
        goto error;
    }

    av_log(s, AV_LOG_DEBUG, "Frame %"PRIu32": %"PRIu32"x%"PRIu32", "
                            "%"PRIu32" bytes, ts=%"PRIu64" usecs, %s\n",
           frame_info.dwCurrentFrame, frame_info.dwWidth, frame_info.dwHeight,
           frame_info.dwByteSize, frame_info.ulTimestampUs,
           frame_info.bIsNewFrame ? "new" : "old");

    frame = av_frame_alloc();
    if (!frame) {
        res = AVERROR(ENOMEM);
        goto error;
    }

    frame->hw_frames_ctx = av_buffer_ref(ctx->hwframes_ref);
    if (!frame->hw_frames_ctx) {
        res = AVERROR(ENOMEM);
        goto error;
    }

    frame->buf[0] = av_buffer_create((uint8_t*)dev_ptr, frame_info.dwByteSize,
                                     &free_noop, s, AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0]) {
        res = AVERROR(ENOMEM);
        goto error;
    }

    frame->format = AV_PIX_FMT_CUDA;
    frame->width  = frame_info.dwWidth;
    frame->height = frame_info.dwHeight;

    res = av_image_fill_arrays(frame->data, frame->linesize,
                               frame->buf[0]->data, hwframes->sw_format,
                               frame->width, frame->height, 4);
    if (res < 0)
        goto error;

    // YUV420P is a special case.
    // Nvenc expects the U/V planes in swapped order from how ffmpeg expects them, also chroma is half-aligned
    if (hwframes->sw_format == AV_PIX_FMT_YUV420P) {
        frame->linesize[1] = frame->linesize[2] = frame->linesize[0] / 2;
        frame->data[2]     = frame->data[1];
        frame->data[1]     = frame->data[2] + frame->linesize[2] * (hwframes->height / 2);
    }

    pkt->buf = av_buffer_create((uint8_t*)frame, sizeof(*frame),
                                &free_av_frame, s, 0);
    if (!pkt->buf) {
        res = AVERROR(ENOMEM);
        goto error;
    }

    pkt->data = (uint8_t*)frame;
    pkt->size = sizeof(*frame);
    pkt->flags |= AV_PKT_FLAG_TRUSTED;
    pkt->pts = pts;
    pkt->duration = ctx->frame_duration;

    return 0;

error:
    av_frame_free(&frame);

    return res;
}
#endif // CONFIG_CUDA

static av_cold int create_capture_handle(AVFormatContext *s)
{
    NvFBCContext *ctx = s->priv_data;
    NVFBC_CREATE_HANDLE_PARAMS nvch_params = {
        .dwVersion                 = NVFBC_CREATE_HANDLE_PARAMS_VER,
        .bExternallyManagedContext = NVFBC_FALSE,
    };
    NVFBC_GET_STATUS_PARAMS nvgs_params = {
        .dwVersion  = NVFBC_GET_STATUS_PARAMS_VER,
    };
    NVFBCSTATUS nv_res;

    nv_res = ctx->funcs.nvFBCCreateHandle(&ctx->handle, &nvch_params);
    if (nv_res != NVFBC_SUCCESS) {
        const char *desc;
        int res = error_nv2av(nv_res, &desc);
        av_log(s, AV_LOG_ERROR, "Cannot create NvFBC handle: %s.\n", desc);
        return res;
    } else {
        ctx->has_handle = 1;
    }

    nv_res = ctx->funcs.nvFBCGetStatus(ctx->handle, &nvgs_params);
    if (nv_res != NVFBC_SUCCESS) {
        av_log(s, AV_LOG_ERROR, "Cannot get NvFBC status: %s.\n",
               ctx->funcs.nvFBCGetLastErrorStr(ctx->handle));
        return error_nv2av(nv_res, NULL);
    }

    av_log(s, AV_LOG_VERBOSE, "NvFBC status:\n");
    av_log(s, AV_LOG_VERBOSE, "- Library API version: %"PRIu32".%"PRIu32"\n",
           (nvgs_params.dwNvFBCVersion >> 8) & 0xFF,
           nvgs_params.dwNvFBCVersion & 0xFF);
    av_log(s, AV_LOG_VERBOSE, "- Capture supported: %s\n",
           nvgs_params.bIsCapturePossible ? "yes" : "no");
    av_log(s, AV_LOG_VERBOSE, "- Capture currently running: %s\n",
           nvgs_params.bCurrentlyCapturing ? "yes" : "no");
    av_log(s, AV_LOG_VERBOSE, "- Capture creatable: %s\n",
           nvgs_params.bCanCreateNow ? "yes" : "no");
    av_log(s, AV_LOG_VERBOSE, "- Screen size: %"PRIu32"x%"PRIu32"\n",
           nvgs_params.screenSize.w, nvgs_params.screenSize.h);
    av_log(s, AV_LOG_VERBOSE, "- RandR extension available: %s\n",
           nvgs_params.bXRandRAvailable ? "yes" : "no");
#if NVFBC_CHECK_VERSION(1, 8)
    av_log(s, AV_LOG_VERBOSE, "- X server in modeset: %s\n",
           nvgs_params.bInModeset ? "yes" : "no");
#endif
    av_log(s, AV_LOG_VERBOSE, "- %"PRIu32" outputs connected:\n",
           nvgs_params.dwOutputNum);

    for (uint32_t dw = 0; dw < nvgs_params.dwOutputNum; dw++) {
        av_log(s, AV_LOG_VERBOSE,
               "  - %"PRIu32": %s (%"PRIu32"x%"PRIu32"+%"PRIu32"+%"PRIu32")\n",
               nvgs_params.outputs[dw].dwId,
               nvgs_params.outputs[dw].name,
               nvgs_params.outputs[dw].trackedBox.w,
               nvgs_params.outputs[dw].trackedBox.h,
               nvgs_params.outputs[dw].trackedBox.x,
               nvgs_params.outputs[dw].trackedBox.y);
    }

    if (nvgs_params.bCanCreateNow == NVFBC_FALSE) {
        av_log(s, AV_LOG_ERROR,
               "Cannot create a capture session on this system.\n");
        return AVERROR_EXTERNAL;
    }

    // look for requested output (if any)
    if (ctx->output_name) {
        int found = 0;

        for (uint32_t dw = 0; dw < nvgs_params.dwOutputNum; dw++) {
            if (!strcmp(ctx->output_name, nvgs_params.outputs[dw].name)) {
                // store output geometry
                ctx->x = nvgs_params.outputs[dw].trackedBox.x;
                ctx->y = nvgs_params.outputs[dw].trackedBox.y;
                ctx->w = nvgs_params.outputs[dw].trackedBox.w;
                ctx->h = nvgs_params.outputs[dw].trackedBox.h;
                ctx->output_id = nvgs_params.outputs[dw].dwId;

                found = 1;
                break;
            }
        }

        if (!found) {
            av_log(s, AV_LOG_ERROR, "Output '%s' not found\n",
                   ctx->output_name);
            return AVERROR(EINVAL);
        }
    }

    // store output frame size equal to capture box if not specified
    if (ctx->frame_width == 0)
        ctx->frame_width = ctx->w;
    if (ctx->frame_height == 0)
        ctx->frame_height = ctx->h;

    return 0;
}

static av_cold int create_stream(AVFormatContext *s)
{
    NvFBCContext *ctx = s->priv_data;
    AVStream *st;
    int64_t frame_size_bits;

    frame_size_bits = (int64_t)ctx->frame_width * ctx->frame_height * nvfbc_formats[ctx->format_idx].bpp;
    if (frame_size_bits / 8 + AV_INPUT_BUFFER_PADDING_SIZE > INT_MAX) {
        av_log(s, AV_LOG_ERROR, "Capture area is too large.\n");
        return AVERROR_PATCHWELCOME;
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    avpriv_set_pts_info(st, 64, 1, 1000000); /* 64 bits pts in us */

    st->avg_frame_rate = ctx->framerate;

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->width      = ctx->frame_width;
    st->codecpar->height     = ctx->frame_height;
    st->codecpar->bit_rate   = av_rescale(frame_size_bits, ctx->framerate.num,
                                                           ctx->framerate.den);
    if (ctx->hwdevice_ref) {
        st->codecpar->codec_id = AV_CODEC_ID_WRAPPED_AVFRAME;
        st->codecpar->format   = AV_PIX_FMT_CUDA;
    } else {
        st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
        st->codecpar->format   = ctx->format;
    }

    return 0;
}

static av_cold int nvfbc_read_close(AVFormatContext *s)
{
    NvFBCContext *ctx = s->priv_data;
    NVFBCSTATUS nv_res;

    if (ctx->has_capture_session) {
        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS params = {
            .dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER,
        };

        nv_res = ctx->funcs.nvFBCDestroyCaptureSession(ctx->handle, &params);
        if (nv_res != NVFBC_SUCCESS) {
            av_log(s, AV_LOG_WARNING, "Cannot destroy capture session: %s.\n",
                   ctx->funcs.nvFBCGetLastErrorStr(ctx->handle));
        }
        ctx->has_capture_session = 0;
    }

    if (ctx->has_handle) {
        NVFBC_DESTROY_HANDLE_PARAMS params = {
            .dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER,
        };

        nv_res = ctx->funcs.nvFBCDestroyHandle(ctx->handle, &params);
        if (nv_res != NVFBC_SUCCESS) {
            av_log(s, AV_LOG_WARNING, "Cannot destroy NvFBC handle: %s.\n",
                   ctx->funcs.nvFBCGetLastErrorStr(ctx->handle));
        }
        ctx->has_handle = 0;
    }

    av_buffer_unref(&ctx->hwframes_ref);
    av_buffer_unref(&ctx->hwdevice_ref);

    nvfbc_free_functions(&ctx->dl);

    return 0;
}

static av_cold int nvfbc_read_header(AVFormatContext *s)
{
    NvFBCContext *ctx = s->priv_data;
    Display *display = NULL;
    Screen *screen;
    int screen_w, screen_h;
    int count, res;
    char trailer;

    // load NvFBC library
    res = nvfbc_load_libraries(s);
    if (res < 0)
        goto error;

    // prepare X11 display
    display = XOpenDisplay(NULL);
    if (!display) {
        av_log(s, AV_LOG_ERROR, "Could not open the X11 display.\n");
        res = AVERROR(EIO);
        goto error;
    }
    screen = XDefaultScreenOfDisplay(display);
    screen_w = XWidthOfScreen(screen);
    screen_h = XHeightOfScreen(screen);

    // parse URL for definition of the capture box (treat "-" as whole display)
    if (s->url && s->url[0] && strcmp(s->url, "pipe:")) {
        // first format: size and optional position
        count = sscanf(s->url, " %d x %d + %d + %d %c",
                       &ctx->w, &ctx->h, &ctx->x, &ctx->y, &trailer);
        if (count != 2 && count != 4) {
            ctx->w = ctx->h = 0;

            // second format: position only
            count = sscanf(s->url, " + %d + %d %c", &ctx->x, &ctx->y, &trailer);
            if (count != 2) {
                ctx->x = ctx->y = 0;

                // no box definition, capture specific output
                ctx->output_name = s->url;
            }
        }
    }

    // compute capture region
    if (!ctx->output_name) {
        if (ctx->w == 0)
            ctx->w = FFMAX(screen_w - ctx->x, 0);
        if (ctx->h == 0)
            ctx->h = FFMAX(screen_h - ctx->y, 0);

        // check capture region
        if (ctx->x < 0 || ctx->y < 0) {
            av_log(s, AV_LOG_ERROR, "Invalid capture position +%d+%d\n",
                   ctx->x, ctx->y);
            res = AVERROR(EINVAL);
            goto error;
        }

        if (ctx->x + ctx->w > screen_w || ctx->y + ctx->h > screen_h) {
            av_log(s, AV_LOG_ERROR,
                   "Capture area %dx%d+%d+%d extends outside the screen %dx%d.\n",
                   ctx->w, ctx->h, ctx->x, ctx->y, screen_w, screen_h);
            res = AVERROR(EINVAL);
            goto error;
        }
    }

    // compute stream information
    ctx->time_base = av_inv_q(ctx->framerate);
    ctx->frame_duration = av_rescale_q(1, ctx->time_base, AV_TIME_BASE_Q);
    ctx->time_frame = av_gettime_relative();

    for (ctx->format_idx = 0;
         ctx->format_idx < FF_ARRAY_ELEMS(nvfbc_formats);
         ctx->format_idx++) {
        if (nvfbc_formats[ctx->format_idx].pix_fmt == ctx->format)
            break;
    }
    if (ctx->format_idx >= FF_ARRAY_ELEMS(nvfbc_formats)) {
        av_log(s, AV_LOG_ERROR, "Unsupported pixel format %s.\n",
               av_get_pix_fmt_name(ctx->format));
        res = AVERROR(EINVAL);
        goto error;
    }

    // prepare execution
    res = create_capture_handle(s);
    if (res < 0)
        goto error;

#if CONFIG_CUDA
    if (ctx->hwdevice_name)
        res = create_capture_session_tocuda(s);
    else
#endif
        res = create_capture_session_tosys(s);
    if (res < 0)
        goto error;

    res = create_stream(s);
    if (res < 0)
        goto error;

    // release display, NvFBC has its own pointer
    XCloseDisplay(display);

    return 0;

error:
    nvfbc_read_close(s);

    if (display)
        XCloseDisplay(display);

    return res;
}

static int nvfbc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
#if CONFIG_CUDA
    NvFBCContext *ctx = s->priv_data;

    if (ctx->hwdevice_ref)
        return nvfbc_read_packet_tocuda(s, pkt);
    else
#endif
        return nvfbc_read_packet_tosys(s, pkt);
}


static const AVClass nvfbc_class = {
    .class_name = "nvfbc indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT,
};

const AVInputFormat ff_nvfbc_demuxer = {
    .name           = "nvfbc",
    .long_name      = NULL_IF_CONFIG_SMALL("X11 screen capture, using NvFBC"),
    .priv_data_size = sizeof(NvFBCContext),
    .read_header    = nvfbc_read_header,
    .read_packet    = nvfbc_read_packet,
    .read_close     = nvfbc_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &nvfbc_class,
};
