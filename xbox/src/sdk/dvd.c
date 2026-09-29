/* dvd.c - the Dolphin DVD API over the user's disc image (.iso/.gcm/.ciso).
 *
 * The FST is read from the image at boot. Async reads run on one worker
 * thread and complete like the GameCube's DVD interrupt did: the callback
 * runs with interrupts disabled (the recursive OS lock), from that thread.
 * Synchronous reads take the same image lock on the calling thread. */
#include <dolphin/dvd.h>
#include <dolphin/os.h>
#include <aurora/dvd.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xhw.h"
#include "xsdk.h"

static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* ======================================================================
 * Image reader
 * ====================================================================== */
static FILE* s_img;
static xhw_mutex* s_img_lock;
/* CISO: block map, present blocks stored in order after 0x8000 */
static u32 s_ciso_block;
static u32* s_ciso_index;   /* block -> file offset, 0xFFFFFFFF = zero block */
static u32 s_ciso_blocks;

static int img_read_raw(u32 off, void* dst, u32 len) {
    xhw_commit(dst, len);   /* the kernel writes it: a lazy MEM1 chunk must exist first */
    if (fseek(s_img, (long)off, SEEK_SET) != 0) return 0;
    return fread(dst, 1, len, s_img) == len;
}

static int img_read(u32 off, void* dst, u32 len) {
    u8* d = (u8*)dst;
    int ok = 1;
    xhw_mutex_lock(s_img_lock);
    if (!s_ciso_index) {
        ok = img_read_raw(off, d, len);
    } else {
        while (len && ok) {
            u32 blk = off / s_ciso_block, in = off % s_ciso_block;
            u32 n = s_ciso_block - in;
            if (n > len) n = len;
            if (blk >= s_ciso_blocks || s_ciso_index[blk] == 0xFFFFFFFFu) memset(d, 0, n);
            else ok = img_read_raw(s_ciso_index[blk] + in, d, n);
            off += n;
            d += n;
            len -= n;
        }
    }
    xhw_mutex_unlock(s_img_lock);
    return ok;
}

static int img_open(const char* path) {
    u8 hdr[8];
    s_img = fopen(path, "rb");
    if (!s_img) return 0;
    if (!s_img_lock) s_img_lock = xhw_mutex_create();
    if (!img_read_raw(0, hdr, sizeof hdr)) return 0;
    if (memcmp(hdr, "CISO", 4) == 0) {
        u8* map = (u8*)malloc(0x8000 - 8);
        u32 i, pos = 0x8000;
        s_ciso_block = (u32)hdr[4] | (u32)hdr[5] << 8 | (u32)hdr[6] << 16 | (u32)hdr[7] << 24;
        s_ciso_blocks = 0x8000 - 8;
        s_ciso_index = (u32*)malloc(s_ciso_blocks * sizeof(u32));
        if (!map || !s_ciso_index || !s_ciso_block || !img_read_raw(8, map, 0x8000 - 8)) return 0;
        for (i = 0; i < s_ciso_blocks; i++) {
            s_ciso_index[i] = map[i] ? pos : 0xFFFFFFFFu;
            if (map[i]) pos += s_ciso_block;
        }
        free(map);
        xhw_logf("[DVD] CISO, %u KB blocks", s_ciso_block / 1024);
    }
    return 1;
}

/* ======================================================================
 * FST
 * ====================================================================== */
static DVDDiskID s_disk_id;
static u8* s_fst;          /* big-endian, as on the disc */
static u32 s_fst_entries;
static const char* s_fst_names;
static char s_locale_ext[4];

static int fst_is_dir(u32 i) { return s_fst[i * 12] != 0; }
static const char* fst_name(u32 i) { return s_fst_names + (be32(s_fst + i * 12) & 0x00FFFFFF); }
static u32 fst_w1(u32 i) { return be32(s_fst + i * 12 + 4); }   /* file: offset; dir: parent */
static u32 fst_w2(u32 i) { return be32(s_fst + i * 12 + 8); }   /* file: length; dir: next */

void aurora_dvd_set_locale_extension(const char* ext) {
    if (ext) snprintf(s_locale_ext, sizeof s_locale_ext, "%s", ext);
    else s_locale_ext[0] = '\0';
}

