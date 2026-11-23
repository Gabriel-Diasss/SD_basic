#include "wav_player.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

extern I2S_HandleTypeDef hi2s3;           /* ajuste se usar outro I2S */
#define WAV_I2S hi2s3

#define BUF_SAMPLES 16384                 /* int16 no total (estéreo intercalado) */
#define HALF        (BUF_SAMPLES / 2)

static int16_t dma_buf[BUF_SAMPLES];

static volatile uint8_t half0_free = 0, half1_free = 0;

void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *h) { if (h == &WAV_I2S) half0_free = 1; }
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *h)     { if (h == &WAV_I2S) half1_free = 1; }

static uint16_t volume = 256;

typedef struct {
    uint16_t fmt, ch, bits;
    uint32_t rate, data_size;
} wav_info_t;

void wav_set_volume(uint16_t vol) { volume = (vol > 256) ? 256 : vol; }


static int wav_parse(FIL *f, wav_info_t *w) {
    uint8_t h[12], ck[8], fm[16];
    UINT br;
    int got_fmt = 0;

    if (f_read(f, h, 12, &br) != FR_OK || br != 12) return -1;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;

    while (f_read(f, ck, 8, &br) == FR_OK && br == 8) {
        uint32_t sz = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((uint32_t)ck[7] << 24);

        if (!memcmp(ck, "fmt ", 4)) {
            if (f_read(f, fm, 16, &br) != FR_OK || br != 16) return -1;
            w->fmt  = fm[0]  | (fm[1] << 8);
            w->ch   = fm[2]  | (fm[3] << 8);
            w->rate = fm[4]  | (fm[5] << 8) | (fm[6] << 16) | ((uint32_t)fm[7] << 24);
            w->bits = fm[14] | (fm[15] << 8);
            if (sz > 16) f_lseek(f, f_tell(f) + (sz - 16));
            got_fmt = 1;
        } else if (!memcmp(ck, "data", 4)) {
            w->data_size = sz;
            return got_fmt ? 0 : -1;
        } else {
            f_lseek(f, f_tell(f) + sz + (sz & 1));   /* pula LIST, etc. */
        }
    }
    return -1;
}

static volatile FRESULT read_err = FR_OK;

static uint32_t read_chunked(FIL *f, void *dst, uint32_t want) {
    uint8_t *p = (uint8_t *)dst;
    uint32_t total = 0;
    while (want) {
        UINT n = (want > 512) ? 512 : want;
        UINT br = 0;
        FRESULT r = f_read(f, p, n, &br);
        if (r != FR_OK) { read_err = r; break; }
        total += br; p += br; want -= br;
        if (br < n) break;
    }
    return total;
}


/* Preenche meio buffer. Retorna quantos bytes reais foram lidos do arquivo. */
static uint32_t fill_half(FIL *f, int16_t *dst, const wav_info_t *w, uint32_t *remaining) {
    UINT br = 0;
    uint32_t frames = HALF / 2;

    if (w->ch == 2) {
        uint32_t want = frames * 4;
        if (want > *remaining) want = *remaining;
        /* estéreo */
        br = read_chunked(f, dst, want);      /* era f_read(f, dst, want, &br); */
        memset((uint8_t *)dst + br, 0, frames * 4 - br);
    } else {
        static int16_t tmp[HALF / 2];
        uint32_t want = frames * 2;
        if (want > *remaining) want = *remaining;
        /* mono */
        br = read_chunked(f, tmp, want);      /* era f_read(f, tmp, want, &br); */
        uint32_t n = br / 2;
        for (uint32_t i = 0; i < n; i++) dst[2 * i] = dst[2 * i + 1] = tmp[i];
        memset(&dst[2 * n], 0, (frames - n) * 4);
    }
    *remaining -= br;

    if (volume < 256)
        for (uint32_t i = 0; i < HALF; i++) dst[i] = (int16_t)((dst[i] * volume) >> 8);

    return br;
}

static void set_rate(uint32_t rate) {
    if (WAV_I2S.Init.AudioFreq == rate) return;
    HAL_I2S_DeInit(&WAV_I2S);
    WAV_I2S.Init.AudioFreq = rate;
    HAL_I2S_Init(&WAV_I2S);
}

FRESULT wav_play(const char *path) {
    FIL f;
    wav_info_t w;

    FRESULT res = f_open(&f, path, FA_READ);
    if (res != FR_OK) { printf("wav: f_open %d\r\n", res); return res; }

    if (wav_parse(&f, &w) || w.fmt != 1 || w.bits != 16 || w.ch < 1 || w.ch > 2) {
        printf("wav: formato nao suportado\r\n");
        f_close(&f);
        return FR_INVALID_OBJECT;
    }
    printf("wav: %lu Hz, %u canais, %lu bytes\r\n", w.rate, w.ch, w.data_size);

    set_rate(w.rate);

    uint32_t remaining = w.data_size;
    fill_half(&f, &dma_buf[0],    &w, &remaining);
    fill_half(&f, &dma_buf[HALF], &w, &remaining);

    half0_free = half1_free = 0;
    HAL_I2S_Transmit_DMA(&WAV_I2S, (uint16_t *)dma_buf, BUF_SAMPLES);

    uint32_t underruns = 0, max_ms = 0, n = 0;
    uint8_t silent_halves = 0;

    while (silent_halves < 2) {
        if (half0_free) {
            half0_free = 0;
            uint32_t t0 = HAL_GetTick();
            if (fill_half(&f, &dma_buf[0], &w, &remaining) == 0) silent_halves++;
            uint32_t dt = HAL_GetTick() - t0;
            if (dt > max_ms) max_ms = dt;
            if (half1_free) underruns++;
            n++;
        }
        if (half1_free) {
            half1_free = 0;
            uint32_t t0 = HAL_GetTick();
            if (fill_half(&f, &dma_buf[HALF], &w, &remaining) == 0) silent_halves++;
            uint32_t dt = HAL_GetTick() - t0;
            if (dt > max_ms) max_ms = dt;
            if (half0_free) underruns++;
            n++;
        }
        if (n >= 20) {
            n = 0;
            printf("fill max=%lu ms (limite 93) underruns=%lu\r\n", max_ms, underruns);
        }
    }
    printf("fim: remaining=%lu read_err=%d\r\n", remaining, read_err);
    HAL_I2S_DMAStop(&WAV_I2S);
    f_close(&f);
    return FR_OK;
}
