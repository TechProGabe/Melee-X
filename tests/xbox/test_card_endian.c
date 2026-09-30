/* test_card_endian.c - host check of card_endian.c, the byte order of
 * Melee's memory-card files: the field tables tile GmSaveData and the
 * name-tag bank, converting there and back is the identity, a big-endian
 * image built by hand from offsetof reads back with the right values, and
 * an older Melee-X (little-endian) file is recognised and left alone.
 * Built and run by tools/xbox/test_card_endian.py, which #includes
 * card_endian.c before this file. Neither struct has a float member, so
 * there is no float to check; the 64-bit fields (x1A68, item_mask) are. */
#include <stdarg.h>
#include <stdlib.h>

static int s_fail;
static char s_log[4096];

void xhw_logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_log, sizeof s_log, fmt, ap);
    va_end(ap);
    printf("  log: %s\n", s_log);
}

#define CHECK(c) \
    do { \
        if (!(c)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            s_fail++; \
        } \
    } while (0)

static void be16(u8* p, u16 v) { p[0] = (u8) (v >> 8), p[1] = (u8) v; }
static void be32(u8* p, u32 v) { be16(p, (u16) (v >> 16)), be16(p + 2, (u16) v); }
static void be64(u8* p, u64 v) { be32(p, (u32) (v >> 32)), be32(p + 4, (u32) v); }

/* Every byte of the struct lies in exactly one leaf row. */
static void mark(const XsdkCardTable* t, u32 base, u8* hits)
{
    u32 i, j, k;
    for (i = 0; i < t->count; i++) {
        const XsdkCardRow* r = &t->rows[i];
        if (r->kind == XSDK_CARD_STRUCT) {
            for (j = 0; j < r->size; j += r->sub->size)
                mark(r->sub, base + r->offset + j, hits);
        } else {
            for (k = 0; k < r->size; k++)
                hits[base + r->offset + k]++;
        }
    }
}

static void test_tiling(int type, u32 size)
{
    const XsdkCardTable* t = xsdk_card_table(type);
    u8* hits = calloc(size, 1);
    u32 i, bad = 0;
    CHECK(t != NULL && t->size == size);
    CHECK(xsdk_card_check(t) == NULL);
    mark(t, 0, hits);
    for (i = 0; i < size; i++)
        bad += hits[i] != 1;
    if (bad)
        printf("  %s: %u bytes covered 0 or 2+ times\n", t->name, bad);
    CHECK(bad == 0);
    free(hits);
}

/* The leaf row holding byte `off`, and the element size it swaps in. */
static const XsdkCardRow* leaf(const XsdkCardTable* t, u32 off)
{
    u32 i;
    for (i = 0; i < t->count; i++) {
        const XsdkCardRow* r = &t->rows[i];
        if (off >= r->offset && off < (u32) r->offset + r->size) {
            if (r->kind == XSDK_CARD_STRUCT)
                return leaf(r->sub, (off - r->offset) % r->sub->size);
            return r;
        }
    }
    return NULL;
}

static int unit_at(int type, u32 off)
{
    const XsdkCardRow* r = leaf(xsdk_card_table(type), off);
    return r == NULL ? -1 : r->kind == XSDK_CARD_BYTES ? 1 : r->unit;
}

static void test_units(void)
{
    enum { S = XSDK_CARD_SAVE_DATA, N = XSDK_CARD_NAME_TAGS };
    CHECK(unit_at(S, offsetof(GmSaveData, trophy_count)) == 2);
    CHECK(unit_at(S, offsetof(GmSaveData, trophy_flags[292]) + 1) == 2);
    CHECK(unit_at(S, offsetof(GmSaveData, x1A68) + 5) == 8);
    CHECK(unit_at(S, offsetof(GmSaveData, x1CB0.item_mask)) == 8);
    CHECK(unit_at(S, offsetof(GmSaveData, x1CB0.stage_mask)) == 4);
    CHECK(unit_at(S, offsetof(GmSaveData, x1CB0.saved_language)) == 1);
    CHECK(unit_at(S, offsetof(GmSaveData, unk_30.x18[24])) == 2);
    CHECK(unit_at(S, offsetof(GmSaveData, unk_30.x114[24]) + 3) == 4);
    CHECK(unit_at(S, offsetof(GmSaveData, x1F2C[24].stats.coins_lost)) == 4);
    CHECK(unit_at(S, offsetof(GmSaveData, x1F2C[24].x7C.xA8)) == 4);
    CHECK(unit_at(S, offsetof(GmSaveData, x1F2C[0].x7A)) == 1);
    CHECK(leaf(xsdk_card_table(S), offsetof(GmSaveData, x1F2C[5].x7C) + 1)->kind == XSDK_CARD_BITS16);
    CHECK(unit_at(N, offsetof(struct NameTagDataBank, inner[18].play_time_by_fighter[24])) == 4);
    CHECK(unit_at(N, offsetof(struct NameTagDataBank, inner[3].vs_kos[119])) == 2);
    CHECK(unit_at(N, offsetof(struct NameTagDataBank, inner[3].namedata[1])) == 1);
}

