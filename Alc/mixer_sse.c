#include "config.h"

#ifdef HAVE_XMMINTRIN_H
#include <xmmintrin.h>
#endif

#include "AL/al.h"
#include "AL/alc.h"
#include "alMain.h"
#include "alu.h"

#include "alSource.h"
#include "mixer_defs.h"


static __inline void ApplyCoeffsStep(ALuint Offset, ALfloat (*RESTRICT Values)[2],
                                     const ALuint IrSize,
                                     ALfloat (*RESTRICT Coeffs)[2],
                                     ALfloat (*RESTRICT CoeffStep)[2],
                                     ALfloat left, ALfloat right)
{
    const __m128 lrlr = { left, right, left, right };
    __m128 coeffs, deltas, imp0, imp1;
    __m128 vals = _mm_setzero_ps();
    ALuint i;

    if((Offset&1))
    {
        const ALuint o0 = Offset&HRIR_MASK;
        const ALuint o1 = (Offset+IrSize-1)&HRIR_MASK;

        coeffs = _mm_load_ps(&Coeffs[0][0]);
        deltas = _mm_load_ps(&CoeffStep[0][0]);
        vals = _mm_loadl_pi(vals, (__m64*)&Values[o0][0]);
        imp0 = _mm_mul_ps(lrlr, coeffs);
        coeffs = _mm_add_ps(coeffs, deltas);
        vals = _mm_add_ps(imp0, vals);
        _mm_store_ps(&Coeffs[0][0], coeffs);
        _mm_storel_pi((__m64*)&Values[o0][0], vals);
        for(i = 1;i < IrSize-1;i += 2)
        {
            const ALuint o2 = (Offset+i)&HRIR_MASK;

            coeffs = _mm_load_ps(&Coeffs[i+1][0]);
            deltas = _mm_load_ps(&CoeffStep[i+1][0]);
            vals = _mm_load_ps(&Values[o2][0]);
            imp1 = _mm_mul_ps(lrlr, coeffs);
            coeffs = _mm_add_ps(coeffs, deltas);
            imp0 = _mm_shuffle_ps(imp0, imp1, _MM_SHUFFLE(1, 0, 3, 2));
            vals = _mm_add_ps(imp0, vals);
            _mm_store_ps(&Coeffs[i+1][0], coeffs);
            _mm_store_ps(&Values[o2][0], vals);
            imp0 = imp1;
        }
        vals = _mm_loadl_pi(vals, (__m64*)&Values[o1][0]);
        imp0 = _mm_movehl_ps(imp0, imp0);
        vals = _mm_add_ps(imp0, vals);
        _mm_storel_pi((__m64*)&Values[o1][0], vals);
    }
    else
    {
        for(i = 0;i < IrSize;i += 2)
        {
            const ALuint o = (Offset + i)&HRIR_MASK;

            coeffs = _mm_load_ps(&Coeffs[i][0]);
            deltas = _mm_load_ps(&CoeffStep[i][0]);
            vals = _mm_load_ps(&Values[o][0]);
            imp0 = _mm_mul_ps(lrlr, coeffs);
            coeffs = _mm_add_ps(coeffs, deltas);
            vals = _mm_add_ps(imp0, vals);
            _mm_store_ps(&Coeffs[i][0], coeffs);
            _mm_store_ps(&Values[o][0], vals);
        }
    }
}

