/* card.c - Memory Card in slot A as a folder of .gci files on the HDD:
 * E:\UDATA\<title>\card_a\01-GALE-<file name>.gci
 *
 * Each .gci is the 64-byte GameCube directory entry (big-endian) followed by
 * the file's blocks, the format Dolphin imports and exports, so saves move
 * between this port, Dolphin and a real card (via a GCI tool). Slot B is
 * empty. The card is a 251-block (16 Mbit) card.
 *
 * Every call completes immediately; async callbacks are queued and run with
 * the alarms on the game thread (os.c), as the CARD interrupt handler did. */
#include <dolphin/card.h>
#include <dolphin/os.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xhw.h"
#include "xsdk.h"

#define CHAN_A 0
#define BLOCK 8192
#define CARD_BLOCKS 251          /* user blocks on a Memory Card 251 */
#define CARD_MBITS 16
#define GCI_HEADER 0x40

typedef struct {
    int used;
    CARDDir dir;                 /* native byte order */
    u8* data;                    /* dir.length * BLOCK */
    char path[128];
} File;

static File s_files[CARD_MAX_FILE];
static int s_mounted;
static char s_game[4] = { 'G', 'A', 'L', 'E' };
static char s_maker[2] = { '0', '1' };
static s32 s_xferred;

/* ---- GCI (big-endian directory entry) ---- */
static u16 rd16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
static u32 rd32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void wr16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wr32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static void dir_from_gci(CARDDir* d, const u8* h) {
    memcpy(d->gameName, h, 4);
    memcpy(d->company, h + 4, 2);
    d->_padding0 = h[6];
    d->bannerFormat = h[7];
    memcpy(d->fileName, h + 8, CARD_FILENAME_MAX);
    d->time = rd32(h + 0x28);
    d->iconAddr = rd32(h + 0x2C);
    d->iconFormat = rd16(h + 0x30);
    d->iconSpeed = rd16(h + 0x32);
    d->permission = h[0x34];
    d->copyTimes = h[0x35];
    d->startBlock = rd16(h + 0x36);
    d->length = rd16(h + 0x38);
    d->_padding1[0] = h[0x3A];
    d->_padding1[1] = h[0x3B];
    d->commentAddr = rd32(h + 0x3C);
}

static void dir_to_gci(const CARDDir* d, u8* h) {
    memcpy(h, d->gameName, 4);
    memcpy(h + 4, d->company, 2);
    h[6] = 0xFF;
    h[7] = d->bannerFormat;
    memcpy(h + 8, d->fileName, CARD_FILENAME_MAX);
    wr32(h + 0x28, d->time);
    wr32(h + 0x2C, d->iconAddr);
    wr16(h + 0x30, d->iconFormat);
    wr16(h + 0x32, d->iconSpeed);
    h[0x34] = d->permission;
    h[0x35] = d->copyTimes;
    wr16(h + 0x36, d->startBlock);
    wr16(h + 0x38, d->length);
    h[0x3A] = h[0x3B] = 0xFF;
    wr32(h + 0x3C, d->commentAddr);
}

static void card_dir(char* out, size_t cap) { snprintf(out, cap, "%scard_a", xhw_save_dir()); }

