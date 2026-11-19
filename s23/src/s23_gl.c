/*
 * s23_gl.c -- THE GPU PICTURE: a built frame (src/s23_build.h) drawn with OpenGL, organised like an ordinary PC renderer.
 *
 *   ASSETS, uploaded once (the ROM is data, nothing more):
 *     the texture tiles (tss1cg*, 32 MB of 8-bit pens)  -> an 8192 x 4096 single-channel texture
 *     the decoded tile map (tss1ccrl/h: tile, flips)     -> a 2048 x 1024 RGB texture
 *   PER FRAME, small uploads: the palette (32768 pens), the C412 stencil SRAM, the text layer's pen map, and ONE vertex buffer holding every
 *   polygon of the frame as triangles in the hardware's draw order (z-sort key, far to near).
 *   The target is stored TOP ROW FIRST (window y = screen y), so GL's fill rule breaks pixel-centre ties the way the hardware's scan does.
 *   PASSES, into an off-screen target at 640 x 480 times the internal-resolution scale:
 *     1. clear to the C404 background colour
 *     2. the text layer's pixels (all of them sit under the polygons at first)
 *     3. the polygons: ONE draw call. GL draws the primitives of a call in order, so blending against what is already there reproduces the
 *        hardware's painter's order exactly, without a depth buffer. The fragment shader does what the board's pixel pipeline does: the
 *        tile map -> tile -> pen lookup per pixel (no texture baking), the 2/4/8-bit colour modes, the stencil, Gouraud shade, the polygon
 *        colour fade, the screen fade, alpha by colour / by pen and the 50% blend.
 *     4. the same triangles into a small priority target (MAME's priority map: which text pixels a polygon lets through again)
 *     5. the text pixels the priority target marks, over the polygons
 *   The software back end (src/s23_render.c s23_soft_raster) draws the same built frame at 640 x 480 on the CPU: it is the fallback for a
 *   machine without GLSL 1.30 and the oracle this file is gated against (S23_GPU_GATE, src/s23_main.c).
 *
 * Needs OpenGL 3.0 / GLSL 1.30 (in a compatibility context) and 8192-texel textures: every Intel GPU since Sandy Bridge (HD 2000 / 3000).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include "s23_build.h"
#include "s23_game.h"
#include "s23_host.h"
#include "eng_display.h"

/* the post-1.1 entry points, from the driver at run time (the same on Linux and Windows) */
#define GLFN(T, n) static T p_##n;
#define GLFNS \
    GLFN(PFNGLCREATESHADERPROC, glCreateShader) GLFN(PFNGLSHADERSOURCEPROC, glShaderSource) GLFN(PFNGLCOMPILESHADERPROC, glCompileShader) \
    GLFN(PFNGLGETSHADERIVPROC, glGetShaderiv) GLFN(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) GLFN(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    GLFN(PFNGLATTACHSHADERPROC, glAttachShader) GLFN(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) GLFN(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    GLFN(PFNGLGETPROGRAMIVPROC, glGetProgramiv) GLFN(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) GLFN(PFNGLUSEPROGRAMPROC, glUseProgram) \
    GLFN(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) GLFN(PFNGLUNIFORM1IPROC, glUniform1i) GLFN(PFNGLUNIFORM3IPROC, glUniform3i) \
    GLFN(PFNGLUNIFORM4IPROC, glUniform4i) GLFN(PFNGLGENBUFFERSPROC, glGenBuffers) GLFN(PFNGLBINDBUFFERPROC, glBindBuffer) \
    GLFN(PFNGLBUFFERDATAPROC, glBufferData) GLFN(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    GLFN(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) GLFN(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) \
    GLFN(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) GLFN(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    GLFN(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) GLFN(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    GLFN(PFNGLACTIVETEXTUREPROC, glActiveTexture) GLFN(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate) \
    GLFN(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) GLFN(PFNGLUNIFORM1FPROC, glUniform1f) GLFN(PFNGLBLENDEQUATIONPROC, glBlendEquation)
GLFNS
#undef GLFN
static int load_fns(void)
{
    int ok = 1;
#define GLFN(T, n) if (!(p_##n = (T)SDL_GL_GetProcAddress(#n))) { fprintf(stderr, "[GPU] no %s\n", #n); ok = 0; }
    GLFNS
#undef GLFN
    return ok;
}

/* the ROM data, owned by src/s23_render.c */
void s23_render_assets(const uint8_t **texrom, const uint32_t **tm_decoded, const uint8_t **tattr);

static const char *vs_poly =
    "#version 130\n"
    "in vec4 a_pos; in vec3 a_uvs; in vec4 a_par; in vec4 a_clip; in vec2 a_ext; in vec4 a_uvb;\n"
    "out vec3 v_uvs; flat out vec4 v_par; flat out vec4 v_clip; flat out vec2 v_ext; flat out vec4 v_uvb;\n"
    "void main() { gl_Position = a_pos; v_uvs = a_uvs; v_par = a_par; v_clip = a_clip; v_ext = a_ext; v_uvb = a_uvb; }\n";
static const char *fs_poly =
    "#version 130\n"
    "uniform sampler2D t_rom, t_map, t_pal, t_sten, t_near, t_nearz, t_photo;\n"
    "uniform int u_ex, u_scale, u_h, u_prio, u_pfade, u_ff, u_alpha, u_alpha_pen, u_near, u_depth, u_photo;\n"
    "uniform float u_keep;\n"
    "uniform ivec3 u_pcol, u_fcol, u_fogc;\n"
    "uniform sampler2D t_fog; uniform int u_fog, u_seam;\n"
    "in vec3 v_uvs; flat in vec4 v_par; flat in vec4 v_clip; flat in vec2 v_ext; flat in vec4 v_uvb;\n"
    "uint b8(vec4 t, int c) { return uint(t[c] * 255.0 + 0.5); }\n"
    "uniform int u_filt, u_aniso;\n"                                   /* the optional 3D texture filter for polygons without the no-filter flag (1024): 1 bilinear, 2 trilinear-like (u_aniso 0) or anisotropic (u_aniso = 2..16), 3 sharp bilinear, 4 sharp trilinear */
    "uint tex_pen(uint tx, uint ty) {\n"                               /* the pen at texel (tx, ty): tile map, its flips, the texture ROM -- ty already holds the polygon's tbase */
    "  uint id = ((tx >> 4u) & 0xFFu) | ((ty << 4u) & 0x1FFF00u);\n"
    "  vec4 m = texelFetch(t_map, ivec2(int(id & 2047u), int(id >> 11u)), 0);\n"
    "  uint mb = b8(m, 2); uint tile = b8(m, 0) | (b8(m, 1) << 8u) | ((mb & 1u) << 16u); uint a = mb >> 1u;\n"
    "  if ((a & 1u) != 0u) ty = ~ty;\n"
    "  if ((a & 2u) != 0u) tx = ~tx;\n"
    "  if ((a & 4u) != 0u) { uint t = tx; tx = ty; ty = t; }\n"
    "  uint ri = ((tile << 8u) | ((ty << 4u) & 0xF0u) | (tx & 0x0Fu)) & 0x1FFFFFFu;\n"
    "  return b8(texelFetch(t_rom, ivec2(int(ri & 8191u), int(ri >> 13u)), 0), 0);\n"
    "}\n"
    "vec3 pen_col(uint pen, uint pbase, uint pshift, uint pmask) {\n"
    "  uint ci = pbase + ((pen >> pshift) & pmask);\n"
    "  return texelFetch(t_pal, ivec2(int(ci & 255u), int((ci >> 8u) & 127u)), 0).rgb * 255.0;\n"
    "}\n"
    /* THE FILTERS' TEXEL FETCHES. The board shows texel floor(u): texel i covers [i, i+1), its centre is i + 0.5. Every filtered fetch
     * keeps to THIS polygon's texels (v_uvb, its uv box: the first texel and the last one the board shows): a tap past its edge takes the
     * edge texel, never the next texel of the tile map (another piece of art). */
    "vec3 tap_bl(vec2 uv, vec2 sw, uint zb, uint pbase, uint pshift, uint pmask) {\n"   /* bilinear round uv - 0.5; sw > 1 narrows the blend between two texels (SHARP: sw = pixels per texel) */
    "  vec2 p = uv - 0.5, f0 = floor(p), w = clamp((p - f0 - 0.5) * sw + 0.5, 0.0, 1.0);\n"
    "  int x0 = clamp(int(f0.x), int(v_uvb.x), int(v_uvb.z)), x1 = clamp(int(f0.x) + 1, int(v_uvb.x), int(v_uvb.z));\n"
    "  int y0 = clamp(int(f0.y), int(v_uvb.y), int(v_uvb.w)), y1 = clamp(int(f0.y) + 1, int(v_uvb.y), int(v_uvb.w));\n"
    "  vec3 c00 = pen_col(tex_pen(uint(x0), uint(y0) + zb), pbase, pshift, pmask), c10 = pen_col(tex_pen(uint(x1), uint(y0) + zb), pbase, pshift, pmask);\n"
    "  vec3 c01 = pen_col(tex_pen(uint(x0), uint(y1) + zb), pbase, pshift, pmask), c11 = pen_col(tex_pen(uint(x1), uint(y1) + zb), pbase, pshift, pmask);\n"
    "  return mix(mix(c00, c10, w.x), mix(c01, c11, w.x), w.y);\n"
    "}\n"
    "vec3 tap_nn(vec2 uv, uint zb, uint pbase, uint pshift, uint pmask) {\n"   /* the texel under uv, for the wide footprints (taps spaced a texel or more apart) */
    "  vec2 r = floor(uv);\n"
    "  return pen_col(tex_pen(uint(clamp(int(r.x), int(v_uvb.x), int(v_uvb.z))), uint(clamp(int(r.y), int(v_uvb.y), int(v_uvb.w))) + zb), pbase, pshift, pmask);\n"
    "}\n"
    "void main() {\n"
    "  vec2 d_x = dFdx(v_uvs.xy), d_y = dFdy(v_uvs.xy);\n"                 /* the texel footprint of one fragment: before any discard, derivatives need the whole 2x2 quad */
    "  ivec2 fc = ivec2(gl_FragCoord.xy) * (u_prio != 0 ? u_scale : 1);\n"   /* (the priority target is at board resolution) */
    "  ivec2 px = ivec2(fc.x / u_scale - u_ex, fc.y / u_scale);\n"
    "  if (px.x < int(v_clip.x) || px.x >= int(v_clip.z) || px.y < int(v_clip.y) || px.y >= int(v_clip.w)) discard;\n"
    "  int fl = int(v_par.w);\n"
    "  uint tx = uint(int(v_uvs.x)), ty = uint(int(v_uvs.y));\n"
    "  if ((fl & 1) != 0) {\n"                                            /* the stencil: a 0 bit in the C412 SRAM hides the pixel */
    "    uint offs = ((ty << 6u) | (tx >> 4u)) & 0x1FFFFu;\n"
    "    vec4 s = texelFetch(t_sten, ivec2(int(offs & 511u), int(offs >> 9u)), 0);\n"
    "    uint w = b8(s, 0) | (b8(s, 3) << 8u);\n"
    "    if (((w >> ((tx & 15u) ^ 15u)) & 1u) == 0u) discard;\n"
    "  }\n"
    "  if (u_seam != 0) { float e = v_ext.x; gl_FragColor = vec4(fract(e * 0.137), fract(e * 0.311), fract(e * 0.0731), 1.0); return; }\n"   /* S23_SEAMTEST: coverage */
    "  uint ptx = tx, pty = ty;\n"                                      /* the photo page: page-relative texel */
    "  ty += uint(v_par.z);\n"
    "  float oz = gl_FragCoord.w;\n"                                      /* 1/z: what the software renderer interpolates */
    "  if (u_near != 0) {\n"                                              /* EXACT DEPTH pre-pass: the nearest opaque pixel, its object and band */
    "    if ((fl & 2) != 0 || (u_alpha != 255 && (fl & 4) != 0) || (fl & 512) != 0) discard;\n"
    "    gl_FragDepth = clamp(0.5 + log2(max(oz, 1e-30)) / 256.0, 0.0, 1.0);\n"
    "    int e = int(v_ext.x);\n"
    "    gl_FragColor = vec4(float(e & 255) / 255.0, float((e >> 8) & 255) / 255.0, v_ext.y / 255.0, (fl & 256) != 0 ? 0.75 : 1.0); return;\n"   /* alpha 0.75: s23_game.depth_role 1 */
    "  }\n"
    "  if (u_depth != 0) {\n"                                             /* hidden if ANOTHER object of this band is clearly nearer here */
    "    ivec2 fp = fc; vec4 nn = texelFetch(t_near, fp, 0);\n"
    "    if ((fl & 512) != 0) { if (nn.a > 0.5 && nn.a < 0.9) discard; }\n"   /* depth_role 2: under a role-1 owner wherever it is nearest */
    "    else if ((fl & 256) != 0 && nn.a > 0.9 && (int(nn.r * 255.0 + 0.5) | (int(nn.g * 255.0 + 0.5) << 8)) != int(v_ext.x)) { }\n"   /* an owner is never hidden by another model (the raft floor under the water) */
    "    else if (nn.a > 0.5) {\n"
    "      int ne = int(nn.r * 255.0 + 0.5) | (int(nn.g * 255.0 + 0.5) << 8), nb = int(nn.b * 255.0 + 0.5);\n"
    "      if (((ne != int(v_ext.x)) ? (u_depth & 1) != 0 : (u_depth & 2) != 0) && nb == int(v_ext.y) && oz < exp2((texelFetch(t_nearz, fp, 0).r - 0.5) * 256.0) * u_keep) discard;\n"
    "    }\n"
    "  }\n"
    "  int cmode = int(v_par.y); uint pbase = uint(v_par.x); uint pmask = 0xFFu; uint pshift = 0u;\n"
    "  if ((cmode & 4) != 0) { pbase += 0xECu + uint((cmode & 8) << 1); pmask = 3u; pshift = uint(2 * (~cmode & 3)); }\n"
    "  else if ((cmode & 2) != 0) { pbase += 0xE0u + uint((cmode & 8) << 1); pmask = 15u; pshift = uint(4 * (~cmode & 1)); }\n"
    "  uint pen = tex_pen(tx, ty);\n"                                      /* the tile map: tile number and its flips, then the texture ROM */
    "  if (u_prio != 0) {\n"                                               /* the priority target: R text-or-7, G any polygon, A last was 7 */
    "    int pv = (fl >> 3) & 7;\n"
    "    gl_FragColor = vec4(pv == 7 ? 1.0 : 0.0, pv != 0 ? 1.0 : 0.0, 0.0, pv == 7 ? 1.0 : 0.0); return;\n"
    "  }\n"
    "  vec3 cf = pen_col(pen, pbase, pshift, pmask);\n"
    "  int prow = int(v_par.z) + int(pty) - u_photo;\n"            /* the photo page: any way a polygon reaches its rows */
    "  if (u_photo != 0 && prow >= 0 && prow < 256 && ptx < 1024u) { cf = texelFetch(t_photo, ivec2(int(ptx), prow), 0).rgb * 255.0; pen = 1u; }\n"
    "  else if (u_filt != 0 && (fl & 1024) == 0) {\n"                   /* THE OPTIONAL 3D TEXTURE FILTER (Display > 3D texture filter), the same five as the System 22 games:
                                                                           1 BILINEAR, 3 SHARP BILINEAR (the blend between two texels only across about one screen pixel at their border, so the texels
                                                                           stay solid however big they are on screen; plain bilinear where a texel is smaller than a pixel), 2 TRILINEAR and 4 SHARP
                                                                           TRILINEAR. The board's texture is paletted and tile-mapped, with no mip chain to sample, so the trilinear modes average the
                                                                           footprint of the fragment directly: the texel-space derivatives give its size, and a grid of taps over it stands in for the
                                                                           mip level (a square the size of the longer side, like an isotropic mip) or, with u_aniso, the true parallelogram (N taps along
                                                                           the long axis, the short side kept sharp). Where the footprint is a texel or less (magnified) they are the bilinear -- the
                                                                           sharp one for sharp trilinear. Never the camera board's photo page (Gunmen Wars): its pixels are a picture, not texels */
    "    uint zb = uint(v_par.z);\n"
    "    vec2 tpp = vec2(length(vec2(d_x.x, d_y.x)), length(vec2(d_x.y, d_y.y)));\n"   /* texels per screen pixel, per axis */
    "    vec2 sw = (u_filt == 3 || u_filt == 4) ? max(1.0 / max(tpp, vec2(1e-4)), vec2(1.0)) : vec2(1.0);\n"
    "    float l1 = length(d_x), l2 = length(d_y), lmaj = max(l1, l2), lmin = min(l1, l2);\n"
    "    int am = 1, an = 1; vec2 ax = vec2(0.0), ay = vec2(0.0);\n"
    "    if ((u_filt == 2 || u_filt == 4) && lmaj > 1.0) {\n"
    "      lmaj = min(lmaj, 128.0);\n"
    "      vec2 mj = (l1 >= l2 ? d_x : d_y) * (lmaj / max(max(l1, l2), 1e-6)), mn = l1 >= l2 ? d_y : d_x;\n"
    "      vec2 pd = vec2(-mj.y, mj.x) / lmaj;\n"                          /* unit vector across the long axis */
    "      float ln = lmaj;\n"
    "      if (u_aniso >= 2) { ln = max(min(lmin, lmaj), lmaj / float(u_aniso)); mn = lmin > 1e-4 ? mn * (ln / lmin) : pd * ln; }\n"
    "      else mn = pd * ln;\n"
    "      ax = mj; ay = mn;\n"
    "      am = clamp(int(ceil(lmaj)), 1, u_aniso >= 2 ? 16 : 8); an = clamp(int(ceil(ln)), 1, u_aniso >= 2 ? 4 : 8);\n"
    "    }\n"
    "    if (am * an == 1) cf = tap_bl(v_uvs.xy, sw, zb, pbase, pshift, pmask);\n"
    "    else {\n"
    "      vec3 acc = vec3(0.0); bool bl = am * an <= 4;\n"
    "      for (int i = 0; i < am; i++) for (int j = 0; j < an; j++) {\n"
    "        vec2 p = v_uvs.xy + ax * ((float(i) + 0.5) / float(am) - 0.5) + ay * ((float(j) + 0.5) / float(an) - 0.5);\n"
    "        acc += bl ? tap_bl(p, vec2(1.0), zb, pbase, pshift, pmask) : tap_nn(p, zb, pbase, pshift, pmask);\n"
    "      }\n"
    "      cf = acc / float(am * an);\n"
    "    }\n"
    "  }\n"
    "  ivec3 col = ivec3(cf + 0.5);\n"
    "  int sh = clamp(int(v_uvs.z), 0, (fl & 64) != 0 ? 127 : 63);\n"
    "  col = min((col * sh) >> 6, ivec3(255));\n"
    "  if (u_fog != 0 && (fl & 128) != 0) {\n"                          /* the depth fog: the level at z * 64, toward the fog colour */
    "    int cz = clamp(int(64.0 / max(oz, 1e-30)), 0, 8191);\n"
    "    int ff = int(texelFetch(t_fog, ivec2(cz & 255, cz >> 8), 0).r * 255.0 + 0.5);\n"
    "    if (ff != 0) col = (col * (256 - ff) + u_fogc * ff) >> 8;\n"
    "  }\n"
    "  if (u_pfade != 0) col = (col * u_pcol) >> 8;\n"
    "  if (u_ff != 255) col = (col * u_ff + u_fcol * (256 - u_ff)) >> 8;\n"
    "  bool ua = u_alpha != 255 && ((fl & 4) != 0 || int(pen) == u_alpha_pen);\n"
    "  float al = ua ? float(u_alpha) / 256.0 : ((fl & 2) != 0 ? 0.5 : 1.0);\n"
    "  gl_FragColor = vec4(vec3(col) / 255.0, al);\n"
    "}\n";
static const char *vs_text =
    "#version 130\n"
    "in vec4 a_pos;\n"
    "void main() { gl_Position = a_pos; }\n";
static const char *fs_text =
    "#version 130\n"
    "uniform sampler2D t_mix, t_pal, t_prio, t_band;\n"
    "uniform int u_ex, u_scale, u_h, u_pass, u_fade, u_ff, u_alpha, u_amask, u_c12, u_c13;\n"
    "uniform ivec3 u_fcol;\n"
    "uniform int u_nts, u_td0, u_td1; uniform ivec4 u_ts0, u_ts1;\n"   /* text-layer shifts (widescreen HUD): rect x0,y0,x1,y1 moved by td */
    "bool in_ts(ivec2 p, ivec4 r) { return p.x >= r.x && p.x < r.z && p.y >= r.y && p.y < r.w; }\n"
    "uniform int u_nst; uniform ivec4 u_st0, u_st1;\n"   /* stretch bands (widescreen HUD): rows y0..y1, the part left of L to the left
                                                          * edge, right of R to the right edge, L..R stretched between (Crisis Zone's bars) */
    "int st_x(int x, ivec4 s) { if (x < s.z - u_ex) return x + u_ex; if (x >= s.w + u_ex) return x - u_ex;\n"
    "  return s.z + (x - (s.z - u_ex)) * (s.w - s.z) / (s.w - s.z + 2 * u_ex); }\n"
    "void main() {\n"
    "  ivec2 px = ivec2(int(gl_FragCoord.x) / u_scale - u_ex, int(gl_FragCoord.y) / u_scale);\n"
    "  if (u_nst > 0 && px.y >= u_st0.x && px.y < u_st0.y) px.x = st_x(px.x, u_st0);\n"
    "  else if (u_nst > 1 && px.y >= u_st1.x && px.y < u_st1.y) px.x = st_x(px.x, u_st1);\n"
    "  if (u_nts > 0) {\n"
    "    ivec2 a = ivec2(px.x - u_td0, px.y), b = ivec2(px.x - u_td1, px.y);\n"
    "    if (in_ts(a, u_ts0)) px = a; else if (u_nts > 1 && in_ts(b, u_ts1)) px = b;\n"
    "    else if (in_ts(px, u_ts0) || (u_nts > 1 && in_ts(px, u_ts1))) discard;\n"   /* (moved away from here) */
    "  }\n"
    "  uint mv;\n"
    "  if (px.x < 0 || px.x >= 640) {\n"      /* widescreen: a text row opaque all the way across (the cut-scenes' letterbox) continues
                                                   into the side strips in its commonest pen (b->band); anything else stays in the 4:3 centre */
    "    vec4 a = texelFetch(t_band, ivec2(px.y, 0), 0);\n"
    "    mv = uint(a.r * 255.0 + 0.5) | (uint(a.a * 255.0 + 0.5) << 8u);\n"
    "  } else {\n"
    "    vec4 m = texelFetch(t_mix, ivec2(px.x, px.y), 0);\n"
    "    mv = uint(m.r * 255.0 + 0.5) | (uint(m.a * 255.0 + 0.5) << 8u);\n"
    "  }\n"
    "  if ((mv & 0x8000u) == 0u) discard;\n"
    "  if (u_pass == 0) { gl_FragColor = vec4(1.0, 0.0, 0.0, 0.0); return; }\n"       /* the priority target starts as 'text here' */
    "  uint pen = mv & 0x7FFFu;\n"
    "  bool ua0 = u_alpha != 255 && (int(pen & 15u) == u_amask || (int(pen & 255u) >= u_c12 && int(pen & 255u) <= u_c13));\n"
    /* the 1x priority target misses slivers the internal resolution draws (session 18: the pylons' edges as grey lines through HOW TO
     * PLAY in the in-between pictures): SOLID text goes over again everywhere -- twice is the same pixel; see-through text keeps the test */
    "  if (u_pass == 2) { vec4 p = texelFetch(t_prio, ivec2(gl_FragCoord.xy) / u_scale, 0); if (!(p.r > 0.5 && (p.g > 0.5 || !ua0) && p.a < 0.5)) discard; }\n"
    "  ivec3 col = ivec3(texelFetch(t_pal, ivec2(int(pen & 255u), int(pen >> 8u)), 0).rgb * 255.0 + 0.5);\n"
    "  if (u_fade != 0) col = (col * u_ff + u_fcol * (256 - u_ff)) >> 8;\n"
    "  bool ua = u_alpha != 255 && (int(pen & 15u) == u_amask || (int(pen & 255u) >= u_c12 && int(pen & 255u) <= u_c13));\n"
    "  gl_FragColor = vec4(vec3(col) / 255.0, ua ? float(u_alpha) / 256.0 : 1.0);\n"
    "}\n";

static const char *fs_gamma =
    "#version 130\n"
    "uniform sampler2D t_src, t_lut;\n"
    "void main() {\n"
    "  vec3 c = texelFetch(t_src, ivec2(gl_FragCoord.xy), 0).rgb;\n"
    "  ivec3 i = ivec3(c * 255.0 + 0.5);\n"
    "  gl_FragColor = vec4(texelFetch(t_lut, ivec2(i.r, 0), 0).r, texelFetch(t_lut, ivec2(i.g, 0), 0).g, texelFetch(t_lut, ivec2(i.b, 0), 0).b, 1.0);\n"
    "}\n";
static int ok, scale = 1, ex, fbw, fbh;                  /* ex: widescreen margin each side, in board pixels (0 = 4:3) */
static GLuint prog_poly, prog_text, prog_gamma, vbo, quad_vbo, t_out, fbo_out, t_lut;
static GLint ug[2];
static GLuint t_fog, t_band, t_rom, t_map, t_pal, t_sten, t_mix, t_col, t_prio, fbo_col, fbo_prio;
static GLint up[32], ut[23];
static GLuint t_near, t_nearz, fbo_near;
static GLuint t_photo; static int photo_gen_up = -1;   /* the photo page (s23_photo_tex) and the version uploaded */          /* the exact-depth pre-pass target: nearest object / band (colour), its 1/z (depth) */
enum { A_POS, A_UVS, A_PAR, A_CLIP, A_EXT, A_UVB };
#define VFLOATS 21
static float *verts; static size_t vcap;

static GLuint shader(GLenum kind, const char *src)
{
    const GLuint s = p_glCreateShader(kind);
    p_glShaderSource(s, 1, &src, NULL); p_glCompileShader(s);
    GLint good = 0; p_glGetShaderiv(s, GL_COMPILE_STATUS, &good);
    if (!good) { char log[2048]; p_glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "[GPU] shader: %s\n", log); return 0; }
    return s;
}
static GLuint program(const char *vs, const char *fs)
{
    const GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    const GLuint p = p_glCreateProgram();
    p_glAttachShader(p, v); p_glAttachShader(p, f);
    p_glBindAttribLocation(p, A_POS, "a_pos"); p_glBindAttribLocation(p, A_UVS, "a_uvs");
    p_glBindAttribLocation(p, A_PAR, "a_par"); p_glBindAttribLocation(p, A_CLIP, "a_clip"); p_glBindAttribLocation(p, A_EXT, "a_ext");
    p_glBindAttribLocation(p, A_UVB, "a_uvb");
    p_glLinkProgram(p);
    GLint good = 0; p_glGetProgramiv(p, GL_LINK_STATUS, &good);
    if (!good) { char log[2048]; p_glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "[GPU] link: %s\n", log); return 0; }
    return p;
}
static GLuint texture(GLint ifmt, int w, int h, GLenum fmt, const void *data)
{
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
    return t;
}
static int targets(void)
{
    fbw = (640 + 2 * ex) * scale; fbh = 480 * scale;
    if (!t_col) { glGenTextures(1, &t_col); glGenTextures(1, &t_prio); glGenTextures(1, &t_out);
                  p_glGenFramebuffers(1, &fbo_col); p_glGenFramebuffers(1, &fbo_prio); p_glGenFramebuffers(1, &fbo_out); }
    const GLuint tt[3] = { t_col, t_prio, t_out }, ff[3] = { fbo_col, fbo_prio, fbo_out };
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, tt[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (tt[i] == t_prio) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fbw / scale, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);   /* board pixels */
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fbw, fbh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_glBindFramebuffer(GL_FRAMEBUFFER, ff[i]);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tt[i], 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[GPU] render target %dx%d incomplete\n", fbw, fbh); p_glBindFramebuffer(GL_FRAMEBUFFER, 0); return 0; }
    }
    if (!t_near) { glGenTextures(1, &t_near); glGenTextures(1, &t_nearz); p_glGenFramebuffers(1, &fbo_near); }
    glBindTexture(GL_TEXTURE_2D, t_near);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fbw, fbh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, t_nearz);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, 0x884C /* GL_TEXTURE_COMPARE_MODE */, 0 /* GL_NONE: sample the value */);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x8CAC /* GL_DEPTH_COMPONENT32F */, fbw, fbh, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_near);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t_near, 0);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, 0x8D00 /* GL_DEPTH_ATTACHMENT */, GL_TEXTURE_2D, t_nearz, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[GPU] exact-depth target incomplete: depth off\n"); fbo_near = 0; }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

