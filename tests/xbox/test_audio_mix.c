/* test_audio_mix.c - host check that src/pc/audio.c's block decoder
 * (decode_samples / src_next) mixes the same bits as the per-sample
 * next_sample() it replaced. The current mix_voice runs side by side with
 * audio_mix_ref.c, the code as it was, on random voices: ADPCM, PCM16, PCM8
 * and unknown formats; addresses anywhere in ARAM and across its end (the
 * bounds checks, a header read before a failed check); end and loop
 * addresses inside and outside the decoded range, on header nibbles and on
 * loop targets above the end; random predictor/scale bytes, coefficients
 * and histories up to +-32768; ratios 0, 1.0, random up to AX's 4.0, tiny,
 * and unclamped ones that wrap frac; volume ramps across 0 and 32767; every
 * dry/aux send combination. Each voice runs several frames in a row, and
 * the dry mix, both aux busses and the whole Voice must match bit for bit;
 * voices that end mid-frame leave every remainder of four outputs (the SSE
 * loops' tails). Then render_frame's output clamp against the scalar loop,
 * on random, huge, tiny, signed-zero, infinite and NaN samples, and the aux
 * reverb against its per-sample network: random settings (damping past
 * both clamps, crosstalk on and off), lines full of random samples at random
 * positions (a stretch ends at every line's wrap), several frames in a row;
 * the output, every line, the positions and the filter state.
 * Built and run by tools/xbox/test_audio_mix.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/audio.c"
#include "audio_mix_ref.c"

/* what audio.c links against */
BOOL OSDisableInterrupts(void) { return 0; }
BOOL OSRestoreInterrupts(BOOL level) { return level; }
bool SDL_InitSubSystem(uint32_t flags) { (void)flags; return false; }
const char* SDL_GetError(void) { return ""; }
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID dev, const SDL_AudioSpec* spec,
                                           SDL_AudioStreamCallback cb, void* userdata)
{
    (void)dev, (void)spec, (void)cb, (void)userdata;
    return NULL;
}
bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len)
{
    (void)stream, (void)buf, (void)len;
    return true;
}
int SDL_GetAudioStreamQueued(SDL_AudioStream* stream) { (void)stream; return 0; }
bool SDL_SetAudioStreamGain(SDL_AudioStream* stream, float gain) { (void)stream, (void)gain; return true; }
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream) { (void)stream; return true; }
void SDL_DestroyAudioStream(SDL_AudioStream* stream) { (void)stream; }
u8* aurora_aram_base(void) { return NULL; }

static u64 s_rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (u32)(s_rng >> 16);
}
static u32 rnd_n(u32 n) { return n ? rnd() % n : 0; }
static s16 rnd_s16(void)
{
    switch (rnd_n(8)) {
    case 0: return -32768;
    case 1: return 32767;
    case 2: return (s16)(rnd_n(64) - 32);
    default: return (s16)rnd();
    }
}

static void dummy_fx(void* a, void* b) { (void)a, (void)b; }