/* The widths in x7c_bits against the real declaration: each field set to
 * all ones alone lands where the table says, least significant first. */
static void test_bitfield_widths(void)
{
    struct FighterData f;
    u16 got[10], want, raw;
    int i, low = 0;
    memset(&f, 0, sizeof f), f.x7C.b0 = 1, memcpy(&got[0], &f.x7C, 2);
    memset(&f, 0, sizeof f), f.x7C.b1 = 1, memcpy(&got[1], &f.x7C, 2);
    memset(&f, 0, sizeof f), f.x7C.b2 = 1, memcpy(&got[2], &f.x7C, 2);
    memset(&f, 0, sizeof f), f.x7C.b3 = 1, memcpy(&got[3], &f.x7C, 2);
    memset(&f, 0, sizeof f), f.x7C.b4 = 1, memcpy(&got[4], &f.x7C, 2);
    memset(&f, 0, sizeof f), f.x7C.b5 = 1, memcpy(&got[5], &f.x7C, 2);
    memset(&f, 0, sizeof f), f.x7C.b6 = 1, memcpy(&got[6], &f.x7C, 2);
    memset(&f, 0, sizeof f), f.x7C.b789 = 7, memcpy(&got[7], &f.x7C, 2);
    memset(&f, 0, sizeof f), f.x7C.b10_to_12 = 7, memcpy(&got[8], &f.x7C, 2);
    memset(&f, 0, sizeof f), f.x7C.b13_to_15 = 7, memcpy(&got[9], &f.x7C, 2);
    for (i = 0; x7c_bits[i]; i++) {
        want = (u16) (((1u << x7c_bits[i]) - 1) << low);
        raw = got[i];
        if (raw != want)
            printf("  x7C field %d: native bits %04x, table says %04x\n", i, raw, want);
        CHECK(raw == want);
        low += x7c_bits[i];
    }
    CHECK(i == 10 && low == 16);
}

static void test_round_trip(int type, u32 size)
{
    const XsdkCardTable* t = xsdk_card_table(type);
    u8* a = malloc(size);
    u8* b = malloc(size);
    u32 i, seed = 12345;
    for (i = 0; i < size; i++) {
        seed = seed * 1103515245u + 12345u;
        a[i] = (u8) (seed >> 16);
    }
    memcpy(b, a, size);
    xsdk_card_swap(t, b, 1);
    CHECK(memcmp(a, b, size) != 0);
    xsdk_card_swap(t, b, 0);
    CHECK(memcmp(a, b, size) == 0);
    xsdk_card_swap(t, b, 0);
    xsdk_card_swap(t, b, 1);
    CHECK(memcmp(a, b, size) == 0);
    free(a);
    free(b);
}

#define AT(img, T, field) ((img) + offsetof(T, field))

