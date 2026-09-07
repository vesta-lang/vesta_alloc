/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file support/bench_stats.h
 * @brief How a benchmark in this library turns samples into ONE number, and
 *        how it says whether that number can be believed.
 *
 * This is the sampling discipline `bench_memcpy.cpp` and `bench_memset.cpp`
 * arrived at, lifted out so a third benchmark does not have to arrive at it
 * again.  Those two still carry their own copies -- identical, verified with
 * `diff` -- and should move over here; they are not touched now because they
 * are open in another working session.  Two copies of a summarising rule that
 * drift apart give tables that disagree without ever looking wrong, so this
 * note is part of the file, not a courtesy.
 *
 * TWO SUMMARIES, NOT ONE, because a time and a ratio do not have the same kind
 * of noise:
 *
 *   - @c summarize, for TIMES.  Timing noise can only ADD -- a context switch,
 *     an interrupt, a frequency drop -- never subtract.  So once sorted, the
 *     low samples are the clean ones and the high samples are themselves plus
 *     contamination.  Keeping the bottom half throws the contamination away
 *     without having to pick a threshold.
 *   - @c summarize_ratio, for RATIOS.  A ratio has no such property: the noise
 *     can land in the numerator and push it up, or in the denominator and pull
 *     it down.  Keeping the bottom half there would not be cleaning, it would
 *     be BIASING -- towards the flattering side or the damning one depending on
 *     the row.  So it trims both ends instead.
 *
 * And why the mean of what survives rather than the minimum: the minimum is the
 * least contaminated sample, but it is ONE sample -- little bias, a lot of
 * sampling variance -- which is why two runs in a row could order things
 * differently.  Averaging what is left cuts that variance without letting back
 * in what was already thrown out.
 *
 * @code
 * double t[kReps];
 * for (int r = 0; r < kReps; ++r) t[r] = measure_once();
 * double spread = 0.0;
 * const double ns = bench_stats::summarize(t, kReps, &spread);
 * // `spread` is how far the median sits from the best: near zero means the
 * // machine was quiet and this row's verdict can be trusted.
 * @endcode
 */

#ifndef VESTA_SUPPORT_BENCH_STATS_H
#define VESTA_SUPPORT_BENCH_STATS_H

namespace bench_stats {

/**
 * @brief Sorts @p v in place.
 *
 * Insertion sort on purpose: the arrays here are a couple of dozen samples, and
 * at that size it beats anything with a better exponent while fitting in a
 * dozen lines that need no explaining.
 *
 * @param v Samples, sorted in place.
 * @param n How many.
 */
inline void sort_samples(double *v, int n) {
    for (int i = 1; i < n; ++i) {
        const double key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = key;
    }
}

/**
 * @brief Summarises TIME samples: mean of the clean half, with the noise apart.
 *
 * Both at once, because each one covers what the other cannot.  See the file
 * header for why the bottom half is the clean one and why it is a mean and not
 * the minimum.
 *
 * The noise is measured with what was discarded: how far the median sits from
 * the best sample.  Close together means the machine was quiet and this row can
 * name a winner; far apart means it cannot, however good the ratio looks.
 *
 * @param v         Samples; SORTED IN PLACE, so the caller loses their order.
 * @param n         How many.
 * @param out_noise Where to leave how far the median sits from the minimum,
 *                  relative to the minimum.
 * @return The mean of the bottom half.
 */
inline double summarize(double *v, int n, double *out_noise) {
    sort_samples(v, n);

    const double best = v[0];
    const double med = v[n / 2];
    *out_noise = best > 0.0 ? (med - best) / best : 0.0;

    const int keep = n / 2 > 0 ? n / 2 : 1;
    double sum = 0.0;
    for (int i = 0; i < keep; ++i)
        sum += v[i];
    return sum / double(keep);
}

/**
 * @brief Summarises RATIO samples, which do not summarise like times.
 *
 * Trimmed at BOTH ends -- the top quarter and the bottom quarter go, the middle
 * is averaged.  Symmetric, robust, and it still uses half the samples instead
 * of the single one a median would use.
 *
 * @param v         Samples; SORTED IN PLACE.
 * @param n         How many.
 * @param out_noise Where to leave the spread, measured as what separates the
 *                  two quartiles divided by the centre.
 * @return The mean of the middle.
 */
inline double summarize_ratio(double *v, int n, double *out_noise) {
    sort_samples(v, n);

    const int q1 = n / 4;
    const int q3 = n - 1 - n / 4;
    const double mid = v[n / 2];
    *out_noise = mid > 0.0 ? (v[q3] - v[q1]) / mid : 0.0;

    double sum = 0.0;
    int cnt = 0;
    for (int i = q1; i <= q3; ++i) {
        sum += v[i];
        ++cnt;
    }
    return cnt > 0 ? sum / double(cnt) : mid;
}

} // namespace bench_stats

#endif // VESTA_SUPPORT_BENCH_STATS_H
