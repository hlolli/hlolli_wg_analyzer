#include "speech_temporal.h"

#include "dsp.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TEMPORAL_HOP_MS 5U
#define TEMPORAL_WINDOW_MS 15U
#define TEMPORAL_BANDS 18U
#define TEMPORAL_LEVEL_FLOOR 1e-5
#define TEMPORAL_BAND_FLOOR 1e-4

static const double temporal_band_edges[TEMPORAL_BANDS - 1U] = {
    200.0, 350.0, 500.0, 700.0, 1000.0, 1400.0, 2000.0, 2800.0,
    4000.0, 5600.0, 8000.0, 11300.0, 16000.0, 22400.0, 32000.0,
    45200.0, 64000.0
};

typedef struct TemporalScratch {
    double *audio[2];
    HwaDspComplex *fft;
    double *hann;
    uint64_t start;
    uint64_t end;
    uint64_t bytes;
    size_t fft_size;
    size_t window_size;
    size_t band_count;
    double bin_hz;
} TemporalScratch;

static size_t temporal_ms(unsigned rate, unsigned milliseconds, size_t minimum)
{
    size_t samples = (size_t)(((uint64_t)rate * milliseconds + 500U) / 1000U);
    return samples < minimum ? minimum : samples;
}

static int temporal_check_memory(uint64_t bytes, uint64_t available,
                                  char *error, size_t error_size)
{
    if (bytes > available || bytes > SIZE_MAX) {
        hwa_set_error(error, error_size, "speech temporal comparison exceeds the work memory limit");
        return -1;
    }
    return 0;
}

static int temporal_scratch_open(TemporalScratch *scratch, unsigned rate, size_t window_size,
                                 uint64_t available, char *error, size_t error_size)
{
    scratch->window_size = window_size;
    scratch->fft_size = 1U;
    while (scratch->fft_size < window_size) scratch->fft_size *= 2U;
    scratch->bin_hz = (double)rate / (double)scratch->fft_size;
    scratch->band_count = 1U;
    while (scratch->band_count < TEMPORAL_BANDS &&
           temporal_band_edges[scratch->band_count - 1U] < 0.5 * (double)rate) {
        ++scratch->band_count;
    }
    scratch->bytes = 3U * (uint64_t)window_size * sizeof(double) +
                     (uint64_t)scratch->fft_size * sizeof(*scratch->fft);
    if (temporal_check_memory(scratch->bytes, available, error, error_size) != 0) return -1;

    scratch->audio[0] = malloc(window_size * sizeof(double));
    scratch->audio[1] = malloc(window_size * sizeof(double));
    scratch->hann = malloc(window_size * sizeof(*scratch->hann));
    scratch->fft = malloc(scratch->fft_size * sizeof(*scratch->fft));
    if (scratch->audio[0] == NULL || scratch->audio[1] == NULL ||
        scratch->hann == NULL || scratch->fft == NULL) {
        hwa_set_error(error, error_size, "cannot allocate speech temporal workspace");
        return -1;
    }
    if (hwa_dsp_hann(scratch->hann, window_size) != HWA_DSP_OK) {
        hwa_set_error(error, error_size, "cannot create speech temporal analysis window");
        return -1;
    }
    return 0;
}

static void temporal_scratch_close(TemporalScratch *scratch)
{
    free(scratch->audio[0]);
    free(scratch->audio[1]);
    free(scratch->fft);
    free(scratch->hann);
}

static int temporal_fill(TemporalScratch *scratch, const HWAWavReader *const readers[2],
                         uint64_t start, uint64_t end, HWASpeechWork *work,
                         char *error, size_t error_size)
{
    size_t keep = (size_t)(scratch->end - start);
    size_t discard = (size_t)(start - scratch->start);
    size_t side;

    for (side = 0U; side < 2U; ++side) {
        double *audio = scratch->audio[side];
        if (discard != 0U && keep != 0U) memmove(audio, audio + discard, keep * sizeof(*audio));
        if (scratch->end < end) {
            HwaPairedSampleSpan newly_read = {
                .start_sample = scratch->end, .end_sample = end, .present = 1, .valid = 1
            };
            if (hwa_speech_audio_mono(readers[side], newly_read, work, audio + keep,
                                      error, error_size) != 0) return -1;
        }
    }
    scratch->start = start;
    scratch->end = end;
    return 0;
}

