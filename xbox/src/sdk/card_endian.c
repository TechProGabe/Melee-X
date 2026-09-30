/* card_endian.c - Melee's memory-card files in the GameCube's byte order.
 *
 * The save data (GmSaveData) and the seven name-tag banks are stored
 * big-endian on a GameCube card and in Dolphin's .gci files. HSD's card layer
 * moves them as bytes, so on this little-endian port they have to be
 * converted at the card boundary (src/melee/lb/lbcardgame.c): a file is
 * converted in place right after it is read, and a create or write goes out
 * from a big-endian copy. HSD's block checksums are made and checked on the
 * bytes the card holds, so they match the GameCube's.
 *
 * Each struct is a table of rows, one per member in declaration order, with
 * the compiler's padding as rows of their own, so every byte is listed. The
 * rows come from offsetof/sizeof of the game's structs (<melee/gm/types.h>),
 * a table that doesn't add up to sizeof its struct stops the build, and
 * tests/xbox/test_card_endian.c checks that the rows tile it in order.
 *
 * Files written by Melee-X before this conversion are little-endian. They
 * are recognised by a vote over the fields (vote below) and read as they
 * are; the game's next save writes them big-endian. */
#include <Runtime/platform.h>
#include <melee/gm/types.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "card_endian.h"
#include "xhw.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "card_endian.c converts between big-endian and a little-endian host"
#endif

#define N_ROWS(a) (sizeof(a) / sizeof((a)[0]))
#define MSIZE(T, m) sizeof(((T*) 0)->m)
#define MEND(T, m) (offsetof(T, m) + MSIZE(T, m))

/* Row lists are X-macros: M##_FIELD etc. become ROW_* (the row) and SUM_*
 * (its size, for the compile-time total). */
