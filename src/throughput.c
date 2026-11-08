#include "throughput.h"
#include "gfx.h"
#include "raster.h"
#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Hysteresis: only change raster when the candidate is either clearly better
 * (>25% larger area) or clearly necessary (<15% of what we need). Without this,
 * a link hovering near a boundary flips quality every few frames, which looks
 * far worse than a slightly lower steady resolution. */
#define UPGRADE_RATIO   1.25
#define DOWNGRADE_RATIO 0.85

/* Keep 35% of the measured bandwidth in reserve: terminal parsers are bursty
 * and the write() measurement under-reports the parser's cost. */
#define HEADROOM 0.65

struct Throughput {
    int cols, rows, cell_w, cell_h;
    int target_fps;

    double bps;             /* EMA bytes/sec */
    double bpc;             /* EMA bytes/cell */
    double decode_ms;

    bool    bpc_valid;
    int64_t last_frame_ns;
    int     forced_w, forced_h;

    int    cur_w, cur_h;
    BackendId last_backend;

    TpAdvice advice;
    char     why_buf[128];
};

Throughput *tp_new(int cols, int rows, int cell_w, int cell_h, int target_fps)
{
    Throughput *t = xcalloc(1, sizeof *t);
    t->cols = cols;
    t->rows = rows;
    t->cell_w = cell_w > 0 ? cell_w : 8;
    t->cell_h = cell_h > 0 ? cell_h : 16;
    t->target_fps = target_fps > 0 ? target_fps : 30;
    t->bps = 0;
    t->bpc = 0;
    t->last_backend = BK__COUNT;
    return t;
}

void tp_free(Throughput *t) { free(t); }

void tp_resize(Throughput *t, int cols, int rows, int cell_w, int cell_h)
{
    t->cols = cols;
    t->rows = rows;
    if (cell_w > 0) t->cell_w = cell_w;
    if (cell_h > 0) t->cell_h = cell_h;
    /* bytes-per-cell was measured at the old geometry; it is still a good
     * starting estimate, but mark it soft so the first frames re-measure fast. */
}

void tp_force_raster(Throughput *t, int w, int h)
{
    t->forced_w = w;
    t->forced_h = h;
}

void tp_observe(Throughput *t, long bytes_written, double frame_dt_s,
                double decode_ms, double entropy, BackendId backend)
{
    /* Throughput must be bytes per unit of WALL TIME, not bytes per unit of
     * syscall time. Measuring bytes/write_s reports absurd figures (gigabytes
     * per second) on a pipe that empties as fast as we fill it, and would make
     * the controller think every link is infinite. Use the frame interval,
     * which is the rate the link actually has to sustain. */
    int64_t now = now_ns();
    if (bytes_written > 0) {
        double dt = frame_dt_s;
        if (dt <= 0.0 && t->last_frame_ns > 0)
            dt = (double)(now - t->last_frame_ns) / 1e9;
        if (dt > 1e-4 && dt < 5.0) {
            double inst = (double)bytes_written / dt;
            /* 0.15 reacts within ~7 frames (~230 ms at 30 fps) and ignores a
             * single hiccup. */
            t->bps = t->bps > 0 ? t->bps * 0.85 + inst * 0.15 : inst;
        }
        t->last_frame_ns = now;
    }

    if (bytes_written > 0 && t->cols > 0 && t->rows > 0) {
        double cells = (double)t->cols * (double)t->rows;
        double inst_bpc = (double)bytes_written / cells;
        /* Re-measure quickly when the backend changes, slowly otherwise. */
        double alpha = (backend != t->last_backend || !t->bpc_valid) ? 1.0 : 0.25;
        t->bpc = t->bpc > 0 ? t->bpc * (1 - alpha) + inst_bpc * alpha : inst_bpc;
        t->bpc_valid = true;
    }

    t->decode_ms = t->decode_ms > 0 ? t->decode_ms * 0.8 + decode_ms * 0.2 : decode_ms;
    (void)entropy;
    t->last_backend = backend;
}

/* Cells we can afford per frame while holding target_fps, given measured
 * bandwidth, bytes-per-cell and the headroom reserve. */
static double affordable_cells(const Throughput *t)
{
    if (t->bps <= 0 || t->bpc <= 0) return (double)t->cols * (double)t->rows;
    double budget_per_frame = t->bps * HEADROOM / (double)t->target_fps;
    return budget_per_frame / t->bpc;
}

