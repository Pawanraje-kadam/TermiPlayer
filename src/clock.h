/* clock.h — the A/V master clock.
 *
 * Principle 1 of the plan: audio is the master clock, video follows. If the
 * terminal cannot keep up, frames are dropped; audio never stutters and never
 * speeds up.
 *
 * Two details make this more than "compare two numbers":
 *
 *  1. SMOOTHING. Raw device positions jitter. We keep an exponential moving
 *     average of the difference between the audio clock's *rate* and real time,
 *     and correct against the smoothed value, or video visibly judders.
 *
 *  2. SEEK BEHAVIOUR. After a seek the clock jumps and the cushion is stale.
 *     We hold the clock until the first post-seek frame arrives, then re-base,
 *     rather than letting video race to catch a position nothing is playing at.
 *
 * The decision thresholds mirror ffplay's compute_target_delay: small
 * differences are absorbed by waiting, large ones by dropping or repeating.
 */
#ifndef TP_CLOCK_H
#define TP_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ACT_SHOW,      /* draw this frame now */
    ACT_HOLD,      /* drop it: we are behind, skip to the next */
    ACT_WAIT,      /* too early: sleep for act.wait_ns then show */
} FrameAction;

typedef struct {
    FrameAction action;
    int64_t     wait_ns;
    double      av_diff;
} FrameDecision;

typedef struct Clock Clock;

Clock *clock_new(void);
void   clock_free(Clock *c);

/* Feed the master clock. Call once per loop with the audio sink's position (or
 * the monotonic fallback). `now_ns` is the loop timestamp. */
void clock_set_audio_position(Clock *c, int64_t frames, int rate, int64_t now_ns);

/* Fallback master when there is no audio at all. */
void clock_start_monotonic(Clock *c, int64_t now_ns);

/* What we believe the playhead is, in seconds, smoothed. */
double clock_position(const Clock *c);
/* Raw (unsmoothed) position, for tests and --stats. */
double clock_position_raw(const Clock *c);

void clock_set_paused(Clock *c, bool paused, int64_t now_ns);
bool clock_paused(const Clock *c);

/* Playback rate (0.25 .. 4.0). */
void   clock_set_speed(Clock *c, double speed);
double clock_speed(const Clock *c);

/* Jump the master clock (after a seek). Holds the next decision until the
 * first frame for the new position is presented. */
void clock_seek(Clock *c, double seconds, int64_t now_ns);

/* Decide what to do with a video frame that is due at `pts` seconds. */
FrameDecision clock_decide(Clock *c, double pts, int64_t now_ns, int64_t frame_duration_ns);

/* Counters for --stats. */
typedef struct {
    int64_t shown, dropped, repeated;
    double  av_diff_ms;      /* last decision */
    double  av_diff_avg_ms;  /* running mean */
} ClockStats;

ClockStats clock_stats(const Clock *c);

#endif /* TP_CLOCK_H */