static double temporal_quantum(const HWAFormat *format)
{
    if (format->encoding != HWA_ENCODING_PCM) return 0.0;
    return ldexp(1.0, 1 - (int)format->valid_bits_per_sample);
}

static double temporal_level(double rms, double baseline)
{
    double floor_rms = fmax(baseline * TEMPORAL_LEVEL_FLOOR, DBL_MIN);
    return 20.0 * (log10(fmax(rms, floor_rms)) - log10(baseline));
}

static int temporal_signal(double rms, double baseline, double quantum)
{
    return rms > fmax(8.0 * quantum, TEMPORAL_LEVEL_FLOOR * baseline);
}

static void temporal_hop(TemporalScratch *scratch, const double baseline[2], const double quantum[2],
                         HWASpeechTemporalFrame *frame)
{
    size_t start = (size_t)(frame->start_sample - scratch->start);
    size_t count = (size_t)(frame->end_sample - frame->start_sample);
    const double *left = scratch->audio[0] + start;
    const double *right = scratch->audio[1] + start;
    double power[2] = {0.0, 0.0};
    double rms[2];
    int audible[2];
    double residual = 0.0;
    double rounding;
    size_t index;

    for (index = 0U; index < count; ++index) {
        power[0] += left[index] * left[index];
        power[1] += right[index] * right[index];
    }
    for (index = 0U; index < 2U; ++index) {
        rms[index] = sqrt(power[index] / (double)count);
        frame->level[index] = temporal_level(rms[index], baseline[index]);
        audible[index] = temporal_signal(rms[index], baseline[index], quantum[index]);
    }
    frame->edited = (audible[0] && rms[1] <= fmax(quantum[1], TEMPORAL_LEVEL_FLOOR * baseline[1])) ||
                    (audible[1] && rms[0] <= fmax(quantum[0], TEMPORAL_LEVEL_FLOOR * baseline[0]));
    if (!audible[0] || !audible[1]) return;

    /* Removing one RMS gain from each cell leaves only waveform shape. A
     * separate comparison to the typical gain catches proportional edits. */
    rounding = quantum[0] / rms[0] + quantum[1] / rms[1];
    for (index = 0U; index < count; ++index) {
        double delta = left[index] / rms[0] - right[index] / rms[1];
        residual += delta * delta;
    }
    frame->edited = sqrt(residual / (double)count) > fmax(1e-4, 2.0 * rounding);
}

static int temporal_spectrum(TemporalScratch *scratch, size_t side, size_t padding,
                             HWASpeechWork *work, double *shape, char *error, size_t error_size)
{
    double powers[TEMPORAL_BANDS] = {0};
    double total = 0.0;
    size_t available = (size_t)(scratch->end - scratch->start);
    size_t last_bin = scratch->fft_size / 2U;
    size_t index, band = 0U;
    double nyquist = (double)last_bin * scratch->bin_hz;

    if (hwa_speech_charge_transforms(work, 1U, error, error_size) != 0) return -1;
    memset(scratch->fft, 0, scratch->fft_size * sizeof(*scratch->fft));
    for (index = 0U; index < available; ++index) {
        size_t position = padding + index;
        scratch->fft[position].real = scratch->audio[side][index] * scratch->hann[position];
    }
    if (hwa_dsp_fft(scratch->fft, scratch->fft_size, 0) != HWA_DSP_OK) {
        hwa_set_error(error, error_size, "speech temporal FFT failed");
        return -1;
    }
    for (index = 0U; index <= last_bin; ++index) {
        double real = scratch->fft[index].real;
        double imag = scratch->fft[index].imag;
        double power = real * real + imag * imag;
        double low = fmax(0.0, ((double)index - 0.5) * scratch->bin_hz);
        double high = fmin(nyquist, ((double)index + 0.5) * scratch->bin_hz);
        double width = high - low;
        if (index != 0U && index != last_bin) power *= 2.0;
        total += power;
        while (band + 1U < scratch->band_count && temporal_band_edges[band] < high) {
            double edge = temporal_band_edges[band];
            powers[band] += power * fmax(0.0, edge - low) / width;
            if (edge > low) low = edge;
            ++band;
        }
        powers[band] += power * (high - low) / width;
    }
    for (band = 0U; band < scratch->band_count; ++band) {
        double share = total > 0.0 ? powers[band] / total : 0.0;
        shape[band] = 10.0 * log10(fmax(TEMPORAL_BAND_FLOOR, share));
    }
    return 0;
}