/* a random voice; `lim` is the format's address space (nibbles, words or bytes) */
static void make_voice(Voice* v)
{
    AXPB* pb = &v->vpb.pb;
    u32 lim, r = rnd_n(20);
    int i;

    memset(v, 0, sizeof(*v));
    v->used = true;
    pb->state = rnd_n(16) != 0;
    pb->addr.format = r < 12 ? AX_FORMAT_ADPCM : r < 15 ? AX_FORMAT_PCM16 : r < 19 ? AX_FORMAT_PCM8 : 3;
    lim = pb->addr.format == AX_FORMAT_ADPCM ? PC_ARAM_SIZE * 2u :
          pb->addr.format == AX_FORMAT_PCM16 ? PC_ARAM_SIZE / 2u : PC_ARAM_SIZE;
    pb->addr.loopFlag = rnd_n(2);
    switch (rnd_n(6)) {
    case 0: v->cur_addr = lim - rnd_n(1200); break;          /* runs off the end of ARAM */
    case 1: v->cur_addr = lim + rnd_n(64) - 32; break;      /* starts at or past it */
    case 2: v->cur_addr = rnd_n(64); break;
    default: v->cur_addr = rnd_n(lim); break;
    }
    switch (rnd_n(6)) {
    case 0: v->end_addr = v->cur_addr - rnd_n(64); break;   /* behind: runs to the end of ARAM */
    case 1: v->end_addr = (v->cur_addr + rnd_n(400)) & ~15u | rnd_n(2); break; /* a header nibble */
    case 2: v->end_addr = rnd(); break;
    default: v->end_addr = v->cur_addr + rnd_n(1500); break;
    }
    switch (rnd_n(5)) {
    case 0: v->loop_addr = v->end_addr + rnd_n(0x30000); break; /* HPS: above the end */
    case 1: v->loop_addr = (v->cur_addr + rnd_n(300)) & ~15u; break;
    case 2: v->loop_addr = lim - rnd_n(40); break;
    default: v->loop_addr = v->cur_addr + rnd_n(600) - 100; break;
    }
    v->pred_scale = rnd_n(4) ? (u16)rnd_n(0x80) : (u16)rnd();
    v->yn1 = rnd_s16();
    v->yn2 = rnd_s16();
    for (i = 0; i < 8; i++) {
        pb->adpcm.a[i][0] = (u16)rnd_s16();
        pb->adpcm.a[i][1] = (u16)rnd_s16();
    }
    pb->adpcmLoop.loop_pred_scale = rnd_n(4) ? (u16)rnd_n(0x80) : (u16)rnd();
    pb->adpcmLoop.loop_yn1 = (u16)rnd_s16();
    pb->adpcmLoop.loop_yn2 = (u16)rnd_s16();

    switch (rnd_n(10)) {
    case 0: r = 0; break;
    case 1: case 2: case 3: r = 0x10000; break;
    case 4: r = 1 + rnd_n(0x200); break;
    case 5: r = 0x40000; break;
    default: r = 1 + rnd_n(0x40000); break;
    }
    set_addr(&pb->src.ratioHi, &pb->src.ratioLo, r);
    switch (rnd_n(8)) {
    case 0: case 1: v->frac = 0; break;
    case 2: case 3: v->frac = rnd_n(0x60000); break;   /* a whole step left over at an end */
    case 4: v->frac = rnd_n(0x100000); break;          /* past SRC_MAX at 4.0: the stepped loop */
    default: v->frac = rnd_n(0x10000); break;
    }
    v->prev = rnd_s16();
    v->cur = rnd_s16();

    pb->ve.currentVolume = rnd_n(4) ? (u16)rnd_n(32768) : rnd_n(2) ? 0 : (u16)rnd();
    pb->ve.currentDelta = rnd_n(3) == 0 ? 0 : rnd_n(2) ? (s16)(rnd_n(512) - 256) : rnd_s16();
    pb->mix.vL = rnd_n(4) ? (u16)rnd() : 0;
    pb->mix.vR = rnd_n(4) ? (u16)rnd() : 0;
    pb->mix.vAuxAL = rnd_n(2) ? (u16)rnd() : 0;
    pb->mix.vAuxAR = rnd_n(2) ? (u16)rnd() : 0;
    pb->mix.vAuxBL = rnd_n(3) ? 0 : (u16)rnd();
    pb->mix.vAuxBR = rnd_n(3) ? 0 : (u16)rnd();
    v->vpb.priority = rnd_n(4) ? (int)rnd_n(0x40) : 0x1D;
    v->is_stream = rnd_n(4) == 0;
}

static float s_out_init[AX_FRAME * 2];
static long s_aux_init[2][3][AX_FRAME]; /* NOLINT: the busses are long */

/* the reverb's lines, positions and filter state */
static int reverb_same(const struct AXFX_REVHI_WORK* a, const struct AXFX_REVHI_WORK* b)
{
    int i;
    if (memcmp(a->lpLastout, b->lpLastout, sizeof(a->lpLastout))) {
        return 0;
    }
    for (i = 0; i < 2 * 9; i++) {
        const struct AXFX_REVHI_DELAYLINE* la = i < 9 ? &a->C[i] : &a->AP[i - 9];
        const struct AXFX_REVHI_DELAYLINE* lb = i < 9 ? &b->C[i] : &b->AP[i - 9];
        if (la->inPoint != lb->inPoint || la->outPoint != lb->outPoint || la->length != lb->length ||
            (la->inputs != NULL && memcmp(la->inputs, lb->inputs, (size_t)la->length * sizeof(float)))) {
            return 0;
        }
    }
    return 1;
}

static float rnd_f(float lo, float hi) { return lo + (hi - lo) * (float)rnd_n(1000001) / 1000000.0f; }

