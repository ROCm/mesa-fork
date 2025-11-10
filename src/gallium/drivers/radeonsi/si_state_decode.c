/*
 * Copyright 2012 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */
#include "si_pipe.h"
#include "si_query.h"
#include "sid.h"
#include "util/fast_idiv_by_const.h"
#include "util/format/u_format.h"
#include "util/format/u_format_s3tc.h"
#include "util/hash_table.h"
#include "util/u_dual_blend.h"
#include "util/u_helpers.h"
#include "util/u_memory.h"
#include "util/u_resource.h"
#include "util/u_upload_mgr.h"
#include "util/u_blend.h"
#include "util/u_process.h"

#include "ac_cmdbuf.h"
#include "ac_descriptors.h"
#include "ac_formats.h"
#include "gfx10_format_table.h"

void si_init_screen_state_functions(struct si_screen *sscreen);


/*
 * Format support testing
 */

static uint32_t si_translate_buffer_dataformat(struct pipe_screen *screen,
                                               const struct util_format_description *desc,
                                               int first_non_void)
{
   assert(((struct si_screen *)screen)->info.gfx_level <= GFX9);

   return ac_translate_buffer_dataformat(desc, first_non_void);
}

static unsigned si_is_vertex_format_supported(struct pipe_screen *screen, enum pipe_format format,
                                              unsigned usage)
{
   struct si_screen *sscreen = (struct si_screen *)screen;
   const struct util_format_description *desc;
   int first_non_void;
   unsigned data_format;

   assert((usage & ~(PIPE_BIND_SHADER_IMAGE | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_VERTEX_BUFFER)) ==
          0);

   desc = util_format_description(format);

   /* There are no native 8_8_8 or 16_16_16 data formats, and we currently
    * select 8_8_8_8 and 16_16_16_16 instead. This works reasonably well
    * for read-only access (with caveats surrounding bounds checks), but
    * obviously fails for write access which we have to implement for
    * shader images. Luckily, OpenGL doesn't expect this to be supported
    * anyway, and so the only impact is on PBO uploads / downloads, which
    * shouldn't be expected to be fast for GL_RGB anyway.
    */
   if (desc->block.bits == 3 * 8 || desc->block.bits == 3 * 16) {
      if (usage & (PIPE_BIND_SHADER_IMAGE | PIPE_BIND_SAMPLER_VIEW)) {
         usage &= ~(PIPE_BIND_SHADER_IMAGE | PIPE_BIND_SAMPLER_VIEW);
         if (!usage)
            return 0;
      }
   }

   if (sscreen->info.gfx_level >= GFX10) {
      const struct gfx10_format *fmt = &ac_get_gfx10_format_table(sscreen->info.gfx_level)[format];
      unsigned first_image_only_format = sscreen->info.gfx_level >= GFX11 ? 64 : 128;

      if (!fmt->img_format || fmt->img_format >= first_image_only_format)
         return 0;
      return usage;
   }

   first_non_void = util_format_get_first_non_void_channel(format);
   data_format = si_translate_buffer_dataformat(screen, desc, first_non_void);
   if (data_format == V_008F0C_BUF_DATA_FORMAT_INVALID)
      return 0;

   return usage;
}

static bool si_is_zs_format_supported(enum pipe_format format)
{
   if (format == PIPE_FORMAT_Z16_UNORM_S8_UINT)
      return false;

   return ac_is_zs_format_supported(format);
}

static bool si_is_reduction_mode_supported(struct pipe_screen *screen, enum pipe_format format)
{
   struct si_screen *sscreen = (struct si_screen *)screen;

   return ac_is_reduction_mode_supported(&sscreen->info, format, true);
}

static bool si_is_format_supported(struct pipe_screen *screen, enum pipe_format format,
                                   enum pipe_texture_target target, unsigned sample_count,
                                   unsigned storage_sample_count, unsigned usage)
{
   struct si_screen *sscreen = (struct si_screen *)screen;
   unsigned retval = 0;

   if (target >= PIPE_MAX_TEXTURE_TYPES) {
      PRINT_ERR("radeonsi: unsupported texture type %d\n", target);
      return false;
   }

   /* Require PIPE_BIND_SAMPLER_VIEW support when PIPE_BIND_RENDER_TARGET
    * is requested.
    */
   if (usage & PIPE_BIND_RENDER_TARGET)
      usage |= PIPE_BIND_SAMPLER_VIEW;

   if ((target == PIPE_TEXTURE_3D || target == PIPE_TEXTURE_CUBE) &&
        !sscreen->info.has_3d_cube_border_color_mipmap)
      return false;

   if (util_format_get_num_planes(format) >= 2)
      return false;

   if (MAX2(1, sample_count) < MAX2(1, storage_sample_count))
      return false;

   if (sample_count > 1) {
      if (!screen->caps.texture_multisample)
         return false;

      /* Only power-of-two sample counts are supported. */
      if (!util_is_power_of_two_or_zero(sample_count) ||
          !util_is_power_of_two_or_zero(storage_sample_count))
         return false;

      /* Chips with 1 RB don't increment occlusion queries at 16x MSAA sample rate,
       * so don't expose 16 samples there.
       *
       * EQAA also uses max 8 samples because our FMASK fetches only load 32 bits and
       * would need to be changed to 64 bits for 16 samples.
       */
      const unsigned max_samples = 8;

      /* MSAA support without framebuffer attachments. */
      if (format == PIPE_FORMAT_NONE && sample_count <= max_samples)
         return true;

      if (!sscreen->info.has_eqaa_surface_allocator || util_format_is_depth_or_stencil(format)) {
         /* Color without EQAA or depth/stencil. */
         if (sample_count > max_samples || sample_count != storage_sample_count)
            return false;
      } else {
         /* Color with EQAA. */
         if (sample_count > max_samples || storage_sample_count > max_samples)
            return false;
      }
   }

   if ((usage & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT |
                 PIPE_BIND_SHARED | PIPE_BIND_BLENDABLE)) &&
       ac_is_colorbuffer_format_supported(sscreen->info.gfx_level, format)) {
      retval |= usage & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT |
                         PIPE_BIND_SHARED);
      if (!util_format_is_pure_integer(format) && !util_format_is_depth_or_stencil(format))
         retval |= usage & PIPE_BIND_BLENDABLE;
   }

   if ((usage & PIPE_BIND_DEPTH_STENCIL) && si_is_zs_format_supported(format)) {
      retval |= PIPE_BIND_DEPTH_STENCIL;
   }

   if (usage & PIPE_BIND_VERTEX_BUFFER) {
      retval |= si_is_vertex_format_supported(screen, format, PIPE_BIND_VERTEX_BUFFER);
   }

   if (usage & PIPE_BIND_INDEX_BUFFER) {
      if (format == PIPE_FORMAT_R8_UINT ||
          format == PIPE_FORMAT_R16_UINT ||
          format == PIPE_FORMAT_R32_UINT)
         retval |= PIPE_BIND_INDEX_BUFFER;
   }

   if ((usage & PIPE_BIND_LINEAR) && !util_format_is_compressed(format) &&
       !(usage & PIPE_BIND_DEPTH_STENCIL))
      retval |= PIPE_BIND_LINEAR;

   if ((usage & PIPE_BIND_SAMPLER_REDUCTION_MINMAX) &&
       screen->caps.sampler_reduction_minmax &&
       si_is_reduction_mode_supported(screen, format))
      retval |= PIPE_BIND_SAMPLER_REDUCTION_MINMAX;

   return retval == usage;
}

void si_init_screen_state_functions(struct si_screen *sscreen)
{
   sscreen->b.is_format_supported = si_is_format_supported;
}