#include "internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HWA_TRACK_MAX_PEAKS 32U

void hwa_partial_tracking_options_default(HWAPartialTrackingOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->max_peaks = 12U;
    options->min_peak_dbfs = -100.0;
    options->relative_floor_db = -30.0;
    options->max_step_cents = 100.0;
    options->max_points = 2000000U;
    options->max_work_bytes = UINT64_C(536870912);
    options->max_evaluations = UINT64_C(100000000);
}

void hwa_partial_tracks_free(HWAPartialTracks *result)
{
    if (result == NULL) return;
    free(result->points);
    memset(result, 0, sizeof(*result));
}

static int charge(HWAPartialTracks *result, char *error, size_t error_size)
{
    if (result->evaluations == result->options.max_evaluations) {
        hwa_set_error(error, error_size, "partial tracking evaluation limit exceeded");
        return -1;
    }
    result->evaluations++;
    return 0;
}

static int by_bin(const void *a, const void *b)
{
    size_t first = ((const HWAPartialTrackPoint *)a)->bin_index;
    size_t second = ((const HWAPartialTrackPoint *)b)->bin_index;
    return first < second ? -1 : first > second ? 1 : 0;
}

int hwa_track_note_phase_partials(const HWANotePhaseSpectraResult *source,
    const HWAPartialTrackingOptions *options, HWAPartialTracks *result,
    char *error, size_t error_size)
{
    HWAPartialTrackingOptions copied;
    HWAPartialTrackPoint previous[HWA_TRACK_MAX_PEAKS];
    size_t previous_count = 0U;
    size_t capacity;
    size_t row;
    size_t bins;
    size_t fft_size;
    uint64_t retained;
    const HWANotePhaseResult *summary;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (result == NULL) {
        hwa_set_error(error, error_size, "partial tracking result is required");
        return -1;
    }
    if (options != NULL) copied = *options;
    else hwa_partial_tracking_options_default(&copied);
    memset(result, 0, sizeof(*result));
    if (source == NULL || copied.max_peaks == 0U || copied.max_peaks > HWA_TRACK_MAX_PEAKS ||
        !isfinite(copied.min_peak_dbfs) || copied.min_peak_dbfs < -300.0 || copied.min_peak_dbfs > 0.0 ||
        !isfinite(copied.relative_floor_db) || copied.relative_floor_db < -300.0 || copied.relative_floor_db > 0.0 ||
        !isfinite(copied.max_step_cents) || copied.max_step_cents <= 0.0 || copied.max_step_cents > 1200.0 ||
        copied.max_points == 0U || copied.max_work_bytes == 0U || copied.max_evaluations == 0U) {
        hwa_set_error(error, error_size, "invalid partial tracking input or options");
        return -1;
    }
    summary = &source->series.envelope.summary;
    fft_size = summary->options.measurement.fft_size;
    bins = source->bin_count;
    if (fft_size < 256U || fft_size > 16384U || (fft_size & (fft_size-1U)) != 0U ||
        bins != fft_size/2U+1U || summary->format.sample_rate_hz == 0U ||
        summary->options.measurement.hop_size == 0U || summary->options.measurement.hop_size > fft_size ||
        (source->series.frame_count != 0U && (source->bin_powers == NULL || source->series.frames == NULL)) ||
        source->series.frame_count > SIZE_MAX / bins / sizeof(double) ||
        source->series.frame_count > UINT64_MAX / (bins * sizeof(double) + sizeof(HWANotePhaseFrame)) ||
        source->series.frame_count > SIZE_MAX / copied.max_peaks / sizeof(HWAPartialTrackPoint) ||
        source->series.frame_count > SIZE_MAX / sizeof(HWANotePhaseFrame)) {
        hwa_set_error(error, error_size, "invalid partial tracking spectrum shape");
        return -1;
    }
    capacity = source->series.frame_count * copied.max_peaks;
    retained = (uint64_t)source->series.frame_count * (uint64_t)(bins * sizeof(double) + sizeof(HWANotePhaseFrame));
    if (summary->path != NULL) {
        size_t length = strlen(summary->path);
        if ((uint64_t)length + 1U > UINT64_MAX - retained) goto work_limit;
        retained += (uint64_t)length + 1U;
    }
    if (capacity > copied.max_points || retained > copied.max_work_bytes ||
        (uint64_t)capacity * sizeof(HWAPartialTrackPoint) > copied.max_work_bytes - retained) goto work_limit;
    if (capacity != 0U) {
        result->points = calloc(capacity, sizeof(*result->points));
        if (result->points == NULL) {
            hwa_set_error(error, error_size, "cannot allocate partial tracks");
            return -1;
        }
    }
    result->options = copied;
    for (row = 0U; row < source->series.frame_count; ++row) {
        const HWANotePhaseFrame *frame = &source->series.frames[row];
        const double *power = source->bin_powers + row * bins;
        HWAPartialTrackPoint peaks[HWA_TRACK_MAX_PEAKS];
        unsigned char used[HWA_TRACK_MAX_PEAKS] = {0U};
        size_t count = 0U;
        size_t candidates = 0U;
        size_t bin;
        double maximum = 0.0;
        double threshold;
        if (frame->phase_index >= HWA_NOTE_PHASE_COUNT ||
            summary->phases[frame->phase_index].status != HWA_NOTE_PHASE_VALID ||
            frame->center_sample < summary->phases[frame->phase_index].start_sample ||
            frame->center_sample >= summary->phases[frame->phase_index].end_sample ||
            (row != 0U && frame->center_sample <= source->series.frames[row-1U].center_sample)) {
            hwa_set_error(error, error_size, "invalid partial tracking frame clock");
            goto fail;
        }
        if (row == 0U || frame->center_sample - source->series.frames[row-1U].center_sample !=
                            summary->options.measurement.hop_size) previous_count = 0U;
        for (bin = 0U; bin < bins; ++bin) {
            if (charge(result, error, error_size) != 0) goto fail;
            if (!isfinite(power[bin]) || power[bin] < 0.0) {
                hwa_set_error(error, error_size, "invalid partial tracking bin power");
                goto fail;
            }
            if (power[bin] > maximum) maximum = power[bin];
        }
        threshold = fmax(pow(10.0, copied.min_peak_dbfs/10.0),
                         maximum * pow(10.0, copied.relative_floor_db/10.0));
        for (bin = 1U; bin+1U < bins; ++bin) {
            HWAPartialTrackPoint point;
            size_t insert;
            double left;
            double middle;
            double right;
            double curve;
            double offset;
            if (charge(result, error, error_size) != 0) goto fail;
            if (power[bin] < threshold || !(power[bin] > power[bin-1U]) || power[bin] < power[bin+1U]) continue;
            candidates++;
            memset(&point, 0, sizeof(point));
            point.frame_index = row;
            point.bin_index = bin;
            point.bin_power = power[bin];
            left = log(fmax(power[bin-1U], 1e-300));
            middle = log(power[bin]);
            right = log(fmax(power[bin+1U], 1e-300));
            curve = left - 2.0*middle + right;
            offset = curve < 0.0 ? 0.5*(left-right)/curve : 0.0;
            offset = fmax(-0.5, fmin(0.5, offset));
            point.frequency_hz = ((double)bin + offset) * (double)summary->format.sample_rate_hz / (double)fft_size;
            insert = 0U;
            while (insert < count && peaks[insert].bin_power >= point.bin_power) insert++;
            if (insert < copied.max_peaks) {
                size_t end = count < copied.max_peaks ? count++ : count-1U;
                while (end > insert) { peaks[end] = peaks[end-1U]; end--; }
                peaks[insert] = point;
            }
        }
        result->omitted_peak_count += (uint64_t)(candidates - count);
        for (bin = 0U; bin < count; ++bin) {
            size_t prior;
            size_t best = previous_count;
            double distance = copied.max_step_cents;
            for (prior = 0U; prior < previous_count; ++prior) {
                double cents;
                if (charge(result, error, error_size) != 0) goto fail;
                if (used[prior]) continue;
                cents = fabs(1200.0 * log2(peaks[bin].frequency_hz / previous[prior].frequency_hz));
                if (cents <= distance && (best == previous_count || cents < distance ||
                    previous[prior].track_id < previous[best].track_id)) {
                    best = prior;
                    distance = cents;
                }
            }
            peaks[bin].continued = best != previous_count;
            if (best != previous_count) {
                used[best] = 1U;
                peaks[bin].track_id = previous[best].track_id;
            } else peaks[bin].track_id = ++result->track_count;
        }
        qsort(peaks, count, sizeof(*peaks), by_bin);
        if (count != 0U) memcpy(result->points + result->point_count, peaks, count * sizeof(*peaks));
        result->point_count += count;
        memcpy(previous, peaks, count * sizeof(*peaks));
        previous_count = count;
    }
    return 0;
work_limit:
    hwa_set_error(error, error_size, "partial tracks exceed the point or work limit");
    return -1;
fail:
    hwa_partial_tracks_free(result);
    return -1;
}
