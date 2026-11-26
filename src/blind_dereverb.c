#include "blind_dereverb.h"
#include "dsp.h"
#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    LEVELS_PER_SECOND = 100,
    BODY_BLOCKS = 5,
    OFFSET_BLOCKS = 4,
    MIN_TAIL_BLOCKS = 9,
    MAX_TAIL_BLOCKS = 26,
    DECAY_BINS = 26,
    CONSENSUS_BINS = 4
};

typedef struct HwaDereverbRelease {
    size_t start;
    size_t end;
    double decay_seconds;
    size_t decay_bin;
} HwaDereverbRelease;

typedef struct HwaDereverbPhase {
    HwaDspComplex previous;
    HwaDspComplex advance;
    HwaDspComplex curvature;
} HwaDereverbPhase;

typedef struct HwaDereverbFilter {
    size_t length;
    size_t hop;
    double *window;
    double *prediction;
    double *difference;
    HwaDspComplex *spectra;
    HwaDereverbPhase *phases;
} HwaDereverbFilter;

static double hwa_dereverb_levels(const double *samples, unsigned channels, size_t block,
                                 double *levels, size_t count)
{
    size_t histogram[101] = {0U};
    double peak = -INFINITY;
    size_t index;
    size_t accumulated = 0U;
    for (index = 0U; index < count; ++index) {
        size_t first = index * block * channels;
        size_t entries = block * channels;
        double power = 0.0;
        size_t offset;
        for (offset = 0U; offset < entries; ++offset) {
            double sample = samples[first + offset];
            power += sample * sample;
        }
        levels[index] = power > 0.0 ? 10.0 * log10(power / (double)entries) : -INFINITY;
        peak = fmax(peak, levels[index]);
    }
    if (!isfinite(peak)) return INFINITY;
    for (index = 0U; index < count; ++index) {
        double below_peak = fmin(100.0, peak - levels[index]);
        ++histogram[(size_t)below_peak];
    }
    /* Use the quietest tenth of the recording to avoid fitting the noise floor. */
    for (index = 100U; index > 0U; --index) {
        accumulated += histogram[index];
        if (accumulated > count / 10U) break;
    }
    return peak - (double)index;
}

static int hwa_dereverb_release(const double *levels, size_t count, size_t start,
                               double floor, double block_seconds, HwaDereverbRelease *release)
{
    const size_t body_start = start - OFFSET_BLOCKS - BODY_BLOCKS + 1U;
    const size_t body_end = start - OFFSET_BLOCKS;
    double body_max = -INFINITY;
    double body_min = INFINITY;
    double body_sum = 0.0;
    double body;
    double bottom = levels[start];
    double sum_y = 0.0;
    double sum_yy = 0.0;
    double sum_xy = 0.0;
    size_t index;
    size_t available = count - start;
    if (levels[start] < floor + 17.0) return 0;
    /* A steady sounding note must precede a distinct, fast level drop. */
    for (index = body_start; index <= body_end; ++index) {
        body_max = fmax(body_max, levels[index]);
        body_min = fmin(body_min, levels[index]);
        body_sum += levels[index];
    }
    body = body_sum / (double)BODY_BLOCKS;
    if (body_max - body_min > 4.0 || !isfinite(body) ||
        levels[body_start] - levels[body_end] > 2.0 ||
        body - levels[start] < 4.5 || body - levels[start] > 33.0)
        return 0;
    release->end = 0U;
    if (available > MAX_TAIL_BLOCKS) available = MAX_TAIL_BLOCKS;
    for (index = 0U; index < available; ++index) {
        double level = levels[start + index];
        double x = (double)index;
        double y;
        if (level < floor + 8.0 || level > bottom + 5.5) break;
        bottom = fmin(bottom, level);
        y = level - levels[start];
        sum_y += y;
        sum_yy += y * y;
        sum_xy += x * y;
        if (index + 1U >= MIN_TAIL_BLOCKS && y < -8.0) {
            double n = x + 1.0;
            double centered_xx = n * (x * (x + 2.0)) / 12.0;
            double centered_xy = sum_xy - 0.5 * x * sum_y;
            double centered_yy = sum_yy - sum_y * sum_y / n;
            double slope = centered_xy / centered_xx;
            if (slope <= -60.0 * block_seconds / 2.75 &&
                slope >= -60.0 * block_seconds / 0.25 &&
                centered_xy * centered_xy >= 0.80 * centered_xx * centered_yy) {
                double decay = -60.0 * block_seconds / slope;
                release->start = start;
                release->end = start + index + 1U;
                release->decay_seconds = decay;
                release->decay_bin = (size_t)(log(decay / 0.25) / log(1.10));
            }
        }
    }
    return release->end != 0U;
}

