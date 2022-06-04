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
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"

#include "libavformat/avformat.h"
#include "libavformat/internal.h"

#define NVFBC_MIN_VERSION(x,y)  \
    (NVFBC_VERSION_MAJOR > (x) || (NVFBC_VERSION_MAJOR == (x) \
                                   && NVFBC_VERSION_MINOR >= (y)))

typedef struct NvFBCContext {
    const AVClass *class;

    /// Capture region offset and dimensions
    int x, y, w, h;
    /// Output frame size
    int frame_width, frame_height;
    /// Name of the output to capture or NULL for capture box in whole X screen
    const char *output_name;
    /// Pixel format (AV_PIX_FMT_NONE if none specified)
    enum AVPixelFormat format;

    /// Capture framerate
    AVRational framerate;
    /// Time base
    AVRational time_base;
    /// Frame duration (in internal timebase)
    int64_t frame_duration;
    /// Current time
    int64_t time_frame;

    /// Index of the requested format in nvfbc_formats[]
    int format_idx;

    /// Pointer to NvFBC entry functions
    NvfbcFunctions *dl;
    /// Pointers to NvFBC API functions
    NVFBC_API_FUNCTION_LIST funcs;

    /// NvFBC session handle
    NVFBC_SESSION_HANDLE handle;
    /// True if the handle is created, false if not
    int handle_created;
    /// True if the capture session is created, false if not
    int capture_session_created;
    /// Pointer to frame data
    uint8_t *frame_data;
} NvFBCContext;

#define OFFSET(x) offsetof(NvFBCContext, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "video_size", "set capture output size", OFFSET(frame_width), AV_OPT_TYPE_IMAGE_SIZE, { .str = NULL }, 0, 0, FLAGS },
    { "pixel_format", "set pixel format", OFFSET(format), AV_OPT_TYPE_PIXEL_FMT, { .i64 = AV_PIX_FMT_NONE }, -1, INT_MAX, FLAGS },
    { "framerate", "set capture framerate", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, { .str = "pal" }, 0, INT_MAX, FLAGS },
    { NULL },
};

static const struct {
    enum AVPixelFormat pixfmt;
    NVFBC_BUFFER_FORMAT nvfbc_fmt;
    int bpp;
} nvfbc_formats[] = {
    { AV_PIX_FMT_BGRA,      NVFBC_BUFFER_FORMAT_BGRA,       32  },  // native
    { AV_PIX_FMT_ARGB,      NVFBC_BUFFER_FORMAT_ARGB,       32  },
    { AV_PIX_FMT_RGBA,      NVFBC_BUFFER_FORMAT_RGBA,       32  },
    { AV_PIX_FMT_RGB24,     NVFBC_BUFFER_FORMAT_RGB,        24  },
    { AV_PIX_FMT_YUV444P,   NVFBC_BUFFER_FORMAT_YUV444P,    24  },
    { AV_PIX_FMT_NV12,      NVFBC_BUFFER_FORMAT_NV12,       12  },
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
#if NVFBC_MIN_VERSION(1, 8)
    { NVFBC_ERR_VULKAN,         AVERROR_EXTERNAL,       "Vulkan error"              },
#endif
};

static int error_nv2av(NVFBCSTATUS nverr, const char **desc)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(nvfbc_errors); i++) {
        if (nverr == nvfbc_errors[i].nverr) {
            if (desc != NULL)
                *desc = nvfbc_errors[i].desc;

            return nvfbc_errors[i].averr;
        }
    }

    if (desc != NULL)
        *desc = "unknown error";

    return AVERROR_UNKNOWN;
}

static int64_t wait_frame(AVFormatContext *s, AVPacket *pkt)
{
    NvFBCContext *c = s->priv_data;
    int64_t curtime, delay;

    c->time_frame += c->frame_duration;

    for (;;) {
        curtime = av_gettime_relative();
        delay   = c->time_frame - curtime;
        if (delay <= 0)
            break;
        av_usleep(delay);
    }

    return curtime;
}

static void free_noop(void *opaque, uint8_t *data)
{
}

