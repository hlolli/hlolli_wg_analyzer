#include "isolated_note_report.h"

#include "numeric_locale.h"
#include "output.h"

#include <inttypes.h>
#include <stdio.h>

static const char *hwa_note_container(HWAContainer value)
{
    return value == HWA_CONTAINER_RF64 ? "rf64" : "riff";
}

static const char *hwa_note_encoding(HWAEncoding value)
{
    return value == HWA_ENCODING_PCM ? "pcm" : "ieee-float";
}

static int hwa_note_metric_names(FILE *stream, uint32_t mask)
{
    int wrote = 0;

    if (fputc('[', stream) == EOF) return -1;
    if ((mask & HWA_ISOLATED_NOTE_PITCH) != 0U) {
        if (hwa_json_write_string(stream, "pitch") != 0) return -1;
        wrote = 1;
    }
    if ((mask & HWA_ISOLATED_NOTE_PASSIVE_DECAY) != 0U) {
        if ((wrote && fputc(',', stream) == EOF) ||
            hwa_json_write_string(stream, "passive-decay") != 0) {
            return -1;
        }
    }
    return fputc(']', stream) == EOF ? -1 : 0;
}

static int hwa_note_rejection_names(FILE *stream, uint32_t mask)
{
    static const struct HWARejectionName {
        uint32_t flag;
        const char *name;
    } names[] = {
        {HWA_ISOLATED_NOTE_REJECT_SILENCE, "silence"},
        {HWA_ISOLATED_NOTE_REJECT_NOISE, "noise"},
        {HWA_ISOLATED_NOTE_REJECT_OCTAVE, "octave"},
        {HWA_ISOLATED_NOTE_REJECT_BOUNDARY, "boundary"},
        {HWA_ISOLATED_NOTE_REJECT_LOW_SUPPORT, "low-support"},
        {HWA_ISOLATED_NOTE_REJECT_LOW_DYNAMIC_RANGE, "low-dynamic-range"},
        {HWA_ISOLATED_NOTE_REJECT_HIGH_RESIDUAL, "high-residual"},
        {HWA_ISOLATED_NOTE_REJECT_LATE_PULSE, "late-pulse"}
    };
    size_t index;
    int wrote = 0;

    if (fputc('[', stream) == EOF) return -1;
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        if ((mask & names[index].flag) == 0U) continue;
        if ((wrote && fputc(',', stream) == EOF) ||
            hwa_json_write_string(stream, names[index].name) != 0) {
            return -1;
        }
        wrote = 1;
    }
    return fputc(']', stream) == EOF ? -1 : 0;
}

int hwa_isolated_note_report_json(FILE *stream,
                                  const HWAIsolatedNoteResult *result)
{
    HWANumericLocale locale;
    int status = -1;

    if (stream == NULL || result == NULL ||
        hwa_c_numeric_locale_begin(&locale) != 0) {
        return -1;
    }
    if (fputs("{\"schema\":\"hwa-isolated-note\",\"schema_version\":1,"
              "\"command\":\"isolated-note\",\"method\":\""
              HWA_ISOLATED_NOTE_METHOD_VERSION "\",\"path\":",
              stream) == EOF ||
        hwa_json_write_string(stream,
                              result->path != NULL ? result->path : "") != 0 ||
        fprintf(stream,
                ",\"expected_hz\":%.17g,\"requested_mask\":%u,"
                "\"valid_mask\":%u,\"rejection_mask\":%u,"
                "\"requested_metrics\":",
                result->expected_hz,
                (unsigned)result->requested_mask,
                (unsigned)result->valid_mask,
                (unsigned)result->rejection_mask) < 0 ||
        hwa_note_metric_names(stream, result->requested_mask) != 0 ||
        fputs(",\"valid_metrics\":", stream) == EOF ||
        hwa_note_metric_names(stream, result->valid_mask) != 0 ||
        fputs(",\"rejections\":", stream) == EOF ||
        hwa_note_rejection_names(stream, result->rejection_mask) != 0 ||
        fprintf(stream,
                ",\"format\":{\"container\":\"%s\",\"encoding\":\"%s\","
                "\"sample_rate_hz\":%u,\"channels\":%u,"
                "\"bits_per_sample\":%u,\"valid_bits_per_sample\":%u,"
                "\"frames\":%" PRIu64 "},"
                "\"pitch\":{\"valid\":%s,\"hz\":%.17g,\"cents\":%.17g,"
                "\"confidence\":%.17g,\"coverage\":%.17g,"
                "\"window_count\":%" PRIu64 ","
                "\"accepted_window_count\":%" PRIu64 ","
                "\"start_sample\":%" PRIu64 ",\"end_sample\":%" PRIu64 "},"
                "\"decay\":{\"valid\":%s,"
                "\"slope_db_per_second\":%.17g,\"t60_seconds\":%.17g,"
                "\"support_seconds\":%.17g,\"dynamic_range_db\":%.17g,"
                "\"residual_db\":%.17g,\"floor_dbfs\":%.17g,"
                "\"point_count\":%" PRIu64 ","
                "\"start_sample\":%" PRIu64 ",\"end_sample\":%" PRIu64 "},"
                "\"work\":{\"peak_bytes\":%" PRIu64 ","
                "\"evaluations\":%" PRIu64 "}}\n",
                hwa_note_container(result->format.container),
                hwa_note_encoding(result->format.encoding),
                (unsigned)result->format.sample_rate_hz,
                (unsigned)result->format.channels,
                (unsigned)result->format.bits_per_sample,
                (unsigned)result->format.valid_bits_per_sample,
                result->format.frames,
                result->pitch.valid ? "true" : "false",
                result->pitch.hz, result->pitch.cents,
                result->pitch.confidence, result->pitch.coverage,
                result->pitch.window_count,
                result->pitch.accepted_window_count,
                result->pitch.start_sample, result->pitch.end_sample,
                result->decay.valid ? "true" : "false",
                result->decay.slope_db_per_second,
                result->decay.t60_seconds,
                result->decay.support_seconds,
                result->decay.dynamic_range_db,
                result->decay.residual_db,
                result->decay.floor_dbfs,
                result->decay.point_count,
                result->decay.start_sample, result->decay.end_sample,
                result->peak_work_bytes, result->evaluation_count) < 0) {
        goto cleanup;
    }
    status = 0;

cleanup:
    if (hwa_c_numeric_locale_end(&locale) != 0) status = -1;
    return status;
}
