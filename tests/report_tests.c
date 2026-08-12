#include "report.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HWA_TEST_OUTPUT_CAPACITY 65536U

typedef int (*HWAAnalysisWriter)(FILE *, const HWAAnalysis *);
typedef int (*HWACompareWriter)(FILE *, const HWAAnalysis *, const HWAAnalysis *);

static int hwa_test_fail(const char *test, const char *detail, int line)
{
    (void)fprintf(stderr, "%s:%d: %s\n", test, line, detail);
    return 0;
}

#define HWA_CHECK(test_name, condition, detail)                             \
    do {                                                                    \
        if (!(condition)) {                                                 \
            return hwa_test_fail((test_name), (detail), __LINE__);          \
        }                                                                   \
    } while (0)

static int hwa_capture_analysis(HWAAnalysisWriter writer,
                                const HWAAnalysis *analysis,
                                char *output,
                                size_t capacity)
{
    FILE *stream;
    long length;
    size_t read_size;
    int ok = 1;

    if (writer == NULL || analysis == NULL || output == NULL || capacity == 0U) {
        return 0;
    }
    stream = tmpfile();
    if (stream == NULL) {
        return 0;
    }
    if (writer(stream, analysis) != 0 || fflush(stream) != 0 ||
        fseek(stream, 0L, SEEK_END) != 0) {
        ok = 0;
    }
    length = ok ? ftell(stream) : -1L;
    if (length < 0L || (size_t)length >= capacity ||
        fseek(stream, 0L, SEEK_SET) != 0) {
        ok = 0;
    }
    read_size = ok ? fread(output, 1U, (size_t)length, stream) : 0U;
    if (!ok || read_size != (size_t)length || ferror(stream)) {
        ok = 0;
    } else {
        output[read_size] = '\0';
    }
    if (fclose(stream) != 0) {
        ok = 0;
    }
    return ok;
}

static int hwa_capture_compare(HWACompareWriter writer,
                               const HWAAnalysis *reference,
                               const HWAAnalysis *model,
                               char *output,
                               size_t capacity)
{
    FILE *stream;
    long length;
    size_t read_size;
    int ok = 1;

    if (writer == NULL || reference == NULL || model == NULL || output == NULL ||
        capacity == 0U) {
        return 0;
    }
    stream = tmpfile();
    if (stream == NULL) {
        return 0;
    }
    if (writer(stream, reference, model) != 0 || fflush(stream) != 0 ||
        fseek(stream, 0L, SEEK_END) != 0) {
        ok = 0;
    }
    length = ok ? ftell(stream) : -1L;
    if (length < 0L || (size_t)length >= capacity ||
        fseek(stream, 0L, SEEK_SET) != 0) {
        ok = 0;
    }
    read_size = ok ? fread(output, 1U, (size_t)length, stream) : 0U;
    if (!ok || read_size != (size_t)length || ferror(stream)) {
        ok = 0;
    } else {
        output[read_size] = '\0';
    }
    if (fclose(stream) != 0) {
        ok = 0;
    }
    return ok;
}