static int temporal_analyze(TemporalScratch *scratch, const HWAWavReader *const readers[2],
                            const double baseline[2], const double quantum[2],
                            HWASpeechTemporal *result, HWASpeechWork *work,
                            char *error, size_t error_size)
{
    double previous[2][TEMPORAL_BANDS] = {{0}};
    double previous_delta[2][TEMPORAL_BANDS] = {{0}};
    uint64_t total = readers[0]->format.frames;
    size_t index;

    for (index = 0U; index < result->count; ++index) {
        HWASpeechTemporalFrame *frame = &result->frames[index];
        size_t half = result->window_samples / 2U;
        size_t right = result->window_samples - half;
        uint64_t center, left, start, end;
        double shapes[2][TEMPORAL_BANDS];
        double distance = 0.0;
        size_t side, band;

        frame->start_sample = (uint64_t)index * result->hop_samples;
        frame->end_sample = frame->start_sample + (total - frame->start_sample < result->hop_samples
            ? total - frame->start_sample : result->hop_samples);
        center = frame->start_sample + (frame->end_sample - frame->start_sample) / 2U;
        left = center < half ? center : half;
        start = center - left;
        end = center + (total - center < right ? total - center : right);
        if (temporal_fill(scratch, readers, start, end, work, error, error_size) != 0) return -1;
        temporal_hop(scratch, baseline, quantum, frame);
        for (side = 0U; side < 2U; ++side) {
            const double *shape = shapes[side];
            double change = 0.0;
            double acceleration = 0.0;

            frame->spectral_change[side] = NAN;
            frame->spectral_acceleration[side] = NAN;
            if (temporal_spectrum(scratch, side, half - (size_t)left, work, shapes[side],
                                   error, error_size) != 0) return -1;
            for (band = 0U; band < scratch->band_count; ++band) {
                double delta = shape[band] - previous[side][band];
                double second = delta - previous_delta[side][band];
                change += delta * delta;
                acceleration += second * second;
                previous_delta[side][band] = delta;
                previous[side][band] = shape[band];
            }
            if (index > 0U) frame->spectral_change[side] = sqrt(change / (double)scratch->band_count);
            if (index > 1U) {
                result->frames[index - 1U].spectral_acceleration[side] =
                    sqrt(acceleration / (double)scratch->band_count);
            }
        }
        for (band = 0U; band < scratch->band_count; ++band) {
            double delta = shapes[1][band] - shapes[0][band];
            distance += delta * delta;
        }
        frame->spectral_distance = sqrt(distance / (double)scratch->band_count);
    }
    return 0;
}