static size_t hwa_dereverb_consensus(HwaDereverbRelease *releases, size_t count,
                                    double *decay_seconds)
{
    size_t histogram[DECAY_BINS] = {0U};
    size_t best = 0U;
    size_t best_support = 0U;
    size_t support = 0U;
    size_t index;
    double sum = 0.0;
    for (index = 0U; index < count; ++index)
        ++histogram[releases[index].decay_bin];
    for (index = 0U; index < DECAY_BINS; ++index) {
        support += histogram[index];
        if (index >= CONSENSUS_BINS) support -= histogram[index - CONSENSUS_BINS];
        if (index + 1U >= CONSENSUS_BINS && support > best_support) {
            best = index + 1U - CONSENSUS_BINS;
            best_support = support;
        }
    }
    if (best_support < 2U) return 0U;
    support = 0U;
    for (index = 0U; index < count; ++index) {
        size_t bin = releases[index].decay_bin;
        if (bin >= best && bin < best + CONSENSUS_BINS) {
            sum += releases[index].decay_seconds;
            releases[support++] = releases[index];
        }
    }
    *decay_seconds = sum / (double)support;
    return support;
}

static void hwa_dereverb_filter_free(HwaDereverbFilter *filter)
{
    free(filter->phases);
    free(filter->difference);
    free(filter->spectra);
    free(filter->prediction);
    free(filter->window);
}

static int hwa_dereverb_filter_init(HwaDereverbFilter *filter, unsigned channels,
                                   uint32_t rate, size_t span, uint64_t budget,
                                   char *error, size_t error_size)
{
    size_t length = 256U;
    size_t bins;
    uint64_t bytes;
    while (length * 40U < rate) length *= 2U;
    bins = length / 2U + 1U;
    bytes = (length + 1U) * sizeof(double) +
            bins * sizeof(double) + bins * channels * sizeof(HwaDereverbPhase) +
            length * channels * sizeof(HwaDspComplex) + span * channels * sizeof(double);
    if (bytes > budget) {
        hwa_set_error(error, error_size, "blind dereverberation exceeds the work limit");
        return -1;
    }
    filter->length = length;
    filter->hop = length / 4U;
    filter->window = malloc((length + 1U) * sizeof(*filter->window));
    filter->prediction = malloc(bins * sizeof(*filter->prediction));
    filter->spectra = malloc(length * channels * sizeof(*filter->spectra));
    filter->phases = malloc(bins * channels * sizeof(*filter->phases));
    filter->difference = malloc(span * channels * sizeof(*filter->difference));
    if (filter->window == NULL || filter->prediction == NULL || filter->spectra == NULL ||
        filter->phases == NULL || filter->difference == NULL) {
        hwa_set_error(error, error_size, "cannot allocate blind dereverberation filters");
        return -1;
    }
    /* The first length entries form a periodic Hann. Four squared overlaps sum to 1.5. */
    return hwa_dsp_hann(filter->window, length + 1U);
}

