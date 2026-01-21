/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "drm-uapi/drm_fourcc.h"
#include "si_pipe.h"
#include "ac_surface.h"
#include "frontend/drm_driver.h"
#include "util/format/u_format.h"
#include "util/os_time.h"
// #include "util/u_log.h"
#include "util/u_memory.h"
#include "util/u_pack_color.h"
#include "util/u_resource.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"

#include <errno.h>
#include <inttypes.h>

#include "amd/addrlib/inc/addrinterface.h"
#include "ac_formats.h"

static enum radeon_surf_mode si_choose_tiling(struct si_screen *sscreen,
                                              const struct pipe_resource *templ,
                                              bool tc_compatible_htile);

static bool si_texture_is_aux_plane(const struct pipe_resource *resource);


static int si_init_surface(struct si_screen *sscreen, struct radeon_surf *surface,
                           const struct pipe_resource *ptex, enum radeon_surf_mode array_mode,
                           uint64_t modifier, bool is_imported, bool is_scanout,
                           bool is_flushed_depth, bool tc_compatible_htile)
{
   int r;
   unsigned bpe;
   uint64_t flags = 0;

   if (!is_flushed_depth && ptex->format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT) {
      bpe = 4; /* stencil is allocated separately */
   } else {
      bpe = util_format_get_blocksize(ptex->format);
      assert(util_is_power_of_two_or_zero(bpe));
   }

   if (ptex->bind & PIPE_BIND_SHARED)
      flags |= RADEON_SURF_SHAREABLE;

   if (is_imported)
      flags |= RADEON_SURF_IMPORTED | RADEON_SURF_SHAREABLE;

   if (ptex->flags & PIPE_RESOURCE_FLAG_SPARSE)
      flags |= RADEON_SURF_PRT;

   if (ptex->bind & (PIPE_BIND_VIDEO_DECODE_DPB | PIPE_BIND_VIDEO_ENCODE_DPB))
      flags |= RADEON_SURF_VIDEO_REFERENCE;

   surface->modifier = modifier;

   r = sscreen->ws->surface_init(sscreen->ws, &sscreen->info, ptex, flags, bpe, array_mode,
                                 surface);
   if (r) {
      return r;
   }

   return 0;
}

static bool si_resource_get_param(struct pipe_screen *screen, struct pipe_context *context,
                                  struct pipe_resource *resource, unsigned plane, unsigned layer,
                                  unsigned level,
                                  enum pipe_resource_param param, unsigned handle_usage,
                                  uint64_t *value)
{
   while (plane && resource->next && !si_texture_is_aux_plane(resource->next)) {
      --plane;
      resource = resource->next;
   }

   struct si_screen *sscreen = (struct si_screen *)screen;
   struct si_texture *tex = (struct si_texture *)resource;
   struct winsys_handle whandle;

   /* Compute texture modifier when needed.
    * This allows to return the correct values for the PIPE_RESOURCE_PARAM_NPLANES and
    * PIPE_RESOURCE_PARAM_MODIFIER queries.
    */
   if ((param == PIPE_RESOURCE_PARAM_NPLANES || param == PIPE_RESOURCE_PARAM_MODIFIER) &&
       resource->target != PIPE_BUFFER &&
       (sscreen->debug_flags & DBG(EXPORT_MODIFIER)))
         ac_compute_surface_modifier(&sscreen->info, &tex->surface, resource->nr_samples);

   switch (param) {
   case PIPE_RESOURCE_PARAM_NPLANES:
      if (resource->target == PIPE_BUFFER)
         *value = 1;
      else if (tex->num_planes > 1)
         *value = tex->num_planes;
      else
         *value = ac_surface_get_nplanes(&tex->surface);
      return true;

   case PIPE_RESOURCE_PARAM_STRIDE:
      if (resource->target == PIPE_BUFFER)
         *value = 0;
      else
         *value = ac_surface_get_plane_stride(sscreen->info.gfx_level,
                                              &tex->surface, plane, level);
      return true;

   case PIPE_RESOURCE_PARAM_OFFSET:
      if (resource->target == PIPE_BUFFER) {
         *value = 0;
      } else {
         uint64_t level_offset = 0;
         if (sscreen->info.gfx_level >= GFX9 && tex->surface.is_linear)
            level_offset = tex->surface.u.gfx9.offset[level];
         *value = ac_surface_get_plane_offset(sscreen->info.gfx_level,
                                              &tex->surface, plane, layer)  + level_offset;
      }
      return true;

   case PIPE_RESOURCE_PARAM_MODIFIER:
      *value = tex->surface.modifier;
      return true;

   case PIPE_RESOURCE_PARAM_HANDLE_TYPE_SHARED:
   case PIPE_RESOURCE_PARAM_HANDLE_TYPE_KMS:
   case PIPE_RESOURCE_PARAM_HANDLE_TYPE_FD:
      memset(&whandle, 0, sizeof(whandle));

      if (param == PIPE_RESOURCE_PARAM_HANDLE_TYPE_SHARED)
         whandle.type = WINSYS_HANDLE_TYPE_SHARED;
      else if (param == PIPE_RESOURCE_PARAM_HANDLE_TYPE_KMS)
         whandle.type = WINSYS_HANDLE_TYPE_KMS;
      else if (param == PIPE_RESOURCE_PARAM_HANDLE_TYPE_FD)
         whandle.type = WINSYS_HANDLE_TYPE_FD;

      if (!screen->resource_get_handle(screen, context, resource, &whandle, handle_usage))
         return false;

      *value = whandle.handle;
      return true;
   case PIPE_RESOURCE_PARAM_LAYER_STRIDE:
      break;
   case PIPE_RESOURCE_PARAM_DISJOINT_PLANES:
      if (resource->target == PIPE_BUFFER)
         *value = false;
      else
         *value = tex->num_planes > 1;
      return true;
   }
   return false;
}