/* the reverb against ref_axfx_reverb_run; returns the mismatches */
static long test_reverb(int trials) /* NOLINT */
{
    long fails = 0; /* NOLINT */
    int t, f, i, k;

    for (t = 0; t < trials; t++) {
        static struct AXFX_REVHI_WORK a, b;
        long ca[2][AX_FRAME], cb[2][AX_FRAME]; /* NOLINT: the busses are long */
        struct AXFX_BUFFERUPDATE ua = {ca[0], ca[1], NULL}, ub = {cb[0], cb[1], NULL};
        float damping = rnd_n(5) ? rnd_f(0.0f, 0.95f) : rnd_f(-0.5f, 1.5f);
        float crosstalk = rnd_n(2) ? 0.0f : rnd_f(-0.5f, 1.0f);

        if (!axfx_reverb_init(&a, rnd_f(-1.0f, 1.0f), rnd_f(0.0f, 1.0f), rnd_f(0.0f, 10.0f), damping, crosstalk)) {
            fprintf(stderr, "reverb init failed\n");
            return 1;
        }
        a.lpLastout[0] = rnd_f(-30000.0f, 30000.0f);
        a.lpLastout[1] = rnd_f(-30000.0f, 30000.0f);
        for (k = 0; k < 2 * 9; k++) {
            struct AXFX_REVHI_DELAYLINE* l = k < 9 ? &a.C[k] : &a.AP[k - 9];
            if (l->inputs == NULL) {
                continue;
            }
            for (i = 0; i < l->length; i++) {
                l->inputs[i] = rnd_n(4) ? rnd_f(-40000.0f, 40000.0f) : 0.0f;
            }
            /* at the start, the end and anywhere */
            l->inPoint = l->outPoint = rnd_n(4) == 0 ? 0 : rnd_n(4) == 0 ? l->length - 1 - (long)rnd_n(3) :
                                                                         (long)rnd_n((u32)l->length);
        }
        b = a;
        for (k = 0; k < 2 * 9; k++) {
            struct AXFX_REVHI_DELAYLINE* la = k < 9 ? &a.C[k] : &a.AP[k - 9];
            struct AXFX_REVHI_DELAYLINE* lb = k < 9 ? &b.C[k] : &b.AP[k - 9];
            if (la->inputs != NULL) {
                lb->inputs = malloc((size_t)la->length * sizeof(float));
                memcpy(lb->inputs, la->inputs, (size_t)la->length * sizeof(float));
            }
        }
        for (f = 0; f < 8; f++) {
            for (i = 0; i < AX_FRAME; i++) {
                ca[0][i] = rnd_n(3) ? (s32)rnd() >> 15 : 0;
                ca[1][i] = rnd_n(3) ? (s32)rnd() >> 15 : 0;
            }
            memcpy(cb, ca, sizeof(ca));
            ref_axfx_reverb_run(&a, &ua);
            axfx_reverb_run(&b, &ub);
            if (memcmp(ca, cb, sizeof(ca)) || !reverb_same(&a, &b)) {
                if (++fails <= 10) {
                    fprintf(stderr, "MISMATCH reverb trial %d frame %d: out %s\n", t, f,
                            memcmp(ca, cb, sizeof(ca)) ? "differs" : "same");
                }
                break;
            }
        }
        axfx_reverb_shutdown(&a);
        axfx_reverb_shutdown(&b);
    }
    return fails;
}

static void set_globals(void)
{
    memcpy(s_auxA.ch, s_aux_init[0], sizeof(s_auxA.ch));
    memcpy(s_auxB.ch, s_aux_init[1], sizeof(s_auxB.ch));
}