static int nvfbc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    NvFBCContext *c = s->priv_data;
    NVFBC_FRAME_GRAB_INFO frameInfo = {};
    NVFBC_TOSYS_GRAB_FRAME_PARAMS nvtsgf_params = {
        .dwVersion      = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER,
        .dwFlags        = NVFBC_TOSYS_GRAB_FLAGS_NOWAIT,
        .pFrameGrabInfo = &frameInfo,
        .dwTimeoutMs    = 0,
    };
    int64_t pts;
    NVFBCSTATUS result;

    wait_frame(s, pkt);

    result = c->funcs.nvFBCToSysGrabFrame(c->handle, &nvtsgf_params);
    if (result != NVFBC_SUCCESS) {
       av_log(s, AV_LOG_ERROR, "Cannot grab framebuffer: %s.\n",
               c->funcs.nvFBCGetLastErrorStr(c->handle));
        return error_nv2av(result, NULL);
    }

    pts = av_gettime();

    pkt->buf = av_buffer_create(c->frame_data, frameInfo.dwByteSize, free_noop,
                                s, AV_BUFFER_FLAG_READONLY);
    if (pkt->buf == NULL)
        return AVERROR(ENOMEM);

    pkt->data = c->frame_data;
    pkt->size = frameInfo.dwByteSize;

    pkt->dts = pts;
    pkt->pts = pts;
    pkt->duration = c->frame_duration;

    return 0;
}

static av_cold int create_stream(AVFormatContext *s)
{
    NvFBCContext *c = s->priv_data;
    AVStream *st;
    int64_t frame_size_bits;

    frame_size_bits = (int64_t)c->frame_width * c->frame_height * nvfbc_formats[c->format_idx].bpp;
    if (frame_size_bits / 8 + AV_INPUT_BUFFER_PADDING_SIZE > INT_MAX) {
        av_log(s, AV_LOG_ERROR, "Capture area is too large.\n");
        return AVERROR_PATCHWELCOME;
    }

    st = avformat_new_stream(s, NULL);
    if (st == NULL)
        return AVERROR(ENOMEM);

    avpriv_set_pts_info(st, 64, 1, 1000000); /* 64 bits pts in us */

    st->avg_frame_rate = c->framerate;

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->format     = nvfbc_formats[c->format_idx].pixfmt;
    st->codecpar->width      = c->frame_width;
    st->codecpar->height     = c->frame_height;
    st->codecpar->bit_rate   = av_rescale(frame_size_bits, c->framerate.num, c->framerate.den);

    return 0;
}

