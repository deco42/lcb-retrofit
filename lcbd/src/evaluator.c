#include "evaluator.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "x87.h"

typedef struct {
    double n, sx, sy, sxx, sxy;
} LsSums;

static EvalPoint point_at(const EvalPoint *a, int i)
{
    if (i < 0 || i >= EVAL_POINTS_MAX) {
        return (EvalPoint){0, 0};
    }

    return a[i];
}

static void ls_add(LsSums *sums, EvalPoint p)
{
    sums->n = (double)((ext)sums->n + 1);
    sums->sx = (double)((ext)sums->sx + p.x);
    sums->sy = (double)((ext)sums->sy + p.y);
    sums->sxx = (double)((ext)sums->sxx + (ext)p.x * p.x);
    sums->sxy = (double)((ext)sums->sxy + (ext)p.x * p.y);
}

static ext ls_det(const LsSums *sums) { return (ext)sums->n * sums->sxx - (ext)sums->sx * sums->sx; }

static double ls_slope(const LsSums *sums, ext det)
{
    return (double)(((ext)sums->n * sums->sxy - (ext)sums->sx * sums->sy) / det);
}

static double ls_intercept(const LsSums *sums, ext det)
{
    return (double)(((ext)sums->sxx * sums->sy - (ext)sums->sx * sums->sxy) / det);
}

static float dist_sq(EvalPoint a, EvalPoint b) { return (float)(x87_sq((ext)a.x - b.x) + x87_sq((ext)a.y - b.y)); }

static double distance(EvalPoint p, ext x, ext y)
{
    return sqrt((double)(x87_sq((ext)p.x - x) + x87_sq((ext)p.y - y)));
}

static float atan_deg(double slope) { return (float)(x87_atan(slope) * 180 / X87_PI); }

static ext die_line_y(const Evaluator *ev, ext x) { return x * ev->die_a + ev->die_b; }

// Where the normal of the die line through p meets the die line
static ext die_foot_x(const Evaluator *ev, EvalPoint p)
{
    return (((ext)p.y - (ext)p.x * ev->die_normal) - ev->die_b) / ((ext)ev->die_a - ev->die_normal);
}

#define FIT_KEEP_SQ 150 // the second fit keeps the points closer than sqrt(150) to the first line
#define NO_SECTION 999.999f

// Angle of the least-squares line through pts[from, to), fitted again through the points near the first line
static float line_fit(const EvalPoint *pts, int from, int to)
{
    LsSums s1 = {0}, s2 = {0};
    double a = 0, b = 0, slope = -1;

    for (int i = from; i < to; i++) {
        ls_add(&s1, point_at(pts, i));
    }

    ext det = ls_det(&s1);
    if (det != 0) {
        a = ls_slope(&s1, det);
        b = ls_intercept(&s1, det);
    }

    double pseudo_normal = (double)((ext)a + X87_PI_2);
    for (int i = from; i < to; i++) {
        EvalPoint p = point_at(pts, i);
        double xf = (double)((((ext)p.y - (ext)p.x * pseudo_normal) - b) / ((ext)a - pseudo_normal));
        double yf = (double)((ext)a * xf + b);
        if (x87_sq((ext)xf - p.x) + x87_sq((ext)yf - p.y) < FIT_KEEP_SQ) {
            ls_add(&s2, p);
        }
    }

    det = ls_det(&s2);
    if (det != 0) {
        slope = ls_slope(&s2, det);
    }

    return atan_deg(slope);
}

static ext gap(float a, float b) { return fabsl((ext)b - a); }

static float mean2(float a, float b) { return (float)(((ext)a + b) / 2); }

