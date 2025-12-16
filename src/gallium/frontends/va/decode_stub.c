/**************************************************************************
 *
 * Copyright 2010 Thomas Balling Sørensen & Orasanu Lucian.
 * Copyright 2014 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "pipe/p_screen.h"

#include "util/u_memory.h"
#include "util/u_handle_table.h"
#include "util/u_surface.h"
#include "util/u_video.h"
#include "util/u_process.h"

#include "vl/winsys/vl_winsys.h"
#include "vl/vl_video_buffer.h"
#include "va_private.h"

VAStatus
vlVaQueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list, int *num_formats)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaCreateImage(VADriverContextP ctx, VAImageFormat *format, int width, int height, VAImage *image)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaDeriveImage(VADriverContextP ctx, VASurfaceID surface, VAImage *image)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaDestroyImage(VADriverContextP ctx, VAImageID image)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaSetImagePalette(VADriverContextP ctx, VAImageID image, unsigned char *palette)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaGetImage(VADriverContextP ctx, VASurfaceID surface, int x, int y,
             unsigned int width, unsigned int height, VAImageID image)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static void
vlVaUploadImage(vlVaDriver *drv, vlVaSurface *surf, vlVaBuffer *buf, VAImage *image)
{
   return;
}

VAStatus
vlVaPutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image,
             int src_x, int src_y, unsigned int src_width, unsigned int src_height,
             int dest_x, int dest_y, unsigned int dest_width, unsigned int dest_height)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaQuerySubpictureFormats(VADriverContextP ctx, VAImageFormat *format_list,
                           unsigned int *flags, unsigned int *num_formats)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaCreateSubpicture(VADriverContextP ctx, VAImageID image,
                     VASubpictureID *subpicture)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaDestroySubpicture(VADriverContextP ctx, VASubpictureID subpicture)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaSubpictureImage(VADriverContextP ctx, VASubpictureID subpicture, VAImageID image)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaSetSubpictureChromakey(VADriverContextP ctx, VASubpictureID subpicture,
                           unsigned int chromakey_min, unsigned int chromakey_max, unsigned int chromakey_mask)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaSetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID subpicture, float global_alpha)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaAssociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture,
                        VASurfaceID *target_surfaces, int num_surfaces,
                        short src_x, short src_y, unsigned short src_width,
                        unsigned short src_height, short dest_x, short dest_y,
                        unsigned short dest_width, unsigned short dest_height,
                        unsigned int flags)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaDeassociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture,
                          VASurfaceID *target_surfaces, int num_surfaces)
{
   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

