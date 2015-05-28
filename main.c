#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define VOTE_ALLOC_COUNT 1024
#define BLOCK_SAMPLES 14

#define ALSA_PLAY 1
#include <alsa/asoundlib.h>
snd_pcm_t* ALSA_PCM;
FILE* ALSA_WAVE = NULL;

static int64_t ERROR_AVG = 0;
static int64_t ERROR_SAMP_COUNT = 0;

/* Used to build distribution set of coefficients */
struct coef_pair_vote
{
    int16_t a1, a2;
    unsigned votes;
};

static int64_t filter(int64_t* samps, int m)
{
    int i;
    int64_t avg = 0;
    for (i=0 ; i<BLOCK_SAMPLES ; ++i)
        avg += samps[i] * samps[i+m];
    return avg / BLOCK_SAMPLES;
}

static void adpcm_autocorrelate(struct coef_pair_vote* out,
                                int16_t* samps)
{
    int64_t expSamps[BLOCK_SAMPLES+3];
    int i;
    for (i=-1 ; i<BLOCK_SAMPLES+2 ; ++i)
        expSamps[i+1] = samps[i];

    int64_t fn1 = filter(&expSamps[1], -1);
    int64_t f0 = filter(&expSamps[1], 0);
    int64_t f1 = filter(&expSamps[1], 1);
    int64_t f2 = filter(&expSamps[1], 2);

    int a1 = 0;
    int a2 = 0;
    int64_t denom = f0 * f0 - fn1 * f1;
    if (denom)
    {
        a1 = (f0 * f1 - f1 * f2) / denom;
        a2 = (f0 * f2 - fn1 * f1) / denom;
    }

    //printf("%d %d\n", a1, a2);

    out->a1 = a1;
    out->a2 = a2;
}

static void adpcm_block_vote(struct coef_pair_vote** begin,
                             struct coef_pair_vote** end,
                             struct coef_pair_vote** max,
                             int16_t* samps)
{
    struct coef_pair_vote vote = {};
    adpcm_autocorrelate(&vote, samps);
    int a1 = vote.a1;
    int a2 = vote.a2;

    struct coef_pair_vote* it;
    for (it = *begin ; it < *end ; ++it)
    {
        if (it->a1 == a1 && it->a2 == a2)
        {
            ++it->votes;
            return;
        }
    }

    if (*end == *max)
    {
        size_t md = *max - *begin;
        *begin = realloc(*begin, (md + VOTE_ALLOC_COUNT) * sizeof(struct coef_pair_vote));
        *end = *begin + md;
        *max = *begin + md + VOTE_ALLOC_COUNT;
    }

    (*end)->a1 = vote.a1;
    (*end)->a2 = vote.a2;
    (*end)->votes = 1;
    ++(*end);
}

static const int nibble_to_int[16] = {0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1};

static inline int samp_clamp(int val)
{
    if (val < -32768) val = -32768;
    if (val > 32767) val = 32767;
    return val;
}