// In extended mode the leg is cut into two or three sections, and a section whose angle does not agree with the
// others is left out
static float leg_line(const LegLineParams *par, const EvalPoint *pts, int n, EvalResult *res)
{
    int head = n - par->last_section_points; // points before the last section
    int half = head / 2;
    float tol = par->tolerance;
    float lambda;

    if (par->extended && (ext)half * par->cover + half >= par->min_points) {
        int to1 = (int)floorl((ext)half * par->cover + half);
        int to2 = (int)floorl((ext)half * par->cover) + head;
        if (to2 > n) {
            to2 = n;
        }

        float s1 = line_fit(pts, 0, to1), s2 = line_fit(pts, half, to2), s3 = line_fit(pts, head, n);
        float s_all = line_fit(pts, 0, n);

        if (gap(s2, s3) < tol && gap(s1, s3) < tol) {
            lambda = s_all;
        } else if (gap(s2, s3) < tol) {
            lambda = mean2(s2, s3);
        } else if (gap(s1, s3) < tol) {
            lambda = mean2(s1, s3);
        } else if (gap(s1, s2) < tol) {
            lambda = mean2(s1, s2);
        } else {
            lambda = s3;
        }

        res->section[0] = s1;
        res->section[1] = s2;
        res->section[2] = s3;
        res->full_line = s_all;
    } else if (par->extended && (ext)head * par->cover + head >= par->min_points) {
        int to1 = (int)floorl((ext)head * par->cover + head);
        if (to1 > n) {
            to1 = n;
        }

        float s1 = line_fit(pts, 0, to1), s2 = line_fit(pts, head, n), s_all = line_fit(pts, 0, n);
        lambda = gap(s1, s2) < tol ? s_all : s2;
        res->section[0] = s1;
        res->section[1] = s2;
        res->section[2] = NO_SECTION;
        res->full_line = s_all;
    } else {
        lambda = (par->extended && par->last_section_points < n) ? line_fit(pts, head, n) : line_fit(pts, 0, n);
        res->section[0] = res->section[1] = res->section[2] = NO_SECTION;
    }

    return lambda;
}

#define MIN_QUALITY 20
#define REF_MATCH 20

static int read_candidates(const uint8_t *frame, const Evaluator *ev, EvalPoint *cand, int *ref_index)
{
    int n = 0;
    *ref_index = 0;
    for (int i = 0; i < FRAME_RECORDS; i++) {
        const uint8_t *record = frame_record(frame, i);
        if (record_quality(record) <= MIN_QUALITY) {
            continue;
        }

        if (!ev->ref_frame && n > 0 && fabsl((ext)ev->ref_point.x - cand[n - 1].x) < REF_MATCH &&
            fabsl((ext)ev->ref_point.y - cand[n - 1].y) < REF_MATCH) {
            *ref_index = n - 1;
        }

        cand[n].x = (float)((ext)record_x(record) / (ext)FRAME_X_SCALE);
        cand[n].y = (float)record_y(record);
        n++;
    }

    return n;
}

#define FIELD_X_MIN 50
#define FIELD_X_MAX 2680
#define FIELD_Y_MIN 50
#define FIELD_Y_MAX 4046
#define LEG_OFF_DIE_LINE 25
#define LEG_BEFORE_REF 20 // candidates
#define LEG_RADIUS_SQ 2692639
#define DIE_RADIUS_SQ 1514609
#define DIE_LINE_DIST 100

static int build_profile(const Evaluator *ev, const EvalPoint *cand, int n_cand, int ref_index, EvalPoint *profile,
                         int *n_rejected, int *n_die_side)
{
    EvalPoint ref = ev->ref_point;
    bool die_line_test = ev->line_ready && (float)((ext)ev->die_a - ev->die_normal) != 0;
    int n = 0;
    *n_rejected = *n_die_side = 0;

    for (int i = 1; i <= n_cand - 2; i++) {
        EvalPoint p = cand[i];
        bool in_field = p.x > FIELD_X_MIN && p.x < FIELD_X_MAX && p.y > FIELD_Y_MIN && p.y < FIELD_Y_MAX;
        if (!in_field || p.x == cand[i - 1].x) {
            continue;
        }

        if (ev->ref_frame) {
            profile[n++] = p;
        } else if (p.x > ref.x && p.y > ref.y) {
            (*n_rejected)++;
        } else if (p.x <= ref.x || p.y == ref.y) {
            float die_y = (float)die_line_y(ev, p.x);
            bool off_die_line = fabsl((ext)die_y - p.y) > LEG_OFF_DIE_LINE || ref_index - i > LEG_BEFORE_REF;
            bool near_last_leg = !ev->line_ready || dist_sq(p, ev->last_leg_end) < LEG_RADIUS_SQ;
            if (off_die_line && near_last_leg) {
                profile[n++] = p;
            }
        } else if (dist_sq(p, ref) < DIE_RADIUS_SQ) {
            bool keep = true;
            if (die_line_test) {
                float xf = (float)die_foot_x(ev, p);
                float yf = (float)die_line_y(ev, xf);
                keep = p.x < xf || (float)distance(p, xf, yf) < DIE_LINE_DIST;
            }

            if (keep) {
                profile[n++] = p;
                (*n_die_side)++;
            }
        }
    }

    return n - 1;
}