static av_cold int create_capture_session(AVFormatContext *s)
{
    NvFBCContext *c = s->priv_data;
    NVFBC_CREATE_HANDLE_PARAMS nvch_params = {
        .dwVersion                 = NVFBC_CREATE_HANDLE_PARAMS_VER,
        .bExternallyManagedContext = NVFBC_FALSE,
    };
    NVFBC_GET_STATUS_PARAMS nvgs_params = {
        .dwVersion  = NVFBC_GET_STATUS_PARAMS_VER,
    };
    NVFBC_CREATE_CAPTURE_SESSION_PARAMS nvccs_params = {
        .dwVersion                   = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER,
        .eCaptureType                = NVFBC_CAPTURE_TO_SYS,
        .bDisableAutoModesetRecovery = NVFBC_TRUE,
        .bWithCursor                 = NVFBC_TRUE,
        .eTrackingType               = NVFBC_TRACKING_SCREEN,
        .bPushModel                  = NVFBC_FALSE,
        .dwSamplingRateMs            = av_rescale_q(c->frame_duration, AV_TIME_BASE_Q, (AVRational){ 1, 1000 }),
        .captureBox                  = {
            .x = c->x,
            .y = c->y,
            .w = c->w,
            .h = c->h,
        },
        .bRoundFrameSize             = NVFBC_FALSE,
    };
    NVFBC_TOSYS_SETUP_PARAMS nvtss_params = {
        .dwVersion     = NVFBC_TOSYS_SETUP_PARAMS_VER,
        .eBufferFormat = nvfbc_formats[c->format_idx].nvfbc_fmt,
        .ppBuffer      = (void **)&c->frame_data,
        .bWithDiffMap  = NVFBC_FALSE,
    };
    NVFBCSTATUS result;

    result = c->funcs.nvFBCCreateHandle(&c->handle, &nvch_params);
    if (result != NVFBC_SUCCESS) {
        const char *desc;
        int ret = error_nv2av(result, &desc);
        av_log(s, AV_LOG_ERROR, "Cannot create NvFBC handle: %s.\n", desc);
        return ret;
    } else {
        c->handle_created = 1;
    }

    result = c->funcs.nvFBCGetStatus(c->handle, &nvgs_params);
    if (result != NVFBC_SUCCESS) {
        av_log(s, AV_LOG_ERROR, "Cannot get NvFBC status: %s.\n",
               c->funcs.nvFBCGetLastErrorStr(c->handle));
        return error_nv2av(result, NULL);
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
#if NVFBC_MIN_VERSION(1, 8)
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
    if (c->output_name != NULL) {
        int found = 0;

        for (uint32_t dw = 0; dw < nvgs_params.dwOutputNum; dw++) {
            if (!strcmp(c->output_name, nvgs_params.outputs[dw].name)) {
                // store output geometry
                c->x = nvgs_params.outputs[dw].trackedBox.x;
                c->y = nvgs_params.outputs[dw].trackedBox.y;
                c->w = nvgs_params.outputs[dw].trackedBox.w;
                c->h = nvgs_params.outputs[dw].trackedBox.h;

                // update session parameters
                nvccs_params.eTrackingType = NVFBC_TRACKING_OUTPUT;
                nvccs_params.dwOutputId = nvgs_params.outputs[dw].dwId;

                found = 1;
                break;
            }
        }

        if (!found) {
            av_log(s, AV_LOG_ERROR, "Output '%s' not found\n", c->output_name);
            return AVERROR(EINVAL);
        }
    }

    // store output frame size equal to capture box if not specified
    if (c->frame_width == 0)
        c->frame_width = c->w;
    if (c->frame_height == 0)
        c->frame_height = c->h;

    nvccs_params.frameSize.w = c->frame_width;
    nvccs_params.frameSize.h = c->frame_height;

    result = c->funcs.nvFBCCreateCaptureSession(c->handle, &nvccs_params);
    if (result != NVFBC_SUCCESS) {
        av_log(s, AV_LOG_ERROR, "Cannot create capture session: %s.\n",
               c->funcs.nvFBCGetLastErrorStr(c->handle));
        return error_nv2av(result, NULL);
    } else {
        c->capture_session_created = 1;
    }

    result = c->funcs.nvFBCToSysSetUp(c->handle, &nvtss_params);
    if (result != NVFBC_SUCCESS) {
        av_log(s, AV_LOG_ERROR, "Cannot set up capture to system memory: %s.\n",
               c->funcs.nvFBCGetLastErrorStr(c->handle));
        return error_nv2av(result, NULL);
    }

    return 0;
}

static av_cold int nvfbc_load_libraries(AVFormatContext *s)
{
    NvFBCContext *c = s->priv_data;
    NVFBCSTATUS result;
    const char *desc;
    int ret;

    ret = nvfbc_load_functions(&c->dl, s);
    if (ret < 0)
        return ret;

    av_log(s, AV_LOG_VERBOSE, "Built for NvFBC API version %u.%u.\n",
           NVFBC_VERSION_MAJOR, NVFBC_VERSION_MINOR);

    c->funcs.dwVersion = NVFBC_VERSION;

    result = c->dl->NvFBCCreateInstance(&c->funcs);
    if (result != NVFBC_SUCCESS) {
        ret = error_nv2av(result, &desc);
        av_log(s, AV_LOG_ERROR, "Cannot create NvFBC instance: %s.\n", desc);
        return ret;
    }

    return 0;
}

static av_cold int nvfbc_free_libraries(AVFormatContext *s)
{
    NvFBCContext *c = s->priv_data;

    nvfbc_free_functions(&c->dl);

    return 0;
}

static av_cold int nvfbc_read_close(AVFormatContext *s)
{
    NvFBCContext *c = s->priv_data;
    NVFBCSTATUS result;

    if (c->capture_session_created) {
        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS params = {
            .dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER,
        };

        result = c->funcs.nvFBCDestroyCaptureSession(c->handle, &params);
        if (result != NVFBC_SUCCESS) {
            av_log(s, AV_LOG_WARNING, "Cannot destroy capture session: %s.\n",
                   c->funcs.nvFBCGetLastErrorStr(c->handle));
        }
        c->capture_session_created = 0;
    }

    if (c->handle_created) {
        NVFBC_DESTROY_HANDLE_PARAMS params = {
            .dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER,
        };

        result = c->funcs.nvFBCDestroyHandle(c->handle, &params);
        if (result != NVFBC_SUCCESS) {
            av_log(s, AV_LOG_WARNING, "Cannot destroy NvFBC handle: %s.\n",
                   c->funcs.nvFBCGetLastErrorStr(c->handle));
        }
        c->handle_created = 0;
    }

    nvfbc_free_libraries(s);

    return 0;
}

static av_cold int nvfbc_read_header(AVFormatContext *s)
{
    NvFBCContext *c = s->priv_data;
    Display *display = NULL;
    Screen *screen;
    int screen_w, screen_h;
    int count, ret;

    // load NvFBC library
    ret = nvfbc_load_libraries(s);
    if (ret < 0)
        goto error;

    // prepare X11 display
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        av_log(s, AV_LOG_ERROR, "Could not open the X11 display.\n");
        ret = AVERROR(EIO);
        goto error;
    }
    screen = XDefaultScreenOfDisplay(display);
    screen_w = XWidthOfScreen(screen);
    screen_h = XHeightOfScreen(screen);

    // parse URL for definition of the capture box
    if (s->url != NULL && s->url[0] != '\0') {
        // complete definition: size and position
        count = sscanf(s->url, "%dx%d+%d+%d", &c->w, &c->h, &c->x, &c->y);
        if (count != 4) {
            c->w = c->h = 0;

            // partial definition: position only
            count = sscanf(s->url, "+%d+%d", &c->x, &c->y);
            if (count != 2) {
                c->x = c->y = 0;

                // no box definition, capture specific output
                c->output_name = s->url;
            }
        }
    }

    // compute capture region
    if (c->output_name == NULL) {
        if (c->w == 0)
            c->w = FFMAX(screen_w - c->x, 0);
        if (c->h == 0)
            c->h = FFMAX(screen_h - c->y, 0);

        // check capture region
        if (c->x < 0 || c->y < 0) {
            av_log(s, AV_LOG_ERROR, "Invalid capture position +%d+%d\n",
                   c->x, c->y);
            ret = AVERROR(EINVAL);
            goto error;
        }

        if (c->x + c->w > screen_w || c->y + c->h > screen_h) {
            av_log(s, AV_LOG_ERROR,
                   "Capture area %dx%d+%d+%d extends outside the screen %dx%d.\n",
                   c->w, c->h, c->x, c->y, screen_w, screen_h);
            ret = AVERROR(EINVAL);
            goto error;
        }
    }

    // compute stream information
    c->time_base = av_inv_q(c->framerate);
    c->frame_duration = av_rescale_q(1, c->time_base, AV_TIME_BASE_Q);
    c->time_frame = av_gettime_relative();

    if (c->format != AV_PIX_FMT_NONE) {
        c->format_idx = -1;
        for (int i = 0; i < FF_ARRAY_ELEMS(nvfbc_formats); i++) {
            if (nvfbc_formats[i].pixfmt == c->format) {
                c->format_idx = i;
                break;
            }
        }
        if (c->format_idx == -1) {
            av_log(s, AV_LOG_ERROR, "Unsupported pixel format.\n");
            ret = AVERROR(EINVAL);
            goto error;
        }
    }

    // prepare execution
    ret = create_capture_session(s);
    if (ret < 0)
        goto error;

    ret = create_stream(s);
    if (ret < 0)
        goto error;

    // release display, NvFBC has its own pointer
    XCloseDisplay(display);

    return 0;

error:
    nvfbc_read_close(s);

    if (display != NULL)
        XCloseDisplay(display);

    return ret;
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