static __inline void ApplyCoeffs(ALuint Offset, ALfloat (*RESTRICT Values)[2],
                                 const ALuint IrSize,
                                 ALfloat (*RESTRICT Coeffs)[2],
                                 ALfloat left, ALfloat right)
{
    const __m128 lrlr = { left, right, left, right };
    __m128 vals = _mm_setzero_ps();
    __m128 coeffs;
    ALuint i;

    if((Offset&1))
    {
        const ALuint o0 = Offset&HRIR_MASK;
        const ALuint o1 = (Offset+IrSize-1)&HRIR_MASK;
        __m128 imp0, imp1;

        coeffs = _mm_load_ps(&Coeffs[0][0]);
        vals = _mm_loadl_pi(vals, (__m64*)&Values[o0][0]);
        imp0 = _mm_mul_ps(lrlr, coeffs);
        vals = _mm_add_ps(imp0, vals);
        _mm_storel_pi((__m64*)&Values[o0][0], vals);
        for(i = 1;i < IrSize-1;i += 2)
        {
            const ALuint o2 = (Offset+i)&HRIR_MASK;

            coeffs = _mm_load_ps(&Coeffs[i+1][0]);
            vals = _mm_load_ps(&Values[o2][0]);
            imp1 = _mm_mul_ps(lrlr, coeffs);
            imp0 = _mm_shuffle_ps(imp0, imp1, _MM_SHUFFLE(1, 0, 3, 2));
            vals = _mm_add_ps(imp0, vals);
            _mm_store_ps(&Values[o2][0], vals);
            imp0 = imp1;
        }
        vals = _mm_loadl_pi(vals, (__m64*)&Values[o1][0]);
        imp0 = _mm_movehl_ps(imp0, imp0);
        vals = _mm_add_ps(imp0, vals);
        _mm_storel_pi((__m64*)&Values[o1][0], vals);
    }
    else
    {
        for(i = 0;i < IrSize;i += 2)
        {
            const ALuint o = (Offset + i)&HRIR_MASK;

            coeffs = _mm_load_ps(&Coeffs[i][0]);
            vals = _mm_load_ps(&Values[o][0]);
            vals = _mm_add_ps(vals, _mm_mul_ps(lrlr, coeffs));
            _mm_store_ps(&Values[o][0], vals);
        }
    }
}


void MixDirect_SSE(ALsource *Source, ALCdevice *Device, DirectParams *params,
  const ALfloat *RESTRICT data, ALuint srcchan,
  ALuint OutPos, ALuint SamplesToDo, ALuint BufferSize)
{
    ALfloat (*RESTRICT DryBuffer)[MaxChannels];
    ALfloat *RESTRICT ClickRemoval, *RESTRICT PendingClicks;
    ALIGN(16) ALfloat DrySend[MaxChannels];
    ALIGN(16) ALfloat value[4];
    FILTER *DryFilter;
    ALuint pos;
    ALuint c;
    (void)Source;

    DryBuffer = Device->DryBuffer;
    ClickRemoval = Device->ClickRemoval;
    PendingClicks = Device->PendingClicks;
    DryFilter = &params->iirFilter;

    for(c = 0;c < MaxChannels;c++)
        DrySend[c] = params->Gains[srcchan][c];

    pos = 0;
    if(OutPos == 0)
    {
        value[0] = lpFilter2PC(DryFilter, srcchan, data[pos]);
        for(c = 0;c < MaxChannels;c++)
            ClickRemoval[c] -= value[0]*DrySend[c];
    }
    for(pos = 0;pos < BufferSize-3;pos += 4)
    {
        __m128 val4;

        value[0] = lpFilter2P(DryFilter, srcchan, data[pos  ]);
        value[1] = lpFilter2P(DryFilter, srcchan, data[pos+1]);
        value[2] = lpFilter2P(DryFilter, srcchan, data[pos+2]);
        value[3] = lpFilter2P(DryFilter, srcchan, data[pos+3]);
        val4 = _mm_load_ps(value);

        for(c = 0;c < MaxChannels;c++)
        {
            const __m128 gain = _mm_set1_ps(DrySend[c]);
            __m128 dry4;

            value[0] = DryBuffer[OutPos  ][c];
            value[1] = DryBuffer[OutPos+1][c];
            value[2] = DryBuffer[OutPos+2][c];
            value[3] = DryBuffer[OutPos+3][c];
            dry4 = _mm_load_ps(value);

            dry4 = _mm_add_ps(dry4, _mm_mul_ps(val4, gain));

            _mm_store_ps(value, dry4);
            DryBuffer[OutPos  ][c] = value[0];
            DryBuffer[OutPos+1][c] = value[1];
            DryBuffer[OutPos+2][c] = value[2];
            DryBuffer[OutPos+3][c] = value[3];
        }

        OutPos += 4;
    }
    for(;pos < BufferSize;pos++)
    {
        value[0] = lpFilter2P(DryFilter, srcchan, data[pos]);
        for(c = 0;c < MaxChannels;c++)
            DryBuffer[OutPos][c] += value[0]*DrySend[c];
        OutPos++;
    }
    if(OutPos == SamplesToDo)
    {
        value[0] = lpFilter2PC(DryFilter, srcchan, data[pos]);
        for(c = 0;c < MaxChannels;c++)
            PendingClicks[c] += value[0]*DrySend[c];
    }
}
#define NO_MIXDIRECT


#define SUFFIX SSE
#include "mixer_inc.c"
#undef SUFFIX
