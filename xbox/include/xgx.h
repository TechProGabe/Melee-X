/* xgx.h - the GX front end (xbox/src/sdk/gx*, game triple) and the NV2A back
 * end (xbox/src/hw/nv2a*, nxdk triple) share this state and API.
 *
 * Both triples must lay these structs out identically: only 32-bit scalars,
 * floats and byte arrays, no bit-fields, no 64-bit members. The front end
 * keeps XgxState current as the game calls GX; the back end reads it at each
 * draw and diffs it against what it last sent to the GPU. */
#ifndef XGX_H
#define XGX_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XGX_MAX_TEV 16
#define XGX_MAX_TEXGEN 8
#define XGX_MAX_MAPS 8
#define XGX_MAX_LIGHTS 8
#define XGX_NUM_POSMTX 10
#define XGX_NUM_TEXMTX 10
#define XGX_NUM_PTMTX 20

/* Logical EFB the game draws into (GameCube pixels). */
#define XGX_EFB_W 640
#define XGX_EFB_H 480

typedef struct {
    float pos[3];
    float dir[3];      /* spot: direction; specular: half-angle vector */
    float a[3];        /* angular attenuation a0 a1 a2 */
    float k[3];        /* distance attenuation k0 k1 k2 */
    uint8_t color[4];
} XgxLight;

typedef struct {
    uint32_t enable;   /* lighting on */
    uint32_t amb_src;  /* 0 register, 1 vertex */
    uint32_t mat_src;
    uint32_t light_mask;
    uint32_t diff_fn;  /* GXDiffuseFn */
    uint32_t attn_fn;  /* GXAttnFn */
} XgxChan;

typedef struct {
    uint32_t type;       /* GXTexGenType */
    uint32_t src;        /* GXTexGenSrc */
    uint32_t mtx;        /* GXTexMtx (row index 30..57, 60 identity) */
    uint32_t normalize;
    uint32_t pt_mtx;     /* GXPTTexMtx (64..123, 125 identity) */
} XgxTexGen;

typedef struct {
    uint32_t cin[4], ain[4];
    uint32_t cop, aop;             /* GXTevOp */
    uint32_t cbias, abias;         /* GXTevBias */
    uint32_t cscale, ascale;       /* GXTevScale */
    uint32_t cclamp, aclamp;
    uint32_t cout, aout;           /* GXTevRegID */
    uint32_t texcoord, texmap;     /* GXTexCoordID / GXTexMapID (0xFF: none) */
    uint32_t chan;                 /* GXChannelID */
    uint32_t kcsel, kasel;
    uint32_t ras_swap, tex_swap;
    /* indirect */
    uint32_t ind_stage, ind_format, ind_bias, ind_mtx, ind_wrap_s, ind_wrap_t;
    uint32_t ind_add_prev, ind_utc_lod, ind_alpha;
} XgxTevStage;

typedef struct {
    uint32_t tex;                   /* back-end texture handle, 0: none */
    uint32_t w, h;                  /* image size (the handle may be padded) */
    uint32_t wrap_s, wrap_t;        /* GXTexWrapMode */
    uint32_t min_filter, mag_filter;/* GXTexFilter */
    float lod_bias;
} XgxMap;

typedef struct {
    /* transform */
    float proj[4][4];
    uint32_t proj_ortho;
    float viewport[6];              /* x y w h near far, EFB pixels */
    int32_t scissor[4];             /* x y w h, EFB pixels */
    float posmtx[XGX_NUM_POSMTX][3][4];
    float nrmmtx[XGX_NUM_POSMTX][3][3];
    float texmtx[XGX_NUM_TEXMTX][3][4];
    float ptmtx[XGX_NUM_PTMTX][3][4];
    uint32_t cur_posmtx;            /* GX_PNMTX0..9 (0, 3, .. 27) */
    uint32_t posmtx_mask;           /* matrices (bit per PNMTX) loaded since the back end last read them */
    uint32_t cur_texmtx[XGX_MAX_TEXGEN];

    /* lighting: COLOR0, ALPHA0, COLOR1, ALPHA1 */
    uint32_t nchans;
    XgxChan chan[4];
    uint8_t amb[2][4];
    uint8_t mat[2][4];
    XgxLight light[XGX_MAX_LIGHTS];

    /* texgen */
    uint32_t ntexgen;
    XgxTexGen texgen[XGX_MAX_TEXGEN];

    /* TEV */
    uint32_t ntev;
    XgxTevStage tev[XGX_MAX_TEV];
    int16_t tevreg[4][4];           /* PREV REG0 REG1 REG2, S10 rgba */
    uint8_t konst[4][4];
    uint8_t swap[4][4];             /* swap tables: source channel per r g b a */
    uint32_t nind;
    uint32_t ind_order[4][2];       /* texcoord, texmap */
    uint32_t ind_scale[4][2];
    float ind_mtx[3][2][3];

    /* pixel */
    uint32_t alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;
    uint32_t blend_type, blend_src, blend_dst, blend_logic;
    uint32_t z_enable, z_func, z_update;
    uint32_t color_update, alpha_update;
    uint32_t dst_alpha_enable, dst_alpha;
    uint32_t cull;                  /* GXCullMode */
    uint32_t fog_type;
    float fog_start, fog_end, fog_near, fog_far;
    uint8_t fog_color[4];
    uint32_t dither;

    /* textures */
    XgxMap map[XGX_MAX_MAPS];

    /* front end: which groups changed since the last draw (XGX_DIRTY_*) */
    uint32_t dirty;
} XgxState;

