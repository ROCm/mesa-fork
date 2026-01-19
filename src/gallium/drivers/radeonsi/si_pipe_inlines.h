/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 * Copyright 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SI_PIPE_INLINES_H
#define SI_PIPE_INLINES_H

static inline void si_compute_reference(struct si_compute **dst, struct si_compute *src)
{
   if (pipe_reference(&(*dst)->sel.base.reference, &src->sel.base.reference))
      si_destroy_compute(*dst);

   *dst = src;
}

static inline void
si_shader_selector_reference(struct si_context *sctx, /* sctx can optionally be NULL */
                             struct si_shader_selector **dst, struct si_shader_selector *src)
{
   if (*dst == src)
      return;

   struct si_screen *sscreen = src ? src->screen : (*dst)->screen;
   util_shader_reference(&sctx->b, &sscreen->live_shader_cache, (void **)dst, src);
}

static inline bool vi_dcc_enabled(struct si_texture *tex, unsigned level)
{
   /* Gfx12 always returns false because DCC is transparent to the driver.
    * I think DCC doesn't have to be disabled if a color buffer is simultaneously bound as a sampler.
    */
   return !tex->is_depth && tex->surface.meta_offset && level < tex->surface.num_meta_levels;
}

static inline uint64_t si_get_atom_bit(struct si_context *sctx, struct si_atom *atom)
{
   return 1ull << (atom - sctx->atoms.array);
}

static inline void si_set_atom_dirty(struct si_context *sctx, struct si_atom *atom, bool dirty)
{
   uint64_t bit = si_get_atom_bit(sctx, atom);

   if (dirty)
      sctx->dirty_atoms |= bit;
   else
      sctx->dirty_atoms &= ~bit;
}

static inline bool si_is_atom_dirty(struct si_context *sctx, struct si_atom *atom)
{
   return (sctx->dirty_atoms & si_get_atom_bit(sctx, atom)) != 0;
}

static inline void si_mark_atom_dirty(struct si_context *sctx, struct si_atom *atom)
{
   si_set_atom_dirty(sctx, atom, true);
}

/* This should be evaluated at compile time if all parameters except sctx are constants. */
static ALWAYS_INLINE struct si_shader_ctx_state *
si_get_vs_inline(struct si_context *sctx, enum si_has_tess has_tess, enum si_has_gs has_gs)
{
   if (has_gs)
      return &sctx->shader.gs;
   if (has_tess)
      return &sctx->shader.tes;

   return &sctx->shader.vs;
}

static ALWAYS_INLINE struct si_shader *
si_get_api_vs_inline(struct si_context *sctx, enum amd_gfx_level gfx_level,
                     enum si_has_tess has_tess, enum si_has_gs has_gs)
{
   if (gfx_level >= GFX9 && has_tess)
      return sctx->queued.named.hs; /* this can also be the passthrough TCS */
   else if (gfx_level >= GFX9 && has_gs)
      return sctx->shader.gs.current;
   else
      return sctx->shader.vs.current;
}

static inline struct si_shader_ctx_state *si_get_vs(struct si_context *sctx)
{
   if (sctx->shader.gs.cso)
      return &sctx->shader.gs;
   else if (sctx->shader.tes.cso)
      return &sctx->shader.tes;
   else if (sctx->shader.vs.cso)
      return &sctx->shader.vs;
   else
      return &sctx->ms_shader_state;
}

