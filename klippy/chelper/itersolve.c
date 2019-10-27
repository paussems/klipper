// Iterative solver for kinematic moves
//
// Copyright (C) 2018-2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <math.h> // fabs
#include <string.h> // memset
#include "compiler.h" // __visible
#include "itersolve.h" // itersolve_gen_steps
#include "pyhelper.h" // errorf
#include "stepcompress.h" // queue_append_start
#include "trapq.h" // struct move

struct timepos {
    double time, position;
};

// Find step using "false position" method
static struct timepos
itersolve_find_step(struct stepper_kinematics *sk, struct move *m
                    , struct timepos low, struct timepos high
                    , double target)
{
    sk_calc_callback calc_position_cb = sk->calc_position_cb;
    struct timepos best_guess = high;
    low.position -= target;
    high.position -= target;
    if (!high.position)
        // The high range was a perfect guess for the next step
        return best_guess;
    int high_sign = signbit(high.position);
    if (high_sign == signbit(low.position))
        // The target is not in the low/high range - return low range
        return (struct timepos){ low.time, target };
    for (;;) {
        double guess_time = ((low.time*high.position - high.time*low.position)
                             / (high.position - low.position));
        if (fabs(guess_time - best_guess.time) <= .000000001)
            break;
        best_guess.time = guess_time;
        best_guess.position = calc_position_cb(sk, m, guess_time);
        double guess_position = best_guess.position - target;
        int guess_sign = signbit(guess_position);
        if (guess_sign == high_sign) {
            high.time = guess_time;
            high.position = guess_position;
        } else {
            low.time = guess_time;
            low.position = guess_position;
        }
    }
    return best_guess;
}

// Generate step times for a stepper during a move
int32_t __visible
itersolve_gen_steps(struct stepper_kinematics *sk, struct move *m)
{
    struct stepcompress *sc = sk->sc;
    sk_calc_callback calc_position_cb = sk->calc_position_cb;
    double half_step = .5 * sk->step_dist;
    double mcu_freq = stepcompress_get_mcu_freq(sc);
    struct timepos last = { 0., sk->commanded_pos }, low = last, high = last;
    double seek_time_delta = 0.000100;
    int sdir = stepcompress_get_step_dir(sc);
    struct queue_append qa = queue_append_start(sc, m->print_time, .5);
    for (;;) {
        // Determine if next step is in forward or reverse direction
        double dist = high.position - last.position;
        if (fabs(dist) < half_step) {
        seek_new_high_range:
            if (high.time >= m->move_t)
                // At end of move
                break;
            // Need to increase next step search range
            low = high;
            high.time = last.time + seek_time_delta;
            seek_time_delta += seek_time_delta;
            if (high.time > m->move_t)
                high.time = m->move_t;
            high.position = calc_position_cb(sk, m, high.time);
            continue;
        }
        int next_sdir = dist > 0.;
        if (unlikely(next_sdir != sdir)) {
            // Direction change
            if (fabs(dist) < half_step + .000000001)
                // Only change direction if going past midway point
                goto seek_new_high_range;
            if (last.time >= low.time && high.time > last.time) {
                // Must seek new low range to avoid re-finding previous time
                high.time = (last.time + high.time) * .5;
                high.position = calc_position_cb(sk, m, high.time);
                continue;
            }
            int ret = queue_append_set_next_step_dir(&qa, next_sdir);
            if (ret)
                return ret;
            sdir = next_sdir;
        }
        // Find step
        double target = last.position + (sdir ? half_step : -half_step);
        struct timepos next = itersolve_find_step(sk, m, low, high, target);
        // Add step at given time
        int ret = queue_append(&qa, next.time * mcu_freq);
        if (ret)
            return ret;
        seek_time_delta = next.time - last.time;
        if (seek_time_delta < .000000001)
            seek_time_delta = .000000001;
        last.position = target + (sdir ? half_step : -half_step);
        last.time = next.time;
        low = next;
        if (last.time >= high.time)
            // The high range is no longer valid - recalculate it
            goto seek_new_high_range;
    }
    queue_append_finish(qa);
    sk->commanded_pos = last.position;
    if (sk->post_cb)
        sk->post_cb(sk);
    return 0;
}

void __visible
itersolve_set_stepcompress(struct stepper_kinematics *sk
                           , struct stepcompress *sc, double step_dist)
{
    sk->sc = sc;
    sk->step_dist = step_dist;
}

double __visible
itersolve_calc_position_from_coord(struct stepper_kinematics *sk
                                   , double x, double y, double z)
{
    struct move m;
    memset(&m, 0, sizeof(m));
    move_fill(&m, 0., 0., 1., 0., x, y, z, 0., 1., 0., 0., 1., 0.);
    return sk->calc_position_cb(sk, &m, 0.);
}

void __visible
itersolve_set_commanded_pos(struct stepper_kinematics *sk, double pos)
{
    sk->commanded_pos = pos;
}

double __visible
itersolve_get_commanded_pos(struct stepper_kinematics *sk)
{
    return sk->commanded_pos;
}