static void si_texture_get_info(struct pipe_screen *screen, struct pipe_resource *resource,
                                unsigned *pstride, unsigned *poffset)
{
   uint64_t value;

   if (pstride) {
      si_resource_get_param(screen, NULL, resource, 0, 0, 0, PIPE_RESOURCE_PARAM_STRIDE, 0, &value);
      *pstride = value;
   }

   if (poffset) {
      si_resource_get_param(screen, NULL, resource, 0, 0, 0, PIPE_RESOURCE_PARAM_OFFSET, 0, &value);
      *poffset = value;
   }
}

static bool si_texture_get_handle(struct pipe_screen *screen, struct pipe_context *ctx,
                                  struct pipe_resource *resource, struct winsys_handle *whandle,
                                  unsigned usage)
{
   struct si_screen *sscreen = (struct si_screen *)screen;
   struct si_resource *res = si_resource(resource);
   struct si_texture *tex = (struct si_texture *)resource;
   unsigned stride, offset, slice_size;
   uint64_t modifier = DRM_FORMAT_MOD_INVALID;

   if (resource->target != PIPE_BUFFER) {
      unsigned plane = whandle->plane;

      /* Individual planes are chained pipe_resource instances. */
      while (plane && resource->next && !si_texture_is_aux_plane(resource->next)) {
         resource = resource->next;
         --plane;
      }

      res = si_resource(resource);
      tex = (struct si_texture *)resource;

      /* This is not supported now, but it might be required for OpenCL
       * interop in the future.
       */
      if (resource->nr_samples > 1 || tex->is_depth) {
         return false;
      }

      whandle->size = tex->buffer.bo_size;

      if (plane) {
         whandle->offset = ac_surface_get_plane_offset(sscreen->info.gfx_level,
                                                       &tex->surface, plane, 0);
         whandle->stride = ac_surface_get_plane_stride(sscreen->info.gfx_level,
                                                       &tex->surface, plane, 0);
         whandle->modifier = tex->surface.modifier;
         return sscreen->ws->buffer_get_handle(sscreen->ws, res->buf, whandle);
      }


   } else {
      /* Buffers */
      slice_size = 0;
   }

   si_texture_get_info(screen, resource, &stride, &offset);

   if (res->b.is_shared) {
      /* USAGE_EXPLICIT_FLUSH must be cleared if at least one user
       * doesn't set it.
       */
      res->external_usage |= usage & ~PIPE_HANDLE_USAGE_EXPLICIT_FLUSH;
      if (!(usage & PIPE_HANDLE_USAGE_EXPLICIT_FLUSH))
         res->external_usage &= ~PIPE_HANDLE_USAGE_EXPLICIT_FLUSH;
   } else {
      res->b.is_shared = true;
      res->external_usage = usage;
   }

   whandle->stride = stride;
   whandle->offset = offset + slice_size * whandle->layer;
   whandle->modifier = modifier;

   return sscreen->ws->buffer_get_handle(sscreen->ws, res->buf, whandle);
}

