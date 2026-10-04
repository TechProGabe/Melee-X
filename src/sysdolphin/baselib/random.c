#include "random.h"

#ifdef TARGET_XBOX
/* PORT: the RNG trace (env MX_RAND_TRACE, test builds;
 * xbox/src/sdk/simhash.c): every draw reports its new seed and its caller.
 * Observes only; an empty function in a release. HSD_Randi's draw is the
 * one HSD_Rand inlined into it, so its caller is HSD_Randi's. */
void xsdk_rand_draw(u32 seed, void* caller);
#define RAND_DRAW() xsdk_rand_draw(*HSD_RandSeedPtr, __builtin_return_address(0))
#else
#define RAND_DRAW() ((void) 0)
#endif

static u32 seed = 1;
u32* HSD_RandSeedPtr = &seed;

s32 HSD_Rand(void)
{
    *HSD_RandSeedPtr = *HSD_RandSeedPtr * 214013 + 2531011;
    RAND_DRAW();
    return *HSD_RandSeedPtr >> 0x10;
}

f32 HSD_Randf(void)
{
    *HSD_RandSeedPtr = *HSD_RandSeedPtr * 214013 + 2531011;
    RAND_DRAW();
    return (f32) (*HSD_RandSeedPtr >> 0x10) / (1 << 16);
}

s32 HSD_Randi(s32 max_val)
{
    return max_val * HSD_Rand() / (1 << 16);
}

void _HSD_RandForgetMemory(void* low, void* high)
{
    if (low <= (void*) HSD_RandSeedPtr && (void*) HSD_RandSeedPtr < high) {
        HSD_RandSeedPtr = &seed;
    }
}