static int temporal_double_order(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static int temporal_local_gain(HWASpeechTemporal *result, const double baseline[2],
                               const double quantum[2], uint64_t available,
                               char *error, size_t error_size)
{
    uint64_t bytes = (uint64_t)result->count * sizeof(double);
    size_t copies = 0U;
    size_t index;
    double *gains;
    double offset = 0.0;

    if (temporal_check_memory(bytes, available, error, error_size) != 0) return -1;
    gains = malloc((size_t)bytes);
    if (gains == NULL) {
        hwa_set_error(error, error_size, "cannot allocate speech temporal gain workspace");
        return -1;
    }

    /* The median over audible, shape-matched cells isolates uniform gain
     * without spreading an isolated edit into every otherwise identical hop. */
    for (index = 0U; index < result->count; ++index) {
        const HWASpeechTemporalFrame *frame = &result->frames[index];
        double a = baseline[0] * pow(10.0, 0.05 * frame->level[0]);
        double b = baseline[1] * pow(10.0, 0.05 * frame->level[1]);
        if (!frame->edited && temporal_signal(a, baseline[0], quantum[0]) &&
                              temporal_signal(b, baseline[1], quantum[1])) {
            gains[copies++] = frame->level[1] - frame->level[0];
        }
    }
    if (copies != 0U) {
        qsort(gains, copies, sizeof(*gains), temporal_double_order);
        offset = 0.5 * (gains[(copies - 1U) / 2U] + gains[copies / 2U]);
    }
    free(gains);

    for (index = 0U; index < result->count; ++index) {
        HWASpeechTemporalFrame *frame = &result->frames[index];
        double a = baseline[0] * pow(10.0, 0.05 * frame->level[0]);
        double b = baseline[1] * pow(10.0, 0.05 * frame->level[1]);

        if (temporal_signal(a, baseline[0], quantum[0]) &&
            temporal_signal(b, baseline[1], quantum[1])) {
            double rounding = quantum[0] / a + quantum[1] / b;
            double tolerance_db = fmax(0.02, 20.0 * log10(1.0 + 2.0 * rounding));
            if (fabs(frame->level[1] - frame->level[0] - offset) > tolerance_db) frame->edited = 1;
        }
        frame->level[0] += 0.5 * offset;
        frame->level[1] -= 0.5 * offset;
    }
    return 0;
}

int hwa_speech_temporal_measure(const HWAWavReader *reference, const HWAWavReader *candidate,
                                double reference_rms, double candidate_rms, HWASpeechWork *work,
                                HWASpeechTemporal *result, char *error, size_t error_size)
{
    const HWAWavReader *const readers[2] = {reference, candidate};
    TemporalScratch scratch = {0};
    double baseline[2], quantum[2];
    uint64_t total = reference->format.frames;
    unsigned rate = reference->format.sample_rate_hz;
    uint64_t available_before = work->available_bytes;
    uint64_t available = available_before;
    uint64_t count;
    int status = -1;

    memset(result, 0, sizeof(*result));
    if (rate != candidate->format.sample_rate_hz || total != candidate->format.frames) {
        hwa_set_error(error, error_size, "speech temporal comparison requires a shared sample clock");
        return -1;
    }
    result->hop_samples = temporal_ms(rate, TEMPORAL_HOP_MS, 1U);
    result->window_samples = temporal_ms(rate, TEMPORAL_WINDOW_MS, 3U);
    if (total == 0U) return 0;
    count = 1U + (total - 1U) / result->hop_samples;
    if (total > (work->max_frames - work->decoded_frames) / 2U) {
        hwa_set_error(error, error_size, "speech temporal comparison exceeds the decoded frame limit");
        return -1;
    }
    if (count > (work->max_transforms - work->transforms) / 2U) {
        hwa_set_error(error, error_size, "speech temporal comparison exceeds the transform limit");
        return -1;
    }
    if (count > available / sizeof(*result->frames) || count > SIZE_MAX / sizeof(*result->frames)) {
        hwa_set_error(error, error_size, "speech temporal comparison exceeds the work memory limit");
        return -1;
    }
    result->count = (size_t)count;
    result->retained_bytes = count * sizeof(*result->frames);
    available -= result->retained_bytes;
    result->frames = calloc(result->count, sizeof(*result->frames));
    if (result->frames == NULL) {
        hwa_set_error(error, error_size, "cannot allocate speech temporal frames");
        goto done;
    }
    baseline[0] = reference_rms > 0.0 ? reference_rms : (candidate_rms > 0.0 ? candidate_rms : 1.0);
    baseline[1] = candidate_rms > 0.0 ? candidate_rms : baseline[0];
    quantum[0] = temporal_quantum(&reference->format);
    quantum[1] = temporal_quantum(&candidate->format);
    if (temporal_scratch_open(&scratch, rate, result->window_samples, available,
                               error, error_size) != 0) goto done;
    work->available_bytes = available - scratch.bytes;
    status = temporal_analyze(&scratch, readers, baseline, quantum, result, work, error, error_size);
done:
    temporal_scratch_close(&scratch);
    work->available_bytes = available_before;
    if (status == 0) status = temporal_local_gain(result, baseline, quantum, available, error, error_size);
    if (status != 0) hwa_speech_temporal_free(result);
    return status;
}

void hwa_speech_temporal_free(HWASpeechTemporal *result)
{
    free(result->frames);
    memset(result, 0, sizeof(*result));
}