static void hwa_init_analysis(HWAAnalysis *analysis,
                              HWAChannelMetrics *channel,
                              char *path)
{
    size_t band;

    (void)memset(analysis, 0, sizeof(*analysis));
    (void)memset(channel, 0, sizeof(*channel));
    analysis->path = path;
    analysis->format.container = HWA_CONTAINER_RIFF;
    analysis->format.encoding = HWA_ENCODING_PCM;
    analysis->format.channels = 1U;
    analysis->format.sample_rate_hz = 48000U;
    analysis->format.bits_per_sample = 24U;
    analysis->format.valid_bits_per_sample = 24U;
    analysis->format.block_align = 3U;
    analysis->options.channel_mode = HWA_CHANNEL_KEEP;
    analysis->options.decode_block_frames = 4096U;
    analysis->options.frame_size = 2048U;
    analysis->options.hop_size = 512U;
    analysis->options.silence_threshold_dbfs = -60.0;
    analysis->options.max_input_bytes = 1024U;
    analysis->options.max_input_frames = 1024U;
    analysis->options.max_work_bytes = 1024U;
    analysis->options.max_transforms = 16U;
    analysis->options.max_track_points = 16U;
    analysis->options.max_spectrum_values = 1024U;
    analysis->options.max_lag_samples = 32U;
    analysis->options.true_peak_oversample = 4U;
    analysis->analyzed_channels = 1U;
    analysis->channels = channel;
    analysis->activity.threshold_dbfs = -60.0;

    channel->peak = 0.25;
    channel->true_peak = 901.125;
    channel->rms = 0.125;
    channel->dc_offset = 0.0;
    channel->crest_factor = 902.125;
    channel->zero_crossing_rate = 0.25;

    analysis->loudness.integrated_lufs = 903.125;
    analysis->loudness.loudness_range_lu = 904.125;
    analysis->loudness.momentary_max_lufs = 905.125;
    analysis->loudness.short_term_max_lufs = 906.125;
    analysis->spectrum.centroid_hz = 907.125;
    analysis->spectrum.spread_hz = 908.125;
    analysis->spectrum.rolloff_85_hz = 909.125;
    analysis->spectrum.flatness = 910.125;
    analysis->spectrum.slope_db_per_octave = 911.125;
    analysis->spectrum.mean_flux = 912.125;
    analysis->activity.silence_fraction = 913.125;
    analysis->activity.active_start_seconds = 914.125;
    analysis->activity.active_end_seconds = 915.125;
    analysis->stereo.available = 1;
    analysis->stereo.correlation = 916.125;
    analysis->stereo.mid_rms = 917.125;
    analysis->stereo.side_rms = 918.125;
    analysis->stereo.balance_db = 919.125;
    analysis->stereo.width_ratio = 920.125;
    analysis->stereo.interchannel_delay_samples = 921.125;
    analysis->stereo.interchannel_delay_seconds = 922.125;
    analysis->stereo.interchannel_delay_confidence = 923.125;
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        analysis->spectrum.band_power[band] = 930.125 + (double)band;
        analysis->stereo.band_width[band] = 950.125 + (double)band;
    }
}

static int hwa_contains(const char *text, const char *part)
{
    return text != NULL && part != NULL && strstr(text, part) != NULL;
}

static size_t hwa_count_matches(const char *text, const char *part)
{
    size_t count = 0U;
    size_t part_length;
    const char *cursor;

    if (text == NULL || part == NULL || part[0] == '\0') {
        return 0U;
    }
    part_length = strlen(part);
    cursor = text;
    while ((cursor = strstr(cursor, part)) != NULL) {
        ++count;
        cursor += part_length;
    }
    return count;
}

