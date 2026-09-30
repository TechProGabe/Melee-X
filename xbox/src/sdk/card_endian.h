/* card_endian.h - byte order of Melee's memory-card files (card_endian.c).
 * Shared with the host test tests/xbox/test_card_endian.c; the game code
 * (src/melee/lb/lbcardgame.c) declares the two calls it makes itself. */
#ifndef CARD_ENDIAN_H
#define CARD_ENDIAN_H

#include <dolphin/types.h>

/* File types, numbered as lbcardgame.c's manifest (its file_flags). */
enum {
    XSDK_CARD_SAVE_DATA = 0,   /* GmSaveData */
    XSDK_CARD_NAME_TAGS = 1,   /* struct NameTagDataBank */
};

/* Row kinds. BYTES rows are left alone; SWAP, MASK and TROPHY rows are
 * arrays of `unit`-byte integers, reversed; they differ only in how they
 * vote on a file's byte order (card_endian.c, vote). */
enum {
    XSDK_CARD_BYTES,
    XSDK_CARD_SWAP,     /* counters and other values: the smaller reading wins */
    XSDK_CARD_MASK,     /* bit sets: no vote */
    XSDK_CARD_TROPHY,   /* trophy_flags: 0x8000 | 0x4000 | count */
    XSDK_CARD_BITS16,   /* a u16 bit-field unit, `bits` = widths in order */
    XSDK_CARD_STRUCT,   /* size / sub->size elements of `sub` */
};

typedef struct XsdkCardTable XsdkCardTable;

typedef struct {
    u16 offset, size;   /* bytes, in the struct */
    u8 kind, unit;
    const XsdkCardTable* sub;
    const u8* bits;     /* 0-terminated */
} XsdkCardRow;

struct XsdkCardTable {
    const char* name;
    u32 size;
    const XsdkCardRow* rows;
    u32 count;
};

const XsdkCardTable* xsdk_card_table(int type);

/* NULL when the rows tile the table's struct (and every sub-table) exactly,
 * in order, with each row a whole number of units; else what is wrong. */
const char* xsdk_card_check(const XsdkCardTable* t);

/* In place: to_card 1 = native -> big-endian, 0 = big-endian -> native. */
void xsdk_card_swap(const XsdkCardTable* t, void* data, int to_card);

/* Fields whose value is more plausible read big-endian / little-endian. */
void xsdk_card_vote(const XsdkCardTable* t, const void* data, int* be, int* le);

/* A file the card layer just read into `data`: native order afterwards.
 * Big-endian (GameCube, Dolphin, this port since the fix) is converted;
 * little-endian (saved by Melee-X before it) is left as it is. Logs the
 * decision; `index` is the manifest slot, for the log. Returns 1 if the
 * file was little-endian. */
int xsdk_card_from_card(int type, void* data, int index);

/* dst = src in the card's byte order (dst != src). */
void xsdk_card_to_card(int type, void* dst, const void* src);

#endif