static double hwa_dereverb_phase_weight(HwaDereverbPhase *history, HwaDspComplex bin,
                                        double magnitude)
{
    HwaDspComplex phase = {0.0, 0.0};
    HwaDspComplex previous = history->previous;
    HwaDspComplex advance;
    HwaDspComplex curvature;
    double change;
    if (magnitude > 0.0) {
        phase.real = bin.real / magnitude;
        phase.imag = bin.imag / magnitude;
    }
    advance.real = phase.real * previous.real + phase.imag * previous.imag;
    advance.imag = phase.imag * previous.real - phase.real * previous.imag;
    curvature.real = advance.real * history->advance.real + advance.imag * history->advance.imag;
    curvature.imag = advance.imag * history->advance.real - advance.real * history->advance.imag;
    change = 1.0 - (curvature.real * history->curvature.real + curvature.imag * history->curvature.imag);
    history->previous = phase;
    history->advance = advance;
    history->curvature = curvature;
    /* Smooth phase motion can come from a sustained or changing pitch. */
    return fmin(1.0, fmax(0.0, (change - 0.0005) / 0.02));
}

static int hwa_dereverb_filter_span(HwaDereverbFilter *filter, double *samples, unsigned channels,
                                   size_t start, size_t span, int apply, double decay_step)
{
    size_t length = filter->length;
    size_t bins = length / 2U + 1U;
    size_t offset;
    size_t index;
    unsigned channel;
    double varying_power = 0.0;
    double total_power = 0.0;
    if (apply) memset(filter->difference, 0, span * channels * sizeof(*filter->difference));
    memset(filter->phases, 0, bins * channels * sizeof(*filter->phases));
    for (offset = 0U; offset < span; offset += filter->hop) {
        if (!apply && offset + length > span) break;
        for (channel = 0U; channel < channels; ++channel) {
            HwaDspComplex *spectrum = filter->spectra + length * channel;
            for (index = 0U; index < length; ++index) {
                size_t position = offset + index;
                double sample = position < span ? samples[(start + position) * channels + channel] : 0.0;
                spectrum[index] = (HwaDspComplex){sample * filter->window[index], 0.0};
            }
            if (hwa_dsp_fft(spectrum, length, 0) != 0) return -1;
        }
        for (index = 0U; index < bins; ++index) {
            double power = 0.0;
            double weighted_power = 0.0;
            double change;
            for (channel = 0U; channel < channels; ++channel) {
                HwaDspComplex bin = filter->spectra[length * channel + index];
                double bin_power = bin.real * bin.real + bin.imag * bin.imag;
                HwaDereverbPhase *phase = &filter->phases[bins * channel + index];
                double weight = hwa_dereverb_phase_weight(phase, bin, sqrt(bin_power));
                power += bin_power;
                weighted_power += weight * bin_power;
            }
            if (!apply) {
                if (offset >= 3U * filter->hop) {
                    varying_power += weighted_power;
                    total_power += power;
                }
                continue;
            }
            /* An exponentially decaying estimate from the start of the observed tail. */
            if (offset == 0U) filter->prediction[index] = power;
            change = 0.0;
            if (power > 0.0 && offset >= 3U * filter->hop) {
                double fraction = fmin(1.0, filter->prediction[index] / power);
                double weight = 0.40 + 0.60 * (weighted_power / power);
                change = sqrt(1.0 - 0.75 * fraction * weight) - 1.0;
            }
            filter->prediction[index] *= decay_step;
            for (channel = 0U; channel < channels; ++channel) {
                HwaDspComplex *spectrum = filter->spectra + length * channel;
                spectrum[index].real *= change;
                spectrum[index].imag *= change;
                if (index > 0U && index < length / 2U) {
                    spectrum[length - index].real *= change;
                    spectrum[length - index].imag *= change;
                }
            }
        }
        if (!apply) continue;
        for (channel = 0U; channel < channels; ++channel) {
            HwaDspComplex *spectrum = filter->spectra + length * channel;
            if (hwa_dsp_fft(spectrum, length, 1) != 0) return -1;
            for (index = 0U; index < length && offset + index < span; ++index)
                filter->difference[(offset + index) * channels + channel] +=
                    spectrum[index].real * filter->window[index] / 1.5;
        }
    }
    if (!apply) return varying_power > 0.08 * total_power;
    /* Fade the modification to zero before the end of the supported tail. */
    for (index = 0U; index < span; ++index) {
        double fade = fmin(1.0, (double)(span - 1U - index) / (double)(length / 2U));
        for (channel = 0U; channel < channels; ++channel)
            samples[(start + index) * channels + channel] +=
                fade * filter->difference[index * channels + channel];
    }
    return 0;
}