/**
 * Common function for si_texture_create and si_texture_from_handle.
 *
 * \param screen       screen
 * \param base         resource template
 * \param surface      radeon_surf
 * \param plane0       if a non-zero plane is being created, this is the first plane
 * \param imported_buf from si_texture_from_handle
 * \param offset       offset for non-zero planes or imported buffers
 * \param alloc_size   the size to allocate if plane0 != NULL
 * \param alignment    alignment for the allocation
 */
static struct si_texture *si_texture_create_object(struct pipe_screen *screen,
                                                   const struct pipe_resource *base,
                                                   const struct radeon_surf *surface,
                                                   const struct si_texture *plane0,
                                                   struct pb_buffer_lean *imported_buf,
                                                   uint64_t offset, unsigned pitch_in_bytes,
                                                   uint64_t alloc_size, unsigned alignment)
{
   struct si_texture *tex;
   struct si_resource *resource;
   struct si_screen *sscreen = (struct si_screen *)screen;

   if (!sscreen->info.has_3d_cube_border_color_mipmap &&
       (base->last_level > 0 ||
        base->target == PIPE_TEXTURE_3D ||
        base->target == PIPE_TEXTURE_CUBE)) {
      assert(0);
      return NULL;
   }

   tex = CALLOC_STRUCT_CL(si_texture);
   if (!tex)
      goto error;

   resource = &tex->buffer;
   resource->b.b = *base;
   pipe_reference_init(&resource->b.b.reference, 1);
   resource->b.b.screen = screen;

   /* don't include stencil-only formats which we don't support for rendering */
   tex->is_depth = util_format_has_depth(util_format_description(tex->buffer.b.b.format));
   tex->surface = *surface;

   if (!ac_surface_override_offset_stride(&sscreen->info, &tex->surface,
                                          tex->buffer.b.b.array_size,
                                          tex->buffer.b.b.last_level + 1,
                                          offset, pitch_in_bytes / tex->surface.bpe))
      goto error;

   if (plane0) {
      /* The buffer is shared with the first plane. */
      resource->bo_size = plane0->buffer.bo_size;
      resource->bo_alignment_log2 = plane0->buffer.bo_alignment_log2;
      resource->flags = plane0->buffer.flags;
      resource->domains = plane0->buffer.domains;

      radeon_bo_reference(sscreen->ws, &resource->buf, plane0->buffer.buf);
      resource->gpu_address = plane0->buffer.gpu_address;
   } else if (!(surface->flags & RADEON_SURF_IMPORTED)) {
      if (base->flags & PIPE_RESOURCE_FLAG_SPARSE)
         resource->b.b.flags |= PIPE_RESOURCE_FLAG_UNMAPPABLE;
      if (base->bind & PIPE_BIND_PRIME_BLIT_DST)
         resource->b.b.flags |= SI_RESOURCE_FLAG_GL2_BYPASS;

      /* Create the backing buffer. */
      si_init_resource_fields(sscreen, resource, alloc_size, alignment);


      if (!si_alloc_resource(sscreen, resource))
         goto error;
   } else {
      resource->buf = imported_buf;
      resource->gpu_address = sscreen->ws->buffer_get_virtual_address(resource->buf);
      resource->bo_size = imported_buf->size;
      resource->bo_alignment_log2 = imported_buf->alignment_log2;
      resource->domains = sscreen->ws->buffer_get_initial_domain(resource->buf);
      if (sscreen->ws->buffer_get_flags)
         resource->flags = sscreen->ws->buffer_get_flags(resource->buf);
   }
   return tex;

error:
   FREE_CL(tex);
   return NULL;
}

static enum pipe_format
si_get_plane_format(enum pipe_format format, unsigned plane)
{
   enum pipe_format fmt = util_format_get_plane_format(format, plane);

   switch (fmt) {
   case PIPE_FORMAT_X6R10_UNORM:
   case PIPE_FORMAT_X4R12_UNORM:
      return PIPE_FORMAT_R16_UNORM;
   case PIPE_FORMAT_X6R10X6G10_UNORM:
   case PIPE_FORMAT_X4R12X4G12_UNORM:
      return PIPE_FORMAT_R16G16_UNORM;
   default:
      return fmt;
   }
}

