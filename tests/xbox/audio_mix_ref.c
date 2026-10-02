/* audio_mix_ref.c - src/pc/audio.c's sample fetch and voice mixer as they
 * were before the block decoder (dev fda070e), renamed ref_*: the reference
 * tests/xbox/test_audio_mix.c compares the current mix_voice against, bit for
 * bit. #included after audio.c, whose Voice, AXPB, clamp16, globals and
 * is_music_stream it uses. */

static bool ref_next_sample(Voice* v, s16* out) {
    AXPB* pb = &v->vpb.pb;
    u16 format = pb->addr.format;

    if (!pb->state || !s_aram) {
        return false;
    }

    /* Every read below indexes the 16MB ARAM buffer directly. A voice whose
     * address pair is wrong -- a torn AXSetVoiceAddr from the game thread, a
     * bank whose ARAM allocation was freed, a .ssm header read with the wrong
     * relocation -- would otherwise read up to 2GB past the buffer. Ending
     * the voice is what the mixer already does for a finished one, so the
     * voice is reaped instead of faulting. */
    switch (format) {
    case AX_FORMAT_ADPCM: {
        /* 16 nibbles per frame: 2 header nibbles, 14 sample nibbles. */
        if ((v->cur_addr & 15) == 0) {
            if ((v->cur_addr >> 1) + 2 > PC_ARAM_SIZE) {
                return false;
            }
            v->pred_scale = s_aram[v->cur_addr >> 1];
            v->cur_addr += 2;
        }
        if ((v->cur_addr >> 1) >= PC_ARAM_SIZE) {
            return false;
        }
        u8 byte = s_aram[v->cur_addr >> 1];
        s32 nibble = (v->cur_addr & 1) ? (byte & 0xF) : (byte >> 4);
        nibble = nibble >= 8 ? nibble - 16 : nibble;
        s32 scale = 1 << (v->pred_scale & 0xF);
        u32 coef = (v->pred_scale >> 4) & 7;
        s32 c0 = (s16)pb->adpcm.a[coef][0];
        s32 c1 = (s16)pb->adpcm.a[coef][1];
        /* s64 accumulator: a bank read with the wrong relocation yields
         * coefficients and a scale whose four terms overshoot s32, and
         * signed overflow is undefined behaviour that -O2 may fold the
         * clamp away against. */
        s64 acc = (s64)nibble * scale * 2048 + 1024 + (s64)c0 * v->yn1 + (s64)c1 * v->yn2;
        s32 sample = clamp16(acc >> 11);
        v->yn2 = v->yn1;
        v->yn1 = sample;
        *out = (s16)sample;
        break;
    }
    case AX_FORMAT_PCM16: {
        const u8* p;
        if (v->cur_addr >= PC_ARAM_SIZE / 2) {
            return false;
        }
        p = &s_aram[v->cur_addr * 2];
        *out = (s16)((p[0] << 8) | p[1]);
        break;
    }
    case AX_FORMAT_PCM8:
        if (v->cur_addr >= PC_ARAM_SIZE) {
            return false;
        }
        *out = (s16)((s8)s_aram[v->cur_addr] * 256);
        break;
    default:
        return false;
    }

    /* AX end addresses are inclusive. HPS jumps into the next ring block
     * here, then updates endAddress at the following synth callback. That
     * loop target can be ABOVE the old end: equality after decoding (not a
     * range check before it) lets the new block advance in the meantime. */
    if (v->cur_addr == v->end_addr) {
        if (pb->addr.loopFlag) {
            v->cur_addr = v->loop_addr;
            if (format == AX_FORMAT_ADPCM) {
                v->pred_scale = pb->adpcmLoop.loop_pred_scale;
                v->yn1 = (s16)pb->adpcmLoop.loop_yn1;
                v->yn2 = (s16)pb->adpcmLoop.loop_yn2;
            }
        } else {
            pb->state = 0;
        }
    } else {
        v->cur_addr++;
    }
    return true;
}