int hwa_blind_dereverb(double *samples, size_t frames, unsigned channels,
                       uint32_t sample_rate, uint64_t max_work_bytes,
                       HwaBlindDereverbResult *result, char *error, size_t error_size)
{
    HwaDereverbFilter filter = {0};
    HwaDereverbRelease *releases = NULL;
    double *levels = NULL;
    size_t block;
    size_t blocks;
    size_t capacity;
    size_t count = 0U;
    size_t start;
    size_t index;
    uint64_t release_bytes;
    double floor;
    double block_seconds;
    double decay_seconds;
    double decay_step;
    int status = -1;
    if (samples == NULL || result == NULL || (channels != 1U && channels != 2U) ||
        sample_rate < 8000U || sample_rate > 192000U ||
        frames > SIZE_MAX / sizeof(double) / channels) {
        hwa_set_error(error, error_size, "invalid blind dereverberation arguments");
        return -1;
    }
    *result = (HwaBlindDereverbResult){0};
    block = (sample_rate + LEVELS_PER_SECOND / 2U) / LEVELS_PER_SECOND;
    blocks = frames / block;
    if (blocks < 2U * MIN_TAIL_BLOCKS + OFFSET_BLOCKS + BODY_BLOCKS) return 0;
    capacity = blocks / MIN_TAIL_BLOCKS;
    release_bytes = capacity * sizeof(*releases);
    if (release_bytes > max_work_bytes ||
        blocks > (max_work_bytes - release_bytes) / sizeof(*levels)) {
        hwa_set_error(error, error_size, "blind dereverberation exceeds the work limit");
        return -1;
    }
    levels = malloc(blocks * sizeof(*levels));
    releases = malloc(capacity * sizeof(*releases));
    if (levels == NULL || releases == NULL) {
        hwa_set_error(error, error_size, "cannot allocate blind dereverberation analysis");
        goto cleanup;
    }
    floor = hwa_dereverb_levels(samples, channels, block, levels, blocks);
    block_seconds = (double)block / (double)sample_rate;
    start = OFFSET_BLOCKS + BODY_BLOCKS - 1U;
    while (start + MIN_TAIL_BLOCKS <= blocks) {
        HwaDereverbRelease release;
        if (hwa_dereverb_release(levels, blocks, start, floor, block_seconds, &release)) {
            releases[count++] = release;
            start = release.end;
        } else {
            ++start;
        }
    }
    free(levels);
    levels = NULL;
    if (count >= 2U) {
        size_t eligible = 0U;
        if (hwa_dereverb_filter_init(&filter, channels, sample_rate, MAX_TAIL_BLOCKS * block,
                                     max_work_bytes - release_bytes, error, error_size) != 0)
            goto cleanup;
        /* Check phase motion before counting independent room-like releases. */
        for (index = 0U; index < count; ++index) {
            const HwaDereverbRelease *release = &releases[index];
            int supported = hwa_dereverb_filter_span(&filter, samples, channels, release->start * block,
                                                       (release->end - release->start) * block, 0, 0.0);
            if (supported < 0) {
                hwa_set_error(error, error_size, "cannot analyze the inferred room tail");
                goto cleanup;
            }
            if (supported) releases[eligible++] = *release;
        }
        count = hwa_dereverb_consensus(releases, eligible, &decay_seconds);
    } else {
        count = 0U;
    }
    if (count != 0U) {
        decay_step = pow(10.0, -6.0 * (double)filter.hop / ((double)sample_rate * decay_seconds));
        for (index = 0U; index < count; ++index) {
            const HwaDereverbRelease *release = &releases[index];
            if (hwa_dereverb_filter_span(&filter, samples, channels, release->start * block,
                                         (release->end - release->start) * block, 1, decay_step) != 0) {
                hwa_set_error(error, error_size, "cannot filter the inferred room tail");
                goto cleanup;
            }
        }
        *result = (HwaBlindDereverbResult){decay_seconds, count};
    }
    status = 0;
cleanup:
    hwa_dereverb_filter_free(&filter);
    free(releases);
    free(levels);
    return status;
}