static inline bool si_get_streamout_enable_state(struct si_context *sctx)
{
   /* For GFX11, return whether NGG streamout queries are enabled. For older gens, return whether
    * streamout hw is enabled.
    *
    * Note that when both PRIMITIVES_GENERATED and SO_OVERFLOW queries are enabled and XFB is
    * disabled, SO_OVERFLOW queries will incorrectly return true because PRIMITIVES_GENERATED
    * is incremented and PRIMITIVES_EMITTED is not. The problem is that SO_OVERFLOW queries
    * are implemented by comparing PRIMITIVES_GENERATED and PRIMITIVES_EMITTED, however, when
    * XFB is disabled, SO_OVERFLOW queries should increment neither PRIMITIVES_GENERATED nor
    * PRIMITIVES_EMITTED, but when a separate PRIMITIVES_GENERATED is active, we should increment
    * it. So the 2 queries are in conflict when XFB is disabled.
    *
    * Possible solutions:
    * - For NGG: Emulate SO_OVERFLOW queries using memory stores separately from PRIMITIVES_GENERATED.
    * - For legacy: Emulate SO_OVERFLOW queries using memory stores, same as NGG.
    */
   if (sctx->gfx_level >= GFX11) {
      /* Enable NGG streamout queries when PRIMITIVES_GENERATED queries are active or when
       * streamout is enabled and any streamout queries except PRIMITIVES_GENERATED are active.
       */
      return sctx->streamout.prims_gen_query_enabled ||
            (sctx->streamout.streamout_enabled &&
              (sctx->streamout.num_ngg_queries -
               sctx->streamout.prims_gen_query_enabled > 0));
   } else {
      return sctx->streamout.streamout_enabled || sctx->streamout.prims_gen_query_enabled;
   }
}

static inline unsigned si_optimal_tcc_alignment(struct si_context *sctx, unsigned upload_size)
{
   unsigned alignment, tcc_cache_line_size;

   /* If the upload size is less than the cache line size (e.g. 16, 32),
    * the whole thing will fit into a cache line if we align it to its size.
    * The idea is that multiple small uploads can share a cache line.
    * If the upload size is greater, align it to the cache line size.
    */
   alignment = util_next_power_of_two(upload_size);
   tcc_cache_line_size = sctx->screen->info.tcc_cache_line_size;
   return MIN2(alignment, tcc_cache_line_size);
}

static inline void si_saved_cs_reference(struct si_saved_cs **dst, struct si_saved_cs *src)
{
   if (pipe_reference(&(*dst)->reference, &src->reference))
      si_destroy_saved_cs(*dst);

   *dst = src;
}

static inline void si_make_CB_shader_coherent(struct si_context *sctx, unsigned num_samples,
                                              bool shaders_read_metadata, bool dcc_pipe_aligned)
{
   sctx->barrier_flags |= SI_BARRIER_SYNC_AND_INV_CB | SI_BARRIER_INV_VMEM;
   sctx->force_shader_coherency.with_cb = false;

   if (sctx->gfx_level >= GFX10 && sctx->gfx_level < GFX12) {
      if (sctx->screen->info.tcc_rb_non_coherent)
         sctx->barrier_flags |= SI_BARRIER_INV_L2;
      else if (shaders_read_metadata)
         sctx->barrier_flags |= SI_BARRIER_INV_L2_METADATA;
   } else if (sctx->gfx_level == GFX9) {
      /* Single-sample color is coherent with shaders on GFX9, but
       * L2 metadata must be flushed if shaders read metadata.
       * (DCC, CMASK).
       */
      if (num_samples >= 2 || (shaders_read_metadata && !dcc_pipe_aligned))
         sctx->barrier_flags |= SI_BARRIER_INV_L2;
      else if (shaders_read_metadata)
         sctx->barrier_flags |= SI_BARRIER_INV_L2_METADATA;
   } else if (sctx->gfx_level <= GFX8) {
      /* GFX6-GFX8 */
      sctx->barrier_flags |= SI_BARRIER_INV_L2;
   }

   si_mark_atom_dirty(sctx, &sctx->atoms.s.barrier);
}