enum {
    XGX_DIRTY_PROJ = 1u << 0,
    XGX_DIRTY_VIEWPORT = 1u << 1,
    XGX_DIRTY_POSMTX = 1u << 2,
    XGX_DIRTY_TEXMTX = 1u << 3,
    XGX_DIRTY_LIGHTS = 1u << 4,
    XGX_DIRTY_CHANS = 1u << 5,
    XGX_DIRTY_TEXGEN = 1u << 6,
    XGX_DIRTY_TEV = 1u << 7,
    XGX_DIRTY_TEVREG = 1u << 8,
    XGX_DIRTY_PIXEL = 1u << 9,
    XGX_DIRTY_FOG = 1u << 10,
    XGX_DIRTY_MAPS = 1u << 11,
    XGX_DIRTY_SCISSOR = 1u << 12,
    XGX_DIRTY_ALL = 0xFFFFFFFFu,
};

/* Canonical vertex, built by the front end from GX attributes. Only the
 * attributes present in the draw are written; offsets are bytes into the
 * vertex, -1 when absent. pos is 3 floats, nrm 3 floats (a GX normal), col
 * 4 bytes RGBA, mtx 1 float (the PNMTXIDX row, 0..27), tc 2 floats. */
typedef struct {
    uint32_t stride;
    int32_t off_pos, off_nrm, off_col[2], off_mtx;
    int32_t off_tc[8];
} XgxLayout;

/* primitives (GXPrimitive values) */
enum {
    XGX_QUADS = 0x80, XGX_TRIANGLES = 0x90, XGX_TRISTRIP = 0x98, XGX_TRIFAN = 0xA0,
    XGX_LINES = 0xA8, XGX_LINESTRIP = 0xB0, XGX_POINTS = 0xB8,
};

/* ---- back end (hw) ---- */
int xgx_init(void);
/* Vertex space for up to `count` vertices of `stride` bytes; the pointer
 * stays valid until the matching xgx_draw. NULL: too many (split the draw). */
void* xgx_vtx_alloc(uint32_t count, uint32_t stride);
void xgx_draw(uint32_t prim, uint32_t count, const XgxLayout* layout, XgxState* st);
/* Clear the current EFB region (logical rect) to the given colour / depth. */
void xgx_clear(const int32_t rect[4], const uint8_t rgba[4], uint32_t z24, int color, int alpha, int depth);
/* End the frame and flip; black: output black (VISetBlack). */
void xgx_present(int black);
/* Frames presented so far (the watchdog's heartbeat line). */
unsigned xgx_present_count(void);
/* One [FBDUMP] screenshot of the next presented frame. */
void xgx_fbdump_next(void);

/* Texture data formats handed to the back end. All but DXT1 are rows top to
 * bottom (the back end swizzles); DXT1 is 4x4 blocks in rows. Level 0 then
 * the smaller mip levels, each level's data following the previous one. */
enum {
    XGX_TEX_ARGB8 = 0,   /* uint32 A8R8G8B8; any size (NPOT is resampled) */
    XGX_TEX_RGB565,      /* uint16; power-of-two only */
    XGX_TEX_AY8,         /* uint8 intensity = alpha = luminance (GX I4/I8) */
    XGX_TEX_A8Y8,        /* uint8 pairs: luminance, alpha (GX IA4/IA8) */
    XGX_TEX_DXT1,        /* DXT1 blocks (GX CMPR, reordered and byte-swapped) */
};
/* Returns a handle or 0 when the pool is full. */
uint32_t xgx_tex_create(uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt, const void* data);
void xgx_tex_destroy(uint32_t tex);   /* deferred until the GPU is done */
uint32_t xgx_tex_pool_free_kb(void);
/* EFB -> texture: copies the logical rect into a texture of dst_w x dst_h
 * (point-sampled). intensity: convert to grey (I4/I8/IA copies).
 * reuse: the texture the previous copy to the same destination made; when it
 * has the same size it is refilled in place (no allocation, no deferred free)
 * and returned. Otherwise a new texture is made. The copy is read at the next
 * power-of-two size, so it is never resampled. */
uint32_t xgx_tex_from_efb(const int32_t src[4], uint32_t dst_w, uint32_t dst_h, int intensity, uint32_t reuse);
/* EFB -> CPU (GXCopyTex into memory the game reads): logical rect, RGBA8 out */
void xgx_read_efb(const int32_t src[4], uint32_t dst_w, uint32_t dst_h, uint8_t* rgba);

/* Output geometry: the framebuffer and the part of it the 640x480 logical EFB
 * maps onto (the content rect: the full screen, or pillarboxed). */
void xgx_output_size(uint32_t* fb_w, uint32_t* fb_h);
void xgx_set_content_aspect(float display_aspect);
void xgx_content_size(uint32_t* w, uint32_t* h);   /* in display-aspect pixels */

#ifdef __cplusplus
}
#endif
#endif