#define CHAIN_START 8
#define CHAIN_MIN_LAST 21
#define CHAIN_X_MIN 1315
#define CHAIN_LOOKBACK 5
#define CHAIN_DY_MIN 25
#define CHAIN_DY_MAX 250
#define CHAIN_DX_MAX 200

static int trace_die_edge(EvalPoint *profile, int last, LsSums *flank)
{
    EvalPoint chain[EVAL_POINTS_MAX];
    int n = 0;
    *flank = (LsSums){0};

    for (int i = last; i >= 0; i--) {
        EvalPoint p = profile[i];
        if (n < CHAIN_START || last < CHAIN_MIN_LAST) {
            if (n < CHAIN_START) {
                chain[n++] = p;
            }
        } else if (p.x >= CHAIN_X_MIN) {
            EvalPoint q = chain[n - CHAIN_LOOKBACK];
            ext dy = (ext)p.y - q.y;
            if (dy > CHAIN_DY_MIN && dy < CHAIN_DY_MAX && fabsl((ext)p.x - q.x) < CHAIN_DX_MAX) {
                chain[n++] = p;
            }
        } else {
            ls_add(flank, p);
        }
    }

    for (int i = 0; i < n; i++) {
        profile[i] = chain[n - 1 - i];
    }

    return n - 1;
}

#define REF_X_MIN 1215
#define REF_X_MAX 1900
#define FIRST_REF_SKIP 5
#define NEXT_REF_SKIP_START 10
#define NEXT_REF_SKIP_END 1
#define DIE_FIT_MIN 21
#define FLANK_MIN 21
#define FLANK_PARALLEL 2

static void reference_frame(Evaluator *ev, const EvalPoint *profile, int last, const LsSums *flank)
{
    EvalResult *res = &ev->res;
    ev->ref_point = profile[0];
    ev->ref_frame = false;
    ev->line_ready = false;
    res->leg_angle = 0;

    if (!(ev->ref_point.x > REF_X_MIN && ev->ref_point.x < REF_X_MAX)) {
        ev->good_refs = 0;
        return;
    }

    bool first = ev->good_refs == 0;
    int from = first ? FIRST_REF_SKIP : NEXT_REF_SKIP_START;
    int to = first ? last - FIRST_REF_SKIP : last - NEXT_REF_SKIP_END;
    LsSums die = {0};
    int n_die = 0;
    EvalPoint previous = {0, 0};

    for (int i = from; i < to; i++) {
        EvalPoint p = profile[i];
        if (!(dist_sq(p, ev->ref_point) < DIE_RADIUS_SQ)) {
            continue;
        }

        if (!first) {
            double xf = (double)die_foot_x(ev, p);
            double yf = (double)die_line_y(ev, xf);
            if (!(p.x <= xf || distance(p, xf, yf) < DIE_LINE_DIST)) {
                continue;
            }
        }

        // The fit leaves out the last die point, so each point is added once the next one is found
        if (n_die > 0) {
            ls_add(&die, previous);
        }

        previous = p;
        n_die++;
    }

    res->die_points = n_die;

    if (n_die > DIE_FIT_MIN) {
        double a = 0, b = 0;
        ext det = ls_det(&die);
        if (det != 0) {
            a = ls_slope(&die, det);
            b = ls_intercept(&die, det);
        }

        ev->good_refs++;
        ev->fits++;
        ev->die_a = (double)(((ext)(ev->good_refs - 1) * ev->die_a + a) / ev->good_refs);
        ev->die_b = (double)(((ext)(ev->good_refs - 1) * ev->die_b + b) / ev->good_refs);
        ev->die_normal = (double)x87_tan((double)(x87_atan(ev->die_a) + X87_PI_2));
        ev->die_angle = atan_deg(ev->die_a);
    }

    if (flank->n >= FLANK_MIN) {
        // The determinant is 0 only for a vertical flank: the slope is infinite (+-90 degrees, correct) or NaN
        float flank_angle = atan_deg(ls_slope(flank, ls_det(flank)));
        if (!isnan(flank_angle) && fabsl((ext)ev->die_angle - flank_angle) < FLANK_PARALLEL) {
            ev->good_refs = 0;
        }
    }
}