int s23_gl_ok(void) { return ok; }
int s23_gl_scale(void) { return scale; }
int s23_gl_wide(void) { return ex; }
/* widescreen: the picture is (640 + 2 * e) x 480 board pixels; e = 0 is the board's 4:3 */
void s23_gl_set_wide(int e)
{
    if (e < 0) e = 0;
    if (e > 400) e = 400;
    if (e == ex) return;
    ex = e;
    if (ok && !targets()) { ex = 0; targets(); }
    fprintf(stderr, "[GPU] picture %d x %d board pixels (%s)\n", 640 + 2 * ex, 480, ex ? "widescreen" : "4:3");
}
void s23_gl_set_scale(int s)
{
    if (s < 1) s = 1;
    if (s > 4) s = 4;
    if (s == scale) return;
    scale = s;
    if (ok && !targets()) { scale = 1; targets(); }
    fprintf(stderr, "[GPU] internal resolution %d x %d\n", fbw, fbh);
}

int s23_gl_init(void)
{
    ok = 0;
    const char *e = getenv("S23_GPU");
    if (e && *e == '0') { fprintf(stderr, "[GPU] off (S23_GPU=0): the software picture\n"); return 0; }
    const char *sl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    int maj = 0, min = 0; if (sl) sscanf(sl, "%d.%d", &maj, &min);
    GLint maxtex = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    if (maj * 100 + min < 130 || maxtex < 8192) {
        fprintf(stderr, "[GPU] needs GLSL 1.30 and 8192-texel textures (have GLSL %s, %d): the software picture\n", sl ? sl : "?", maxtex);
        return 0;
    }
    if (!load_fns()) return 0;
    if (!(prog_poly = program(vs_poly, fs_poly)) || !(prog_text = program(vs_text, fs_text)) || !(prog_gamma = program(vs_text, fs_gamma))) return 0;
    ug[0] = p_glGetUniformLocation(prog_gamma, "t_src"); ug[1] = p_glGetUniformLocation(prog_gamma, "t_lut");
    static const char *pn[] = { "t_rom", "t_map", "t_pal", "t_sten", "u_scale", "u_h", "u_prio", "u_pfade", "u_ff", "u_alpha", "u_alpha_pen", "u_pcol", "u_fcol", "u_ex",
                                "t_near", "t_nearz", "u_near", "u_depth", "u_keep", "t_photo", "u_photo", "t_fog", "u_fog", "u_fogc", "u_seam", "u_filt", "u_aniso" };   /* (the fog: the author's 0.10, after ours) */
    for (int i = 0; i < 27; i++) up[i] = p_glGetUniformLocation(prog_poly, pn[i]);
    static const char *tn[] = { "t_mix", "t_pal", "t_prio", "u_scale", "u_h", "u_pass", "u_fade", "u_ff", "u_alpha", "u_amask", "u_c12", "u_c13", "u_fcol", "u_ex", "t_band",
                                "u_nts", "u_ts0", "u_ts1", "u_td0", "u_td1", "u_nst", "u_st0", "u_st1" };
    for (int i = 0; i < 23; i++) ut[i] = p_glGetUniformLocation(prog_text, tn[i]);

    const uint8_t *texrom; const uint32_t *tm; const uint8_t *tattr;
    s23_render_assets(&texrom, &tm, &tattr);
    if (!texrom) { fprintf(stderr, "[GPU] no ROM data\n"); return 0; }
    t_rom = texture(GL_LUMINANCE8, 8192, 4096, GL_LUMINANCE, texrom);
    uint8_t *map = malloc(0x200000u * 3);
    for (uint32_t id = 0; id < 0x200000u; id++) {
        const uint32_t tile = tm[id] >> 8;
        map[3 * id] = (uint8_t)tile; map[3 * id + 1] = (uint8_t)(tile >> 8); map[3 * id + 2] = (uint8_t)((tile >> 16 & 1) | tattr[id] << 1);
    }
    t_map = texture(GL_RGB8, 2048, 1024, GL_RGB, map);
    free(map);
    t_pal = texture(GL_RGBA8, 256, 128, GL_BGRA, NULL);
    t_sten = texture(GL_LUMINANCE8_ALPHA8, 512, 256, GL_LUMINANCE_ALPHA, NULL);
    t_mix = texture(GL_LUMINANCE8_ALPHA8, 640, 480, GL_LUMINANCE_ALPHA, NULL);
    t_band = texture(GL_LUMINANCE8_ALPHA8, 480, 1, GL_LUMINANCE_ALPHA, NULL);
    t_lut = texture(GL_RGB8, 256, 1, GL_RGB, NULL);
    t_photo = texture(GL_RGBA8, S23_PHOTO_W, 256, GL_BGRA, NULL);
    t_fog = texture(GL_LUMINANCE8, 256, 32, GL_LUMINANCE, NULL);   /* the depth fog's level table (s23_built.fogtab; texture unit 7) */
    p_glGenBuffers(1, &vbo); p_glGenBuffers(1, &quad_vbo);
    static const float quad[] = { -1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1,  -1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 1 };
    p_glBindBuffer(GL_ARRAY_BUFFER, quad_vbo); p_glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    { const char *s = getenv("S23_RES"); if (s) scale = atoi(s) < 1 ? 1 : atoi(s) > 4 ? 4 : atoi(s); }
    { const char *s = getenv("S23_WIDE"); if (s) { const float a = (float)atof(s); ex = a > 1.34f ? (int)(480 * a / 2 + 0.5f) - 320 : 0; } }   /* tests: S23_WIDE=1.7778 */
    if (!targets()) return 0;
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) { fprintf(stderr, "[GPU] GL error %04X during setup: the software picture\n", err); return 0; }
    ok = 1;
    fprintf(stderr, "[GPU] the picture is drawn on the GPU (%s, GLSL %s), internal resolution %d x %d\n", (const char *)glGetString(GL_RENDERER), sl, fbw, fbh);
    return 1;
}

