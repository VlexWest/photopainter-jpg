/*****************************************************************************
* | File      	:   GUI_JPGfile.c
* | Function    :   Baseline JPEG -> 7-color frame buffer, decoded on device
* | Info        :   TJpgDec (ChaN) streams the image MCU row by MCU row; each
*                   row band is resampled (nearest neighbour) into screen
*                   space and Floyd-Steinberg dithered against the measured
*                   panel palette. Peak RAM on top of the 192 KB frame buffer
*                   is ~45 KB, so a 12 MP phone photo fits into the RP2040.
******************************************************************************/
#include "GUI_JPGfile.h"
#include "GUI_Paint.h"
#include "tjpgd.h"
#include "ff.h"
#include "hardware/watchdog.h"

#include <stdlib.h>
#include <string.h>

#define OUT_W           800
#define OUT_H           480
#define WORK_SZ         (12 * 1024)  /* TJpgDec pool: JD_FASTDECODE=2 needs ~3.1 KB + 6 KB LUT + JD_SZBUF */
#define BAND_MAX_BYTES  (30 * 1024)
#define PORTRAIT_CW     1           /* portrait photos: 1 = top of the photo to the right, 0 = to the left */

/* Colors as the 7.3" F panel really shows them (Pimoroni "saturation 0.5"
 * blend). Dithering against these instead of pure RGB gives truer tones. */
static const int16_t PAL[7][3] = {
    {28, 24, 28},     /* 0 black  */
    {255, 255, 255},  /* 1 white  */
    {29, 173, 35},    /* 2 green  */
    {30, 29, 175},    /* 3 blue   */
    {205, 36, 37},    /* 4 red    */
    {232, 223, 35},   /* 5 yellow */
    {216, 123, 36},   /* 6 orange */
};

typedef struct {
    FIL fil;
    uint8_t orient;         /* EXIF orientation: 1, 3, 6 or 8 */
    int sw, sh;             /* decoded (scaled) source size */
    int cx, cy, cw, ch;     /* crop rectangle, source pixels */
    int rw, rh;             /* rendered size, in source orientation */
    int ox, oy;             /* screen offset of the rendered area */
    uint16_t srcx[OUT_W];   /* rendered column -> source column */
    uint16_t srcy[OUT_W];   /* rendered row -> source row (rh <= 800) */
    uint16_t *band;         /* RGB565 ring of bandRows x rw */
    int bandRows;
    int dyNext;             /* first rendered row not yet dithered */
    int dxNext;             /* first rendered column not yet filled, current MCU row */
    int16_t *errCur, *errNext;  /* Floyd-Steinberg error, (rw + 2) * 3, x16 */
} JpgCtx;

/* ---------- EXIF orientation ---------- */

static uint16_t rd16(const uint8_t *p, int le) { return le ? (p[0] | p[1] << 8) : (p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p, int le) { return le ? (rd16(p, 1) | (uint32_t)rd16(p + 2, 1) << 16) : ((uint32_t)rd16(p, 0) << 16 | rd16(p + 2, 0)); }

static uint8_t read_exif_orient(FIL *fil)
{
    uint8_t b[12];
    UINT br;

    if (f_read(fil, b, 2, &br) != FR_OK || br != 2 || b[0] != 0xFF || b[1] != 0xD8) return 1;
    for (int seg = 0; seg < 16; seg++) {
        if (f_read(fil, b, 4, &br) != FR_OK || br != 4 || b[0] != 0xFF) return 1;
        uint8_t marker = b[1];
        unsigned len = b[2] << 8 | b[3];
        if (len < 2 || marker == 0xDA || marker == 0xD9) return 1;   /* SOS/EOI: no EXIF */
        FSIZE_t next = f_tell(fil) + len - 2;
        if (marker == 0xE1) {
            if (f_read(fil, b, 6, &br) != FR_OK || br != 6 || memcmp(b, "Exif\0\0", 6) != 0) return 1;
            FSIZE_t tiff = f_tell(fil);
            if (f_read(fil, b, 8, &br) != FR_OK || br != 8) return 1;
            int le = (b[0] == 'I');
            uint32_t ifd = rd32(b + 4, le);
            if (f_lseek(fil, tiff + ifd) != FR_OK || f_read(fil, b, 2, &br) != FR_OK || br != 2) return 1;
            unsigned n = rd16(b, le);
            if (n > 64) n = 64;
            for (unsigned i = 0; i < n; i++) {
                if (f_read(fil, b, 12, &br) != FR_OK || br != 12) return 1;
                if (rd16(b, le) == 0x0112) {
                    uint16_t o = rd16(b + 8, le);
                    return (o == 3 || o == 6 || o == 8) ? o : 1;
                }
            }
            return 1;
        }
        if (f_lseek(fil, next) != FR_OK) return 1;
    }
    return 1;
}

/* ---------- decode callbacks ---------- */

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    JpgCtx *c = (JpgCtx *)jd->device;
    UINT br = 0;
    watchdog_update();
    if (buf) {
        if (f_read(&c->fil, buf, n, &br) != FR_OK) return 0;
        return br;
    }
    return f_lseek(&c->fil, f_tell(&c->fil) + n) == FR_OK ? n : 0;
}