static enum radeon_surf_mode si_choose_tiling(struct si_screen *sscreen,
                                              const struct pipe_resource *templ,
                                              bool tc_compatible_htile)
{
   const struct util_format_description *desc = util_format_description(templ->format);
   bool is_depth_stencil = util_format_is_depth_or_stencil(templ->format) &&
                           !(templ->flags & SI_RESOURCE_FLAG_FLUSHED_DEPTH);

   /* MSAA resources must be 2D tiled. */
   if (templ->nr_samples > 1)
      return RADEON_SURF_MODE_2D;

   /* Transfer resources should be linear. */
   if (templ->flags & SI_RESOURCE_FLAG_FORCE_LINEAR)
      return RADEON_SURF_MODE_LINEAR_ALIGNED;

   /* Avoid Z/S decompress blits by forcing TC-compatible HTILE on GFX8,
    * which requires 2D tiling.
    */
   if (sscreen->info.gfx_level == GFX8 && tc_compatible_htile)
      return RADEON_SURF_MODE_2D;

   /* Handle common candidates for the linear mode.
    * Compressed textures and DB surfaces must always be tiled.
    */
   if (!is_depth_stencil && !util_format_is_compressed(templ->format)) {
      if (sscreen->debug_flags & DBG(NO_TILING) ||
          (templ->bind & PIPE_BIND_SCANOUT && sscreen->debug_flags & DBG(NO_DISPLAY_TILING)))
         return RADEON_SURF_MODE_LINEAR_ALIGNED;

      /* Tiling doesn't work with the 422 (SUBSAMPLED) formats. */
      if (desc->layout == UTIL_FORMAT_LAYOUT_SUBSAMPLED)
         return RADEON_SURF_MODE_LINEAR_ALIGNED;

      /* Cursors are linear on AMD GCN.
       * (XXX double-check, maybe also use RADEON_SURF_SCANOUT) */
      if (templ->bind & PIPE_BIND_CURSOR)
         return RADEON_SURF_MODE_LINEAR_ALIGNED;

      if (templ->bind & PIPE_BIND_LINEAR)
         return RADEON_SURF_MODE_LINEAR_ALIGNED;

      /* Textures with a very small height are recommended to be linear. */
      if (templ->target == PIPE_TEXTURE_1D || templ->target == PIPE_TEXTURE_1D_ARRAY ||
          /* Only very thin and long 2D textures should benefit from
           * linear_aligned. */
          templ->height0 <= 2)
         return RADEON_SURF_MODE_LINEAR_ALIGNED;

      /* Textures likely to be mapped often. */
      if (templ->usage == PIPE_USAGE_STAGING || templ->usage == PIPE_USAGE_STREAM)
         return RADEON_SURF_MODE_LINEAR_ALIGNED;
   }

   /* Make small textures 1D tiled. */
   if (templ->width0 <= 16 || templ->height0 <= 16 || (sscreen->debug_flags & DBG(NO_2D_TILING)))
      return RADEON_SURF_MODE_1D;

   /* The allocator will switch to 1D if needed. */
   return RADEON_SURF_MODE_2D;
}