static inline void si_make_DB_shader_coherent(struct si_context *sctx, unsigned num_samples,
                                              bool include_stencil, bool shaders_read_metadata)
{
   sctx->barrier_flags |= SI_BARRIER_SYNC_AND_INV_DB | SI_BARRIER_INV_VMEM;
   sctx->force_shader_coherency.with_db = false;

   if (sctx->gfx_level >= GFX10 && sctx->gfx_level < GFX12) {
      if (sctx->screen->info.tcc_rb_non_coherent)
         sctx->barrier_flags |= SI_BARRIER_INV_L2;
      else if (shaders_read_metadata)
         sctx->barrier_flags |= SI_BARRIER_INV_L2_METADATA;
   } else if (sctx->gfx_level == GFX9) {
      /* Single-sample depth (not stencil) is coherent with shaders
       * on GFX9, but L2 metadata must be flushed if shaders read
       * metadata.
       */
      if (num_samples >= 2 || include_stencil)
         sctx->barrier_flags |= SI_BARRIER_INV_L2;
      else if (shaders_read_metadata)
         sctx->barrier_flags |= SI_BARRIER_INV_L2_METADATA;
   } else if (sctx->gfx_level <= GFX8) {
      /* GFX6-GFX8 */
      sctx->barrier_flags |= SI_BARRIER_INV_L2;
   }

   si_mark_atom_dirty(sctx, &sctx->atoms.s.barrier);
}

static inline bool si_can_sample_zs(struct si_texture *tex, bool stencil_sampler)
{
   return (stencil_sampler && tex->can_sample_s) || (!stencil_sampler && tex->can_sample_z);
}

static inline bool si_htile_enabled(struct si_texture *tex, unsigned level, unsigned zs_mask)
{
   struct si_screen *sscreen = si_screen(tex->buffer.b.b.screen);

   /* Gfx12 should never call this. */
   assert(sscreen->info.gfx_level < GFX12);

   if (zs_mask == PIPE_MASK_S && (tex->htile_stencil_disabled || !tex->surface.has_stencil))
      return false;

   if (!tex->is_depth || !tex->surface.meta_offset)
      return false;

   if (sscreen->info.gfx_level >= GFX8) {
      return level < tex->surface.num_meta_levels;
   } else {
      /* GFX6-7 don't have TC-compatible HTILE, which means they have to run
       * a decompression pass for every mipmap level before texturing, so compress
       * only one level to reduce the number of decompression passes to a minimum.
       */
      return level == 0;
   }
}

static inline bool vi_tc_compat_htile_enabled(struct si_texture *tex, unsigned level,
                                              unsigned zs_mask)
{
   struct si_screen *sscreen = si_screen(tex->buffer.b.b.screen);

   /* Gfx12 should never call this. */
   assert(sscreen->info.gfx_level < GFX12);

   assert(!tex->tc_compatible_htile || tex->surface.meta_offset);
   return tex->tc_compatible_htile && si_htile_enabled(tex, level, zs_mask);
}

static inline unsigned si_get_ps_iter_samples(struct si_context *sctx)
{
   if (sctx->gfx11_force_msaa_num_samples_zero)
      return 1;

   if (sctx->ps_uses_fbfetch)
      return sctx->framebuffer.nr_color_samples;

   return MIN2(sctx->ps_iter_samples, sctx->framebuffer.nr_color_samples);
}

static inline bool si_any_colorbuffer_written(struct si_context *sctx)
{
   if (sctx->queued.named.rasterizer->rasterizer_discard)
      return false;

   struct si_shader_selector *ps = sctx->shader.ps.cso;
   if (!ps || !ps->info.colors_written_4bit)
      return false;

   return (sctx->framebuffer.colorbuf_enabled_4bit &
           sctx->queued.named.blend->cb_target_enabled_4bit &
           (ps->info.color0_writes_all_cbufs ? ~0 : ps->info.colors_written_4bit)) != 0;
}

#define UTIL_ALL_PRIM_LINE_MODES                                                                   \
   ((1 << MESA_PRIM_LINES) | (1 << MESA_PRIM_LINE_LOOP) | (1 << MESA_PRIM_LINE_STRIP) |            \
    (1 << MESA_PRIM_LINES_ADJACENCY) | (1 << MESA_PRIM_LINE_STRIP_ADJACENCY))