static int hwa_test_empty_invalid_values(void)
{
    static const char test_name[] = "empty-invalid-values";
    char output[HWA_TEST_OUTPUT_CAPACITY];
    char path[] = "empty.wav";
    HWAAnalysis analysis;
    HWAChannelMetrics channel;

    hwa_init_analysis(&analysis, &channel, path);
    HWA_CHECK(test_name,
              hwa_capture_analysis(hwa_report_analysis_json, &analysis, output,
                                   sizeof(output)),
              "could not capture JSON report");
    HWA_CHECK(test_name, hwa_contains(output, "\"true_peak\":null"),
              "invalid true peak must be JSON null");
    HWA_CHECK(test_name, hwa_contains(output, "\"crest_factor\":null"),
              "invalid crest factor must be JSON null");
    HWA_CHECK(test_name, hwa_contains(output, "\"integrated_lufs\":null"),
              "invalid integrated loudness must be JSON null");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"loudness_range_lu\":null"),
              "invalid loudness range must be JSON null");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"momentary_max_lufs\":null"),
              "invalid momentary loudness must be JSON null");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"short_term_max_lufs\":null"),
              "invalid short-term loudness must be JSON null");
    HWA_CHECK(test_name, hwa_contains(output, "\"centroid_hz\":null"),
              "invalid spectrum must be JSON null");
    HWA_CHECK(test_name, hwa_contains(output, "\"silence_fraction\":null"),
              "unclassified activity must be JSON null");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"active_start_seconds\":null"),
              "invalid active start must be JSON null");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"active_end_seconds\":null"),
              "invalid active end must be JSON null");
    HWA_CHECK(test_name, hwa_contains(output, "\"correlation\":null"),
              "invalid stereo correlation must be JSON null");
    HWA_CHECK(test_name, hwa_contains(output, "\"width_ratio\":null"),
              "invalid stereo width must be JSON null");
    HWA_CHECK(test_name, !hwa_contains(output, "903.125"),
              "invalid numeric sentinel leaked into JSON");

    HWA_CHECK(test_name,
              hwa_capture_analysis(hwa_report_analysis_text, &analysis, output,
                                   sizeof(output)),
              "could not capture text report");
    HWA_CHECK(test_name, hwa_contains(output, "True peak: n/a"),
              "invalid true peak must be text n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Crest factor: n/a"),
              "invalid crest factor must be text n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Integrated: n/a LUFS"),
              "invalid integrated loudness must be text n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Range: n/a LU"),
              "invalid loudness range must be text n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Momentary max: n/a LUFS"),
              "invalid momentary loudness must be text n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Short-term max: n/a LUFS"),
              "invalid short-term loudness must be text n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Centroid: n/a Hz"),
              "invalid spectrum must be text n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Silence: n/a"),
              "unclassified activity must be text n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Active span: n/a"),
              "invalid active span must be text n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Correlation: n/a"),
              "invalid stereo correlation must be text n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Width ratio: n/a"),
              "invalid stereo width must be text n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Delay: n/a samples"),
              "invalid stereo delay must be text n/a");
    HWA_CHECK(test_name, !hwa_contains(output, "903.125"),
              "invalid numeric sentinel leaked into text");
    return 1;
}

static int hwa_test_silent_and_short_values(void)
{
    static const char test_name[] = "silent-and-short-values";
    char output[HWA_TEST_OUTPUT_CAPACITY];
    char path[] = "short-silence.wav";
    HWAAnalysis analysis;
    HWAChannelMetrics channel;

    hwa_init_analysis(&analysis, &channel, path);
    analysis.format.frames = 9600U;
    analysis.format.data_bytes = 28800U;
    analysis.format.duration_seconds = 0.2;
    analysis.activity.classified_valid = 1;
    analysis.activity.silence_fraction = 1.0;
    analysis.loudness.momentary_valid = 1;
    analysis.loudness.momentary_max_lufs = -48.25;

    HWA_CHECK(test_name,
              hwa_capture_analysis(hwa_report_analysis_json, &analysis, output,
                                   sizeof(output)),
              "could not capture short JSON report");
    HWA_CHECK(test_name, hwa_contains(output, "\"silence_fraction\":1"),
              "valid silent fraction must remain numeric");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"active_start_seconds\":null"),
              "silent input has no active start");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"active_end_seconds\":null"),
              "silent input has no active end");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"momentary_max_lufs\":-48.25"),
              "valid short momentary loudness must remain numeric");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"short_term_max_lufs\":null"),
              "invalid short-term loudness must be JSON null");

    HWA_CHECK(test_name,
              hwa_capture_analysis(hwa_report_analysis_text, &analysis, output,
                                   sizeof(output)),
              "could not capture short text report");
    HWA_CHECK(test_name, hwa_contains(output, "Silence: 1.000000000"),
              "valid silent fraction must remain numeric in text");
    HWA_CHECK(test_name, hwa_contains(output, "Active span: n/a"),
              "silent input must show an unavailable active span");
    HWA_CHECK(test_name,
              hwa_contains(output, "Momentary max: -48.250000000 LUFS"),
              "valid momentary loudness must remain numeric in text");
    HWA_CHECK(test_name, hwa_contains(output, "Short-term max: n/a LUFS"),
              "invalid short-term loudness must be text n/a");
    return 1;
}