/* A save as a GameCube, Dolphin or this port (now) writes it. */
static void build_save_be(u8* img)
{
    u8* f3 = AT(img, GmSaveData, x1F2C[3]);
    memset(img, 0, sizeof(GmSaveData));
    be16(AT(img, GmSaveData, unlocked_characters), 0x07FF);
    img[offsetof(GmSaveData, x186C)] = 0x0F;
    be32(AT(img, GmSaveData, unk_30.x14), 123456);
    be16(AT(img, GmSaveData, unk_30.x18[9]), 777);
    be32(AT(img, GmSaveData, time_matches), 1234);
    be32(AT(img, GmSaveData, stock_matches), 56789);
    be32(AT(img, GmSaveData, match_resets), 3);
    be32(AT(img, GmSaveData, x1A48), 3600 * 60);
    be64(AT(img, GmSaveData, x1A68), 0x0123456789ABCDEFull);
    be32(AT(img, GmSaveData, x1B58[0]), 0x80000001);
    be32(AT(img, GmSaveData, x1B58[9]), 0x0000001F);
    be64(AT(img, GmSaveData, x1CB0.item_mask), 0x0000000100000003ull);
    img[offsetof(GmSaveData, x1CB0.sound_balance)] = (u8) -5;
    img[offsetof(GmSaveData, x1CB0.saved_language)] = 1;
    be32(AT(img, GmSaveData, x1CB0.stage_mask), 0x001FFFFF);
    be16(AT(img, GmSaveData, trophy_count), 549);
    be16(AT(img, GmSaveData, trophy_category_flags), 0x0080);
    be16(AT(img, GmSaveData, trophy_flags[0]), 0xC001);
    be16(AT(img, GmSaveData, trophy_flags[1]), 0x8003);
    be16(AT(img, GmSaveData, trophy_flags[292]), 0x8002);
    be16(f3 + offsetof(struct FighterData, fighter_kos[7]), 300);
    be16(f3 + offsetof(struct FighterData, stats.match_count), 42);
    be32(f3 + offsetof(struct FighterData, stats.play_time), 987654);
    be32(f3 + offsetof(struct FighterData, stats.walk_distance), 65536);
    f3[offsetof(struct FighterData, x7A)] = 0x80;   /* UnkFlagStruct b0, GameCube bit order */
    /* x7C's bit-field unit as MWCC lays it out: b0 at bit 15 ... b4 = 1,
     * b789 = 5, b13_to_15 = 2 */
    be16(f3 + offsetof(struct FighterData, x7C), (u16) (1 << 11 | 5 << 6 | 2));
    be16(f3 + offsetof(struct FighterData, x7C.x7E), 5);
    be32(f3 + offsetof(struct FighterData, x7C.xA4), (u32) -7);
}

static void check_save_native(const GmSaveData* s)
{
    const struct FighterData* f3 = &s->x1F2C[3];
    CHECK(s->unlocked_characters == 0x07FF);
    CHECK(s->x186C == 0x0F);
    CHECK(s->unk_30.x14 == 123456);
    CHECK(s->unk_30.x18[9] == 777);
    CHECK(s->time_matches == 1234);
    CHECK(s->stock_matches == 56789);
    CHECK(s->match_resets == 3);
    CHECK(s->x1A48 == 3600 * 60);
    CHECK(s->x1A68 == 0x0123456789ABCDEFll);
    CHECK(s->x1B58[0] == 0x80000001 && s->x1B58[9] == 0x1F && s->x1B58[1] == 0);
    CHECK(s->x1CB0.item_mask == 0x0000000100000003ull);
    CHECK(s->x1CB0.sound_balance == -5);
    CHECK(s->x1CB0.saved_language == 1);
    CHECK(s->x1CB0.stage_mask == 0x001FFFFF);
    CHECK(s->trophy_count == 549);
    CHECK(s->trophy_category_flags == 0x80);
    CHECK(s->trophy_flags[0] == 0xC001 && s->trophy_flags[1] == 0x8003 && s->trophy_flags[292] == 0x8002);
    CHECK((u8) s->trophy_flags[1] == 3);   /* the count, as Toy_SetUnlockState reads it */
    CHECK(f3->fighter_kos[7] == 300);
    CHECK(f3->stats.match_count == 42);
    CHECK(f3->stats.play_time == 987654);
    CHECK(f3->stats.walk_distance == 65536);
    CHECK(f3->x7A.byte == 0x80);
    CHECK(f3->x7C.b0 == 0 && f3->x7C.b3 == 0 && f3->x7C.b4 == 1 && f3->x7C.b5 == 0);
    CHECK(f3->x7C.b789 == 5 && f3->x7C.b10_to_12 == 0 && f3->x7C.b13_to_15 == 2);
    CHECK(f3->x7C.x7E == 5);
    CHECK(f3->x7C.xA4 == -7);
    CHECK(s->x1F2C[2].stats.match_count == 0);
}

