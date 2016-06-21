#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

void DSPCorrelateCoefs(short* source, int samples, short* coefs);
void DSPEncodeFrame(short* source, int samples, unsigned char* dest, short* coefs);

#define VOTE_ALLOC_COUNT 1024
#define CORRELATE_SAMPLES 0x3800 /* 1024 packets */
#define PACKET_SAMPLES 14

#ifdef __linux__
#define ALSA_PLAY 1
#endif
#if ALSA_PLAY
#include <alsa/asoundlib.h>
snd_pcm_t* ALSA_PCM;
#endif
FILE* WAVE_FILE_OUT = NULL;

/* Standard DSPADPCM header */
struct dspadpcm_header
{
    uint32_t num_samples;
    uint32_t num_nibbles;
    uint32_t sample_rate;
    uint16_t loop_flag;
    uint16_t format; /* 0 for ADPCM */
    uint32_t loop_start;
    uint32_t loop_end;
    uint32_t zero;
    int16_t coef[16];
    int16_t gain;
    int16_t ps;
    int16_t hist1;
    int16_t hist2;
    int16_t loop_ps;
    int16_t loop_hist1;
    int16_t loop_hist2;
    uint16_t pad[11];
};


int main(int argc, char** argv)
{
    int i,p,s;

    if (argc < 3)
    {
        printf("Usage: dspenc <wavin> <dspout>\n");
        return 0;
    }

    FILE* fin = fopen(argv[1], "rb");
    if (!fin)
    {
        fprintf(stderr, "'%s' won't open - %s\n", argv[1], strerror(errno));
        fclose(fin);
        return -1;
    }
    char riffcheck[4];
    fread(riffcheck, 1, 4, fin);
    if (memcmp(riffcheck, "RIFF", 4))
    {
        fprintf(stderr, "'%s' not a valid RIFF file\n", argv[1]);
        fclose(fin);
        return -1;
    }
    fseek(fin, 4, SEEK_CUR);
    fread(riffcheck, 1, 4, fin);
    if (memcmp(riffcheck, "WAVE", 4))
    {
        fprintf(stderr, "'%s' not a valid WAVE file\n", argv[1]);
        fclose(fin);
        return -1;
    }

    uint32_t samplerate = 0;
    uint32_t samplecount = 0;
    while (fread(riffcheck, 1, 4, fin) == 4)
    {
        uint32_t chunkSz;
        fread(&chunkSz, 1, 4, fin);
        if (!memcmp(riffcheck, "fmt ", 4))
        {
            uint16_t fmt;
            fread(&fmt, 1, 2, fin);
            if (fmt != 1)
            {
                fprintf(stderr, "'%s' has invalid format %u\n", argv[1], fmt);
                fclose(fin);
                return -1;
            }

            uint16_t nchan;
            fread(&nchan, 1, 2, fin);
            if (nchan != 1)
            {
                fprintf(stderr, "'%s' must have 1 channel, not %u\n", argv[1], nchan);
                fclose(fin);
                return -1;
            }

            fread(&samplerate, 1, 4, fin);
            fseek(fin, 4, SEEK_CUR);

            uint16_t bytesPerSample;
            fread(&bytesPerSample, 1, 2, fin);
            if (bytesPerSample != 2)
            {
                fprintf(stderr, "'%s' must have 2 bytes per sample, not %u\n", argv[1], bytesPerSample);
                fclose(fin);
                return -1;
            }

            uint16_t bitsPerSample;
            fread(&bitsPerSample, 1, 2, fin);
            if (bitsPerSample != 16)
            {
                fprintf(stderr, "'%s' must have 16 bits per sample, not %u\n", argv[1], bitsPerSample);
                fclose(fin);
                return -1;
            }
        }
        else if (!memcmp(riffcheck, "data", 4))
        {
            samplecount = chunkSz / 2;
            break;
        }
        else
            fseek(fin, chunkSz, SEEK_CUR);
    }

    if (!samplerate || !samplecount)
    {
        fprintf(stderr, "'%s' must have a valid data chunk following a fmt chunk\n", argv[1]);
        fclose(fin);
        return -1;
    }

#if ALSA_PLAY
    snd_pcm_open(&ALSA_PCM, "default", SND_PCM_STREAM_PLAYBACK, 0);
    snd_pcm_hw_params_t *hwparams;
    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_hw_params_any(ALSA_PCM, hwparams);
    snd_pcm_hw_params_set_access(ALSA_PCM, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(ALSA_PCM, hwparams, SND_PCM_FORMAT_S16_LE);
    unsigned int sample_rate = samplerate;
    snd_pcm_hw_params_set_rate_near(ALSA_PCM, hwparams, &sample_rate, 0);
    snd_pcm_hw_params_set_channels(ALSA_PCM, hwparams, 1);
    snd_pcm_hw_params(ALSA_PCM, hwparams);
#endif

    char wavePathOut[1024];
    snprintf(wavePathOut, 1024, "%s.wav", argv[2]);
    WAVE_FILE_OUT = fopen(wavePathOut, "wb");
    for (i=0 ; i<11 ; ++i)
        fwrite("\0\0\0\0", 1, 4, WAVE_FILE_OUT);

    printf("\e[?25l"); /* hide the cursor */

    int packetCount = ((samplecount / PACKET_SAMPLES) + PACKET_SAMPLES - 1) & ~(PACKET_SAMPLES - 1);
    size_t sampsBufSz = (packetCount * PACKET_SAMPLES + 2) * 2;
    int16_t* sampsBuf = malloc(sampsBufSz);
    memset(sampsBuf, 0, sampsBufSz);
    int16_t* samps = sampsBuf+2;
    fread(sampsBuf, samplecount, 2, fin);
    fclose(fin);

    int16_t coefs[16];
    DSPCorrelateCoefs(samps, samplecount, coefs);

    /* Open output file */
    FILE* fout = fopen(argv[2], "wb");
    struct dspadpcm_header header = {};
    header.num_samples = __builtin_bswap32(packetCount * PACKET_SAMPLES);
    header.num_nibbles = __builtin_bswap32(packetCount * 16);
    header.sample_rate = __builtin_bswap32(samplerate);
    for (i=0 ; i<16 ; ++i)
        header.coef[i] = __builtin_bswap16(coefs[i]);
    header.hist1 = __builtin_bswap16(samps[-1]);
    header.hist2 = __builtin_bswap16(samps[-2]);

    /* Execute encoding-predictor for each block */
    int16_t convSamps[16] = {samps[-2], samps[-1]};
    unsigned char block[8];
    for (p=0 ; p<packetCount ; ++p)
    {
        for (s=0 ; s<PACKET_SAMPLES ; ++s)
            convSamps[s+2] = samps[p*PACKET_SAMPLES+s];

        DSPEncodeFrame(convSamps, PACKET_SAMPLES, block, coefs);

#if ALSA_PLAY
        snd_pcm_writei(ALSA_PCM, convSamps+2, PACKET_SAMPLES);
#endif
        fwrite(convSamps+2, 2, PACKET_SAMPLES, WAVE_FILE_OUT);

        convSamps[0] = convSamps[14];
        convSamps[1] = convSamps[15];
        
        if (p == 0)
        {
            header.ps = __builtin_bswap16(block[0]);
            fwrite(&header, 1, sizeof(header), fout);
        }

        fwrite(block, 1, 8, fout);
        if (!(p%48))
            printf("\rPREDICT [ %d / %d ]          ", p+1, packetCount);
    }
    printf("\rPREDICT [ %d / %d ]          ", p, packetCount);
    printf("\nDONE! %d samples processed\n", packetCount * PACKET_SAMPLES);
    printf("\e[?25h"); /* show the cursor */

    //printf("ERROR: %ld\n", ERROR_AVG / ERROR_SAMP_COUNT);

    uint32_t totalSz = ftell(WAVE_FILE_OUT) - 8;
    fseek(WAVE_FILE_OUT, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, WAVE_FILE_OUT);
    fwrite(&totalSz, 1, 4, WAVE_FILE_OUT);
    fwrite("WAVE", 1, 4, WAVE_FILE_OUT);
    fwrite("fmt ", 1, 4, WAVE_FILE_OUT);
    uint32_t sixteen = 16;
    uint16_t one = 1;
    uint32_t threetwok = samplerate;
    uint32_t sixfourk = samplerate * 2;
    uint16_t two = 2;
    uint16_t sixteens = 16;
    fwrite(&sixteen, 1, 4, WAVE_FILE_OUT);
    fwrite(&one, 1, 2, WAVE_FILE_OUT);
    fwrite(&one, 1, 2, WAVE_FILE_OUT);
    fwrite(&threetwok, 1, 4, WAVE_FILE_OUT);
    fwrite(&sixfourk, 1, 4, WAVE_FILE_OUT);
    fwrite(&two, 1, 2, WAVE_FILE_OUT);
    fwrite(&sixteens, 1, 2, WAVE_FILE_OUT);
    fwrite("data", 1, 4, WAVE_FILE_OUT);
    totalSz -= 36;
    fwrite(&totalSz, 1, 4, WAVE_FILE_OUT);
    fclose(WAVE_FILE_OUT);

    fclose(fout);
    free(sampsBuf);

#if ALSA_PLAY
    snd_pcm_drain(ALSA_PCM);
    snd_pcm_close(ALSA_PCM);
#endif

    return 0;
}