TpAdvice tp_advise(Throughput *t, BackendId backend)
{
    TpAdvice *a = &t->advice;
    memset(a, 0, sizeof *a);
    a->target_fps = t->target_fps;
    a->cols = t->cols;
    a->rows = t->rows;
    a->cell_w = t->cell_w;
    a->cell_h = t->cell_h;
    a->bytes_per_sec = t->bps;
    a->bytes_per_cell = t->bpc;
    a->decode_ms = t->decode_ms;
    a->fps_cap = t->target_fps;

    /* max raster the cell box allows, per backend geometry */
    int max_w, max_h;
    Rect all = { 0, 0, t->cols, t->rows };
    TermInfo ti = { 0 };
    ti.cell_w = t->cell_w;
    ti.cell_h = t->cell_h;
    gfx_raster_size(backend, &ti, all, &max_w, &max_h);

    if (t->forced_w > 0 && t->forced_h > 0) {
        a->raster_w = t->forced_w;
        a->raster_h = t->forced_h;
        a->why = "forced by --size";
        return *a;
    }

    bool pixel_backend = (backend == BK_KITTY_SHM || backend == BK_KITTY ||
                          backend == BK_ITERM2 || backend == BK_SIXEL);

    /* shm transport is a fixed ~64 bytes/frame: resolution is free. */
    if (backend == BK_KITTY_SHM) {
        a->raster_w = max_w;
        a->raster_h = max_h;
        a->why = "kitty shared-memory transfer: resolution is not bandwidth-bound";
        return *a;
    }

    if (!t->bpc_valid || t->bps <= 0) {
        /* No measurement yet: start conservative and let the first few frames
         * calibrate. Starting small avoids a multi-megabyte first frame over a
         * slow link. */
        a->raster_w = max_w / 2;
        a->raster_h = max_h / 2;
        a->why = "calibrating: no throughput measured yet, starting at half size";
        return *a;
    }

    double cells = affordable_cells(t);
    if (cells < 1) cells = 1;

    if (pixel_backend) {
        /* For pixel backends the relationship is not linear in cells, but the
         * byte cost still scales with pixel area, so solve for a scale factor. */
        double max_px = (double)max_w * (double)max_h;
        double budget = t->bps * HEADROOM / (double)t->target_fps;
        /* estimate bytes per pixel from the last frame, if we know the cells */
        double bpp = t->bpc / ((double)t->cell_w * (double)t->cell_h);
        if (bpp <= 0) bpp = 3;
        double px = budget / bpp;
        if (px > max_px) px = max_px;
        double scale = max_px > 0 ? sqrt(px / max_px) : 1.0;
        if (scale > 1) scale = 1;
        a->raster_w = (int)((double)max_w * scale);
        a->raster_h = (int)((double)max_h * scale);
    } else {
        /* Blocks: cells map directly to the byte cost. Preserve aspect ratio. */
        double full = (double)t->cols * (double)t->rows;
        double scale = full > 0 ? sqrt(cells / full) : 1.0;
        if (scale > 1) scale = 1;
        a->raster_w = (int)((double)max_w * scale);
        a->raster_h = (int)((double)max_h * scale);
    }

    if (a->raster_w < 8) a->raster_w = 8;
    if (a->raster_h < 4) a->raster_h = 4;
    if (a->raster_w > max_w) a->raster_w = max_w;
    if (a->raster_h > max_h) a->raster_h = max_h;

    /* Hysteresis against the previous choice. */
    if (t->cur_w > 0 && t->cur_h > 0) {
        double prev_area = (double)t->cur_w * t->cur_h;
        double new_area = (double)a->raster_w * a->raster_h;
        if (new_area > prev_area * UPGRADE_RATIO && new_area < prev_area / DOWNGRADE_RATIO) {
            a->raster_w = t->cur_w;
            a->raster_h = t->cur_h;
        }
    }
    t->cur_w = a->raster_w;
    t->cur_h = a->raster_h;

    /* If we are CPU-bound rather than bandwidth-bound, cutting resolution will
     * not help: lower the frame rate instead. Video at 20 fps is far more
     * watchable than video at 30 fps that is 5 fps of duplicated frames. */
    double frame_budget_ms = 1000.0 / (double)t->target_fps;
    if (t->decode_ms > frame_budget_ms * 1.15) {
        int fps = (int)(1000.0 / t->decode_ms);
        if (fps < 5) fps = 5;
        if (fps < a->fps_cap) {
            a->fps_cap = fps;
            snprintf(t->why_buf, sizeof t->why_buf,
                     "CPU-bound (decode %.1f ms > %.1f ms budget): capping at %d fps",
                     t->decode_ms, frame_budget_ms, fps);
            a->why = t->why_buf;
            return *a;
        }
    }

    /* On very slow links, indexed colour halves the bytes for a quality cost
     * that is invisible at 80 columns. Only proposed for the blocks backend. */
    if (backend == BK_BLOCKS24) {
        double need = (double)t->target_fps * t->bpc * (double)t->cols * (double)t->rows;
        a->downgrade_backend = t->bps > 0 && need > t->bps * HEADROOM;
    }

    snprintf(t->why_buf, sizeof t->why_buf,
             "%.1f MB/s measured, %.1f B/cell -> %dx%d at %d fps target",
             t->bps / 1e6, t->bpc, a->raster_w, a->raster_h, a->fps_cap);
    a->why = t->why_buf;
    return *a;
}