static struct pipe_resource *
si_texture_create_with_modifier(struct pipe_screen *screen,
                                const struct pipe_resource *templ,
                                uint64_t modifier)
{
   struct si_screen *sscreen = (struct si_screen *)screen;
   bool is_zs = util_format_is_depth_or_stencil(templ->format);

   bool is_flushed_depth = templ->flags & SI_RESOURCE_FLAG_FLUSHED_DEPTH ||
                           templ->flags & SI_RESOURCE_FLAG_FORCE_LINEAR;
   bool tc_compatible_htile = is_zs && !is_flushed_depth &&
                              !(sscreen->debug_flags & DBG(NO_HYPERZ)) &&
                              sscreen->info.has_tc_compatible_htile;

   enum radeon_surf_mode tile_mode = si_choose_tiling(sscreen, templ, tc_compatible_htile);

   /* This allocates textures with multiple planes like NV12 in 1 buffer. */
   enum
   {
      SI_TEXTURE_MAX_PLANES = 3
   };
   struct radeon_surf surface[SI_TEXTURE_MAX_PLANES] = {};
   struct pipe_resource plane_templ[SI_TEXTURE_MAX_PLANES];
   uint64_t plane_offset[SI_TEXTURE_MAX_PLANES] = {};
   uint64_t total_size = 0;
   unsigned max_alignment = 0;
   unsigned num_planes = util_format_get_num_planes(templ->format);
   assert(num_planes <= SI_TEXTURE_MAX_PLANES);

   /* Compute texture or plane layouts and offsets. */
   for (unsigned i = 0; i < num_planes; i++) {
      plane_templ[i] = *templ;
      plane_templ[i].format = si_get_plane_format(templ->format, i);
      plane_templ[i].width0 = util_format_get_plane_width(templ->format, i, templ->width0);
      plane_templ[i].height0 = util_format_get_plane_height(templ->format, i, templ->height0);

      /* Multi-plane allocations need PIPE_BIND_SHARED, because we can't
       * reallocate the storage to add PIPE_BIND_SHARED, because it's
       * shared by 3 pipe_resources.
       */
      if (num_planes > 1)
         plane_templ[i].bind |= PIPE_BIND_SHARED;
      /* Setting metadata on suballocated buffers is impossible. So use PIPE_BIND_CUSTOM to
       * request a non-suballocated buffer.
       */
      if (!is_zs && sscreen->debug_flags & DBG(EXTRA_METADATA))
         plane_templ[i].bind |= PIPE_BIND_CUSTOM;

      if (si_init_surface(sscreen, &surface[i], &plane_templ[i], tile_mode, modifier,
                          false, plane_templ[i].bind & PIPE_BIND_SCANOUT,
                          is_flushed_depth, tc_compatible_htile))
         return NULL;

      plane_templ[i].nr_sparse_levels = surface[i].first_mip_tail_level;

      plane_offset[i] = align64(total_size, 1 << surface[i].surf_alignment_log2);
      total_size = plane_offset[i] + surface[i].total_size;
      max_alignment = MAX2(max_alignment, 1 << surface[i].surf_alignment_log2);
   }

   struct si_texture *plane0 = NULL, *last_plane = NULL;

   for (unsigned i = 0; i < num_planes; i++) {
      struct si_texture *tex =
         si_texture_create_object(screen, &plane_templ[i], &surface[i], plane0, NULL,
                                  plane_offset[i], 0, total_size, max_alignment);
      if (!tex) {
         si_texture_reference(&plane0, NULL);
         return NULL;
      }

      tex->plane_index = i;
      tex->num_planes = num_planes;

      if (!plane0) {
         plane0 = last_plane = tex;
      } else {
         last_plane->buffer.b.b.next = &tex->buffer.b.b;
         last_plane = tex;
      }
   }

   if (num_planes >= 2)
      plane0->multi_plane_format = templ->format;

   return (struct pipe_resource *)plane0;
}

struct pipe_resource *si_texture_create(struct pipe_screen *screen,
                                        const struct pipe_resource *templ)
{
   return si_texture_create_with_modifier(screen, templ, DRM_FORMAT_MOD_INVALID);
}

static void si_query_dmabuf_modifiers(struct pipe_screen *screen,
                                      enum pipe_format format,
                                      int max,
                                      uint64_t *modifiers,
                                      unsigned int *external_only,
                                      int *count)
{
   struct si_screen *sscreen = (struct si_screen *)screen;

   unsigned ac_mod_count = max;
   ac_get_supported_modifiers(&sscreen->info, &(struct ac_modifier_options) {
         .dcc = !(sscreen->debug_flags & (DBG(NO_DCC) | DBG(NO_EXPORTED_DCC))),
         /* Do not support DCC with retiling yet. This needs explicit
          * resource flushes, but the app has no way to promise doing
          * flushes with modifiers. */
         .dcc_retile = !(sscreen->debug_flags & DBG(NO_DCC)),
      }, format, &ac_mod_count,  max ? modifiers : NULL);
   if (max && external_only) {
      for (unsigned i = 0; i < ac_mod_count; ++i)
         external_only[i] = util_format_is_yuv(format);
   }
   *count = ac_mod_count;
}

static bool
si_is_dmabuf_modifier_supported(struct pipe_screen *screen,
                               uint64_t modifier,
                               enum pipe_format format,
                               bool *external_only)
{
   int allowed_mod_count;
   si_query_dmabuf_modifiers(screen, format, 0, NULL, NULL, &allowed_mod_count);

   uint64_t *allowed_modifiers = (uint64_t *)calloc(allowed_mod_count, sizeof(uint64_t));
   if (!allowed_modifiers)
      return false;

   unsigned *external_array = NULL;
   if (external_only) {
      external_array = (unsigned *)calloc(allowed_mod_count, sizeof(unsigned));
      if (!external_array) {
         free(allowed_modifiers);
         return false;
      }
   }

   si_query_dmabuf_modifiers(screen, format, allowed_mod_count, allowed_modifiers,
                            external_array, &allowed_mod_count);

   bool supported = false;
   for (int i = 0; i < allowed_mod_count && !supported; ++i) {
      if (allowed_modifiers[i] != modifier)
         continue;

      supported = true;
      if (external_only)
         *external_only = external_array[i];
   }

   free(allowed_modifiers);
   free(external_array);
   return supported;
}