static void ref_mix_voice(Voice* v, float* out) {
    AXPB* pb = &v->vpb.pb;
    if (!pb->state) {
        return;
    }

    u32 ratio = addr32(pb->src.ratioHi, pb->src.ratioLo);
    s32 vol = pb->ve.currentVolume;
    s32 delta = pb->ve.currentDelta;

    float voice_gain = is_music_stream(v) ? pc_get_music_volume() : pc_get_sfx_volume();
    if (voice_gain < 0.0f) {
        voice_gain = 0.0f;
    }

    float vl = (pb->mix.vL / 32767.0f) * voice_gain;
    float vr = (pb->mix.vR / 32767.0f) * voice_gain;
    float al = (pb->mix.vAuxAL / 32767.0f) * voice_gain;
    float ar = (pb->mix.vAuxAR / 32767.0f) * voice_gain;
    float bl = (pb->mix.vAuxBL / 32767.0f) * voice_gain;
    float br = (pb->mix.vAuxBR / 32767.0f) * voice_gain;
    bool send_a = s_aux_on && (s_auxA.cb != NULL) && (al != 0.0f || ar != 0.0f);
    bool send_b = s_aux_on && (s_auxB.cb != NULL) && (bl != 0.0f || br != 0.0f);

    if (vol < 0) {
        vol = 0;
    } else if (vol > 32767) {
        vol = 32767;
    }

    bool is_silent = ((vol == 0 && delta == 0) || (vl == 0.0f && vr == 0.0f)) && !send_a && !send_b;

    /* If ratio is 0, no source samples can ever be consumed (v->frac += 0).
     * If the voice is silent, nothing is mixed and no samples advance. Early exit! */
    if (ratio == 0) {
        if (is_silent) {
            return;
        }
        float t = (float)v->frac * (1.0f / 65536.0f);
        float s = ((float)v->prev + t * (float)(v->cur - v->prev)) * (1.0f / 32768.0f);
        for (int i = 0; i < AX_FRAME; i++) {
            float g = (float)vol * (1.0f / 32767.0f);
            float sv = s * g;
            out[i * 2] += sv * vl;
            out[i * 2 + 1] += sv * vr;
            if (send_a) {
                s_auxA.ch[0][i] += (long)(sv * al * 32767.0f);
                s_auxA.ch[1][i] += (long)(sv * ar * 32767.0f);
            }
            if (send_b) {
                s_auxB.ch[0][i] += (long)(sv * bl * 32767.0f);
                s_auxB.ch[1][i] += (long)(sv * br * 32767.0f);
            }
            vol += delta;
            if (vol < 0) {
                vol = 0;
            } else if (vol > 32767) {
                vol = 32767;
            }
        }
        pb->ve.currentVolume = (u16)vol;
        set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
        return;
    }

    /* Fast path for silent voices: advance sample decoding, stream ring buffers,
     * and end-of-voice checks without any floating-point arithmetic or buffer writes. */
    if (is_silent) {
        if (ratio == 0x10000) {
            for (int i = 0; i < AX_FRAME; i++) {
                s16 s;
                if (!ref_next_sample(v, &s)) {
                    pb->state = 0;
                    pb->ve.currentVolume = (u16)(vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                    set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                    return;
                }
                v->prev = v->cur;
                v->cur = s;
                vol += delta;
                if (vol < 0) {
                    vol = 0;
                } else if (vol > 32767) {
                    vol = 32767;
                }
            }
        } else {
            for (int i = 0; i < AX_FRAME; i++) {
                v->frac += ratio;
                while (v->frac >= 0x10000) {
                    s16 s;
                    if (!ref_next_sample(v, &s)) {
                        pb->state = 0;
                        pb->ve.currentVolume = (u16)(vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                        set_addr(
                            &pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                        return;
                    }
                    v->prev = v->cur;
                    v->cur = s;
                    v->frac -= 0x10000;
                }
                vol += delta;
                if (vol < 0) {
                    vol = 0;
                } else if (vol > 32767) {
                    vol = 32767;
                }
            }
        }
        pb->ve.currentVolume = (u16)vol;
        set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
        return;
    }

    /* Fast path for 1:1 playback (ratio == 0x10000, 32kHz native GameCube rate).
     * Consumes exactly 1 sample per frame step.
     * When v->frac == 0, no fractional interpolation is performed: s = v->prev. */
    if (ratio == 0x10000 && v->frac == 0) {
        if (delta == 0 && !send_a && !send_b) {
            float scale_l = ((float)vol * (1.0f / 32767.0f)) * vl * (1.0f / 32768.0f);
            float scale_r = ((float)vol * (1.0f / 32767.0f)) * vr * (1.0f / 32768.0f);
            for (int i = 0; i < AX_FRAME; i++) {
                s16 s;
                if (!ref_next_sample(v, &s)) {
                    pb->state = 0;
                    pb->ve.currentVolume = (u16)vol;
                    set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                    return;
                }
                v->prev = v->cur;
                v->cur = s;
                float smp = (float)v->prev;
                out[i * 2] += smp * scale_l;
                out[i * 2 + 1] += smp * scale_r;
            }
            pb->ve.currentVolume = (u16)vol;
            set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
            return;
        }

        for (int i = 0; i < AX_FRAME; i++) {
            s16 s;
            if (!ref_next_sample(v, &s)) {
                pb->state = 0;
                pb->ve.currentVolume = (u16)(vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                return;
            }
            v->prev = v->cur;
            v->cur = s;
            float smp = (float)v->prev * (1.0f / 32768.0f);
            float g = (float)vol * (1.0f / 32767.0f);
            float sv = smp * g;
            out[i * 2] += sv * vl;
            out[i * 2 + 1] += sv * vr;
            if (send_a) {
                s_auxA.ch[0][i] += (long)(sv * al * 32767.0f);
                s_auxA.ch[1][i] += (long)(sv * ar * 32767.0f);
            }
            if (send_b) {
                s_auxB.ch[0][i] += (long)(sv * bl * 32767.0f);
                s_auxB.ch[1][i] += (long)(sv * br * 32767.0f);
            }
            vol += delta;
            if (vol < 0) {
                vol = 0;
            } else if (vol > 32767) {
                vol = 32767;
            }
        }
        pb->ve.currentVolume = (u16)vol;
        set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
        return;
    }

    /* Fast path for ratio == 0x10000 with non-zero phase: phase remains constant throughout. */
    if (ratio == 0x10000) {
        float t = (float)v->frac * (1.0f / 65536.0f);
        for (int i = 0; i < AX_FRAME; i++) {
            s16 s;
            if (!ref_next_sample(v, &s)) {
                pb->state = 0;
                pb->ve.currentVolume = (u16)(vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                return;
            }
            v->prev = v->cur;
            v->cur = s;
            float smp = ((float)v->prev + t * (float)(v->cur - v->prev)) * (1.0f / 32768.0f);
            float g = (float)vol * (1.0f / 32767.0f);
            float sv = smp * g;
            out[i * 2] += sv * vl;
            out[i * 2 + 1] += sv * vr;
            if (send_a) {
                s_auxA.ch[0][i] += (long)(sv * al * 32767.0f);
                s_auxA.ch[1][i] += (long)(sv * ar * 32767.0f);
            }
            if (send_b) {
                s_auxB.ch[0][i] += (long)(sv * bl * 32767.0f);
                s_auxB.ch[1][i] += (long)(sv * br * 32767.0f);
            }
            vol += delta;
            if (vol < 0) {
                vol = 0;
            } else if (vol > 32767) {
                vol = 32767;
            }
        }
        pb->ve.currentVolume = (u16)vol;
        set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
        return;
    }

    /* General sample-rate conversion path (ratio != 0x10000) */
    for (int i = 0; i < AX_FRAME; i++) {
        v->frac += ratio;
        while (v->frac >= 0x10000) {
            s16 s;
            if (!ref_next_sample(v, &s)) {
                pb->state = 0;
                pb->ve.currentVolume = (u16)(vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                /* The mirror the game reads back has to follow on this path
                 * too: stopRange() matches a voice against the bank being
                 * unloaded by pb.addr.currentAddress, and the HPS block swap
                 * derives the stream position from it. Without this the
                 * field stayed at the previous frame's value, reporting the
                 * voice up to one frame short of where it really stopped. */
                set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                /* frac is deliberately left alone. It holds one whole
                 * unsatisfied sample step plus the sub-sample phase, and
                 * nothing at end-of-voice should discard that: on hardware
                 * the DSP's SRC phase survives an end too, and the reset
                 * arrives only with an explicit SRC block (AXSetVoiceSrc,
                 * which the HPS block swap does supply and which sets frac
                 * from currentAddressFrac). A voice restarted with only
                 * AXSetVoiceCurrentAddr therefore keeps its phase, and the
                 * retained whole step is what consumes the first sample of
                 * the new block instead of skipping it. */
                return;
            }
            v->prev = v->cur;
            v->cur = s;
            v->frac -= 0x10000;
        }
        float t = (float)v->frac * (1.0f / 65536.0f);
        float s = ((float)v->prev + t * (float)(v->cur - v->prev)) * (1.0f / 32768.0f);
        float g = (float)vol * (1.0f / 32767.0f);
        float sv = s * g;
        out[i * 2] += sv * vl;
        out[i * 2 + 1] += sv * vr;
        if (send_a) {
            s_auxA.ch[0][i] += (long)(sv * al * 32767.0f);
            s_auxA.ch[1][i] += (long)(sv * ar * 32767.0f);
        }
        if (send_b) {
            s_auxB.ch[0][i] += (long)(sv * bl * 32767.0f);
            s_auxB.ch[1][i] += (long)(sv * br * 32767.0f);
        }
        vol += delta;
        if (vol < 0) {
            vol = 0;
        } else if (vol > 32767) {
            vol = 32767;
        }
    }
    pb->ve.currentVolume = (u16)vol;
    set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
}