#define UTIL_ALL_PRIM_TRIANGLE_MODES \
   ((1 << MESA_PRIM_TRIANGLES) | (1 << MESA_PRIM_TRIANGLE_STRIP) | \
    (1 << MESA_PRIM_TRIANGLE_FAN) | (1 << MESA_PRIM_QUADS) | (1 << MESA_PRIM_QUAD_STRIP) | \
    (1 << MESA_PRIM_POLYGON) | (1 << MESA_PRIM_TRIANGLES_ADJACENCY) | \
    (1 << MESA_PRIM_TRIANGLE_STRIP_ADJACENCY))

static inline bool util_prim_is_lines(unsigned prim)
{
   return ((1 << prim) & UTIL_ALL_PRIM_LINE_MODES) != 0;
}

static inline bool util_rast_prim_is_triangles(unsigned prim)
{
   return ((1 << prim) & UTIL_ALL_PRIM_TRIANGLE_MODES) != 0;
}

static inline void si_need_gfx_cs_space(struct si_context *ctx, unsigned num_draws,
                                        unsigned extra_dw_per_draw)
{
   struct radeon_cmdbuf *cs = &ctx->gfx_cs;
   /* Don't count the needed CS space exactly and just use an upper bound.
    *
    * Also reserve space for stopping queries at the end of IB, because
    * the number of active queries is unlimited in theory.
    */
   unsigned reserve_dw = 2048 + ctx->num_cs_dw_queries_suspend +
      num_draws * (10 + extra_dw_per_draw);

   if (!ctx->ws->cs_check_space(cs, reserve_dw))
      si_flush_gfx_cs(ctx, RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW, NULL);
}

/**
 * Add a buffer to the buffer list for the given command stream (CS).
 *
 * All buffers used by a CS must be added to the list. This tells the kernel
 * driver which buffers are used by GPU commands. Other buffers can
 * be swapped out (not accessible) during execution.
 *
 * The buffer list becomes empty after every context flush and must be
 * rebuilt.
 */
static inline void radeon_add_to_buffer_list(struct si_context *sctx, struct radeon_cmdbuf *cs,
                                             struct si_resource *bo, unsigned usage)
{
   assert(usage);
   sctx->ws->cs_add_buffer(cs, bo->buf, usage | RADEON_USAGE_SYNCHRONIZED,
                           bo->domains);
}

static inline void si_select_draw_vbo(struct si_context *sctx)
{
   pipe_draw_func draw_vbo = sctx->draw_vbo[!!sctx->shader.tes.cso]
                                           [!!sctx->shader.gs.cso]
                                           [sctx->ngg];
   pipe_draw_vertex_state_func draw_vertex_state =
      sctx->draw_vertex_state[!!sctx->shader.tes.cso]
                             [!!sctx->shader.gs.cso]
                             [sctx->ngg];
   assert(draw_vbo);
   assert(draw_vertex_state);

   if (unlikely(sctx->real_draw_vbo)) {
      assert(sctx->real_draw_vertex_state);
      sctx->real_draw_vbo = draw_vbo;
      sctx->real_draw_vertex_state = draw_vertex_state;
   } else {
      assert(!sctx->real_draw_vertex_state);
      sctx->b.draw_vbo = draw_vbo;
      sctx->b.draw_vertex_state = draw_vertex_state;
   }
}

/* Return the number of samples that the rasterizer uses. */
static inline unsigned si_get_num_coverage_samples(struct si_context *sctx)
{
   if (sctx->framebuffer.nr_samples > 1 &&
       sctx->queued.named.rasterizer->multisample_enable)
      return sctx->framebuffer.nr_samples;

   /* Note that smoothing_enabled is set by si_update_shaders. */
   if (sctx->smoothing_enabled)
      return SI_NUM_SMOOTH_AA_SAMPLES;

   return 1;
}