/* rendered (dx, dy) -> screen (X, Y), undoing the EXIF rotation */
static inline void put_px(const JpgCtx *c, int dx, int dy, uint8_t idx)
{
    int X, Y;
    switch (c->orient) {
    case 3:  X = c->ox + c->rw - 1 - dx; Y = c->oy + c->rh - 1 - dy; break;
    case 6:  X = c->ox + c->rh - 1 - dy; Y = c->oy + dx;             break;
    case 8:  X = c->ox + dy;             Y = c->oy + c->rw - 1 - dx; break;
    default: X = c->ox + dx;             Y = c->oy + dy;             break;
    }
    Paint_SetPixel(X, Y, idx);
}

static void dither_row(JpgCtx *c, int dy)
{
    const uint16_t *row = c->band + (dy % c->bandRows) * c->rw;
    int16_t *ec = c->errCur, *en = c->errNext;

    memset(en, 0, (c->rw + 2) * 3 * sizeof(int16_t));
    for (int dx = 0; dx < c->rw; dx++) {
        uint16_t p = row[dx];
        int v[3] = { (p >> 8) & 0xF8, (p >> 3) & 0xFC, (p << 3) & 0xF8 };
        int best = 0, bestd = 1 << 30;
        for (int k = 0; k < 3; k++) {
            v[k] += ec[(dx + 1) * 3 + k] / 16;
            if (v[k] < 0) v[k] = 0; else if (v[k] > 255) v[k] = 255;
        }
        for (int i = 0; i < 7; i++) {
            int d = 0;
            for (int k = 0; k < 3; k++) { int e = v[k] - PAL[i][k]; d += e * e; }
            if (d < bestd) { bestd = d; best = i; }
        }
        for (int k = 0; k < 3; k++) {
            int e = v[k] - PAL[best][k];
            ec[(dx + 2) * 3 + k] += e * 7;
            en[(dx + 0) * 3 + k] += e * 3;
            en[(dx + 1) * 3 + k] += e * 5;
            en[(dx + 2) * 3 + k] += e * 1;
        }
        put_px(c, dx, dy, (uint8_t)best);
    }
    c->errCur = en;
    c->errNext = ec;
}

static int jpg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    JpgCtx *c = (JpgCtx *)jd->device;
    const uint8_t *pix = (const uint8_t *)bitmap;
    int bw = rect->right - rect->left + 1;

    watchdog_update();
    if (c->dyNext >= c->rh) return 0;           /* crop done, stop decoding */
    if (rect->left == 0) c->dxNext = 0;         /* new MCU row */

    /* rendered rows/columns that sample from this block */
    int dy0 = c->dyNext, dy1 = dy0;
    while (dy1 < c->rh && c->srcy[dy1] <= rect->bottom) dy1++;
    int dx0 = c->dxNext, dx1 = dx0;
    while (dx1 < c->rw && c->srcx[dx1] <= rect->right) dx1++;

    for (int y = dy0; y < dy1; y++) {
        const uint8_t *srow = pix + (c->srcy[y] - rect->top) * bw * 3;
        uint16_t *drow = c->band + (y % c->bandRows) * c->rw;
        for (int x = dx0; x < dx1; x++) {
            const uint8_t *p = srow + (c->srcx[x] - rect->left) * 3;
            drow[x] = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
        }
    }
    c->dxNext = dx1;

    if (rect->right + 1 >= c->sw) {             /* last block of the MCU row */
        for (int y = dy0; y < dy1; y++) dither_row(c, y);
        c->dyNext = dy1;
    }
    return 1;
}

/* ---------- geometry ---------- */

