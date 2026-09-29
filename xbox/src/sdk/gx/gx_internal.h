/* gx_internal.h - GX front end state shared by the xbox/src/sdk/gx files. */
#ifndef XSDK_GX_INTERNAL_H
#define XSDK_GX_INTERNAL_H
#include <dolphin/gx.h>
#include <stdbool.h>

#include "xgx.h"

extern XgxState g_xgx;

/* vertex descriptor and attribute formats */
typedef struct {
    uint8_t cnt, type, frac;
} GxAttrFmt;

typedef struct {
    uint8_t desc[GX_VA_MAX_ATTR];            /* GXAttrType per attribute */
    GxAttrFmt vat[8][GX_VA_MAX_ATTR];        /* per GXVtxFmt */
    const uint8_t* array[GX_VA_MAX_ATTR];    /* GXSetArray base */
    uint32_t array_stride[GX_VA_MAX_ATTR];
    uint8_t array_le[GX_VA_MAX_ATTR];        /* 1: native (game-built) data */
    /* copy / clear */
    GXColor clear_color;
    uint32_t clear_z;
    int32_t tex_copy_src[4];
    uint32_t tex_copy_w, tex_copy_h, tex_copy_fmt, tex_copy_mip;
    int32_t disp_copy_src[4];
    int frame_has_copy;
} GxFront;

extern GxFront g_gx;

void gx_vtx_reset(void);
void gx_vtx_flush(void);   /* submit a pending GXBegin batch */
void gx_tex_bind(uint32_t map, const GXTexObj* obj);
void gx_tex_frame_end(void);
void gx_tex_invalidate_all(void);
void gx_tex_note_efb_copy(const void* dest, uint32_t tex, uint32_t w, uint32_t h, uint32_t fmt);
void gx_tex_init(void);

/* Big-endian readers for disc data (display lists, vertex arrays, textures). */
static inline uint16_t gx_be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t gx_be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline float gx_bef(const uint8_t* p) {
    union { uint32_t u; float f; } v;
    v.u = gx_be32(p);
    return v.f;
}

#endif
