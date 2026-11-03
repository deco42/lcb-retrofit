#ifndef LCBD_EVALUATOR_H
#define LCBD_EVALUATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "sensorproto.h"

#define EVAL_POINTS_MAX 300
#define EVAL_SLOPES_MAX 600
#define EVAL_REF_FRAMES 30 // good reference frames needed before measuring starts

typedef struct {
    float x, y;
} EvalPoint;

// Results of the last frame
typedef struct {
    float leg_angle;
    float die_angle;
    EvalPoint ref;
    bool done;      // reference done
    int candidates; // last index
    int good_refs;
    int die_points;   // of the last reference frame
    int die_fits;     // since the last reference restart
    int leg_points;   // last index
    int profile_last; // last index
    bool ref_frame;
    float section[3];
    float full_line;
    float line_angle;
    bool valid;
} EvalResult;

typedef struct {
    LegLineParams par;
    EvalPoint ref_point; // top edge of the die, from the last reference frame
    EvalPoint last_leg_end;
    double die_a, die_b; // die line y = die_a * x + die_b
    double die_normal;   // slope of the normal of the die line
    float die_angle;
    int good_refs;
    int fits;  // since the last reference restart
    bool done; // reference phase finished
    bool ref_frame;
    bool line_ready;                                     // die line accepted for measuring
    bool hold;                                           // a knee found by position tolerates a short die list
    float slope[EVAL_SLOPES_MAX], mean[EVAL_SLOPES_MAX]; // work arrays of the knee and leg searches, never cleared
    EvalResult res;                                      // some values survive frames (line angles, leg points)
} Evaluator;

void evaluator_init(Evaluator *ev, const LegLineParams *par);
void evaluator_restart_reference(Evaluator *ev);
const EvalResult *evaluator_process(Evaluator *ev, const uint8_t frame[FRAME_SIZE]);
static inline bool evaluator_die_found(const Evaluator *ev) { return ev->fits > 0; }
static inline bool evaluator_reference_done(const Evaluator *ev) { return ev->good_refs >= EVAL_REF_FRAMES; }

#endif