/* Fill crop/resample tables for the given TJpgDec scale, return band bytes. */
static size_t setup_geometry(JpgCtx *c, const JDEC *jd, int scale)
{
    int rot = (c->orient == 6 || c->orient == 8);
    c->sw = jd->width >> scale;
    c->sh = jd->height >> scale;
    int Ws = rot ? c->sh : c->sw, Hs = rot ? c->sw : c->sh;   /* as displayed */
    int RW = OUT_W, RH = OUT_H, CW, CH;         /* fill the screen, center-crop the rest */

    if (Ws * OUT_H > Hs * OUT_W) { CH = Hs; CW = Hs * OUT_W / OUT_H; }
    else                         { CW = Ws; CH = Ws * OUT_H / OUT_W; }
    c->ox = (OUT_W - RW) / 2;
    c->oy = (OUT_H - RH) / 2;
    if (rot) { c->rw = RH; c->rh = RW; c->cw = CH; c->ch = CW; }
    else     { c->rw = RW; c->rh = RH; c->cw = CW; c->ch = CH; }
    c->cx = (c->sw - c->cw) / 2;
    c->cy = (c->sh - c->ch) / 2;
    for (int dx = 0; dx < c->rw; dx++) c->srcx[dx] = (uint16_t)(c->cx + dx * c->cw / c->rw);
    for (int dy = 0; dy < c->rh; dy++) c->srcy[dy] = (uint16_t)(c->cy + dy * c->ch / c->rh);

    int mcuh = (jd->msy * 8) >> scale;
    if (mcuh < 1) mcuh = 1;
    c->bandRows = (mcuh * c->rh + c->ch - 1) / c->ch + 2;
    return (size_t)c->bandRows * c->rw * sizeof(uint16_t);
}

UBYTE GUI_ReadJpg_7Color(const char *path, const char **err)
{
    JDEC jd;
    JRESULT rc;
    UBYTE ret = 1;
    JpgCtx *c = NULL;
    void *work = NULL;

    *err = NULL;
    printf("open %s\r\n", path);
    if (!(c = calloc(1, sizeof *c)) || !(work = malloc(WORK_SZ))) { *err = "Out of memory"; goto out; }
    if (f_open(&c->fil, path, FA_READ) != FR_OK) { *err = "Cannot open file"; goto out; }
    c->orient = read_exif_orient(&c->fil);
    f_lseek(&c->fil, 0);

    rc = jd_prepare(&jd, jpg_in, work, WORK_SZ, c);
    if (rc != JDR_OK) {
        printf("jd_prepare %d\r\n", rc);
        *err = (rc == JDR_FMT3) ? "Progressive JPEG not supported" : "Not a valid JPEG";
        goto close;
    }
    /* portrait photo: turn it by 90 degrees so it fills the landscape panel */
    int rot = (c->orient == 6 || c->orient == 8);
    if ((rot ? jd.height : jd.width) < (rot ? jd.width : jd.height)) {
        static const uint8_t cw[9]  = {0, 6, 0, 8, 0, 0, 3, 0, 1};   /* orient -> orient + 90 deg CW */
        static const uint8_t ccw[9] = {0, 8, 0, 6, 0, 0, 1, 0, 3};
        c->orient = PORTRAIT_CW ? cw[c->orient] : ccw[c->orient];
        rot = !rot;
    }
    printf("jpeg %d x %d, orient %d\r\n", jd.width, jd.height, c->orient);

    /* largest 1/2^scale that still leaves >= 800x480 */
    int W = rot ? jd.height : jd.width, H = rot ? jd.width : jd.height;
    int scale = 0;
    for (int s = 3; s >= 1; s--) {
        if ((H >> s) >= OUT_H && (W >> s) >= OUT_W) { scale = s; break; }
    }
    size_t bandBytes = 0;
    for (; scale <= 3; scale++) {               /* shrink further if the band would not fit */
        bandBytes = setup_geometry(c, &jd, scale);
        if (bandBytes <= BAND_MAX_BYTES) break;
    }
    if (scale > 3) { *err = "Image too small"; goto close; }
    printf("scale 1/%d, render %dx%d at %d,%d, band %u B\r\n", 1 << scale, c->rw, c->rh, c->ox, c->oy, (unsigned)bandBytes);

    size_t errN = (size_t)(c->rw + 2) * 3;
    c->errCur = calloc(errN, sizeof(int16_t));
    c->errNext = calloc(errN, sizeof(int16_t));
    c->band = malloc(bandBytes);
    if (!c->errCur || !c->errNext || !c->band) { *err = "Out of memory"; goto close; }

    rc = jd_decomp(&jd, jpg_out, (uint8_t)scale);
    if (rc != JDR_OK && rc != JDR_INTR) {       /* JDR_INTR: we stopped early after the crop */
        printf("jd_decomp %d\r\n", rc);
        *err = "JPEG decode error";
        goto close;
    }
    ret = 0;

close:
    f_close(&c->fil);
out:
    if (c) { free(c->band); free(c->errCur); free(c->errNext); free(c); }
    free(work);
    return ret;
}