#define SLOPE_SPAN 3

static float slope_at(const EvalPoint *profile, int k)
{
    EvalPoint a = point_at(profile, k - SLOPE_SPAN), b = point_at(profile, k);
    if (a.x == b.x) {
        return -1.0f;
    }

    double dy = (double)((ext)a.y - b.y), dx = (double)((ext)a.x - b.x);

    return (float)((ext)dy / dx);
}

static float running_mean(float mean, float value, int n) { return (float)((ext)(n - 1) * mean / n + (ext)value / n); }

#define DEEP_DIE_ANGLE (-68)
#define MAX_REJECTED 35
#define MAX_REJECTED_DEEP 22
#define MIN_DIE_SIDE 24
#define MIN_DIE_SIDE_DEEP 25
#define SLOPE_WARMUP 10
#define KNEE_CHANGE 1 // relative to the running mean
#define KNEE_DROP 250
#define KNEE_DROP_CHANGE 0.5 // relative to the running mean
#define KNEE_SKIP 10
#define KNEE_X_MIN 1000
#define KNEE_X_MAX 2400
#define KNEE_Y_MIN 400
#define KNEE_Y_MAX 3800

static int find_knee_by_slope(Evaluator *ev, const EvalPoint *profile, int *k)
{
    float *slope = ev->slope, *mean = ev->mean;
    int knee = 0, j = 0;

    while (*k > SLOPE_SPAN && knee == 0) {
        j++;
        slope[j] = slope_at(profile, *k);
        mean[j] = j == 1 ? slope[1] : running_mean(mean[j - 1], slope[j], j);
        if (j > SLOPE_WARMUP && slope[j - 1] != 0) {
            ext change = mean[j] == 0 ? 1 : fabsl(((ext)slope[j] - mean[j]) / mean[j]);
            ext drop = (ext)point_at(profile, *k - SLOPE_SPAN).y - point_at(profile, *k).y;
            if (change > KNEE_CHANGE || (drop > KNEE_DROP && change > KNEE_DROP_CHANGE)) {
                knee = *k;
                *k -= KNEE_SKIP;
            }
        }

        (*k)--;
    }

    return knee;
}

static int find_knee_by_position(const Evaluator *ev, const EvalPoint *profile, int *k)
{
    int knee = 0;

    while (*k > SLOPE_SPAN && knee == 0) {
        if (point_at(profile, *k).x < ev->ref_point.x) {
            knee = *k;
            *k -= KNEE_SKIP;
        }

        (*k)--;
    }

    return knee;
}

#define CUT_POINT 5
#define CUT_ANGLE_RAD 0.2617994 // 15 degrees steeper than the die line

static int cut_above_knee(const Evaluator *ev, EvalPoint *profile, int k)
{
    int through = k + CUT_POINT;
    if (through <= 0) {
        return k;
    }

    float cut_slope = (float)x87_tan((double)((ext)ev->die_angle * X87_PI_F / 180 + (ext)CUT_ANGLE_RAD));
    EvalPoint q = point_at(profile, through);
    ext intercept = (ext)q.y - (ext)q.x * cut_slope;
    int kept = 0;

    for (int i = 0; i < through; i++) {
        EvalPoint p = point_at(profile, i);
        if ((ext)p.y < (ext)cut_slope * p.x + intercept) {
            profile[kept++] = p;
        }
    }

    return kept - CUT_POINT;
}