static void adpcm_block_predictor(int* hist1, int* hist2,
                                  int16_t a1best[16], int16_t a2best[16],
                                  int16_t* samps, char blockOut[8])
{
    int i,s;

    /* Coefficient selection pass */
    unsigned epsilon = ~0;
    int bestI;
    int lhist1;
    int lhist2;
    for (i=0 ; i<16 ; ++i)
    {
        int avgErr = 0;
        lhist1 = *hist1;
        lhist2 = *hist2;
        for (s=0 ; s<BLOCK_SAMPLES ; ++s)
        {
            int expSamp = samps[s] << 11;
            int testSamp = lhist1 * a1best[i] + lhist2 * a2best[i];
            int err = expSamp - testSamp - 1024;
            avgErr += err;
            lhist2 = lhist1;
            lhist1 = samp_clamp(testSamp >> 11);
        }
        unsigned avgEpsilon = abs(avgErr);
        if (avgEpsilon < epsilon)
        {
            epsilon = avgEpsilon;
            bestI = i;
        }
    }

    /* Maximum error computation */
    int minErr = 0;
    int maxErr = 0;
    lhist1 = *hist1;
    lhist2 = *hist2;
    for (s=0 ; s<BLOCK_SAMPLES ; ++s)
    {
        int expSamp = samps[s] << 11;
        int testSamp = lhist1 * a1best[bestI] + lhist2 * a2best[bestI];
        int err = expSamp - testSamp - 1024;
        err >>= 11;
        if (err > maxErr)
            maxErr = err;
        if (err < minErr)
            minErr = err;
        lhist2 = lhist1;
        lhist1 = samp_clamp(testSamp >> 11);
    }

    /* Error-containment logarithm */
    int expLog = 0;
    for (i=0 ; i<16 ; ++i)
    {
        if (1<<(i+3) > maxErr && 1<<(i+3) >= -minErr)
        {
            expLog = i;
            break;
        }
    }

#if ALSA_PLAY
    int16_t alsa_buf[BLOCK_SAMPLES];
#endif

    /* Final predictor pass */
    lhist1 = *hist1;
    lhist2 = *hist2;
    char errors[BLOCK_SAMPLES];
    for (s=0 ; s<BLOCK_SAMPLES ; ++s)
    {
        int expSamp = samps[s] << 11;
        int testSamp = lhist1 * a1best[bestI] + lhist2 * a2best[bestI];
        int err = expSamp - testSamp - 1024;
        err >>= 11;
        errors[s] = err >> expLog & 0xf;

        /* Decoder predictor to track history state (and profile error) */
        int lastPred = nibble_to_int[errors[s]] << expLog;
        lastPred <<= 11;
        lastPred += 1024 + testSamp;
        lhist2 = lhist1;
        lhist1 = samp_clamp(lastPred >> 11);

#if ALSA_PLAY
        alsa_buf[s] = lhist1;
        alsa_buf[s] = samps[s];
#endif

        /* PROFILE - sample error */
        //printf("%d\t%d\t%d\t%d\t%d\n", bestI, expLog, samps[s], lhist1, samps[s] - lhist1);
        ERROR_AVG += samps[s] - lhist1;
        ++ERROR_SAMP_COUNT;
    }
    *hist1 = lhist1;
    *hist2 = lhist2;

#if ALSA_PLAY
    snd_pcm_writei(ALSA_PCM, alsa_buf, BLOCK_SAMPLES);
    fwrite(alsa_buf, 2, BLOCK_SAMPLES, ALSA_WAVE);
#endif

    /* Write block out */
    blockOut[0] = bestI << 4 | expLog;
    for (s=0 ; s<BLOCK_SAMPLES ; s+=2)
        blockOut[s/2+1] = (errors[s] & 0xf) << 4 | (errors[s+1] & 0xf);
}