static unsigned
si_get_dmabuf_modifier_planes(struct pipe_screen *pscreen, uint64_t modifier,
                             enum pipe_format format)
{
   unsigned planes = util_format_get_num_planes(format);

   if (AMD_FMT_MOD_GET(TILE_VERSION, modifier) < AMD_FMT_MOD_TILE_VER_GFX12) {
      if (IS_AMD_FMT_MOD(modifier) && planes == 1) {
         if (AMD_FMT_MOD_GET(DCC_RETILE, modifier))
            return 3;
         else if (AMD_FMT_MOD_GET(DCC, modifier))
            return 2;
         else
            return 1;
      }
   }

   return planes;
}

static bool
si_modifier_supports_resource(struct pipe_screen *screen,
                              uint64_t modifier,
                              const struct pipe_resource *templ)
{
   struct si_screen *sscreen = (struct si_screen *)screen;
   uint32_t max_width, max_height;

   if (((templ->bind & PIPE_BIND_LINEAR) || sscreen->debug_flags & DBG(NO_TILING)) &&
       modifier != DRM_FORMAT_MOD_LINEAR)
      return false;

   if ((templ->bind & PIPE_BIND_USE_FRONT_RENDERING) && ac_modifier_has_dcc(modifier))
      return false;

   /* Protected content doesn't support DCC on GFX12. */
   if (sscreen->info.gfx_level >= GFX12 && templ->bind & PIPE_BIND_PROTECTED &&
       IS_AMD_FMT_MOD(modifier) &&
       AMD_FMT_MOD_GET(TILE_VERSION, modifier) >= AMD_FMT_MOD_TILE_VER_GFX12 &&
       AMD_FMT_MOD_GET(DCC, modifier))
      return false;

   ac_modifier_max_extent(&sscreen->info, modifier, &max_width, &max_height);
   return templ->width0 <= max_width && templ->height0 <= max_height;
}

static struct pipe_resource *
si_texture_create_with_modifiers(struct pipe_screen *screen,
                                 const struct pipe_resource *templ,
                                 const uint64_t *modifiers,
                                 int modifier_count)
{
   /* Buffers with modifiers make zero sense. */
   assert(templ->target != PIPE_BUFFER);

   /* Select modifier. */
   int allowed_mod_count;
   si_query_dmabuf_modifiers(screen, templ->format, 0, NULL, NULL, &allowed_mod_count);

   uint64_t *allowed_modifiers = (uint64_t *)calloc(allowed_mod_count, sizeof(uint64_t));
   if (!allowed_modifiers) {
      return NULL;
   }

   /* This does not take external_only into account. We assume it is the same for all modifiers. */
   si_query_dmabuf_modifiers(screen, templ->format, allowed_mod_count, allowed_modifiers, NULL, &allowed_mod_count);

   uint64_t modifier = DRM_FORMAT_MOD_INVALID;

   /* Try to find the first allowed modifier that is in the application provided
    * list. We assume that the allowed modifiers are ordered in descending
    * preference in the list provided by si_query_dmabuf_modifiers. */
   for (int i = 0; i < allowed_mod_count; ++i) {
      bool found = false;
      for (int j = 0; j < modifier_count && !found; ++j)
         if (modifiers[j] == allowed_modifiers[i] && si_modifier_supports_resource(screen, modifiers[j], templ))
            found = true;

      if (found) {
         modifier = allowed_modifiers[i];
         break;
      }
   }

   free(allowed_modifiers);

   if (modifier == DRM_FORMAT_MOD_INVALID) {
      return NULL;
   }
   return si_texture_create_with_modifier(screen, templ, modifier);
}

static bool si_texture_is_aux_plane(const struct pipe_resource *resource)
{
   return resource->flags & SI_RESOURCE_AUX_PLANE;
}