static int hwa_test_invalid_compare_deltas(void)
{
    static const char test_name[] = "invalid-compare-deltas";
    char output[HWA_TEST_OUTPUT_CAPACITY];
    char reference_path[] = "reference.wav";
    char model_path[] = "model.wav";
    HWAAnalysis reference;
    HWAAnalysis model;
    HWAChannelMetrics reference_channel;
    HWAChannelMetrics model_channel;

    hwa_init_analysis(&reference, &reference_channel, reference_path);
    hwa_init_analysis(&model, &model_channel, model_path);
    reference.loudness.integrated_valid = 1;
    reference.loudness.integrated_lufs = -20.0;
    model.loudness.integrated_valid = 0;
    reference.spectrum.valid = 0;
    model.spectrum.valid = 1;
    reference.activity.classified_valid = 1;
    reference.activity.silence_fraction = 0.25;
    model.activity.classified_valid = 0;
    reference.stereo.available = 1;
    reference.stereo.correlation_valid = 1;
    reference.stereo.width_valid = 1;
    model.stereo.available = 0;
    model.stereo.correlation_valid = 1;
    model.stereo.width_valid = 1;

    HWA_CHECK(test_name,
              hwa_capture_compare(hwa_report_compare_json, &reference, &model,
                                  output, sizeof(output)),
              "could not capture compare JSON");
    HWA_CHECK(test_name, hwa_contains(output, "\"integrated_lufs\":null"),
              "one invalid loudness input must make its delta null");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"spectral_centroid_hz\":null"),
              "one invalid spectrum must make centroid delta null");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"spectral_rolloff_85_hz\":null"),
              "one invalid spectrum must make rolloff delta null");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"spectral_flatness\":null"),
              "one invalid spectrum must make flatness delta null");
    HWA_CHECK(test_name, hwa_contains(output, "\"silence_fraction\":null"),
              "one invalid activity input must make its delta null");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"stereo_correlation\":null"),
              "unavailable stereo must make correlation delta null");
    HWA_CHECK(test_name,
              hwa_contains(output, "\"stereo_width_ratio\":null"),
              "unavailable stereo must make width delta null");
    HWA_CHECK(test_name, hwa_count_matches(output, "\"value\":null") ==
                             HWA_BAND_COUNT,
              "each band delta must be null when one spectrum is invalid");

    HWA_CHECK(test_name,
              hwa_capture_compare(hwa_report_compare_text, &reference, &model,
                                  output, sizeof(output)),
              "could not capture compare text");
    HWA_CHECK(test_name, hwa_contains(output, "Integrated loudness: n/a LU"),
              "one invalid loudness input must make its text delta n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Spectral centroid: n/a Hz"),
              "one invalid spectrum must make centroid text delta n/a");
    HWA_CHECK(test_name, hwa_contains(output, "85% rolloff: n/a Hz"),
              "one invalid spectrum must make rolloff text delta n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Silence fraction: n/a"),
              "one invalid activity input must make its text delta n/a");
    HWA_CHECK(test_name, hwa_contains(output, "Stereo width ratio: n/a"),
              "unavailable stereo must make its text delta n/a");
    return 1;
}

static int hwa_csv_field_equals(const char *row,
                                size_t wanted_index,
                                const char *expected)
{
    const char *field_start = row;
    const char *cursor = row;
    size_t field_index = 0U;
    size_t expected_length = strlen(expected);

    for (;;) {
        if (*cursor == ',' || *cursor == '\n' || *cursor == '\r' ||
            *cursor == '\0') {
            if (field_index == wanted_index) {
                return (size_t)(cursor - field_start) == expected_length &&
                       memcmp(field_start, expected, expected_length) == 0;
            }
            if (*cursor != ',') {
                return 0;
            }
            ++field_index;
            field_start = cursor + 1;
        }
        ++cursor;
    }
}

