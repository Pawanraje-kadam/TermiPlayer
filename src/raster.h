/* raster.h — the single pixel format that crosses every boundary.
 *
 * Decode produces Raster, backends consume Raster. Nothing else is allowed
 * to know about pixel formats, strides or colour conversion. Keeping one
 * format costs one memcpy in the worst case and removes a whole class of bugs.
 *
 * RGB24, tightly packed, top-down. (Top-down because every consumer — the
 * terminal — is top-down, so we never flip.)
 */
#ifndef TP_RASTER_H
#define TP_RASTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int     w, h;
    uint8_t *px;   /* w * h * 3 bytes, RGB order */
} Raster;

void raster_init(Raster *r);
void raster_free(Raster *r);
/* (Re)allocate to w x h. Returns false on failure. Contents are undefined. */
bool raster_alloc(Raster *r, int w, int h);
/* Zero the pixel buffer. */
void raster_clear(Raster *r);

/* Copy src into dst, scaling with nearest-neighbour.
 * Nearest is deliberate: at terminal resolutions (<= 320 px wide) any filter
 * costs more than it buys, and nearest keeps thin UI lines and text crisp in
 * the testsrc pattern. Box filtering is a TODO if footage proves aliased. */
void raster_scale_nn(const Raster *src, Raster *dst);

/* Copy an axis-aligned region of src into dst, no scaling.
 * Region is clamped to the source; the remainder of dst is left untouched. */
void raster_blit(const Raster *src, int sx, int sy, int sw, int sh,
                 Raster *dst, int dx, int dy);

/* Fill a rectangle with a solid colour (clamped). */
void raster_fill(Raster *r, int x, int y, int w, int h,
                 uint8_t cr, uint8_t cg, uint8_t cb);

/* Draw a 1px rectangle outline (used by testsrc and the HUD in raster mode). */
void raster_rect(Raster *r, int x, int y, int w, int h,
                 uint8_t cr, uint8_t cg, uint8_t cb);

/* Cheap content statistics used by the throughput controller to decide
 * "can I compress / how many palette entries do I need". Sampled, O(stride). */
typedef struct {
    double mean_abs_delta;   /* average |px - px_left| across a sampled grid */
    double entropy_hint;     /* 0 = flat, 1 = noise; derived from the above */
} RasterStats;

RasterStats raster_stats(const Raster *r);

#endif /* TP_RASTER_H */
