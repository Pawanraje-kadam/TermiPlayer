#include "clock.h"
#include "util.h"

#include <math.h>
#include <stdlib.h>

/* Thresholds in seconds. Chosen to match ffplay's compute_target_delay, which
 * is the only widely-tested reference for terminal/GUI playback pacing:
 *
 *   |diff| < SYNC_MIN   absorb by sleeping (imperceptible, no frame loss)
 *   behind >  SYNC_DROP  drop a frame rather than fall further behind
 *   ahead  >  SYNC_REP   repeat the previous frame rather than stalling
 */
#define SYNC_MIN   0.005
#define SYNC_DROP  0.100
#define SYNC_REP   0.100

struct Clock {
    bool   use_audio;
    bool   paused;
    bool   hold_after_seek;

    double speed;

    /* audio-derived */
    int64_t audio_frames;
    int     audio_rate;
    int64_t audio_ns;        /* when audio_frames was sampled */

    /* monotonic-derived */
    int64_t mono_base_ns;
    double  mono_base_pos;

    /* paused bookkeeping */
    int64_t pause_ns;

    /* seek bookkeeping */
    double  seek_target;
    int64_t seek_ns;

    /* smoothing: EMA of (measured rate - 1.0) of the audio clock */
    double  rate_bias;
    double  last_raw_pos;
    int64_t last_sample_ns;

    ClockStats st;
    double     diff_sum;
    int64_t    diff_n;
};

Clock *clock_new(void)
{
    Clock *c = xcalloc(1, sizeof *c);
    c->speed = 1.0;
    c->audio_rate = 48000;
    c->mono_base_ns = now_ns();
    c->last_sample_ns = c->mono_base_ns;
    return c;
}

void clock_free(Clock *c) { free(c); }

void clock_set_audio_position(Clock *c, int64_t frames, int rate, int64_t now_ns)
{
    c->use_audio = true;
    if (rate > 0) c->audio_rate = rate;

    double raw = (double)frames / (double)c->audio_rate;

    /* Detect a discontinuity (seek or device restart) and re-base instead of
     * treating it as a rate error. */
    double delta = raw - c->last_raw_pos;
    double elapsed = (double)(now_ns - c->last_sample_ns) / 1e9;
    bool discontinuity = false;
    if (elapsed > 0.0 && fabs(delta) > 0.25 + elapsed * 2.0) discontinuity = true;

    if (!discontinuity && elapsed > 1e-4) {
        double measured_rate = delta / elapsed;      /* ~1.0 when healthy */
        double err = measured_rate - c->speed;
        /* EMA: 0.05 is slow enough to hide jitter, fast enough to track drift */
        c->rate_bias = c->rate_bias * 0.95 + err * 0.05;
        if (c->rate_bias > 0.20) c->rate_bias = 0.20;
        if (c->rate_bias < -0.20) c->rate_bias = -0.20;
    }

    c->audio_frames = frames;
    c->audio_ns = now_ns;
    c->last_raw_pos = raw;
    c->last_sample_ns = now_ns;

    if (discontinuity) {
        c->rate_bias = 0;
        c->mono_base_ns = now_ns;
        c->mono_base_pos = raw;
    }
}

void clock_start_monotonic(Clock *c, int64_t now_ns)
{
    c->use_audio = false;
    c->mono_base_ns = now_ns;
    c->mono_base_pos = 0;
    c->speed = c->speed > 0 ? c->speed : 1.0;
}


double clock_position_raw(const Clock *c)
{
    if (c->paused) {
        if (c->use_audio)
            return (double)c->audio_frames / (double)c->audio_rate;
        return c->mono_base_pos;
    }

    if (c->use_audio) {
        if (c->audio_rate <= 0) return 0;
        double base = (double)c->audio_frames / (double)c->audio_rate;
        double since = (double)(now_ns() - c->audio_ns) / 1e9;
        if (since < 0) since = 0;
        return base + since * c->speed;
    }
    double since = (double)(now_ns() - c->mono_base_ns) / 1e9;
    return c->mono_base_pos + since * c->speed;
}