#define SLOPE_JUMP 1.0
#define JUMP_RUN 3
#define START_MEAN_FIRST 7
#define START_MEAN_LAST 10
#define START_WARMUP 11

typedef struct {
    int length, start;
} JumpRun;

static void jump_run_add(JumpRun *run, bool jump, int k)
{
    if (!jump) {
        *run = (JumpRun){0, 0};
        return;
    }

    run->length++;
    if (run->start == 0) {
        run->start = k;
    }
}

static int find_leg_start(Evaluator *ev, const EvalPoint *profile, int k)
{
    float *slope = ev->slope, *mean = ev->mean;
    JumpRun run = {0, 0};
    int j = 0, m = 0, n_start = 0;
    double start_sum = 0;

    while (k > SLOPE_SPAN && run.length < JUMP_RUN) {
        j++;
        slope[j] = slope_at(profile, k);
        if (j > START_WARMUP && slope[j - 1] != 0) {
            jump_run_add(&run, fabsl((ext)slope[j] - mean[m]) > SLOPE_JUMP, k);
        }

        if (run.length == 0) {
            if (j <= START_MEAN_LAST) {
                if (j >= START_MEAN_FIRST) {
                    n_start++;
                    start_sum = (double)((ext)start_sum + slope[j]);
                }

                mean[1] = j == START_MEAN_LAST ? (float)((ext)start_sum / n_start) : slope[j];
                m = 1;
            } else {
                mean[m + 1] = running_mean(mean[m], slope[j], m + 1);
                m++;
            }
        }

        k--;
    }

    return run.start;
}

static int find_leg_end(Evaluator *ev, const EvalPoint *profile, int leg_start, int knee)
{
    float *slope = ev->slope, *mean = ev->mean;
    JumpRun run = {0, 0};
    int first = SLOPE_SPAN + 1;
    int k = leg_start > first ? leg_start : first;
    int j = 0;

    while (k < knee && run.length < JUMP_RUN) {
        j++;
        slope[j] = slope_at(profile, k);
        if (j > SLOPE_WARMUP && slope[j - 1] != 0) {
            jump_run_add(&run, fabsl((ext)slope[j] - mean[j - 1]) > SLOPE_JUMP, k);
        }

        if (run.length == 0) {
            mean[j] = j == 1 ? slope[1] : running_mean(mean[j - 1], slope[j], j);
        }

        k++;
    }

    return run.start ? run.start : knee;
}

#define HOLD_KNEE_DIST 20
#define LEG_MARGIN 10
#define LEG_MIN 11
#define LEG_END_MAX_SQ 4e8f
#define LEG_END_DY_MIN (-500)
#define DIE_LIST_START 11
#define LONG_LEG 71
#define HOLD_MIN_LEG 26
#define HOLD_MIN_ANGLE 70
#define MIN_LEG 16
#define MIN_DIE_LIST 10
#define ANGLE_PENDING 999 // leg angle of the frame that accepts the die line, the angle follows with the next frame