int main(void)
{
    const int trials = 200000;
    long fails = 0, ended = 0, frames = 0, wraps = 0, paths[5] = {0}; /* NOLINT */
    long clamp_fails = 0; /* NOLINT */
    int t, f, i;

    s_aram = malloc(PC_ARAM_SIZE);
    for (i = 0; i < (int)PC_ARAM_SIZE; i++) {
        s_aram[i] = (u8)rnd();
    }
    for (t = 0; t < trials; t++) {
        Voice v0, va, vb;
        float oa[AX_FRAME * 2] __attribute__((aligned(16)));
        float ob[AX_FRAME * 2] __attribute__((aligned(16)));
        long aa[2][3][AX_FRAME]; /* NOLINT */
        int nframes = 1 + rnd_n(6);

        make_voice(&v0);
        /* a few unclamped ratios: frac wraps u32, so src_need gives up and
         * decodes one sample at a time (slow in both: ~10M samples a frame) */
        if (t % 20000 == 7) {
            set_addr(&v0.vpb.pb.src.ratioHi, &v0.vpb.pb.src.ratioLo, 0xFFFF0000u + rnd_n(0x10000));
            v0.vpb.pb.addr.format = AX_FORMAT_PCM8;
            v0.vpb.pb.addr.loopFlag = 1;
            v0.end_addr = v0.cur_addr + 100;
            v0.loop_addr = v0.cur_addr;
            v0.vpb.pb.state = 1;
            nframes = 1;
            wraps++;
        } else if (t % 1000 == 3) {
            set_addr(&v0.vpb.pb.src.ratioHi, &v0.vpb.pb.src.ratioLo, 0x40001 + rnd_n(0x400000));
        }
        s_aux_on = rnd_n(5) != 0;
        s_auxA.cb = rnd_n(4) ? dummy_fx : NULL;
        s_auxB.cb = rnd_n(2) ? dummy_fx : NULL;
        s_sfx_volume = rnd_n(5) ? (float)rnd_n(1001) / 1000.0f : 0.0f;
        s_music_volume = rnd_n(5) ? (float)rnd_n(1001) / 1000.0f : 1.0f;
        {
            u32 ratio = addr32(v0.vpb.pb.src.ratioHi, v0.vpb.pb.src.ratioLo);
            u32 need = ratio == 0x10000 ? AX_FRAME : src_need(v0.frac, ratio);
            paths[ratio == 0 ? 0 : ratio == 0x10000 ? (v0.frac == 0 ? 1 : 2) :
                  need == 0 || need > SRC_MAX ? 4 : 3]++;
        }
        va = v0;
        vb = v0;
        for (f = 0; f < nframes; f++) {
            for (i = 0; i < AX_FRAME * 2; i++) {
                s_out_init[i] = rnd_n(4) ? 0.0f : (float)(s32)rnd() * (1.0f / 2147483648.0f);
            }
            for (i = 0; i < AX_FRAME * 6; i++) {
                (&s_aux_init[0][0][0])[i] = rnd_n(4) ? 0 : (s32)rnd() >> 8;
            }
            memcpy(oa, s_out_init, sizeof(oa));
            memcpy(ob, s_out_init, sizeof(ob));
            set_globals();
            ref_mix_voice(&va, oa);
            memcpy(aa[0], s_auxA.ch, sizeof(aa[0]));
            memcpy(aa[1], s_auxB.ch, sizeof(aa[1]));
            set_globals();
            mix_voice(&vb, ob);
            frames++;
            if (memcmp(oa, ob, sizeof(oa)) || memcmp(aa[0], s_auxA.ch, sizeof(aa[0])) ||
                memcmp(aa[1], s_auxB.ch, sizeof(aa[1])) || memcmp(&va, &vb, sizeof(va))) {
                if (++fails <= 10) {
                    fprintf(stderr,
                        "MISMATCH trial %d frame %d: fmt=%u ratio=%#x state %u/%u cur %u/%u "
                        "frac %#x/%#x yn %d,%d/%d,%d out %s aux %s\n",
                        t, f, v0.vpb.pb.addr.format,
                        addr32(v0.vpb.pb.src.ratioHi, v0.vpb.pb.src.ratioLo), va.vpb.pb.state,
                        vb.vpb.pb.state, va.cur_addr, vb.cur_addr, va.frac, vb.frac, va.yn1, va.yn2,
                        vb.yn1, vb.yn2, memcmp(oa, ob, sizeof(oa)) ? "differs" : "same",
                        memcmp(aa[0], s_auxA.ch, sizeof(aa[0])) || memcmp(aa[1], s_auxB.ch, sizeof(aa[1])) ?
                            "differs" : "same");
                }
                break;
            }
            if (!va.vpb.pb.state) {
                ended++;
                break;
            }
        }
    }
    for (t = 0; t < 200000; t++) {
        float ca[AX_FRAME * 2] __attribute__((aligned(16)));
        float cb[AX_FRAME * 2] __attribute__((aligned(16)));
        static const float special[] = {0.0f, -0.0f, 1.0f, -1.0f, 1.0000001f, -1.0000001f,
                                        0.99999994f, 1e-45f, -1e-45f, 1e30f, -1e30f};
        for (i = 0; i < AX_FRAME * 2; i++) {
            u32 r = rnd();
            switch (rnd_n(6)) {
            case 0: memcpy(&ca[i], &r, 4); break; /* any bits: NaNs, infinities, denormals */
            case 1: ca[i] = special[rnd_n(sizeof(special) / sizeof(special[0]))]; break;
            default: ca[i] = (float)(s32)r * (3.0f / 2147483648.0f); break;
            }
        }
        memcpy(cb, ca, sizeof(ca));
        s_master = rnd_n(4) ? 1.0f : (float)rnd_n(3001) / 1000.0f;
        ref_clamp_frame(ca);
        clamp_frame(cb);
        if (memcmp(ca, cb, sizeof(ca)) && ++clamp_fails <= 10) {
            fprintf(stderr, "MISMATCH clamp trial %d\n", t);
        }
    }
    printf("audio mix: %d voices, %ld frames (%ld voices ended, %ld with wrapping frac): %ld mismatches\n",
           trials, frames, ended, wraps, fails);
    printf("  voices by path: ratio 0 %ld, 1.0 %ld, 1.0 with a phase %ld, resampled %ld, stepped %ld\n",
           paths[0], paths[1], paths[2], paths[3], paths[4]);
    printf("output clamp: 200000 frames: %ld mismatches\n", clamp_fails);
    fails += clamp_fails;
    clamp_fails = test_reverb(3000);
    printf("reverb: 3000 networks, 8 frames each: %ld mismatches\n", clamp_fails);
    fails += clamp_fails;
    free(s_aram);
    return fails != 0;
}