static struct pipe_resource *si_texture_from_winsys_buffer(struct si_screen *sscreen,
                                                           const struct pipe_resource *templ,
                                                           struct pb_buffer_lean *buf, unsigned stride,
                                                           uint64_t offset, uint64_t modifier,
                                                           unsigned usage, bool dedicated,
                                                           bool take_ownership)
{
   struct radeon_surf surface = {};
   struct radeon_bo_metadata metadata = {};
   uint32_t md_version, md_flags;
   struct si_texture *tex;
   int r;

   /* Ignore metadata for non-zero planes. */
   if (offset != 0)
      dedicated = false;

   if (dedicated) {
      sscreen->ws->buffer_get_metadata(sscreen->ws, buf, &metadata, &surface);

      /* Refuse to import texture allocated with a overriden gfx family since
       * the data will be garbage.
       */
      md_version = metadata.metadata[0] & 0xffff;
      md_flags = metadata.metadata[0] >> 16;

      if (metadata.mode != RADEON_SURF_MODE_LINEAR_ALIGNED &&
          modifier == DRM_FORMAT_MOD_INVALID &&
          md_version >= 3 &&
          md_flags & (1u << AC_SURF_METADATA_FLAG_FAMILY_OVERRIDEN_BIT)) {
         mesa_loge("si_texture_from_winsys_buffer: fail texture import due to "
                   "AC_SURF_METADATA_FLAG_FAMILY_OVERRIDEN_BIT being set.");
         return NULL;
      }
   } else {
      /**
       * The bo metadata is unset for un-dedicated images. So we fall
       * back to linear. See answer to question 5 of the
       * VK_KHX_external_memory spec for some details.
       *
       * It is possible that this case isn't going to work if the
       * surface pitch isn't correctly aligned by default.
       *
       * In order to support it correctly we require multi-image
       * metadata to be synchronized between radv and radeonsi. The
       * semantics of associating multiple image metadata to a memory
       * object on the vulkan export side are not concretely defined
       * either.
       *
       * All the use cases we are aware of at the moment for memory
       * objects use dedicated allocations. So lets keep the initial
       * implementation simple.
       *
       * A possible alternative is to attempt to reconstruct the
       * tiling information when the TexParameter TEXTURE_TILING_EXT
       * is set.
       */
      metadata.mode = RADEON_SURF_MODE_LINEAR_ALIGNED;
   }

   r = si_init_surface(sscreen, &surface, templ, metadata.mode, modifier, true,
                       surface.flags & RADEON_SURF_SCANOUT, false, false);
   if (r)
      return NULL;

   /* This is a hack to skip alignment checking for 3D textures */
   if (templ->target == PIPE_TEXTURE_3D)
      stride = 0;

   tex = si_texture_create_object(&sscreen->b, templ, &surface, NULL, buf,
                                  offset, stride, 0, 0);
   if (!tex)
      return NULL;

   if (!take_ownership) {
      struct pb_buffer_lean *tmp = NULL;
      radeon_bo_reference(sscreen->ws, &tmp, buf);
   }

   tex->buffer.b.is_shared = true;
   tex->buffer.external_usage = usage;
   tex->num_planes = 1;
   if (tex->buffer.flags & RADEON_FLAG_ENCRYPTED)
      tex->buffer.b.b.bind |= PIPE_BIND_PROTECTED;

   /* Account for multiple planes with lowered yuv import. */
   struct pipe_resource *next_plane = tex->buffer.b.b.next;
   while (next_plane && !si_texture_is_aux_plane(next_plane)) {
      struct si_texture *next_tex = (struct si_texture *)next_plane;
      ++next_tex->num_planes;
      ++tex->num_planes;
      next_plane = next_plane->next;
   }

   unsigned nplanes = ac_surface_get_nplanes(&tex->surface);
   unsigned plane = 1;
   while (next_plane) {
      struct si_auxiliary_texture *ptex = (struct si_auxiliary_texture *)next_plane;
      if (plane >= nplanes || ptex->buffer != tex->buffer.buf ||
          ptex->offset != ac_surface_get_plane_offset(sscreen->info.gfx_level,
                                                      &tex->surface, plane, 0) ||
          ptex->stride != ac_surface_get_plane_stride(sscreen->info.gfx_level,
                                                      &tex->surface, plane, 0)) {
         si_texture_reference(&tex, NULL);
         return NULL;
      }
      ++plane;
      next_plane = next_plane->next;
   }

   if (plane != nplanes && tex->num_planes == 1) {
      si_texture_reference(&tex, NULL);
      return NULL;
   }

   if (!ac_surface_apply_umd_metadata(&sscreen->info, &tex->surface,
                                      tex->buffer.b.b.nr_storage_samples,
                                      tex->buffer.b.b.last_level + 1,
                                      metadata.size_metadata,
                                      metadata.metadata)) {
      si_texture_reference(&tex, NULL);
      return NULL;
   }

   if (ac_surface_get_plane_offset(sscreen->info.gfx_level, &tex->surface, 0, 0) +
        tex->surface.total_size > buf->size) {
      si_texture_reference(&tex, NULL);
      return NULL;
   }

   assert(tex->surface.tile_swizzle == 0);
   return &tex->buffer.b.b;
}