static void measuring_frame(Evaluator *ev, EvalPoint *profile, int last, int n_rejected, int n_die_side)
{
    EvalResult *res = &ev->res;
    int k = last;
    int knee;

    bool deep = ev->die_angle < DEEP_DIE_ANGLE;
    if (n_rejected < (deep ? MAX_REJECTED_DEEP : MAX_REJECTED) &&
        n_die_side > (deep ? MIN_DIE_SIDE_DEEP : MIN_DIE_SIDE)) {
        knee = find_knee_by_slope(ev, profile, &k);
    } else {
        ev->hold = true;
        knee = find_knee_by_position(ev, profile, &k);
    }

    if (knee == 0) {
        knee = 1;
    }

    EvalPoint kp = point_at(profile, knee);
    bool knee_ok = kp.x > KNEE_X_MIN && kp.x < KNEE_X_MAX && kp.y > KNEE_Y_MIN && kp.y < KNEE_Y_MAX;

    k = cut_above_knee(ev, profile, k);
    int leg_start = find_leg_start(ev, profile, k);
    int leg_end = find_leg_end(ev, profile, leg_start, knee);
    if (ev->hold && abs(leg_end - knee) > HOLD_KNEE_DIST) {
        ev->hold = false;
    }

    // The leg list is profile[leg_first, leg_end - LEG_MARGIN), the die list profile[knee + DIE_LIST_START, last).
    // n_leg and n_die are their sizes minus one, and the leg line uses only the first n_leg points of the leg list
    int leg_first = leg_start + LEG_MARGIN;
    int n_leg = (leg_end - LEG_MARGIN > leg_first ? leg_end - LEG_MARGIN - leg_first : 0) - 1;
    int n_die = (last - knee - DIE_LIST_START > 0 ? last - knee - DIE_LIST_START : 0) - 1;

    ev->last_leg_end = ev->ref_point;
    if (n_leg >= LEG_MIN) {
        EvalPoint leg_end_point = point_at(profile, leg_first + n_leg);
        if (dist_sq(leg_end_point, ev->ref_point) >= LEG_END_MAX_SQ ||
            (ext)leg_end_point.y - ev->ref_point.y <= LEG_END_DY_MIN) {
            n_leg = 0;
        } else {
            ev->last_leg_end = leg_end_point;
        }
    }

    // Zero padded past the end of the profile array, leg_first >= LEG_MARGIN because leg_start >= 0
    EvalPoint leg[EVAL_POINTS_MAX] = {0};
    if (leg_first < EVAL_POINTS_MAX) {
        memcpy(leg, profile + leg_first, (size_t)(EVAL_POINTS_MAX - leg_first) * sizeof(*leg));
    }

    float lambda = leg_line(&ev->par, leg, n_leg, res);
    res->line_angle = lambda;

    // A short leg gives an angle only while the die line is accepted
    if (n_leg < LONG_LEG) {
        res->leg_points = n_leg; // only refreshed on this path
        if (n_leg < HOLD_MIN_LEG || (ext)lambda - ev->die_angle < HOLD_MIN_ANGLE) {
            ev->hold = false;
        }

        if (n_leg < MIN_LEG || (n_die < MIN_DIE_LIST && !ev->hold)) {
            ev->line_ready = false;
            res->leg_angle = 0;
            return;
        }

        if (!ev->line_ready) {
            res->leg_angle = ANGLE_PENDING;
            ev->line_ready = knee_ok;
            return;
        }
    }

    res->leg_angle = (float)((ext)lambda - ev->die_angle);
}

#define VALID_MIN 10
#define VALID_MAX 91

void evaluator_init(Evaluator *ev, const LegLineParams *par)
{
    memset(ev, 0, sizeof(*ev));
    ev->par = *par;
}

void evaluator_restart_reference(Evaluator *ev)
{
    ev->done = false;
    ev->fits = 0;
    ev->good_refs = 0;
}

const EvalResult *evaluator_process(Evaluator *ev, const uint8_t frame[FRAME_SIZE])
{
    EvalPoint cand[EVAL_POINTS_MAX];
    EvalPoint profile[EVAL_POINTS_MAX] = {0}; // reading past the filled part gives (0, 0)
    EvalResult *res = &ev->res;

    if (!ev->done) {
        if (ev->good_refs < EVAL_REF_FRAMES) {
            ev->ref_frame = true;
        } else {
            ev->done = true;
        }
    }

    int ref_index;
    int n_cand = read_candidates(frame, ev, cand, &ref_index);

    int n_rejected, n_die_side;
    int last = build_profile(ev, cand, n_cand, ref_index, profile, &n_rejected, &n_die_side);

    res->ref_frame = ev->ref_frame;
    if (ev->ref_frame) {
        LsSums flank;
        last = trace_die_edge(profile, last, &flank);
        reference_frame(ev, profile, last, &flank);
    } else {
        measuring_frame(ev, profile, last, n_rejected, n_die_side);
    }

    res->candidates = n_cand - 1;
    res->profile_last = last;
    res->die_angle = ev->die_angle;
    res->ref = ev->ref_point;
    res->done = ev->done;
    res->good_refs = ev->good_refs;
    res->die_fits = ev->fits;
    res->valid =
        !res->ref_frame && !isnan(res->leg_angle) && res->leg_angle >= VALID_MIN && res->leg_angle <= VALID_MAX;

    return res;
}