static unsigned ALWAYS_INLINE
si_num_vbos_in_user_sgprs_inline(enum amd_gfx_level gfx_level)
{
   /* This decreases CPU overhead if all descriptors are in user SGPRs because we don't
    * have to allocate and count references for the upload buffer.
    */
   return gfx_level >= GFX9 ? 5 : 1;
}

static inline
void si_check_dirty_buffers_textures(struct si_context *sctx)
{
   /* Recompute and re-emit the texture resource states if needed. */
   unsigned dirty_tex_counter = p_atomic_read(&sctx->screen->dirty_tex_counter);
   if (unlikely(dirty_tex_counter != sctx->last_dirty_tex_counter)) {
      sctx->last_dirty_tex_counter = dirty_tex_counter;
      sctx->framebuffer.dirty_cbufs |= ((1 << sctx->framebuffer.state.nr_cbufs) - 1);
      sctx->framebuffer.dirty_zsbuf = true;
      si_mark_atom_dirty(sctx, &sctx->atoms.s.framebuffer);
      si_update_all_texture_descriptors(sctx);
   }

   unsigned dirty_buf_counter = p_atomic_read(&sctx->screen->dirty_buf_counter);
   if (unlikely(dirty_buf_counter != sctx->last_dirty_buf_counter)) {
      sctx->last_dirty_buf_counter = dirty_buf_counter;
      /* Rebind all buffers unconditionally. */
      si_rebind_buffer(sctx, NULL);
   }
}

static inline void si_set_clip_discard_distance(struct si_context *sctx, float distance)
{
   /* Determine whether the guardband registers change.
    *
    * When we see a value greater than min_clip_discard_distance_watermark, we increase it
    * up to a certain number to eliminate those state changes next time they happen.
    * See the comment at min_clip_discard_distance_watermark.
    */
   if (distance > sctx->min_clip_discard_distance_watermark) {
      /* The maximum number was determined from Viewperf. The number is in units of half-pixels. */
      sctx->min_clip_discard_distance_watermark = MIN2(distance, 6);

      float old_distance = sctx->current_clip_discard_distance;
      float new_distance = MAX2(distance, sctx->min_clip_discard_distance_watermark);

      if (old_distance != new_distance) {
         sctx->current_clip_discard_distance = new_distance;
         si_mark_atom_dirty(sctx, &sctx->atoms.s.guardband);
      }
   }
}

/* Update these two GS_STATE fields. They depend on whatever the last shader before PS is
 * and the rasterizer state.
 *
 * It's expected that hw_vs and ngg are inline constants in draw_vbo after optimizations.
 */
static inline void
si_update_ngg_sgpr_state_provoking_vtx(struct si_context *sctx, struct si_shader *hw_vs, bool ngg)
{
   if (ngg && hw_vs && hw_vs->info.uses_gs_state_provoking_vtx_first) {
      SET_FIELD(sctx->current_gs_state, GS_STATE_PROVOKING_VTX_FIRST,
                sctx->queued.named.rasterizer->flatshade_first);
   }
}

static inline void
si_update_ngg_sgpr_state_out_prim(struct si_context *sctx, struct si_shader *hw_vs, bool ngg)
{
   if (ngg && hw_vs && hw_vs->info.uses_gs_state_outprim)
      SET_FIELD(sctx->current_gs_state, GS_STATE_OUTPRIM, sctx->gs_out_prim);
}

static inline void
si_update_ngg_cull_face_state(struct si_context *sctx)
{
   struct si_state_rasterizer *rs = sctx->queued.named.rasterizer;

   if (sctx->viewport0_y_inverted) {
      SET_FIELD(sctx->current_gs_state, GS_STATE_CULL_FACE_FRONT, rs->ngg_cull_back);
      SET_FIELD(sctx->current_gs_state, GS_STATE_CULL_FACE_BACK, rs->ngg_cull_front);
   } else {
      SET_FIELD(sctx->current_gs_state, GS_STATE_CULL_FACE_FRONT, rs->ngg_cull_front);
      SET_FIELD(sctx->current_gs_state, GS_STATE_CULL_FACE_BACK, rs->ngg_cull_back);
   }
}