/* the frame's polygons as triangles, in draw order: per vertex its clip position (x*w, y*w, 0, w with w = z, so GL interpolates u, v and
 * the shade perspective-correct exactly as the hardware's u/z, v/z, i/z), u v shade, and the polygon's parameters and viewport clip */
static int *vstart, vstart_cap;                          /* vstart[i]: the first vertex of the i-th polygon in draw order (the sprites' splits) */
static int fill_vertices(const s23_built *b)
{
    size_t need = 0;
    if (b->npoly + 1 > vstart_cap) { vstart_cap = b->npoly + 1024; vstart = realloc(vstart, (size_t)vstart_cap * sizeof *vstart); }
    for (int i = 0; i < b->npoly; i++) need += (size_t)(b->polys[i].n - 2) * 3;
    if (need * VFLOATS > vcap) { vcap = need * VFLOATS * 2 + 4096; verts = realloc(verts, vcap * sizeof *verts); }
    float *o = verts;
    for (int i = 0; i < b->npoly; i++) {
        const s23_poly *p = &b->polys[b->order[i]];
        vstart[i] = (int)((o - verts) / VFLOATS);
        const int cl = (320 - p->vp_size_x) + p->vp_offset_x, cr = (320 + p->vp_size_x) + p->vp_offset_x;
        const int ct = (240 - p->vp_size_y) - p->vp_offset_y, cb = (240 + p->vp_size_y) - p->vp_offset_y;
        /* widescreen (Hor+): a viewport that fills the 4:3 screen fills the wide one -- same camera, more world at the sides */
        const int full = ex && cl <= 0 && cr >= 640 && !p->clip43;
        const int ccl = full ? -ex : cl, ccr = full ? 640 + ex : cr;
        const float flags = (float)(p->stencil | p->blend << 1 | p->alpha_enabled << 2 | (p->prioverchar & 7) << 3 | (p->boost != 0) << 6 | p->fog << 7 | p->role << 8 | (p->model < 0) << 10);   /* bits 8-9: s23_game.depth_role; bit 10: an immediate / direct (screen-space) polygon: the 3D texture filter leaves it sharp */   /* bit 7: the author's depth fog (bit 6 is our boost) */
        /* the polygon's uv box for the bilinear filters (fs_poly): the first texel and the last one the board shows (texel floor(u) for u
         * inside the box: ceil(max) - 1, a hair of float slack below an exact integer) */
        float bu0 = 1e30f, bv0 = 1e30f, bu1 = -1e30f, bv1 = -1e30f;
        for (int k = 0; k < p->n; k++) {
            const s23_pvert *v = &p->pv[k];
            const float ooz = v->p[0] != 0 ? v->p[0] : 1e-30f, u = v->p[1] / ooz, vv = v->p[2] / ooz;
            if (u < bu0) bu0 = u; if (u > bu1) bu1 = u; if (vv < bv0) bv0 = vv; if (vv > bv1) bv1 = vv;
        }
        const float uvb[4] = { floorf(bu0 + 1e-3f), floorf(bv0 + 1e-3f), ceilf(bu1 - 1e-3f) - 1.0f, ceilf(bv1 - 1e-3f) - 1.0f };
        const float ub0 = isfinite(uvb[0]) ? uvb[0] : 0, vb0 = isfinite(uvb[1]) ? uvb[1] : 0;
        const float ub1 = !isfinite(uvb[2]) || uvb[2] < ub0 ? ub0 : uvb[2], vb1 = !isfinite(uvb[3]) || uvb[3] < vb0 ? vb0 : uvb[3];
        for (int k = 1; k + 1 < p->n; k++) {
            const int tri[3] = { 0, k, k + 1 };
            /* a triangle with a non-finite or absurd coordinate is dropped: Intel's GL driver crashed on such vertices (Gunmen Wars, in a
             * match: SIGSEGV inside igxelpicd64.dll on the next draw) */
            { int bad = 0;
              for (int j = 0; j < 3 && !bad; j++) { const s23_pvert *v = &p->pv[tri[j]]; const float ooz = v->p[0] != 0 ? v->p[0] : 1e-30f, w = 1.0f / ooz;
                  const float t[6] = { v->x * w, v->y * w, w, v->p[1] * w, v->p[2] * w, v->p[3] * w };
                  for (int q = 0; q < 6; q++) if (!isfinite(t[q]) || fabsf(t[q]) > 1e15f) bad = 1;
                  /* (a vertex a BILLION pixels off-screen: garbage. Was a million -- which dropped whole walls passing beside the camera,
                   * whose near corners project tens of millions of pixels out (Gunmen Wars: black holes in the GPU picture; the software
                   * renderer drew them). The GPU gets x * w, small for those, and clips them itself) */
                  if (fabsf(v->x) > 1e9f || fabsf(v->y) > 1e9f) bad = 1; }
              if (bad) { need -= 3; continue; } }
            for (int j = 0; j < 3; j++) {
                const s23_pvert *v = &p->pv[tri[j]];
                const float ooz = v->p[0] != 0 ? v->p[0] : 1e-30f, w = 1.0f / ooz;
                const float sx = v->x + (float)cl, sy = v->y + (float)ct;
                *o++ = ((sx + (float)ex) / (320.0f + (float)ex) - 1.0f) * w; *o++ = (sy / 240.0f - 1.0f) * w; *o++ = 0; *o++ = w;
                *o++ = v->p[1] * w; *o++ = v->p[2] * w; *o++ = v->p[3] * w;
                *o++ = (float)p->color_base; *o++ = (float)p->cmode; *o++ = (float)p->tbase; *o++ = flags;
                *o++ = (float)ccl; *o++ = (float)ct; *o++ = (float)ccr; *o++ = (float)cb;
                *o++ = (float)((p->entry & 0x3FFF) + 1); *o++ = (float)(p->zkey >> 21);
                *o++ = ub0; *o++ = vb0; *o++ = ub1; *o++ = vb1;
            }
        }
    }
    return (int)need;
}
int s23_depth_on(void); float s23_depth_keep(void);   /* src/s23_render.c: the exact-depth setting */
static void bind_tex(int unit, GLuint t) { p_glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(GL_TEXTURE_2D, t); }
static int glfin_on = -1;   /* $S23_GLFINISH (debug): wait for the GPU after each stage, so a driver fault shows at the stage that caused it */
#define GLFIN(what) do { if (glfin_on < 0) glfin_on = getenv("S23_GLFINISH") != NULL; if (glfin_on) { extern uint32_t rr_frame; fprintf(stderr, "[GLFIN] %u %s\n", rr_frame, what); fflush(stderr); glFinish(); } gl_time(what); } while (0)
/* $S23_GLTIME (debug): the GPU time of each stage (glFinish between them), averaged over 120 frames */
static int gltime_on = -1;
static void gl_time(const char *what)
{
    if (gltime_on < 0) gltime_on = getenv("S23_GLTIME") != NULL;
    if (!gltime_on) return;
    static const char *nm[16]; static double acc[16]; static int nn; static uint64_t last; static uint32_t lastf, frames;
    extern uint32_t rr_frame;
    glFinish();
    const uint64_t now = SDL_GetPerformanceCounter();
    if (rr_frame != lastf) { lastf = rr_frame; if (++frames % 120 == 0) {
        fprintf(stderr, "[GLTIME] ms/frame:"); for (int i = 0; i < nn; i++) fprintf(stderr, " %s %.2f", nm[i], acc[i] / 120.0); fprintf(stderr, "\n");
        for (int i = 0; i < nn; i++) acc[i] = 0; } last = now; return; }
    int i = 0; while (i < nn && nm[i] != what && strcmp(nm[i], what)) i++;
    if (i == nn && nn < 16) nm[nn++] = what;
    if (i < 16) acc[i] += (double)(now - last) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    last = now;
}
static void draw_quad(void)
{
    p_glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    p_glEnableVertexAttribArray(A_POS); p_glVertexAttribPointer(A_POS, 4, GL_FLOAT, GL_FALSE, 0, NULL);
    glDrawArrays(GL_TRIANGLES, 0, 6); GLFIN("quad");
    p_glDisableVertexAttribArray(A_POS);
}
/* GORgON's sprites i0..i1-1, drawn on the CPU (src/s23_render.c) and laid over what is drawn so far: only the rectangle they touch is
 * uploaded and drawn. 1280 wide: board x -320..960, so HUD sprites moved out to a widescreen picture's edges still draw. Leaves the
 * fixed-function state set; the polygon pass calls poly_state() after it */
