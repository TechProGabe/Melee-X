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
 * the dry mix, both aux busses and the whole Voice must match bit for bit.
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
    switch (rnd_n(4)) {
    case 0: v->frac = 0; break;
    case 1: v->frac = rnd_n(0x60000); break;   /* a whole step left over at an end */
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

static void set_globals(void)
{
    memcpy(s_auxA.ch, s_aux_init[0], sizeof(s_auxA.ch));
    memcpy(s_auxB.ch, s_aux_init[1], sizeof(s_auxB.ch));
}

int main(void)
{
    const int trials = 200000;
    long fails = 0, ended = 0, frames = 0, wraps = 0; /* NOLINT */
    int t, f, i;

    s_aram = malloc(PC_ARAM_SIZE);
    for (i = 0; i < (int)PC_ARAM_SIZE; i++) {
        s_aram[i] = (u8)rnd();
    }
    for (t = 0; t < trials; t++) {
        Voice v0, va, vb;
        float oa[AX_FRAME * 2], ob[AX_FRAME * 2];
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
    printf("audio mix: %d voices, %ld frames (%ld voices ended, %ld with wrapping frac): %ld mismatches\n",
           trials, frames, ended, wraps, fails);
    free(s_aram);
    return fails != 0;
}