static int name_eq(const char* a, const char* b, size_t blen) {
    size_t i;
    for (i = 0; i < blen; i++)
        if (!a[i] || tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return a[blen] == '\0';
}

static s32 lookup(const char* path) {
    u32 dir = 0;
    while (*path == '/') path++;
    while (*path) {
        const char* end = strchr(path, '/');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        u32 i = dir + 1, stop = fst_w2(dir);
        int found = 0;
        while (i < stop) {
            if (name_eq(fst_name(i), path, len)) {
                found = 1;
                break;
            }
            i = fst_is_dir(i) ? fst_w2(i) : i + 1;
        }
        if (!found) return -1;
        if (!end) return (s32)i;
        if (!fst_is_dir(i)) return -1;
        dir = i;
        path = end + 1;
        while (*path == '/') path++;
    }
    return (s32)dir;
}

s32 DVDConvertPathToEntrynum(const char* path) {
    s32 e;
    if (!s_fst || !path) return -1;
    e = lookup(path);
    if (e < 0 && s_locale_ext[0]) {
        size_t n = strlen(path);
        if (n > 4 && (strcmp(path + n - 4, ".usd") == 0 || strcmp(path + n - 4, ".dat") == 0)) {
            char alt[256];
            snprintf(alt, sizeof alt, "%.*s.%s", (int)(n - 4), path, s_locale_ext);
            e = lookup(alt);
        }
    }
    return e;
}

BOOL DVDConvertEntrynumToPath(s32 entrynum, char* path, u32 maxlen) {
    if (!s_fst || entrynum < 0 || (u32)entrynum >= s_fst_entries || !maxlen) return FALSE;
    snprintf(path, maxlen, "%s", fst_name((u32)entrynum));
    return TRUE;
}

/* ======================================================================
 * Open / read
 * ====================================================================== */
BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fi) {
    if (!s_fst || entrynum < 0 || (u32)entrynum >= s_fst_entries || fst_is_dir((u32)entrynum)) return FALSE;
    memset(fi, 0, sizeof *fi);
    fi->startAddr = fst_w1((u32)entrynum);
    fi->length = fst_w2((u32)entrynum);
    fi->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDOpen(const char* name, DVDFileInfo* fi) {
    s32 e = DVDConvertPathToEntrynum(name);
    if (e < 0) {
        xhw_logf("[DVD] not on the disc: %s", name);
        return FALSE;
    }
    return DVDFastOpen(e, fi);
}

BOOL DVDClose(DVDFileInfo* fi) {
    (void)fi;
    return TRUE;
}

static s32 read_now(DVDFileInfo* fi, void* addr, s32 length, s32 offset) {
    u32 len = (u32)length;
    if (offset < 0 || (u32)offset >= fi->length) return 0;
    if ((u32)offset + len > fi->length) len = fi->length - (u32)offset;
    if (!img_read(fi->startAddr + (u32)offset, addr, len)) return DVD_RESULT_FATAL_ERROR;
    return (s32)len;
}

s32 DVDReadPrio(DVDFileInfo* fi, void* addr, s32 length, s32 offset, s32 prio) {
    s32 r;
    (void)prio;
    fi->cb.state = DVD_STATE_BUSY;
    r = read_now(fi, addr, length, offset);
    fi->cb.transferredSize = r > 0 ? (u32)r : 0;
    fi->cb.state = r < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
    return r;
}

/* ---- async queue (one worker, FIFO) ---- */
typedef struct Req {
    DVDFileInfo* fi;          /* file read (DVDReadAsyncPrio) */
    DVDCommandBlock* block;   /* or absolute read */
    void* addr;
    s32 length, offset;
    DVDCallback fcb;
    DVDCBCallback bcb;
} Req;

#define QUEUE 64
static Req s_q[QUEUE];
static volatile int s_qhead, s_qcount;
static xhw_mutex* s_qlock;
static xhw_event* s_qevent;
static volatile int s_inflight;

static void worker(void* arg) {
    (void)arg;
    for (;;) {
        Req r;
        s32 res;
        xhw_mutex_lock(s_qlock);
        if (s_qcount == 0) {
            xhw_mutex_unlock(s_qlock);
            xhw_event_wait(s_qevent, 100);
            continue;
        }
        r = s_q[s_qhead];
        xhw_mutex_unlock(s_qlock);

        if (r.fi) {
            res = read_now(r.fi, r.addr, r.length, r.offset);
        } else {
            res = img_read((u32)r.offset, r.addr, (u32)r.length) ? r.length : DVD_RESULT_FATAL_ERROR;
        }
        {
            DVDCommandBlock* cb = r.fi ? &r.fi->cb : r.block;
            BOOL intr = OSDisableInterrupts();
            cb->transferredSize = res > 0 ? (u32)res : 0;
            cb->state = res < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
            xhw_mutex_lock(s_qlock);
            s_qhead = (s_qhead + 1) % QUEUE;
            s_qcount--;
            xhw_mutex_unlock(s_qlock);
            if (r.fi && r.fcb) r.fcb(res, r.fi);
            else if (!r.fi && r.bcb) r.bcb(res, r.block);
            OSRestoreInterrupts(intr);
        }
        __atomic_sub_fetch(&s_inflight, 1, __ATOMIC_RELEASE);
    }
}

static BOOL enqueue(const Req* r) {
    int slot;
    DVDCommandBlock* cb = r->fi ? &r->fi->cb : r->block;
    for (;;) {
        xhw_mutex_lock(s_qlock);
        if (s_qcount < QUEUE) break;
        xhw_mutex_unlock(s_qlock);
        xhw_sleep_ms(1);
    }
    cb->state = DVD_STATE_BUSY;
    cb->addr = r->addr;
    cb->length = (u32)r->length;
    cb->offset = (u32)r->offset;
    slot = (s_qhead + s_qcount) % QUEUE;
    s_q[slot] = *r;
    s_qcount++;
    __atomic_add_fetch(&s_inflight, 1, __ATOMIC_RELEASE);
    xhw_mutex_unlock(s_qlock);
    xhw_event_signal(s_qevent);
    return TRUE;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fi, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio) {
    Req r;
    (void)prio;
    memset(&r, 0, sizeof r);
    r.fi = fi;
    r.addr = addr;
    r.length = length;
    r.offset = offset;
    r.fcb = callback;
    fi->callback = callback;
    return enqueue(&r);
}

int DVDReadAbsAsyncPrio(DVDCommandBlock* block, void* addr, s32 length, s32 offset, DVDCBCallback callback,
                        s32 prio) {
    Req r;
    (void)prio;
    memset(&r, 0, sizeof r);
    r.block = block;
    r.addr = addr;
    r.length = length;
    r.offset = offset;
    r.bcb = callback;
    return enqueue(&r);
}

int aurora_dvd_inflight(void) { return __atomic_load_n(&s_inflight, __ATOMIC_ACQUIRE); }

/* completions run on the worker already */
void xsdk_dvd_deliver(void) {}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) { return block->state; }
s32 DVDGetFileInfoStatus(const DVDFileInfo* fi) { return fi->cb.state; }
s32 DVDGetTransferredSize(DVDFileInfo* fi) { return (s32)fi->cb.transferredSize; }
s32 DVDGetDriveStatus(void) { return aurora_dvd_inflight() ? DVD_STATE_BUSY : DVD_STATE_END; }
BOOL DVDCheckDisk(void) { return s_img != NULL; }
DVDDiskID* DVDGetCurrentDiskID(void) { return &s_disk_id; }
BOOL DVDSetAutoInvalidation(BOOL on) { (void)on; return TRUE; }
int DVDSetAutoFatalMessaging(BOOL on) { (void)on; return 0; }
void DVDPause(void) {}
void DVDResume(void) {}
void DVDReset(void) {}
int DVDResetRequired(void) { return 0; }
void* DVDGetFSTLocation(void) { return s_fst; }