/* Set the primitive type seen by the rasterizer. GS and tessellation affect this.
 * It's expected that hw_vs and ngg are inline constants in draw_vbo after optimizations.
 */
static ALWAYS_INLINE void
si_set_rasterized_prim(struct si_context *sctx, enum mesa_prim rast_prim,
                       struct si_shader *hw_vs, bool ngg)
{
   if (rast_prim != sctx->current_rast_prim) {
      bool is_rect = rast_prim == SI_PRIM_RECTANGLE_LIST;
      bool is_points = rast_prim == MESA_PRIM_POINTS;
      bool is_lines = util_prim_is_lines(rast_prim);

      if (is_points) {
         si_set_clip_discard_distance(sctx, sctx->queued.named.rasterizer->max_point_size);
         sctx->gs_out_prim = V_028A6C_POINTLIST;
      } else if (is_lines) {
         si_set_clip_discard_distance(sctx, sctx->queued.named.rasterizer->line_width);
         sctx->gs_out_prim = V_028A6C_LINESTRIP;
      } else if (is_rect) {
         /* Don't change the clip discard distance for rectangles. */
         sctx->gs_out_prim = V_028A6C_RECTLIST;
      } else {
         si_set_clip_discard_distance(sctx, 0);
         sctx->gs_out_prim = V_028A6C_TRISTRIP;
      }

      sctx->current_rast_prim = rast_prim;
      si_vs_ps_key_update_rast_prim_smooth_stipple(sctx);
      si_update_ngg_sgpr_state_out_prim(sctx, hw_vs, ngg);
   }
}

/* There are 3 ways to flush caches and all of them are correct.
 *
 * 1) sctx->flags |= ...;
 *    si_mark_atom_dirty(sctx, &sctx->atoms.s.barrier); // deferred
 *
 * 2) sctx->flags |= ...;
 *    si_emit_barrier_direct(sctx); // immediate
 *
 * 3) sctx->flags |= ...;
 *    sctx->emit_barrier(sctx, cs); // immediate (2 is better though)
 */
static inline void si_emit_barrier_direct(struct si_context *sctx)
{
   if (sctx->barrier_flags) {
      sctx->emit_barrier(sctx, &sctx->gfx_cs);
      sctx->dirty_atoms &= ~SI_ATOM_BIT(barrier);
   }
}

static inline bool si_is_buffer_idle(struct si_context *sctx, struct si_resource *buf,
                                     unsigned usage)
{
   return !si_cs_is_buffer_referenced(sctx, buf->buf, usage) &&
          sctx->ws->buffer_wait(sctx->ws, buf->buf, 0, usage | RADEON_USAGE_DISALLOW_SLOW_REPLY);
}

static inline bool si_vs_uses_vbos(struct si_shader_selector *sel)
{
   return !sel || !sel->info.base.vs.blit_sgprs_amd;
}

static ALWAYS_INLINE void
si_emit_all_states(struct si_context *sctx, uint64_t skip_atom_mask)
{
   /* Emit states by calling their emit functions. */
   uint64_t dirty = sctx->dirty_atoms & ~skip_atom_mask;

   if (dirty) {
      sctx->dirty_atoms &= skip_atom_mask;

      /* u_bit_scan64 is too slow on i386. */
      if (sizeof(void*) == 8) {
         do {
            unsigned i = u_bit_scan64(&dirty);
            sctx->atoms.array[i].emit(sctx, i);
         } while (dirty);
      } else {
         unsigned dirty_lo = dirty;
         unsigned dirty_hi = dirty >> 32;

         while (dirty_lo) {
            unsigned i = u_bit_scan(&dirty_lo);
            sctx->atoms.array[i].emit(sctx, i);
         }
         while (dirty_hi) {
            unsigned i = 32 + u_bit_scan(&dirty_hi);
            sctx->atoms.array[i].emit(sctx, i);
         }
      }
   }
}

#endif