/*
 * DarwinFire: offline generator for the Mali-G52 (Bifrost v7) SwapEnd compositor.
 * Built inside a Mesa 24.0.2 tree (src/panfrost/lib, PAN_ARCH=7) by .github/workflows/mali-gen.yml.
 *
 * Emits xnu/df_mali_blob.h: one descriptor blob that the XNU kernel copies and maps at the fixed
 * GPU VA DF_MALI_BLOB_VA, containing a pre-frame DCD (Draw) + Renderer State + Blend + Textures +
 * Sampler + Viewport + positions + TLS + the compiled fragment shader:
 *
 *   c   = texelFetch(tex0 = iOS SW frame 1080x2400 BGRA @ 0x20000000, fragcoord)
 *   w   = texelFetch(tex1 = wallpaper   540x1200 BGRA @ 0x30000000, fragcoord / 2)
 *   out = vec4(c.rgb + w.rgb * (1 - max(c.r, c.g, c.b)), 1)
 *
 * The kernel points Framebuffer Parameters "Frame Shader DCDs" at DF_MALI_DCD_VA with
 * Pre Frame 0 = Always, so the shader runs on every pixel of every tile (Mesa pan_blitter preload).
 */
#include <stdio.h>
#include <string.h>
#include "compiler/nir/nir_builder.h"
#include "pan_shader.h"
#include "pan_format.h"
#include "genxml/gen_macros.h"
#include "wrap.h"

#define FB_W        1080
#define FB_H        2400
#define WALL_W      540
#define WALL_H      1200
#define BLOB_VA     0x10000000ULL
#define SWFB_VA     0x20000000ULL
#define WALL_VA     0x30000000ULL

/* blob layout (bytes) */
#define OFF_DCD     0x000   /* 3 x DRAW (pre0, pre1, post) */
#define OFF_RSD     0x200   /* RENDERER_STATE + 1 BLEND */
#define OFF_TEX     0x300   /* 2 x TEXTURE */
#define OFF_SAMP    0x340   /* 1 x SAMPLER */
#define OFF_SURF    0x380   /* 2 x SURFACE_WITH_STRIDE */
#define OFF_VP      0x3c0   /* VIEWPORT */
#define OFF_POS     0x400   /* 4 x vec4 positions */
#define OFF_TLS     0x440   /* LOCAL_STORAGE */
#define OFF_SHADER  0x480   /* shader binary, 128-byte aligned */
#define BLOB_MAX    0x2000

static uint8_t blob[BLOB_MAX];

static nir_def *
txf(nir_builder *b, unsigned idx, nir_def *coord)
{
   nir_tex_instr *tex = nir_tex_instr_create(b->shader, 2);
   tex->op = nir_texop_txf;
   tex->dest_type = nir_type_float32;
   tex->texture_index = idx;
   tex->sampler_index = 0;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   tex->coord_components = 2;
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(b, 0));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(b, &tex->instr);
   return &tex->def;
}