static int spr_next;                                     /* the first sprite not drawn yet this frame */
static void spr_group(const s23_built *b, int i0, int i1)
{
    static uint32_t ovl[1280 * 480]; static GLuint t_spr; int bb[4];
    if (i0 >= i1) return;
    if (!t_spr) { t_spr = texture(GL_RGBA8, 1280, 480, GL_BGRA, NULL); memset(ovl, 0, sizeof ovl); }
    s23_sprites_draw(&b->fs, i0, i1, ovl, 1280, 320, bb);
    if (bb[0] >= bb[2] || bb[1] >= bb[3]) return;
    bind_tex(0, t_spr); glPixelStorei(GL_UNPACK_ROW_LENGTH, 1280);
    glTexSubImage2D(GL_TEXTURE_2D, 0, bb[0], bb[1], bb[2] - bb[0], bb[3] - bb[1], GL_BGRA, GL_UNSIGNED_BYTE, ovl + bb[1] * 1280 + bb[0]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (int y = bb[1]; y < bb[3]; y++) memset(ovl + y * 1280 + bb[0], 0, (size_t)(bb[2] - bb[0]) * 4);   /* clear again for the next group */
    p_glUseProgram(0); p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col); glViewport(0, 0, fbw, fbh);
    glEnable(GL_BLEND); p_glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(-ex, 640 + ex, 0, 480, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_ALPHA_TEST); glDisable(GL_LIGHTING); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); p_glActiveTexture(GL_TEXTURE0); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); glMatrixMode(GL_TEXTURE); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glEnable(GL_TEXTURE_2D); glColor4f(1, 1, 1, 1);
    const float u0 = bb[0] / 1280.f, u1 = bb[2] / 1280.f, v0 = bb[1] / 480.f, v1 = bb[3] / 480.f, x0 = (float)bb[0] - 320, x1 = (float)bb[2] - 320;
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x0, (float)bb[1]); glTexCoord2f(u1, v0); glVertex2f(x1, (float)bb[1]);
    glTexCoord2f(u1, v1); glVertex2f(x1, (float)bb[3]); glTexCoord2f(u0, v1); glVertex2f(x0, (float)bb[3]);
    glEnd();
    if (b->fx.layers & 4) {   /* the text layer goes over sprites as over polygons (session 15: "Congratulations!" under the sun): G = 1 in the priority target */
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_prio); glViewport(0, 0, fbw / scale, 480);
        glDisable(GL_BLEND); glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.f); glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE); glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE); glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PRIMARY_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE); glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
        glColor4f(0, 1, 0, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(u0, v0); glVertex2f(x0, (float)bb[1]); glTexCoord2f(u1, v0); glVertex2f(x1, (float)bb[1]);
        glTexCoord2f(u1, v1); glVertex2f(x1, (float)bb[3]); glTexCoord2f(u0, v1); glVertex2f(x0, (float)bb[3]);
        glEnd();
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDisable(GL_ALPHA_TEST); glColor4f(1, 1, 1, 1);
        glEnable(GL_BLEND);   /* the text pass after the last group blends (session 16: left off, the banners went solid over scenery) */
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col); glViewport(0, 0, fbw, fbh);
    }
    glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix();
}
static void text_pass(const s23_built *b, int pass)
{
    const s23_frame_state *fs = &b->fs;
#define C4(w) (fs->c404[2 * (w) + 1])
    p_glUseProgram(prog_text);
    p_glUniform1i(ut[0], 0); p_glUniform1i(ut[1], 1); p_glUniform1i(ut[2], 2);
    p_glUniform1i(ut[3], pass == 0 ? 1 : scale); p_glUniform1i(ut[4], pass == 0 ? 480 : fbh); p_glUniform1i(ut[5], pass);   /* pass 0: the 1x priority target */
    p_glUniform1i(ut[6], (C4(0x1A) & 2) && C4(0x19)); p_glUniform1i(ut[7], 0xFF - C4(0x19));
    p_glUniform1i(ut[8], 0xFF - C4(0x15)); p_glUniform1i(ut[9], C4(0x14)); p_glUniform1i(ut[10], s23_game.text_no_range ? 0x100 : C4(0x12)); p_glUniform1i(ut[11], C4(0x13));
    p_glUniform3i(ut[12], C4(0x16), C4(0x17), C4(0x18)); p_glUniform1i(ut[13], ex); p_glUniform1i(ut[14], 3);
    { const s23_render_list *l = &fs->list; const int n = ex > 0 ? (l->ntshift < 2 ? l->ntshift : 2) : 0;   /* the text-layer shifts */
      p_glUniform1i(ut[15], n);
      if (n > 0) { p_glUniform4i(ut[16], l->tshift[0][0], l->tshift[0][1], l->tshift[0][2], l->tshift[0][3]); p_glUniform1i(ut[18], l->tshift[0][4]); }
      if (n > 1) { p_glUniform4i(ut[17], l->tshift[1][0], l->tshift[1][1], l->tshift[1][2], l->tshift[1][3]); p_glUniform1i(ut[19], l->tshift[1][4]); }
      const int ns = ex > 0 ? (l->ntstretch < 2 ? l->ntstretch : 2) : 0;   /* the stretch bands */
      p_glUniform1i(ut[20], ns);
      if (ns > 0) p_glUniform4i(ut[21], l->tstretch[0][0], l->tstretch[0][1], l->tstretch[0][2], l->tstretch[0][3]);
      if (ns > 1) p_glUniform4i(ut[22], l->tstretch[1][0], l->tstretch[1][1], l->tstretch[1][2], l->tstretch[1][3]); }
#undef C4
    bind_tex(0, t_mix); bind_tex(1, t_pal); bind_tex(2, t_prio); bind_tex(3, t_band);
    draw_quad();
}