#define ROW_FIELD(T, m, kind, unit) \
    { offsetof(T, m), MSIZE(T, m), XSDK_CARD_##kind, unit, NULL, NULL },
#define ROW_STRUCT(T, m, sub) \
    { offsetof(T, m), MSIZE(T, m), XSDK_CARD_STRUCT, 0, &sub, NULL },
#define ROW_GAP(T, a, b) \
    { MEND(T, a), offsetof(T, b) - MEND(T, a), XSDK_CARD_BYTES, 1, NULL, NULL },
#define ROW_TAIL(T, a) \
    { MEND(T, a), sizeof(T) - MEND(T, a), XSDK_CARD_BYTES, 1, NULL, NULL },
#define ROW_BITS16(T, off, widths) \
    { off, 2, XSDK_CARD_BITS16, 2, NULL, widths },
#define SUM_FIELD(T, m, kind, unit) +MSIZE(T, m)
#define SUM_STRUCT(T, m, sub) +MSIZE(T, m)
#define SUM_GAP(T, a, b) +(offsetof(T, b) - MEND(T, a))
#define SUM_TAIL(T, a) +(sizeof(T) - MEND(T, a))
#define SUM_BITS16(T, off, widths) +2

#define TABLE(name, T, ROWS) \
    static const XsdkCardRow name##_rows[] = { ROWS(ROW, T) }; \
    _Static_assert(0 ROWS(SUM, T) == sizeof(T), #T ": its rows don't add up to sizeof"); \
    static const XsdkCardTable name = { #T, sizeof(T), name##_rows, N_ROWS(name##_rows) };

/* Match statistics, in #FighterData and #NameTagData. */
#define GMSTATS_ROWS(M, T) \
    M##_FIELD(T, sd_count, SWAP, 2) \
    M##_FIELD(T, pad_2, BYTES, 1) \
    M##_FIELD(T, attacks_hit, SWAP, 4) \
    M##_FIELD(T, attacks_total, SWAP, 4) \
    M##_FIELD(T, damage_dealt, SWAP, 4) \
    M##_FIELD(T, damage_taken, SWAP, 4) \
    M##_FIELD(T, damage_recovered, SWAP, 4) \
    M##_FIELD(T, peak_damage, SWAP, 2) \
    M##_FIELD(T, match_count, SWAP, 2) \
    M##_FIELD(T, victories, SWAP, 2) \
    M##_FIELD(T, losses, SWAP, 2) \
    M##_FIELD(T, play_time, SWAP, 4) \
    M##_FIELD(T, total_player_count, SWAP, 4) \
    M##_FIELD(T, walk_distance, SWAP, 4) \
    M##_FIELD(T, run_distance, SWAP, 4) \
    M##_FIELD(T, fall_distance, SWAP, 4) \
    M##_FIELD(T, peak_height, SWAP, 4) \
    M##_FIELD(T, coins_collected, SWAP, 4) \
    M##_FIELD(T, coins_swiped, SWAP, 4) \
    M##_FIELD(T, coins_lost, SWAP, 4)
TABLE(t_stats, struct GmStats, GMSTATS_ROWS)

#define ED98_ROWS(M, T) \
    M##_FIELD(T, x0, SWAP, 4) \
    M##_FIELD(T, x4, SWAP, 4) \
    M##_FIELD(T, padding, BYTES, 1) \
    M##_FIELD(T, xC, SWAP, 4) \
    M##_FIELD(T, x10, SWAP, 4) \
    M##_FIELD(T, x14, SWAP, 4) \
    M##_FIELD(T, x18, SWAP, 4) \
    M##_FIELD(T, x1C, SWAP, 4)
TABLE(t_ed98, struct gmm_retval_ED98, ED98_ROWS)

#define EDB0_ROWS(M, T) \
    M##_FIELD(T, x0, SWAP, 4) \
    M##_FIELD(T, x4, SWAP, 4)
TABLE(t_edb0, struct gmm_retval_EDB0, EDB0_ROWS)

/* x8 is tested as a bit set (gmmultiman.c). */
#define EDBC_ROWS(M, T) \
    M##_FIELD(T, x0, SWAP, 4) \
    M##_FIELD(T, x4, SWAP, 4) \
    M##_FIELD(T, x8, MASK, 4) \
    M##_FIELD(T, xC, SWAP, 4) \
    M##_FIELD(T, x10, SWAP, 4) \
    M##_FIELD(T, x14, SWAP, 4) \
    M##_FIELD(T, x18, SWAP, 2) \
    M##_GAP(T, x18, x4C) \
    M##_FIELD(T, x4C, SWAP, 4) \
    M##_FIELD(T, padding_x4C, BYTES, 1) \
    M##_FIELD(T, xB0, SWAP, 4) \
    M##_FIELD(T, x114, SWAP, 4)
TABLE(t_edbc, struct gmm_retval_EDBC, EDBC_ROWS)

#define X1A8_ROWS(M, T) \
    M##_FIELD(T, x0, SWAP, 4) \
    M##_FIELD(T, x4, BYTES, 1) \
    M##_FIELD(T, x5, BYTES, 1) \
    M##_FIELD(T, x6, BYTES, 1) \
    M##_TAIL(T, x6)
TABLE(t_x1a8, gmm_x1868_1A8_t, X1A8_ROWS)

#define PREFS_ROWS(M, T) \
    M##_FIELD(T, item_freq, BYTES, 1) \
    M##_GAP(T, item_freq, item_mask) \
    M##_FIELD(T, item_mask, MASK, 8) \
    M##_FIELD(T, rumble_enabled, BYTES, 1) \
    M##_FIELD(T, sound_balance, BYTES, 1) \
    M##_FIELD(T, deflicker, BYTES, 1) \
    M##_FIELD(T, saved_language, BYTES, 1) \
    M##_GAP(T, saved_language, stage_mask) \
    M##_FIELD(T, stage_mask, MASK, 4) \
    M##_TAIL(T, stage_mask)
TABLE(t_prefs, struct GamePrefs, PREFS_ROWS)

/* FighterData.x7C opens with a u16 of bit-fields: b0..b6 one bit each, then
 * three 3-bit stock fields. The GameCube (MWCC) allocates bit-fields from
 * the most significant bit, the game triple from the least, so the unit is
 * repacked, not only byte-swapped. */
static const u8 x7c_bits[] = { 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 0 };
typedef __typeof__(((struct FighterData*) 0)->x7C) FighterDataX7C;
#define X7C_ROWS(M, T) \
    M##_BITS16(T, 0, x7c_bits) \
    M##_FIELD(T, x7E, SWAP, 2) \
    M##_FIELD(T, x80, BYTES, 1) \
    M##_FIELD(T, x81, BYTES, 1) \
    M##_FIELD(T, x82, BYTES, 1) \
    M##_FIELD(T, x83, BYTES, 1) \
    M##_FIELD(T, x84, SWAP, 4) \
    M##_FIELD(T, x88, SWAP, 4) \
    M##_FIELD(T, x8C, SWAP, 4) \
    M##_FIELD(T, x90, SWAP, 4) \
    M##_FIELD(T, x94, SWAP, 4) \
    M##_FIELD(T, x98, SWAP, 4) \
    M##_FIELD(T, x9C, SWAP, 4) \
    M##_FIELD(T, xA0, SWAP, 2) \
    M##_FIELD(T, xA2, SWAP, 2) \
    M##_FIELD(T, xA4, SWAP, 4) \
    M##_FIELD(T, xA8, SWAP, 4)
TABLE(t_x7c, FighterDataX7C, X7C_ROWS)
_Static_assert(offsetof(FighterDataX7C, x7E) == 2, "x7C: one u16 of bit-fields before x7E");

/* x7A is an UnkFlagStruct, a DISC_STRUCT: already in GameCube bit order. */
#define FIGHTER_ROWS(M, T) \
    M##_FIELD(T, fighter_kos, SWAP, 2) \
    M##_FIELD(T, padding_0x32, BYTES, 1) \
    M##_STRUCT(T, stats, t_stats) \
    M##_FIELD(T, x78, BYTES, 1) \
    M##_FIELD(T, x79, BYTES, 1) \
    M##_FIELD(T, x7A, BYTES, 1) \
    M##_FIELD(T, x7B, BYTES, 1) \
    M##_STRUCT(T, x7C, t_x7c)
TABLE(t_fighter, struct FighterData, FIGHTER_ROWS)

#define SAVE_ROWS(M, T) \
    M##_FIELD(T, unlocked_characters, MASK, 2) \
    M##_FIELD(T, x186A, MASK, 2) \
    M##_FIELD(T, x186C, BYTES, 1) \
    M##_FIELD(T, pad_5, BYTES, 1) \
    M##_STRUCT(T, unk_8, t_ed98) \
    M##_STRUCT(T, unk_28, t_edb0) \
    M##_STRUCT(T, unk_30, t_edbc) \
    M##_STRUCT(T, unk_1A8, t_x1a8) \
    M##_FIELD(T, time_matches, SWAP, 4) \
    M##_FIELD(T, stock_matches, SWAP, 4) \
    M##_FIELD(T, coin_matches, SWAP, 4) \
    M##_FIELD(T, bonus_matches, SWAP, 4) \
    M##_FIELD(T, stamina_matches, SWAP, 4) \
    M##_FIELD(T, match_resets, SWAP, 4) \
    M##_FIELD(T, x1A30, SWAP, 4) \
    M##_FIELD(T, x1A34, SWAP, 4) \
    M##_FIELD(T, x1A38, SWAP, 4) \
    M##_FIELD(T, x1A3C, SWAP, 4) \
    M##_FIELD(T, x1A40, SWAP, 4) \
    M##_FIELD(T, x1A44, SWAP, 4) \
    M##_FIELD(T, x1A48, SWAP, 4) \
    M##_FIELD(T, x1A4C, SWAP, 4) \
    M##_FIELD(T, x1A50, SWAP, 4) \
    M##_FIELD(T, x1A54, SWAP, 4) \
    M##_FIELD(T, x1A58, SWAP, 4) \
    M##_FIELD(T, x1A5C, SWAP, 4) \
    M##_FIELD(T, x1A60, SWAP, 4) \
    M##_FIELD(T, x1A64, SWAP, 4) \
    M##_FIELD(T, x1A68, MASK, 8) \
    M##_FIELD(T, x1A70, SWAP, 4) \
    M##_FIELD(T, padding_x1A70, BYTES, 1) \
    M##_FIELD(T, x1B3C, BYTES, 1) \
    M##_FIELD(T, pad_2D5, BYTES, 1) \
    M##_FIELD(T, x1B40, MASK, 4) \
    M##_FIELD(T, x1B4C, MASK, 4) \
    M##_FIELD(T, x1B58, MASK, 4) \
    M##_FIELD(T, x1B80, SWAP, 4) \
    M##_FIELD(T, x1C88, MASK, 4) \
    M##_FIELD(T, padding_x1C88, BYTES, 1) \
    M##_STRUCT(T, x1CB0, t_prefs) \
    M##_FIELD(T, trophy_count, SWAP, 2) \
    M##_FIELD(T, trophy_category_flags, MASK, 2) \
    M##_FIELD(T, trophy_flags, TROPHY, 2) \
    M##_FIELD(T, padding_trophy_flags, BYTES, 1) \
    M##_STRUCT(T, x1F2C, t_fighter)
TABLE(t_save, GmSaveData, SAVE_ROWS)
_Static_assert(sizeof(GmSaveData) == 0x1790, "GmSaveData is 0x1790 bytes on the card");

#define NAMETAG_ROWS(M, T) \
    M##_FIELD(T, vs_kos, SWAP, 2) \
    M##_STRUCT(T, stats, t_stats) \
    M##_FIELD(T, play_time_by_fighter, SWAP, 4) \
    M##_FIELD(T, namedata, BYTES, 1) \
    M##_FIELD(T, x1A0, BYTES, 1) \
    M##_FIELD(T, rumble_enabled, BYTES, 1) \
    M##_FIELD(T, x1A2, BYTES, 1) \
    M##_FIELD(T, padding_x1A2, BYTES, 1)
TABLE(t_nametag, struct NameTagData, NAMETAG_ROWS)

#define BANK_ROWS(M, T) M##_STRUCT(T, inner, t_nametag)
TABLE(t_bank, struct NameTagDataBank, BANK_ROWS)
_Static_assert(sizeof(struct NameTagDataBank) == 0x1F2C, "a name-tag bank is 0x1F2C bytes on the card");

const XsdkCardTable* xsdk_card_table(int type)
{
    switch (type) {
    case XSDK_CARD_SAVE_DATA:
        return &t_save;
    case XSDK_CARD_NAME_TAGS:
        return &t_bank;
    default:
        return NULL;
    }
}

const char* xsdk_card_check(const XsdkCardTable* t)
{
    u32 at = 0, i, bits;
    const u8* w;
    for (i = 0; i < t->count; i++) {
        const XsdkCardRow* r = &t->rows[i];
        if (r->offset != at || r->size == 0)
            return t->name;   /* a gap, an overlap or rows out of order */
        switch (r->kind) {
        case XSDK_CARD_STRUCT:
            if (r->sub == NULL || r->size % r->sub->size || xsdk_card_check(r->sub))
                return r->sub ? r->sub->name : t->name;
            break;
        case XSDK_CARD_BITS16:
            for (bits = 0, w = r->bits; *w; w++) bits += *w;
            if (r->size != 2 || bits != 16)
                return t->name;
            break;
        default:
            if (r->unit == 0 || r->size % r->unit || (r->kind == XSDK_CARD_BYTES && r->unit != 1))
                return t->name;
            break;
        }
        at += r->size;
    }
    return at == t->size ? NULL : t->name;
}

static void reverse(u8* p, u32 n)
{
    u32 i;
    for (i = 0; i < n / 2; i++) {
        u8 c = p[i];
        p[i] = p[n - 1 - i];
        p[n - 1 - i] = c;
    }
}

/* A bit-field unit between the GameCube's layout (first field at the top)
 * and the game triple's (first field at the bottom). */
static u16 repack16(u16 v, const u8* widths, int to_card)
{
    u16 out = 0;
    int low = 0;
    for (; *widths; widths++) {
        int w = *widths, high = 16 - low - w;
        u16 mask = (u16) ((1u << w) - 1);
        if (to_card)
            out |= (u16) (((v >> low) & mask) << high);
        else
            out |= (u16) (((v >> high) & mask) << low);
        low += w;
    }
    return out;
}

void xsdk_card_swap(const XsdkCardTable* t, void* data, int to_card)
{
    u8* p = data;
    u32 i, j;
    for (i = 0; i < t->count; i++) {
        const XsdkCardRow* r = &t->rows[i];
        u8* f = p + r->offset;
        switch (r->kind) {
        case XSDK_CARD_BYTES:
            break;
        case XSDK_CARD_STRUCT:
            for (j = 0; j < r->size; j += r->sub->size)
                xsdk_card_swap(r->sub, f + j, to_card);
            break;
        case XSDK_CARD_BITS16:
            if (to_card) {
                u16 v = repack16((u16) (f[0] | f[1] << 8), r->bits, 1);
                f[0] = (u8) (v >> 8), f[1] = (u8) v;
            } else {
                u16 v = repack16((u16) (f[0] << 8 | f[1]), r->bits, 0);
                f[0] = (u8) v, f[1] = (u8) (v >> 8);
            }
            break;
        default:
            for (j = 0; j < r->size; j += r->unit)
                reverse(f + j, r->unit);
            break;
        }
    }
}

static u64 load(const u8* p, u32 n, int big)
{
    u64 v = 0;
    u32 i;
    for (i = 0; i < n; i++)
        v |= (u64) p[big ? i : n - 1 - i] << (8 * (n - 1 - i));
    return v;
}

/* Distance from zero in n-byte two's complement: small counts and small
 * negatives are both "small". */
static u64 magnitude(u64 v, u32 n)
{
    u64 mask = n >= 8 ? ~(u64) 0 : ((u64) 1 << (8 * n)) - 1;
    u64 neg = (0 - v) & mask;
    return v < neg ? v : neg;
}

/* Counters and records are small next to their type's range, and a small
 * value read in the wrong order isn't (0x00000123 <-> 0x23010000), so each
 * one votes for the order that makes it smaller. A trophy_flags entry is
 * 0x8000 | 0x4000 | a count in the low byte and votes for the order in
 * which bits 8-13 are clear. Bit sets and bytes don't vote; equal readings
 * (zero, 0xFFFF...) are no evidence either way. */
void xsdk_card_vote(const XsdkCardTable* t, const void* data, int* be, int* le)
{
    const u8* p = data;
    u32 i, j;
    for (i = 0; i < t->count; i++) {
        const XsdkCardRow* r = &t->rows[i];
        const u8* f = p + r->offset;
        if (r->kind == XSDK_CARD_STRUCT) {
            for (j = 0; j < r->size; j += r->sub->size)
                xsdk_card_vote(r->sub, f + j, be, le);
            continue;
        }
        if (r->kind != XSDK_CARD_SWAP && r->kind != XSDK_CARD_TROPHY)
            continue;
        for (j = 0; j < r->size; j += r->unit) {
            u64 b = load(f + j, r->unit, 1), l = load(f + j, r->unit, 0);
            if (b == l)
                continue;
            if (r->kind == XSDK_CARD_TROPHY) {
                int ok_b = (b & 0x3F00) == 0, ok_l = (l & 0x3F00) == 0;
                *be += ok_b && !ok_l;
                *le += ok_l && !ok_b;
            } else {
                u64 mb = magnitude(b, r->unit), ml = magnitude(l, r->unit);
                *be += mb < ml;
                *le += ml < mb;
            }
        }
    }
}

static int check_once(void)
{
    static int s_state;   /* 0 unchecked, 1 good, -1 broken */
    if (s_state == 0) {
        const char* bad = xsdk_card_check(&t_save);
        if (bad == NULL)
            bad = xsdk_card_check(&t_bank);
        s_state = bad ? -1 : 1;
        if (bad)
            xhw_logf("[CARD] byte-order table for %s doesn't match the struct: files are NOT converted", bad);
    }
    return s_state > 0;
}

int xsdk_card_from_card(int type, void* data, int index)
{
    const XsdkCardTable* t = xsdk_card_table(type);
    char what[24];
    int be = 0, le = 0, little, minority;
    if (t == NULL || !check_once())
        return 0;
    if (type == XSDK_CARD_SAVE_DATA)
        snprintf(what, sizeof what, "save data");
    else
        snprintf(what, sizeof what, "name tags %d", index);
    xsdk_card_vote(t, data, &be, &le);
    little = le > be;
    minority = little ? be : le;
    if (little)
        xhw_logf("[CARD] converted %s: little-endian (an older Melee-X save; votes le %d, be %d), "
                 "the next save writes it big-endian", what, le, be);
    else
        xhw_logf("[CARD] %s big-endian (votes be %d, le %d)", what, be, le);
    /* A Dolphin save that an older Melee-X loaded and saved again is mixed:
     * what the game rewrote went back little-endian. The majority wins; the
     * rewritten fields stay wrong. */
    if (minority >= 4 && minority * 8 >= be + le)
        xhw_logf("[CARD] %s looks mixed (be %d, le %d): read %s-endian, some fields may be wrong", what, be,
                 le, little ? "little" : "big");
    if (!little)
        xsdk_card_swap(t, data, 0);
    return little;
}

void xsdk_card_to_card(int type, void* dst, const void* src)
{
    const XsdkCardTable* t = xsdk_card_table(type);
    if (t == NULL)
        return;
    memcpy(dst, src, t->size);
    if (check_once())
        xsdk_card_swap(t, dst, 1);
}