int
main(int argc, char **argv)
{
   const char *out_path = argc > 1 ? argv[1] : "df_mali_blob.h";
   glsl_type_singleton_init_or_ref();

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, GENX(pan_shader_get_compiler_options)(), "df_composite");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "out0");
   out->data.location = FRAG_RESULT_DATA0;
   out->data.driver_location = 0;

   nir_def *fc = nir_load_frag_coord(&b);
   nir_def *xy = nir_f2i32(&b, nir_trim_vector(&b, fc, 2));
   nir_def *c = txf(&b, 0, xy);
   nir_def *w = txf(&b, 1, nir_ushr_imm(&b, xy, 1));
   nir_def *a = nir_fmax(&b, nir_channel(&b, c, 0),
                         nir_fmax(&b, nir_channel(&b, c, 1), nir_channel(&b, c, 2)));
   nir_def *k = nir_fsub_imm(&b, 1.0, a);
   nir_def *rgb = nir_ffma(&b, nir_trim_vector(&b, w, 3), nir_vec3(&b, k, k, k),
                           nir_trim_vector(&b, c, 3));
   nir_def *res = nir_vec4(&b, nir_channel(&b, rgb, 0), nir_channel(&b, rgb, 1),
                           nir_channel(&b, rgb, 2), nir_imm_float(&b, 1.0));
   nir_store_var(&b, out, res, 0xf);

   struct panfrost_compile_inputs inputs = {
      .gpu_id = 0x7402, /* Mali-G52 */
      .no_idvs = true,
   };
   struct util_dynarray binary;
   struct pan_shader_info info;
   util_dynarray_init(&binary, NULL);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   BITSET_SET(b.shader->info.textures_used, 0);
   BITSET_SET(b.shader->info.textures_used, 1);
   BITSET_SET(b.shader->info.samplers_used, 0);
   pan_shader_preprocess(b.shader, inputs.gpu_id);
   GENX(pan_shader_compile)(b.shader, &inputs, &binary, &info);
   if (binary.size > BLOB_MAX - OFF_SHADER) {
      fprintf(stderr, "shader too big: %u\n", binary.size);
      return 1;
   }
   memcpy(blob + OFF_SHADER, binary.data, binary.size);
   fprintf(stderr, "shader: %u bytes, work regs %u\n", binary.size, info.work_reg_count);

   /* Surfaces + textures */
   pan_pack(blob + OFF_SURF, SURFACE_WITH_STRIDE, cfg) {
      cfg.pointer = SWFB_VA;
      cfg.row_stride = FB_W * 4;
      cfg.surface_stride = FB_W * FB_H * 4;
   }
   pan_pack(blob + OFF_SURF + pan_size(SURFACE_WITH_STRIDE), SURFACE_WITH_STRIDE, cfg) {
      cfg.pointer = WALL_VA;
      cfg.row_stride = WALL_W * 4;
      cfg.surface_stride = WALL_W * WALL_H * 4;
   }
   const struct panfrost_format bgra = GENX(panfrost_pipe_format)[PIPE_FORMAT_B8G8R8A8_UNORM];
   for (unsigned t = 0; t < 2; t++) {
      pan_pack(blob + OFF_TEX + t * pan_size(TEXTURE), TEXTURE, cfg) {
         cfg.dimension = MALI_TEXTURE_DIMENSION_2D;
         cfg.format = bgra.hw;
         cfg.width = t ? WALL_W : FB_W;
         cfg.height = t ? WALL_H : FB_H;
         cfg.swizzle = 0 | (1 << 3) | (2 << 6) | (3 << 9); /* identity */
         cfg.texel_ordering = MALI_TEXTURE_LAYOUT_LINEAR;
         cfg.levels = 1;
         cfg.array_size = 1;
         cfg.sample_count = 1;
         cfg.surfaces = BLOB_VA + OFF_SURF + t * pan_size(SURFACE_WITH_STRIDE);
      }
   }
   pan_pack(blob + OFF_SAMP, SAMPLER, cfg) {
      cfg.normalized_coordinates = false;
      cfg.minify_nearest = true;
      cfg.magnify_nearest = true;
      cfg.clamp_integer_array_indices = false;
   }

   /* Viewport, positions, TLS */
   pan_pack(blob + OFF_VP, VIEWPORT, cfg) {
      cfg.minimum_x = 0.0f;
      cfg.minimum_y = 0.0f;
      cfg.maximum_x = FB_W;
      cfg.maximum_y = FB_H;
      cfg.scissor_minimum_x = 0;
      cfg.scissor_minimum_y = 0;
      cfg.scissor_maximum_x = FB_W - 1;
      cfg.scissor_maximum_y = FB_H - 1;
   }
   float pos[16] = {0, 0, 0, 1, FB_W, 0, 0, 1, 0, FB_H, 0, 1, FB_W, FB_H, 0, 1};
   memcpy(blob + OFF_POS, pos, sizeof(pos));
   pan_pack(blob + OFF_TLS, LOCAL_STORAGE, cfg) {
   }

   /* Renderer state (as Mesa pan_blitter_emit_rsd for a colour blit) + opaque blend */
   pan_pack(blob + OFF_RSD, RENDERER_STATE, cfg) {
      pan_shader_prepare_rsd(&info, BLOB_VA + OFF_SHADER, &cfg);
      cfg.multisample_misc.sample_mask = 0xFFFF;
      cfg.multisample_misc.depth_function = MALI_FUNC_ALWAYS;
      cfg.stencil_mask_misc.stencil_mask_front = 0xFF;
      cfg.stencil_mask_misc.stencil_mask_back = 0xFF;
      cfg.stencil_front.compare_function = MALI_FUNC_ALWAYS;
      cfg.stencil_front.stencil_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_pass = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.mask = 0xFF;
      cfg.stencil_back = cfg.stencil_front;
      cfg.properties.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
      cfg.properties.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;
      cfg.properties.allow_forward_pixel_to_kill = true;
   }
   const struct pan_blendable_format rgba8 =
      GENX(panfrost_blendable_formats)[PIPE_FORMAT_R8G8B8A8_UNORM];
   pan_pack(blob + OFF_RSD + pan_size(RENDERER_STATE), BLEND, cfg) {
      cfg.round_to_fb_precision = true;
      cfg.internal.mode = MALI_BLEND_MODE_OPAQUE;
      cfg.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.equation.color_mask = 0xf;
      cfg.internal.fixed_function.num_comps = 4;
      cfg.internal.fixed_function.conversion.memory_format = rgba8.bifrost[0];
      cfg.internal.fixed_function.conversion.register_format = MALI_REGISTER_FILE_FORMAT_F32;
      cfg.internal.fixed_function.rt = 0;
   }

   /* Pre-frame draw call descriptor (as Mesa pan_preload_emit_dcd, colour, v7) */
   pan_pack(blob + OFF_DCD, DRAW, cfg) {
      cfg.thread_storage = BLOB_VA + OFF_TLS;
      cfg.state = BLOB_VA + OFF_RSD;
      cfg.position = BLOB_VA + OFF_POS;
      cfg.viewport = BLOB_VA + OFF_VP;
      cfg.textures = BLOB_VA + OFF_TEX;
      cfg.samplers = BLOB_VA + OFF_SAMP;
      cfg.clean_fragment_write = false;
   }

   unsigned blob_size = OFF_SHADER + binary.size;
   FILE *f = fopen(out_path, "w");
   if (!f) {
      perror(out_path);
      return 1;
   }
   fprintf(f, "/* Generated by tools/mali_gen/df_mali_gen.c (Mesa panfrost v7) - do not edit */\n");
   fprintf(f, "#define DF_MALI_BLOB_VA  0x%llxULL\n", (unsigned long long)BLOB_VA);
   fprintf(f, "#define DF_MALI_SWFB_VA  0x%llxULL\n", (unsigned long long)SWFB_VA);
   fprintf(f, "#define DF_MALI_WALL_VA  0x%llxULL\n", (unsigned long long)WALL_VA);
   fprintf(f, "#define DF_MALI_DCD_VA   (DF_MALI_BLOB_VA + 0x%x)\n", OFF_DCD);
   fprintf(f, "#define DF_MALI_BLOB_SIZE %u\n", blob_size);
   fprintf(f, "#define DF_MALI_WORK_REGS %u\n", info.work_reg_count);
   fprintf(f, "static const uint8_t df_mali_blob[%u] __attribute__((aligned(128))) = {", blob_size);
   for (unsigned i = 0; i < blob_size; i++) {
      fprintf(f, "%s0x%02x,", (i % 16) ? "" : "\n  ", blob[i]);
   }
   fprintf(f, "\n};\n");
   fclose(f);
   fprintf(stderr, "wrote %s (%u bytes)\n", out_path, blob_size);
   /* ---- self-check: rebuild the kernel's fragment job (df_mali.c df_mali_compose) and run
    * Mesa's pandecode over the whole job chain, so descriptor mistakes show up in CI ---- */
   {
      const uint64_t JOB_VA = 0x40000000ULL, HEAP_VA = 0x50000000ULL, FB_VA = 0x7ce6a000ULL;
      static uint8_t jm[0x800] __attribute__((aligned(64)));
      static uint8_t heapm[0x40000] __attribute__((aligned(4096)));
      uint32_t *job = (uint32_t *)jm, *fbd = (uint32_t *)(jm + 0x100), *rt = fbd + 32;
      uint32_t *tctx = (uint32_t *)(jm + 0x400), *theap = (uint32_t *)(jm + 0x600);
      uint16_t *sloc = (uint16_t *)(jm + 0x700);
      for (int i = 0; i < 64; i++)
         sloc[i] = 128;
      theap[1] = sizeof(heapm);
      theap[2] = (uint32_t)HEAP_VA;            theap[3] = (uint32_t)(HEAP_VA >> 32);
      theap[4] = (uint32_t)HEAP_VA;            theap[5] = (uint32_t)(HEAP_VA >> 32);
      theap[6] = (uint32_t)(HEAP_VA + sizeof(heapm));
      theap[7] = (uint32_t)((HEAP_VA + sizeof(heapm)) >> 32);
      tctx[2] = 0x28;
      tctx[3] = (FB_W - 1) | ((uint32_t)(FB_H - 1) << 16);
      tctx[6] = (uint32_t)(JOB_VA + 0x600);   tctx[7] = 0;
      fbd[0] = 1;
      fbd[4] = (uint32_t)(JOB_VA + 0x700);    fbd[5] = 0;
      fbd[6] = (uint32_t)(BLOB_VA + OFF_DCD); fbd[7] = (uint32_t)((BLOB_VA + OFF_DCD) >> 32);
      fbd[8] = (FB_W - 1) | ((uint32_t)(FB_H - 1) << 16);
      fbd[10] = (FB_W - 1) | ((uint32_t)(FB_H - 1) << 16);
      fbd[11] = (8u << 9) | (1u << 24);
      fbd[12] = (1u << 29);
      fbd[14] = (uint32_t)(JOB_VA + 0x400);   fbd[15] = 0;
      rt[0] = (1u << 26);
      rt[1] = 1u | (19u << 3) | (2u << 8) | ((2u | (1u << 3) | (0u << 6) | (3u << 9)) << 16) | (1u << 31);
      rt[8] = (uint32_t)FB_VA;                rt[9] = 0;
      rt[10] = FB_W * 4;
      rt[11] = FB_W * FB_H * 4;
      job[4] = 1u | (9u << 1) | (1u << 16);
      job[9] = ((FB_W - 1) >> 4) | (((uint32_t)(FB_H - 1) >> 4) << 16);
      uint64_t fbptr = (JOB_VA + 0x100) | 1u;
      job[10] = (uint32_t)fbptr;              job[11] = (uint32_t)(fbptr >> 32);

      struct pandecode_context *ctx = pandecode_create_context(true);
      pandecode_inject_mmap(ctx, BLOB_VA, blob, BLOB_MAX, "blob");
      pandecode_inject_mmap(ctx, JOB_VA, jm, sizeof(jm), "jobmem");
      pandecode_inject_mmap(ctx, HEAP_VA, heapm, sizeof(heapm), "heap");
      fprintf(stderr, "=== pandecode of the kernel compositor job ===\n");
      pandecode_jc(ctx, JOB_VA, 0x7402);
      pandecode_destroy_context(ctx);
   }

   util_dynarray_fini(&binary);
   ralloc_free(b.shader);
   glsl_type_singleton_decref();
   return 0;
}