unsigned s23_gl_draw(const s23_built *b)
{
    if (!ok || !b) return t_out;
    { GLenum e; static int n; while ((e = glGetError()) != GL_NO_ERROR) if (n++ < 30) { extern uint32_t rr_frame; fprintf(stderr, "[GPU] GL error %04X before frame %u (npoly %d)\n", e, rr_frame, b->npoly); } }
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    const s23_fx *f = &b->fx;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    bind_tex(0, t_pal); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 128, GL_BGRA, GL_UNSIGNED_BYTE, b->fs.pens);
    bind_tex(0, t_sten); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 256, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, b->fs.sram);
    if (b->fx.fog) { glPixelStorei(GL_UNPACK_ALIGNMENT, 1); bind_tex(0, t_fog); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 32, GL_LUMINANCE, GL_UNSIGNED_BYTE, b->fogtab); }
    if (f->layers & 4) {
        bind_tex(0, t_mix); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 640, 480, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, b->mix);
        bind_tex(0, t_band); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 480, 1, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, b->band);
    }
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST);
    glViewport(0, 0, fbw, fbh);
    /* 1. the background */
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col);
    static int seam = -1; if (seam < 0) { const char *e = getenv("S23_SEAMTEST"); seam = e && *e != '0'; }   /* tests: coverage, magenta = no polygon */
    if (seam) glClearColor(1, 0, 1, 1); else glClearColor((f->bg >> 16 & 0xFF) / 255.0f, (f->bg >> 8 & 0xFF) / 255.0f, (f->bg & 0xFF) / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* THE PRIORITY TARGET IS AT BOARD RESOLUTION (it only says which text pixels -- 640 x 480 data -- go over the polygons): at a high
     * internal resolution the extra polygon draw and text pass into it were a third of the GPU time (4x: 4-8 ms + most of 12 ms) */
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_prio); glViewport(0, 0, fbw / scale, 480);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, fbw, fbh);
    glEnable(GL_BLEND);
    /* 2. the text layer, under everything (and 'text here' into the priority target) */
    if (f->layers & 4) {
        p_glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_ONE, GL_ZERO);
        glViewport(0, 0, fbw / scale, 480);
        text_pass(b, 0);
        glViewport(0, 0, fbw, fbh);
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        text_pass(b, 1);
    }
    /* 3. + 4. the polygons, one draw call each into the picture and into the priority target */
    const int nv = fill_vertices(b);
    spr_next = 0;
    { static long from = -2; if (from == -2) { const char *e = getenv("S23_GLLOG"); from = e ? atol(e) : -1; }   /* debug: per frame from <frame> */
      extern uint32_t rr_frame;
      if (from >= 0 && (long)rr_frame >= from) { float mx = 0; for (int k = 0; k < nv * VFLOATS; k++) { const float a = fabsf(verts[k]); if (!(a <= mx)) mx = a; }
          fprintf(stderr, "[GLF] frame %u npoly %d nv %d maxabs %g layers %d\n", rr_frame, b->npoly, nv, mx, b->fx.layers); fflush(stderr); } }
    if (nv > 0) {
        p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
        p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * VFLOATS * sizeof(float)), NULL, GL_STREAM_DRAW);       /* orphan: no stall */
        p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * VFLOATS * sizeof(float)), verts, GL_STREAM_DRAW);
        const GLsizei st = VFLOATS * sizeof(float);
        p_glEnableVertexAttribArray(A_POS); p_glVertexAttribPointer(A_POS, 4, GL_FLOAT, GL_FALSE, st, (void *)0);
        p_glEnableVertexAttribArray(A_UVS); p_glVertexAttribPointer(A_UVS, 3, GL_FLOAT, GL_FALSE, st, (void *)(4 * sizeof(float)));
        p_glEnableVertexAttribArray(A_PAR); p_glVertexAttribPointer(A_PAR, 4, GL_FLOAT, GL_FALSE, st, (void *)(7 * sizeof(float)));
        p_glEnableVertexAttribArray(A_CLIP); p_glVertexAttribPointer(A_CLIP, 4, GL_FLOAT, GL_FALSE, st, (void *)(11 * sizeof(float)));
        p_glEnableVertexAttribArray(A_EXT); p_glVertexAttribPointer(A_EXT, 2, GL_FLOAT, GL_FALSE, st, (void *)(15 * sizeof(float)));
        p_glEnableVertexAttribArray(A_UVB); p_glVertexAttribPointer(A_UVB, 4, GL_FLOAT, GL_FALSE, st, (void *)(17 * sizeof(float)));
        p_glUseProgram(prog_poly);
        p_glUniform1i(up[0], 0); p_glUniform1i(up[1], 1); p_glUniform1i(up[2], 2); p_glUniform1i(up[3], 3);
        p_glUniform1i(up[4], scale); p_glUniform1i(up[5], fbh);
        p_glUniform1i(up[7], f->pr || f->pg || f->pb); p_glUniform1i(up[8], (f->fflags & 1) ? 0xFF - f->ffactor : 0xFF);
        p_glUniform1i(up[9], 0xFF - f->alpha); p_glUniform1i(up[10], f->alpha_pen);
        p_glUniform3i(up[11], f->pr, f->pg, f->pb); p_glUniform3i(up[12], f->fr, f->fg, f->fb); p_glUniform1i(up[13], ex);
        bind_tex(0, t_rom); bind_tex(1, t_map); bind_tex(2, t_pal); bind_tex(3, t_sten);
        if (s23_game.photo_page && photo_gen_up != s23_photo_gen) {   /* the camera board's page, when the game changed it */
            photo_gen_up = s23_photo_gen; bind_tex(6, t_photo); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, S23_PHOTO_W, 256, GL_BGRA, GL_UNSIGNED_BYTE, s23_photo_tex); }
        bind_tex(6, t_photo); p_glUniform1i(up[19], 6); p_glUniform1i(up[20], s23_game.photo_page);
        p_glUniform1i(up[24], seam);
        { static int ef = -2, ea = -2;
          if (ef == -2) { const char *e = getenv("S23_TEX_FILTER"); ef = e && *e ? atoi(e) : -1; if (ef > 4) ef = 4; }       /* S23_TEX_FILTER=0|1|2|3|4 (2 trilinear, 3 sharp bilinear, 4 sharp trilinear), S23_TEX_ANISO=0|2|4|8|16 win for a run */
          if (ea == -2) { const char *e = getenv("S23_TEX_ANISO"); ea = e && *e ? atoi(e) : -1; }
          const int tf = ef >= 0 ? ef : g_eng_disp.tex_filter;
          const int ta = (tf == 2 || tf == 4) ? (ea >= 0 ? ea : g_eng_disp.tex_aniso) : 0;
          static int last = -1, lasta = -1;
          if (tf != last || ta != lasta) { last = tf; lasta = ta; fprintf(stderr, "[GPU] 3D texture filter: %s, anisotropy %dx\n", tf == 4 ? "sharp trilinear" : tf == 3 ? "sharp bilinear" : tf == 2 ? "trilinear" : tf ? "bilinear" : "off", ta ? ta : 1); }
          p_glUniform1i(up[25], tf); p_glUniform1i(up[26], ta); }
        bind_tex(7, t_fog); p_glUniform1i(up[21], 7); p_glUniform1i(up[22], f->fog); p_glUniform3i(up[23], f->cr, f->cg, f->cb);
        p_glUniform1i(up[14], 4); p_glUniform1i(up[15], 5); p_glUniform1i(up[16], 0);
        const int depth = (s23_depth_on() || s23_game.depth_within) && fbo_near;   /* depth_within (Rapid River): one object's own polygons only */
        p_glUniform1i(up[17], 0); p_glUniform1f(up[18], s23_depth_keep());
        if (depth) {                                     /* EXACT DEPTH pre-pass: nearest opaque object / band / 1/z per pixel */
            bind_tex(4, 0); bind_tex(5, 0);
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_near);
            glClearColor(0, 0, 0, 0); glClearDepth(0.0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_GREATER); glDepthMask(GL_TRUE);
            p_glUniform1i(up[6], 0); p_glUniform1i(up[16], 1);
            glDrawArrays(GL_TRIANGLES, 0, nv); GLFIN("depth pre-pass");
            glDisable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glClearDepth(1.0); glEnable(GL_BLEND);
            p_glUniform1i(up[16], 0); p_glUniform1i(up[17], (s23_depth_on() || s23_game.depth_within >= 2 ? 1 : 0) | (s23_game.depth_within ? 2 : 0));   /* bit 0: other objects (the menu), bit 1: within one */
            bind_tex(4, t_near); bind_tex(5, t_nearz);
        }
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col);
        p_glUniform1i(up[6], 0);
        p_glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        { int v = 0, done = 0;                           /* the sprites go in among the polygons by depth key (MAME's sort) */
          spr_next = 0;
          for (int i = 0; i < b->npoly && spr_next < b->fs.nspr; i++) {
              const s23_poly *q = &b->polys[b->order[i]];
              if (b->fs.spr[spr_next].z > (q->zkey & 0x1FFFFF)) {
                  int e = spr_next; while (e < b->fs.nspr && b->fs.spr[e].z > (q->zkey & 0x1FFFFF)) e++;
                  v = vstart[i];
                  if (v > done) glDrawArrays(GL_TRIANGLES, done, v - done);
                  done = v; spr_group(b, spr_next, e); spr_next = e;
                  p_glUseProgram(prog_poly); bind_tex(0, t_rom); p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col); glViewport(0, 0, fbw, fbh);
                  glEnable(GL_BLEND); p_glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
              }
          }
          if (nv > done) glDrawArrays(GL_TRIANGLES, done, nv - done);
          GLFIN("polygons"); }
        if (f->layers & 4) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_prio); glViewport(0, 0, fbw / scale, 480);
            p_glUniform1i(up[6], 1);
            p_glBlendEquationSeparate(GL_MAX, GL_FUNC_ADD); p_glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
            glDrawArrays(GL_TRIANGLES, 0, nv); GLFIN("priority");
            glViewport(0, 0, fbw, fbh);
            p_glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        }
        for (int a = A_POS; a <= A_UVB; a++) p_glDisableVertexAttribArray((GLuint)a);
        bind_tex(4, 0); bind_tex(5, 0); bind_tex(6, 0); bind_tex(7, 0); p_glActiveTexture(GL_TEXTURE0);
    }
    spr_group(b, spr_next, b->fs.nspr);                  /* 4b. GORgON's sprites nearer than every polygon (the rest went in among them, step 4) */
    /* 5. the text the polygons let through again */
    if (f->layers & 4) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        text_pass(b, 2);
    }
    /* 5b. a game's own solid rectangles (s23_game.list_hook: Gunmen Wars' camera screen) -- the colour target holds board row y at GL row y */
    if (b->fs.list.nrect) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col); glEnable(GL_SCISSOR_TEST);
        for (int i = 0; i < b->fs.list.nrect && i < S23_RECTS; i++) {
            const s23_rect *r = &b->fs.list.rect[i];
            glScissor((r->x0 + ex) * scale, r->y0 * scale, (r->x1 - r->x0) * scale, (r->y1 - r->y0) * scale);
            glClearColor((r->rgb >> 16 & 0xFF) / 255.0f, (r->rgb >> 8 & 0xFF) / 255.0f, (r->rgb & 0xFF) / 255.0f, 1); glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
    }
    if (b->fs.list.img_on) {                             /* 5c. and its picture */
        static GLuint t_img; if (!t_img) t_img = texture(GL_RGBA8, 128, 128, GL_BGRA, NULL);
        const s23_render_list *L = &b->fs.list;
        bind_tex(0, t_img); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_BGRA, GL_UNSIGNED_BYTE, L->img);
        p_glUseProgram(0); p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col); glViewport(0, 0, fbw, fbh); glDisable(GL_BLEND);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(-ex, 640 + ex, 0, 480, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_ALPHA_TEST); glDisable(GL_LIGHTING); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); p_glActiveTexture(GL_TEXTURE0); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); glMatrixMode(GL_TEXTURE); glLoadIdentity();   /* (fixed-function state the menu's renderer may leave set) */
        glEnable(GL_TEXTURE_2D); glColor4f(1, 1, 1, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(L->img_x0, L->img_y0); glTexCoord2f(1, 0); glVertex2f(L->img_x1, L->img_y0);
        glTexCoord2f(1, 1); glVertex2f(L->img_x1, L->img_y1); glTexCoord2f(0, 1); glVertex2f(L->img_x0, L->img_y1);
        glEnd();
        glDisable(GL_TEXTURE_2D);
        glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix();
    }
    { static int said; if (b->fs.list.nmask && !said++) fprintf(stderr, "[GPU] drawing %d caption mask(s)\n", b->fs.list.nmask); }
    if (b->fs.list.nmask) {                              /* 5d. its captions: alpha masks in their colour */
        static GLuint t_mask; if (!t_mask) { glGenTextures(1, &t_mask); bind_tex(0, t_mask); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); }
        p_glUseProgram(0); p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col); glViewport(0, 0, fbw, fbh);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(-ex, 640 + ex, 0, 480, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_ALPHA_TEST); glDisable(GL_LIGHTING); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); p_glActiveTexture(GL_TEXTURE0); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); glMatrixMode(GL_TEXTURE); glLoadIdentity();   /* (fixed-function state the menu's renderer may leave set) */
        glEnable(GL_TEXTURE_2D); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (int m = 0; m < b->fs.list.nmask && m < S23_MASKS; m++) {
            const s23_mask *k = &b->fs.list.mask[m];
            bind_tex(0, t_mask); glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, k->w, k->h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, k->a);
            glColor4f((k->rgb >> 16 & 0xFF) / 255.0f, (k->rgb >> 8 & 0xFF) / 255.0f, (k->rgb & 0xFF) / 255.0f, 1);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(k->x, k->y); glTexCoord2f(1, 0); glVertex2f(k->x + k->w, k->y);
            glTexCoord2f(1, 1); glVertex2f(k->x + k->w, k->y + k->h); glTexCoord2f(0, 1); glVertex2f(k->x, k->y + k->h);
            glEnd();
        }
        glColor4f(1, 1, 1, 1); glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND);
        glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix();
    }
    /* 6. the C404's GAMMA (the game's own R / G / B curves), into the picture the window shows */
    {
        uint8_t lut[256 * 3];
        for (int i = 0; i < 256; i++) { lut[3 * i] = b->fs.gamma[0][i]; lut[3 * i + 1] = b->fs.gamma[1][i]; lut[3 * i + 2] = b->fs.gamma[2][i]; }
        bind_tex(0, t_lut); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, lut);
        glDisable(GL_BLEND);
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_out);
        p_glUseProgram(prog_gamma); p_glUniform1i(ug[0], 0); p_glUniform1i(ug[1], 1);
        bind_tex(0, t_col); bind_tex(1, t_lut);
        draw_quad();
    }
    /* 7. the 4:3 bars fading in / out over the side strips (s23_render.c: list.pillar_fade) */
    if (ex > 0 && b->fs.list.pillar_fade) {
        p_glUseProgram(0); p_glBindBuffer(GL_ARRAY_BUFFER, 0); for (int u = 3; u >= 0; u--) bind_tex(u, 0);
        glViewport(0, 0, fbw, fbh);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, fbw, 0, fbh, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0, 0, 0, b->fs.list.pillar_fade / 255.0f);
        const float w = (float)(ex * scale);
        glBegin(GL_QUADS);
        glVertex2f(0, 0); glVertex2f(w, 0); glVertex2f(w, (float)fbh); glVertex2f(0, (float)fbh);
        glVertex2f((float)fbw - w, 0); glVertex2f((float)fbw, 0); glVertex2f((float)fbw, (float)fbh); glVertex2f((float)fbw - w, (float)fbh);
        glEnd();
        glColor4f(1, 1, 1, 1);
        glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix();
    }
    /* back to the state the window and the menu expect */
    glDisable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glUseProgram(0); p_glBindBuffer(GL_ARRAY_BUFFER, 0); p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int u = 3; u >= 0; u--) bind_tex(u, 0);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glClearColor(0, 0, 0, 1);
    return t_out;
}

/* the last picture at 640 x 480, top row first (every scale-th pixel when the internal resolution is higher) */
void s23_gl_read(uint32_t *rgba)
{
    if (!ok) { memset(rgba, 0, 640 * 480 * 4); return; }
    static uint8_t *buf; static size_t cap;
    const size_t n = (size_t)fbw * fbh * 4;
    if (n > cap) { buf = realloc(buf, n); cap = n; }
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_out);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) {
            const uint8_t *c = buf + 4 * ((size_t)(y * scale + scale / 2) * fbw + (size_t)((x + ex) * scale + scale / 2));
            rgba[y * 640 + x] = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
        }
}
/* the whole target (fbw x fbh, RGB, top row first) -- the gate's full-resolution shot */
int s23_gl_read_full(uint8_t *rgb, int cap)
{
    if (!ok || cap < fbw * fbh * 3) return 0;
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_out);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbw, fbh, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return fbw;
}