static int hwa_test_invalid_frame_csv_cells(void)
{
    static const char test_name[] = "invalid-frame-csv-cells";
    char output[HWA_TEST_OUTPUT_CAPACITY];
    char path[] = "tracks.wav";
    const char *row;
    size_t field;
    HWAAnalysis analysis;
    HWAChannelMetrics channel;
    HWAFrameMetrics track;

    hwa_init_analysis(&analysis, &channel, path);
    (void)memset(&track, 0, sizeof(track));
    track.time_seconds = 0.25;
    track.rms_dbfs = -80.0;
    track.frame_lufs = 601.125;
    track.pitch_hz = 602.125;
    track.pitch_confidence = 603.125;
    track.onset_strength = 0.125;
    track.spectral_centroid_hz = 604.125;
    track.spectral_rolloff_85_hz = 605.125;
    track.spectral_flatness = 606.125;
    for (field = 0U; field < HWA_BAND_COUNT; ++field) {
        track.band_power_db[field] = 610.125 + (double)field;
    }
    analysis.tracks = &track;
    analysis.track_count = 1U;

    HWA_CHECK(test_name,
              hwa_capture_analysis(hwa_report_frames_csv, &analysis, output,
                                   sizeof(output)),
              "could not capture frame CSV");
    row = strchr(output, '\n');
    HWA_CHECK(test_name, row != NULL && row[1] != '\0',
              "frame CSV has no data row");
    ++row;
    HWA_CHECK(test_name, hwa_csv_field_equals(row, 0U, "0.25"),
              "time cell changed");
    HWA_CHECK(test_name, hwa_csv_field_equals(row, 1U, "-80"),
              "RMS cell changed");
    HWA_CHECK(test_name, hwa_csv_field_equals(row, 2U, ""),
              "invalid loudness cell must be blank");
    HWA_CHECK(test_name, hwa_csv_field_equals(row, 3U, ""),
              "invalid pitch cell must be blank");
    HWA_CHECK(test_name, hwa_csv_field_equals(row, 4U, ""),
              "pitch confidence must be blank when pitch is invalid");
    HWA_CHECK(test_name, hwa_csv_field_equals(row, 5U, "0.125"),
              "onset cell changed");
    for (field = 6U; field < 9U + HWA_BAND_COUNT; ++field) {
        HWA_CHECK(test_name, hwa_csv_field_equals(row, field, ""),
                  "invalid spectrum cells must be blank");
    }
    return 1;
}

static int hwa_test_text_path_escaping(void)
{
    static const char test_name[] = "text-path-escaping";
    static const char expected_prefix[] =
        "File: alpha\\x0abeta\\x09gamma\\x01\\x1b\\\\end\nContainer: ";
    char output[HWA_TEST_OUTPUT_CAPACITY];
    char path[] = "alpha\nbeta\tgamma\x01\x1b\\end";
    HWAAnalysis analysis;
    HWAChannelMetrics channel;

    hwa_init_analysis(&analysis, &channel, path);
    HWA_CHECK(test_name,
              hwa_capture_analysis(hwa_report_analysis_text, &analysis, output,
                                   sizeof(output)),
              "could not capture escaped text report");
    HWA_CHECK(test_name,
              strncmp(output, expected_prefix, sizeof(expected_prefix) - 1U) == 0,
              "path must escape newline, tab, control, ESC, and backslash bytes");
    HWA_CHECK(test_name, strchr(output, '\x01') == NULL,
              "raw control byte leaked into text report");
    HWA_CHECK(test_name, strchr(output, '\x1b') == NULL,
              "raw ESC byte leaked into text report");
    HWA_CHECK(test_name, !hwa_contains(output, "alpha\nbeta"),
              "raw path newline split the File field");
    return 1;
}

int main(void)
{
    int ok = 1;

    ok = hwa_test_empty_invalid_values() && ok;
    ok = hwa_test_silent_and_short_values() && ok;
    ok = hwa_test_invalid_compare_deltas() && ok;
    ok = hwa_test_invalid_frame_csv_cells() && ok;
    ok = hwa_test_text_path_escaping() && ok;
    return ok ? 0 : 1;
}