/* FATX names are at most 42 characters */
static void gci_path(const CARDDir* d, char* out, size_t cap) {
    char name[CARD_FILENAME_MAX + 1], dirp[96];
    size_t i;
    card_dir(dirp, sizeof dirp);
    memcpy(name, d->fileName, CARD_FILENAME_MAX);
    name[CARD_FILENAME_MAX] = '\0';
    for (i = 0; name[i]; i++)
        if (strchr("\\/:*?\"<>|", name[i]) || (unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7E)
            name[i] = '_';
    if (strlen(name) > 30) {
        u32 h = 2166136261u;
        for (i = 0; i < CARD_FILENAME_MAX && d->fileName[i]; i++) h = (h ^ d->fileName[i]) * 16777619u;
        snprintf(name + 21, sizeof name - 21, "~%08x", (unsigned)h);
    }
    snprintf(out, cap, "%s\\%.2s-%.4s-%s.gci", dirp, (const char*)d->company, (const char*)d->gameName, name);
}

static int save_file(File* f) {
    u8 hdr[GCI_HEADER];
    FILE* fp;
    int ok;
    dir_to_gci(&f->dir, hdr);
    fp = fopen(f->path, "wb");
    if (!fp) {
        xhw_logf("[CARD] cannot write %s", f->path);
        return 0;
    }
    ok = fwrite(hdr, 1, GCI_HEADER, fp) == GCI_HEADER &&
         fwrite(f->data, 1, (size_t)f->dir.length * BLOCK, fp) == (size_t)f->dir.length * BLOCK;
    xhw_flush(fp);
    fclose(fp);
    xhw_logf("[CARD] %s %.32s (%u blocks)", ok ? "saved" : "FAILED to save", (const char*)f->dir.fileName,
             (unsigned)f->dir.length);
    return ok;
}

static File* alloc_file(void) {
    int i;
    for (i = 0; i < CARD_MAX_FILE; i++)
        if (!s_files[i].used) return &s_files[i];
    return NULL;
}

#if XHW_AUTOPAD
/* Test builds: .gci files staged on the disc in D:\card_a\ (MX_STAGE_EXTRA)
 * are copied to the card once, so an emulator's empty HDD starts with a save. */
static void seed_from_disc(const char* dirp) {
    void* h;
    xhw_dir_entry e;
    for (h = xhw_dir_first("D:\\card_a\\*.gci", &e); h; h = xhw_dir_next(h, &e) ? h : NULL) {
        char src[160], dst[160];
        static u8 buf[BLOCK];
        FILE *in, *out;
        size_t n;
        snprintf(dst, sizeof dst, "%s\\%s", dirp, e.name);
        if ((out = fopen(dst, "rb"))) {
            fclose(out);
            continue;
        }
        snprintf(src, sizeof src, "D:\\card_a\\%s", e.name);
        if (!(in = fopen(src, "rb"))) continue;
        if ((out = fopen(dst, "wb"))) {
            while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
            xhw_flush(out);
            fclose(out);
            xhw_logf("[CARD] seeded %s from the disc", e.name);
        }
        fclose(in);
    }
}
#endif

static void load_all(void) {
    char dirp[96], pattern[128];
    void* h;
    xhw_dir_entry e;
    card_dir(dirp, sizeof dirp);
    xhw_mkdir(dirp);
#if XHW_AUTOPAD
    seed_from_disc(dirp);
#endif
    snprintf(pattern, sizeof pattern, "%s\\*.gci", dirp);
    for (h = xhw_dir_first(pattern, &e); h; h = xhw_dir_next(h, &e) ? h : NULL) {
        char p[160];
        u8 hdr[GCI_HEADER];
        FILE* fp;
        File* f;
        snprintf(p, sizeof p, "%s\\%s", dirp, e.name);
        if (!(fp = fopen(p, "rb"))) continue;
        f = alloc_file();
        if (f && fread(hdr, 1, GCI_HEADER, fp) == GCI_HEADER) {
            dir_from_gci(&f->dir, hdr);
            f->data = (u8*)calloc(f->dir.length ? f->dir.length : 1, BLOCK);
            if (f->data && f->dir.length && fread(f->data, 1, (size_t)f->dir.length * BLOCK, fp) == (size_t)f->dir.length * BLOCK) {
                f->used = 1;
                snprintf(f->path, sizeof f->path, "%s", p);
                xhw_logf("[CARD] %.32s (%u blocks)", (const char*)f->dir.fileName, f->dir.length);
            } else {
                free(f->data);
                f->data = NULL;
            }
        }
        fclose(fp);
    }
}

static int used_blocks(void) {
    int i, n = 0;
    for (i = 0; i < CARD_MAX_FILE; i++)
        if (s_files[i].used) n += s_files[i].dir.length;
    return n;
}

static int name_matches(const File* f, const char* name) {
    return f->used && memcmp(f->dir.gameName, s_game, 4) == 0 && memcmp(f->dir.company, s_maker, 2) == 0 &&
           strncmp((const char*)f->dir.fileName, name, CARD_FILENAME_MAX) == 0;
}

static s32 find(const char* name) {
    int i;
    for (i = 0; i < CARD_MAX_FILE; i++)
        if (name_matches(&s_files[i], name)) return i;
    return -1;
}

static s32 finish(s32 chan, s32 result, CARDCallback cb) {
    if (cb) xsdk_card_dispatch(cb, chan, result);
    return CARD_RESULT_READY;
}

/* ======================================================================
 * API
 * ====================================================================== */
void CARDInit(const char* game, const char* maker) {
    if (game) memcpy(s_game, game, 4);
    if (maker) memcpy(s_maker, maker, 2);
}

void CARDSetGameAndMaker(const s32 chan, const char* game, const char* maker) {
    (void)chan;
    CARDInit(game, maker);
}

bool aurora_card_is_present(void) { return true; }

int CARDProbe(s32 chan) { return chan == CHAN_A; }

s32 CARDProbeEx(s32 chan, s32* mem_size, s32* sector_size) {
    if (chan != CHAN_A) return CARD_RESULT_NOCARD;
    if (mem_size) *mem_size = CARD_MBITS;
    if (sector_size) *sector_size = BLOCK;
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 chan, void* work, CARDCallback detach, CARDCallback attach) {
    (void)work;
    (void)detach;
    if (chan != CHAN_A) return CARD_RESULT_NOCARD;
    if (!s_mounted) {
        load_all();
        s_mounted = 1;
    }
    return finish(chan, CARD_RESULT_READY, attach);
}

s32 CARDMount(s32 chan, void* work, CARDCallback detach) { return CARDMountAsync(chan, work, detach, NULL); }
s32 CARDUnmount(s32 chan) { return chan == CHAN_A ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }
s32 CARDCheckAsync(s32 chan, CARDCallback cb) { return finish(chan, chan == CHAN_A ? CARD_RESULT_READY : CARD_RESULT_NOCARD, cb); }
s32 CARDCheck(s32 chan) { return chan == CHAN_A ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }
s32 CARDGetXferredBytes(s32 chan) { (void)chan; return s_xferred; }
s32 CARDGetResultCode(s32 chan) { return chan == CHAN_A ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }

s32 CARDFreeBlocks(s32 chan, s32* bytes, s32* files) {
    int i, n = 0;
    if (chan != CHAN_A) return CARD_RESULT_NOCARD;
    for (i = 0; i < CARD_MAX_FILE; i++) n += !s_files[i].used;
    if (bytes) *bytes = (CARD_BLOCKS - used_blocks()) * BLOCK;
    if (files) *files = n;
    return CARD_RESULT_READY;
}

s32 CARDFormatAsync(s32 chan, CARDCallback cb) {
    int i;
    if (chan != CHAN_A) return CARD_RESULT_NOCARD;
    for (i = 0; i < CARD_MAX_FILE; i++) {
        if (!s_files[i].used) continue;
        remove(s_files[i].path);
        free(s_files[i].data);
        memset(&s_files[i], 0, sizeof s_files[i]);
    }
    xhw_logf("[CARD] formatted");
    return finish(chan, CARD_RESULT_READY, cb);
}

s32 CARDFastOpen(s32 chan, s32 file_no, CARDFileInfo* fi) {
    if (chan != CHAN_A) return CARD_RESULT_NOCARD;
    if (file_no < 0 || file_no >= CARD_MAX_FILE || !s_files[file_no].used) return CARD_RESULT_NOFILE;
    fi->chan = chan;
    fi->fileNo = file_no;
    fi->offset = 0;
    fi->length = s_files[file_no].dir.length * BLOCK;
    fi->iBlock = s_files[file_no].dir.startBlock;
    return CARD_RESULT_READY;
}

s32 CARDOpen(s32 chan, const char* name, CARDFileInfo* fi) {
    s32 n;
    if (chan != CHAN_A) return CARD_RESULT_NOCARD;
    n = find(name);
    if (n < 0) return CARD_RESULT_NOFILE;
    return CARDFastOpen(chan, n, fi);
}

s32 CARDClose(CARDFileInfo* fi) {
    fi->chan = -1;
    return CARD_RESULT_READY;
}

s32 CARDCreateAsync(s32 chan, const char* name, u32 size, CARDFileInfo* fi, CARDCallback cb) {
    File* f;
    u32 blocks = (size + BLOCK - 1) / BLOCK;
    if (chan != CHAN_A) return CARD_RESULT_NOCARD;
    if (strlen(name) > CARD_FILENAME_MAX) return CARD_RESULT_NAMETOOLONG;
    if (find(name) >= 0) return finish(chan, CARD_RESULT_EXIST, cb);
    if ((int)blocks > CARD_BLOCKS - used_blocks() || !(f = alloc_file())) return finish(chan, CARD_RESULT_INSSPACE, cb);
    memset(f, 0, sizeof *f);
    memcpy(f->dir.gameName, s_game, 4);
    memcpy(f->dir.company, s_maker, 2);
    f->dir._padding0 = 0xFF;
    strncpy((char*)f->dir.fileName, name, CARD_FILENAME_MAX);
    f->dir.time = (u32)(OSGetTime() / OS_TIMER_CLOCK);
    f->dir.iconAddr = 0xFFFFFFFFu;
    f->dir.commentAddr = 0xFFFFFFFFu;
    f->dir.permission = CARD_ATTR_PUBLIC;
    f->dir.startBlock = (u16)(5 + used_blocks());
    f->dir.length = (u16)blocks;
    f->data = (u8*)calloc(blocks ? blocks : 1, BLOCK);
    if (!f->data) return finish(chan, CARD_RESULT_INSSPACE, cb);
    f->used = 1;
    gci_path(&f->dir, f->path, sizeof f->path);
    save_file(f);
    xhw_logf("[CARD] created %s (%u blocks)", name, (unsigned)blocks);
    CARDFastOpen(chan, (s32)(f - s_files), fi);
    return finish(chan, CARD_RESULT_READY, cb);
}

s32 CARDDeleteAsync(s32 chan, const char* name, CARDCallback cb) {
    s32 n;
    if (chan != CHAN_A) return CARD_RESULT_NOCARD;
    n = find(name);
    if (n < 0) return finish(chan, CARD_RESULT_NOFILE, cb);
    remove(s_files[n].path);
    free(s_files[n].data);
    memset(&s_files[n], 0, sizeof s_files[n]);
    return finish(chan, CARD_RESULT_READY, cb);
}

s32 CARDRenameAsync(s32 chan, const char* old_name, const char* new_name, CARDCallback cb) {
    s32 n;
    File* f;
    if (chan != CHAN_A) return CARD_RESULT_NOCARD;
    if (strlen(new_name) > CARD_FILENAME_MAX) return CARD_RESULT_NAMETOOLONG;
    n = find(old_name);
    if (n < 0) return finish(chan, CARD_RESULT_NOFILE, cb);
    if (find(new_name) >= 0) return finish(chan, CARD_RESULT_EXIST, cb);
    f = &s_files[n];
    remove(f->path);
    memset(f->dir.fileName, 0, CARD_FILENAME_MAX);
    strncpy((char*)f->dir.fileName, new_name, CARD_FILENAME_MAX);
    gci_path(&f->dir, f->path, sizeof f->path);
    save_file(f);
    return finish(chan, CARD_RESULT_READY, cb);
}

s32 CARDReadAsync(const CARDFileInfo* fi, void* addr, s32 length, s32 offset, CARDCallback cb) {
    File* f;
    if (fi->chan != CHAN_A || fi->fileNo < 0 || fi->fileNo >= CARD_MAX_FILE) return CARD_RESULT_NOFILE;
    f = &s_files[fi->fileNo];
    if (!f->used || offset < 0 || length < 0 || offset + length > f->dir.length * BLOCK)
        return finish(fi->chan, CARD_RESULT_LIMIT, cb);
    memcpy(addr, f->data + offset, (size_t)length);
    s_xferred = length;
    return finish(fi->chan, CARD_RESULT_READY, cb);
}

s32 CARDRead(const CARDFileInfo* fi, void* addr, s32 length, s32 offset) {
    return CARDReadAsync(fi, addr, length, offset, NULL);
}

s32 CARDWriteAsync(const CARDFileInfo* fi, const void* addr, s32 length, s32 offset, CARDCallback cb) {
    File* f;
    if (fi->chan != CHAN_A || fi->fileNo < 0 || fi->fileNo >= CARD_MAX_FILE) return CARD_RESULT_NOFILE;
    f = &s_files[fi->fileNo];
    if (!f->used || offset < 0 || length < 0 || offset + length > f->dir.length * BLOCK)
        return finish(fi->chan, CARD_RESULT_LIMIT, cb);
    memcpy(f->data + offset, addr, (size_t)length);
    f->dir.time = (u32)(OSGetTime() / OS_TIMER_CLOCK);
    s_xferred = length;
    return finish(fi->chan, save_file(f) ? CARD_RESULT_READY : CARD_RESULT_IOERROR, cb);
}

s32 CARDWrite(const CARDFileInfo* fi, const void* addr, s32 length, s32 offset) {
    return CARDWriteAsync(fi, addr, length, offset, NULL);
}

/* SDK __CARDUpdateIconOffsets: where the banner, icons and data sit in the file */
static void icon_offsets(const CARDDir* d, CARDStat* st) {
    u32 off = d->iconAddr;
    int i, tlut = 0;
    if (off == 0xFFFFFFFFu) {
        st->bannerFormat = 0;
        st->iconFormat = 0;
        st->iconSpeed = 0;
        off = 0;
    }
    switch (st->bannerFormat & 3) {
        case 1: /* C8 */
            st->offsetBanner = off;
            off += 96 * 32;
            st->offsetBannerTlut = off;
            off += 2 * 256;
            break;
        case 2: /* RGB5A3 */
            st->offsetBanner = off;
            off += 2 * 96 * 32;
            st->offsetBannerTlut = 0xFFFFFFFFu;
            break;
        default:
            st->offsetBanner = st->offsetBannerTlut = 0xFFFFFFFFu;
            break;
    }
    for (i = 0; i < CARD_ICON_MAX; i++) {
        switch ((st->iconFormat >> (2 * i)) & 3) {
            case 1:
                st->offsetIcon[i] = off;
                off += 32 * 32;
                tlut = 1;
                break;
            case 2:
                st->offsetIcon[i] = off;
                off += 2 * 32 * 32;
                break;
            default:
                st->offsetIcon[i] = 0xFFFFFFFFu;
                break;
        }
    }
    if (tlut) {
        st->offsetIconTlut = off;
        off += 2 * 256;
    } else {
        st->offsetIconTlut = 0xFFFFFFFFu;
    }
    st->offsetData = off;
}

s32 CARDGetStatus(s32 chan, s32 file_no, CARDStat* st) {
    const CARDDir* d;
    if (chan != CHAN_A) return CARD_RESULT_NOCARD;
    if (file_no < 0 || file_no >= CARD_MAX_FILE || !s_files[file_no].used) return CARD_RESULT_NOFILE;
    d = &s_files[file_no].dir;
    memset(st, 0, sizeof *st);
    memcpy(st->fileName, d->fileName, CARD_FILENAME_MAX);
    st->length = (u32)d->length * BLOCK;
    st->time = d->time;
    memcpy(st->gameName, d->gameName, 4);
    memcpy(st->company, d->company, 2);
    st->bannerFormat = d->bannerFormat;
    st->iconAddr = d->iconAddr;
    st->iconFormat = d->iconFormat;
    st->iconSpeed = d->iconSpeed;
    st->commentAddr = d->commentAddr;
    icon_offsets(d, st);
    return CARD_RESULT_READY;
}

s32 CARDSetStatusAsync(s32 chan, s32 file_no, const CARDStat* st, CARDCallback cb) {
    File* f;
    if (chan != CHAN_A) return CARD_RESULT_NOCARD;
    if (file_no < 0 || file_no >= CARD_MAX_FILE || !s_files[file_no].used) return finish(chan, CARD_RESULT_NOFILE, cb);
    f = &s_files[file_no];
    f->dir.bannerFormat = st->bannerFormat;
    f->dir.iconAddr = st->iconAddr;
    f->dir.iconFormat = st->iconFormat;
    f->dir.iconSpeed = st->iconSpeed;
    f->dir.commentAddr = st->commentAddr;
    f->dir.time = (u32)(OSGetTime() / OS_TIMER_CLOCK);
    return finish(chan, save_file(f) ? CARD_RESULT_READY : CARD_RESULT_IOERROR, cb);
}

s32 CARDSetStatus(s32 chan, s32 file_no, const CARDStat* st) { return CARDSetStatusAsync(chan, file_no, st, NULL); }