s32 DVDCancel(volatile DVDCommandBlock* block) { (void)block; return 0; }
int DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback cb) {
    if (cb) cb(0, block);
    return 1;
}

/* ---- main.dol, for the fonts lifted out of it (src/pc/discfont.c) ---- */
static u8* s_dol;
static s32 s_dol_size;

const u8* DVDGetDOLLocation(s32* out_size) {
    if (!s_dol && s_img) {
        u8 hdr[0x440];
        u32 dol, fst;
        if (img_read(0, hdr, sizeof hdr)) {
            dol = be32(hdr + 0x420);
            fst = be32(hdr + 0x424);
            if (fst > dol && fst - dol <= (8u << 20) && (s_dol = (u8*)malloc(fst - dol)) != NULL) {
                s_dol_size = (s32)(fst - dol);
                if (!img_read(dol, s_dol, (u32)s_dol_size)) {
                    free(s_dol);
                    s_dol = NULL;
                }
            }
        }
    }
    if (out_size) *out_size = s_dol ? s_dol_size : 0;
    return s_dol;
}

void xsdk_dvd_free_dol(void) {
    free(s_dol);
    s_dol = NULL;
}

/* ======================================================================
 * Boot
 * ====================================================================== */
int xsdk_dvd_open(const char* path, char* why, size_t why_cap) {
    u8 hdr[0x440];
    u32 fst_off, fst_len;
    if (!img_open(path)) {
        snprintf(why, why_cap, "The disc image could not be opened.");
        return 0;
    }
    if (!img_read(0, hdr, sizeof hdr)) {
        snprintf(why, why_cap, "The disc image is too short.");
        return 0;
    }
    if (memcmp(hdr, "GALE01", 6) != 0) {
        snprintf(why, why_cap, "This is not Melee NTSC-U (GALE01): the image says %.6s.", (const char*)hdr);
        return 0;
    }
    if (hdr[7] != 2) {
        snprintf(why, why_cap, "This is Melee revision 1.0%u; Melee-X needs 1.02.", hdr[7]);
        return 0;
    }
    memcpy(&s_disk_id, hdr, sizeof s_disk_id);
    xsdk_fill_disc_id(hdr);
    fst_off = be32(hdr + 0x424);
    fst_len = be32(hdr + 0x428);
    if (!fst_len || fst_len > (4u << 20) || !(s_fst = (u8*)malloc(fst_len)) || !img_read(fst_off, s_fst, fst_len)) {
        snprintf(why, why_cap, "The disc's file table could not be read.");
        return 0;
    }
    s_fst_entries = fst_w2(0);
    s_fst_names = (const char*)s_fst + s_fst_entries * 12;
    xhw_logf("[DVD] GALE01 rev %u, %u FST entries", hdr[7], s_fst_entries);
    return 1;
}

void DVDInit(void) {
    static int started;
    if (started) return;
    started = 1;
    s_qlock = xhw_mutex_create();
    s_qevent = xhw_event_create();
    xhw_thread_start(worker, NULL, 1, 64 * 1024);
}