static struct pipe_resource *si_texture_from_handle(struct pipe_screen *screen,
                                                    const struct pipe_resource *templ,
                                                    struct winsys_handle *whandle, unsigned usage)
{
   struct si_screen *sscreen = (struct si_screen *)screen;
   struct pb_buffer_lean *buf = NULL;

   buf = sscreen->ws->buffer_from_handle(sscreen->ws, whandle,
                                         sscreen->info.max_alignment,
                                         templ->bind & PIPE_BIND_PRIME_BLIT_DST);
   if (!buf)
      return NULL;

   if (templ->target == PIPE_BUFFER)
      return si_buffer_from_winsys_buffer(screen, templ, buf, 0, true);

   if (whandle->plane >= util_format_get_num_planes(whandle->format)) {
      struct si_auxiliary_texture *tex = CALLOC_STRUCT_CL(si_auxiliary_texture);
      if (!tex)
         return NULL;
      tex->b.b = *templ;
      tex->b.b.flags |= SI_RESOURCE_AUX_PLANE;
      tex->stride = whandle->stride;
      tex->offset = whandle->offset;
      tex->buffer = buf;
      pipe_reference_init(&tex->b.b.reference, 1);
      tex->b.b.screen = screen;

      return &tex->b.b;
   }

   return si_texture_from_winsys_buffer(sscreen, templ, buf, whandle->stride, whandle->offset,
                                        whandle->modifier, usage, true, true);
}

static struct pipe_surface *si_create_surface(struct pipe_context *pipe, struct pipe_resource *tex,
                                              const struct pipe_surface *templ)
{
   unsigned level = templ->level;
   unsigned width = u_minify(tex->width0, level);
   unsigned height = u_minify(tex->height0, level);
   unsigned width0 = tex->width0;
   unsigned height0 = tex->height0;

   if (tex->target != PIPE_BUFFER && templ->format != tex->format) {
      const struct util_format_description *tex_desc = util_format_description(tex->format);
      const struct util_format_description *templ_desc = util_format_description(templ->format);

      assert(tex_desc->block.bits == templ_desc->block.bits);

      /* Adjust size of surface if and only if the block width or
       * height is changed. */
      if (tex_desc->block.width != templ_desc->block.width ||
          tex_desc->block.height != templ_desc->block.height) {
         unsigned nblks_x = util_format_get_nblocksx(tex->format, width);
         unsigned nblks_y = util_format_get_nblocksy(tex->format, height);

         width = nblks_x * templ_desc->block.width;
         height = nblks_y * templ_desc->block.height;

         width0 = util_format_get_nblocksx(tex->format, width0);
         height0 = util_format_get_nblocksy(tex->format, height0);
      }
   }

   struct si_surface *surface = CALLOC_STRUCT(si_surface);

   if (!surface)
      return NULL;

   assert(templ->first_layer <= util_max_layer(tex, templ->level));
   assert(templ->last_layer <= util_max_layer(tex, templ->level));

   pipe_reference_init(&surface->base.reference, 1);
   pipe_resource_reference(&surface->base.texture, tex);
   surface->base.context = pipe;
   surface->base.format = templ->format;
   surface->base.level = templ->level;
   surface->base.first_layer = templ->first_layer;
   surface->base.last_layer = templ->last_layer;

   surface->width0 = width0;
   surface->height0 = height0;

   return &surface->base;
}

static void si_surface_destroy(struct pipe_context *pipe, struct pipe_surface *surface)
{
   pipe_resource_reference(&surface->texture, NULL);
   FREE(surface);
}

void si_init_screen_texture_functions(struct si_screen *sscreen)
{
   sscreen->b.resource_from_handle = si_texture_from_handle;
   sscreen->b.resource_get_handle = si_texture_get_handle;
   sscreen->b.resource_create_with_modifiers = si_texture_create_with_modifiers;
}

void si_init_context_texture_functions(struct si_context *sctx)
{
   sctx->b.create_surface = si_create_surface;
   sctx->b.surface_destroy = si_surface_destroy;
}