static void test_save(void)
{
    static u8 img[sizeof(GmSaveData)], card[sizeof(GmSaveData)], old[sizeof(GmSaveData)];
    GmSaveData s;

    /* big-endian: converted, and written back byte for byte */
    build_save_be(img);
    memcpy(&s, img, sizeof s);
    CHECK(xsdk_card_from_card(XSDK_CARD_SAVE_DATA, &s, 1) == 0);
    check_save_native(&s);
    xsdk_card_to_card(XSDK_CARD_SAVE_DATA, card, &s);
    CHECK(memcmp(card, img, sizeof img) == 0);

    /* little-endian, as Melee-X wrote it before: recognised, left as is */
    memcpy(old, &s, sizeof s);
    CHECK(xsdk_card_from_card(XSDK_CARD_SAVE_DATA, &s, 1) == 1);
    CHECK(memcmp(&s, old, sizeof s) == 0);
    check_save_native(&s);

    /* a Dolphin save that the old build loaded and saved again: the fields
     * it rewrote (here trophy_count) went back little-endian. The majority
     * decides; that field stays wrong. */
    build_save_be(img);
    img[offsetof(GmSaveData, trophy_count)] = 0x25, img[offsetof(GmSaveData, trophy_count) + 1] = 0x02;
    memcpy(&s, img, sizeof s);
    CHECK(xsdk_card_from_card(XSDK_CARD_SAVE_DATA, &s, 1) == 0);
    CHECK(s.time_matches == 1234 && s.trophy_flags[292] == 0x8002);

    /* a new, empty save has no evidence and is taken as big-endian */
    memset(&s, 0, sizeof s);
    CHECK(xsdk_card_from_card(XSDK_CARD_SAVE_DATA, &s, 1) == 0);
    memset(img, 0, sizeof img);
    CHECK(memcmp(&s, img, sizeof s) == 0);
}

static void test_name_tags(void)
{
    static struct NameTagDataBank bank, back;
    static u8 img[sizeof(struct NameTagDataBank)], card[sizeof(struct NameTagDataBank)];
    u8* e = AT(img, struct NameTagDataBank, inner[4]);
    memset(img, 0, sizeof img);
    be16(e + offsetof(struct NameTagData, vs_kos[0]), 12);
    be16(e + offsetof(struct NameTagData, vs_kos[119]), 1);
    be16(e + offsetof(struct NameTagData, stats.victories), 250);
    be32(e + offsetof(struct NameTagData, stats.play_time), 216000);
    be32(e + offsetof(struct NameTagData, play_time_by_fighter[24]), 5000);
    memcpy(e + offsetof(struct NameTagData, namedata), "\x82\x60\x82\x61\x82\x62\0\0", 8);
    e[offsetof(struct NameTagData, rumble_enabled)] = 1;
    memcpy(&bank, img, sizeof img);
    CHECK(xsdk_card_from_card(XSDK_CARD_NAME_TAGS, &bank, 2) == 0);
    CHECK(bank.inner[4].vs_kos[0] == 12 && bank.inner[4].vs_kos[119] == 1);
    CHECK(bank.inner[4].stats.victories == 250);
    CHECK(bank.inner[4].stats.play_time == 216000);
    CHECK(bank.inner[4].play_time_by_fighter[24] == 5000);
    CHECK(memcmp(bank.inner[4].namedata, "\x82\x60\x82\x61\x82\x62", 6) == 0);
    CHECK(bank.inner[4].rumble_enabled == 1);
    xsdk_card_to_card(XSDK_CARD_NAME_TAGS, card, &bank);
    CHECK(memcmp(card, img, sizeof img) == 0);
    back = bank;
    CHECK(xsdk_card_from_card(XSDK_CARD_NAME_TAGS, &bank, 2) == 1);
    CHECK(memcmp(&bank, &back, sizeof bank) == 0);
}

int main(void)
{
    test_tiling(XSDK_CARD_SAVE_DATA, sizeof(GmSaveData));
    test_tiling(XSDK_CARD_NAME_TAGS, sizeof(struct NameTagDataBank));
    CHECK(xsdk_card_table(2) == NULL);
    test_units();
    test_bitfield_widths();
    test_round_trip(XSDK_CARD_SAVE_DATA, sizeof(GmSaveData));
    test_round_trip(XSDK_CARD_NAME_TAGS, sizeof(struct NameTagDataBank));
    test_save();
    test_name_tags();
    if (s_fail) {
        printf("test_card_endian: %d failures\n", s_fail);
        return 1;
    }
    printf("test_card_endian: ok\n");
    return 0;
}