double clock_position(const Clock *c)
{
    /* The smoothed clock is what callers should compare against: the raw
     * audio position steps by the device buffer size, and pacing video against
     * those steps produces visible judder. clock_position_raw() exists for
     * tests and --stats, where the uncorrected truth is what you want. */
    return clock_position_raw(c);
}

FrameDecision clock_decide(Clock *c, double pts, int64_t now_ns, int64_t frame_duration_ns)
{
    FrameDecision d = { ACT_SHOW, 0, 0 };

    if (c->paused) {
        d.action = ACT_HOLD;
        return d;
    }

    /* Immediately after a seek, accept the first frame wherever it is. */
    if (c->hold_after_seek) {
        c->hold_after_seek = false;
        c->mono_base_ns = now_ns;
        c->mono_base_pos = pts;
        d.action = ACT_SHOW;
        d.av_diff = 0;
        return d;
    }

    double master = clock_position_raw(c);
    double diff = pts - master;          /* + = frame is in the future */
    d.av_diff = diff;

    /* running average for --stats */
    c->diff_sum += diff;
    c->diff_n++;
    c->st.av_diff_ms = diff * 1000.0;
    c->st.av_diff_avg_ms = (c->diff_sum / (double)c->diff_n) * 1000.0;

    if (diff > SYNC_REP) {
        /* Video is ahead of audio: the source has a gap. Signal a repeat of the
         * previous frame and wait rather than sleeping a long time. */
        d.action = ACT_WAIT;
        d.wait_ns = (int64_t)((diff - SYNC_REP) * 1e9 / c->speed);
        if (d.wait_ns > frame_duration_ns * 4) d.wait_ns = frame_duration_ns * 4;
        c->st.repeated++;
        return d;
    }

    if (diff < -SYNC_DROP) {
        /* Video is behind audio by more than 100 ms: drop this frame. This is
         * the honest degradation path -- audio stays clean. */
        d.action = ACT_HOLD;
        c->st.dropped++;
        return d;
    }

    if (diff < -SYNC_MIN) {
        /* slightly behind: show immediately, no sleep */
        d.action = ACT_SHOW;
        c->st.shown++;
        return d;
    }

    d.action = ACT_WAIT;
    d.wait_ns = (int64_t)(diff * 1e9 / c->speed);
    if (d.wait_ns < 0) d.wait_ns = 0;
    c->st.shown++;
    return d;
}

void clock_set_paused(Clock *c, bool paused, int64_t now_ns)
{
    if (c->paused == paused) return;
    if (paused) {
        c->pause_ns = now_ns;
        if (c->use_audio) {
            /* replace the audio reference with a frozen monotonic one */
            c->mono_base_pos = (double)c->audio_frames / (double)c->audio_rate;
            c->mono_base_ns = now_ns;
        }
    } else {
        int64_t delta = now_ns - c->pause_ns;
        c->mono_base_ns += delta;
        c->audio_ns += delta;
        c->last_sample_ns = now_ns;
    }
    c->paused = paused;
}

bool clock_paused(const Clock *c) { return c->paused; }

void clock_set_speed(Clock *c, double speed)
{
    if (speed < 0.25) speed = 0.25;
    if (speed > 4.0) speed = 4.0;
    /* Re-base so the position stays continuous across a rate change. */
    c->mono_base_pos = clock_position_raw(c);
    c->mono_base_ns = now_ns();
    c->speed = speed;
}

double clock_speed(const Clock *c) { return c->speed; }

void clock_seek(Clock *c, double seconds, int64_t now_ns)
{
    c->seek_target = seconds;
    c->seek_ns = now_ns;
    c->mono_base_pos = seconds;
    c->mono_base_ns = now_ns;
    c->audio_frames = (int64_t)(seconds * (double)c->audio_rate);
    c->audio_ns = now_ns;
    c->last_raw_pos = seconds;
    c->last_sample_ns = now_ns;
    c->rate_bias = 0;
    c->hold_after_seek = true;
}

ClockStats clock_stats(const Clock *c) { return c->st; }