int main(int argc, char** argv)
{
    int i,p;

    if (argc < 3)
        return -1;

#if ALSA_PLAY
    snd_pcm_open(&ALSA_PCM, "default", SND_PCM_STREAM_PLAYBACK, 0);
    snd_pcm_hw_params_t *hwparams;
    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_hw_params_any(ALSA_PCM, hwparams);
    snd_pcm_hw_params_set_access(ALSA_PCM, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(ALSA_PCM, hwparams, SND_PCM_FORMAT_S16_LE);
    unsigned int sample_rate = 32000;
    snd_pcm_hw_params_set_rate_near(ALSA_PCM, hwparams, &sample_rate, 0);
    snd_pcm_hw_params_set_channels(ALSA_PCM, hwparams, 1);
    snd_pcm_hw_params(ALSA_PCM, hwparams);
    char wave_path[1024];
    snprintf(wave_path, 1024, "%s.wav", argv[2]);
    ALSA_WAVE = fopen(wave_path, "wb");
    for (i=0 ; i<11 ; ++i)
        fwrite("\0\0\0\0", 1, 4, ALSA_WAVE);
#endif

    printf("\e[?25l"); /* hide the cursor */

    FILE* fin = fopen(argv[1], "rb");

    fseek(fin, 0, SEEK_END);
    int sampCount = ftell(fin) / 2;
    fseek(fin, 0, SEEK_SET);
    int packetCount = sampCount / BLOCK_SAMPLES;

    int16_t* sampsBuf = calloc(sampCount + 2, 2);
    int16_t* samps = &sampsBuf[2];
    fread(samps, sampCount, 2, fin);
    fclose(fin);

    /* Extrapolate initial prev-samp */
    int16_t da = samps[1] - samps[0];
    samps[-1] = samps[0] - da;
    samps[-2] = samps[-1] - da;

    /* Build distribution set */
    struct coef_pair_vote* votesBegin = calloc(VOTE_ALLOC_COUNT, sizeof(struct coef_pair_vote));
    struct coef_pair_vote* votesEnd = votesBegin;
    struct coef_pair_vote* votesMax = &votesBegin[VOTE_ALLOC_COUNT];
    for (p=0 ; p<packetCount ; ++p)
    {
        adpcm_block_vote(&votesBegin, &votesEnd, &votesMax, &samps[p*BLOCK_SAMPLES]);
        size_t vc = votesEnd - votesBegin;
        printf("\rCORRELATE [ %d / %d ] %zu votes          ", p+1, packetCount, vc);
    }
    printf("\n");

    /* Filter distribution set to statistically best */
    int16_t a1best[16] = {};
    int16_t a2best[16] = {};
    for (i=0 ; i<16 ; ++i)
    {
        unsigned bestVotes = 0;
        struct coef_pair_vote* it;
        struct coef_pair_vote* bestIt = NULL;
        for (it = votesBegin ; it < votesEnd ; ++it)
        {
            if (it->votes > bestVotes)
            {
                bestVotes = it->votes;
                bestIt = it;
            }
        }
        if (bestIt)
        {
            a1best[i] = bestIt->a1;
            a2best[i] = bestIt->a2;
            bestIt->votes = 0;
        }
    }

    /* Open output file */
    FILE* fout = fopen(argv[2], "wb");
    int16_t prev1 = __builtin_bswap16(samps[-1]);
    int16_t prev2 = __builtin_bswap16(samps[-2]);
    fwrite(&prev1, 1, 2, fout);
    fwrite(&prev2, 1, 2, fout);
    for (i=0 ; i<16 ; ++i)
    {
        int16_t bsa = __builtin_bswap16(a1best[i]);
        int16_t bsb = __builtin_bswap16(a2best[i]);
        fwrite(&bsa, 1, 2, fout);
        fwrite(&bsb, 1, 2, fout);
    }

    /* Execute encoding-predictor for each block */
    int hist1 = samps[-1];
    int hist2 = samps[-2];
    char block[8];
    for (p=0 ; p<packetCount ; ++p)
    {
        adpcm_block_predictor(&hist1, &hist2, a1best, a2best, &samps[p*BLOCK_SAMPLES], block);
        fwrite(block, 1, 8, fout);
        printf("\rPREDICT [ %d / %d ]          ", p+1, packetCount);
    }
    printf("\nDONE! %d samples processed\n", packetCount * BLOCK_SAMPLES);
    printf("\e[?25h"); /* show the cursor */

    printf("ERROR: %lld\n", ERROR_AVG / ERROR_SAMP_COUNT);

#if ALSA_PLAY
    snd_pcm_drain(ALSA_PCM);
    snd_pcm_close(ALSA_PCM);
    uint32_t totalSz = ftell(ALSA_WAVE) - 8;
    fseek(ALSA_WAVE, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, ALSA_WAVE);
    fwrite(&totalSz, 1, 4, ALSA_WAVE);
    fwrite("WAVE", 1, 4, ALSA_WAVE);
    fwrite("fmt ", 1, 4, ALSA_WAVE);
    uint32_t sixteen = 16;
    uint16_t one = 1;
    uint32_t threetwok = 32000;
    uint32_t sixfourk = 64000;
    uint16_t two = 2;
    uint16_t sixteens = 16;
    fwrite(&sixteen, 1, 4, ALSA_WAVE);
    fwrite(&one, 1, 2, ALSA_WAVE);
    fwrite(&one, 1, 2, ALSA_WAVE);
    fwrite(&threetwok, 1, 4, ALSA_WAVE);
    fwrite(&sixfourk, 1, 4, ALSA_WAVE);
    fwrite(&two, 1, 2, ALSA_WAVE);
    fwrite(&sixteens, 1, 2, ALSA_WAVE);
    fwrite("data", 1, 4, ALSA_WAVE);
    totalSz -= 36;
    fwrite(&totalSz, 1, 4, ALSA_WAVE);
    fclose(ALSA_WAVE);
#endif

    fclose(fout);
    free(votesBegin);
    free(sampsBuf);
    return 0;
}

