#include "body_envelope_report.h"

#include "numeric_locale.h"

#include <math.h>
#include <stdio.h>

static const char *hwa_body_status_name(HWABodyEnvelopeStatus status)
{
    switch (status) {
    case HWA_BODY_ENVELOPE_VALID:
        return "valid";
    case HWA_BODY_ENVELOPE_LOW_SUPPORT:
        return "low-support";
    case HWA_BODY_ENVELOPE_NO_SUPPORT:
        return "no-support";
    default:
        return "invalid";
    }
}

static int hwa_body_json_string(FILE *stream, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (stream == NULL || text == NULL || fputc('"', stream) == EOF) {
        return -1;
    }
    while (*cursor != 0U) {
        unsigned char value = *cursor++;

        if (value == '"' || value == '\\') {
            if (fputc('\\', stream) == EOF || fputc((int)value, stream) == EOF) {
                return -1;
            }
        } else if (value < 0x20U) {
            if (fprintf(stream, "\\u%04x", (unsigned)value) < 0) return -1;
        } else if (fputc((int)value, stream) == EOF) {
            return -1;
        }
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

static int hwa_body_report_estimate_text(
    FILE *stream,
    const char *label,
    const HWABodyEnvelopeEstimate *estimate)
{
    size_t index;

    if (fprintf(stream,
                "%s: %s\n"
                "  file: %s\n"
                "  confidence: %.3f\n"
                "  frames: %llu used / %llu seen\n"
                "  pitch span: %.2f to %.2f Hz\n"
                "  harmonic observations: %llu\n",
                label, hwa_body_status_name(estimate->status),
                estimate->path != NULL ? estimate->path : "",
                estimate->confidence,
                (unsigned long long)estimate->frames_used,
                (unsigned long long)estimate->frames_seen,
                estimate->pitch_min_hz, estimate->pitch_max_hz,
                (unsigned long long)estimate->observation_count) < 0) {
        return -1;
    }
    for (index = 0U; index < estimate->point_count; ++index) {
        const HWABodyEnvelopePoint *point = &estimate->points[index];
        if (point->valid &&
            fprintf(stream,
                    "  %8.2f Hz  %+8.3f dB  confidence %.3f  "
                    "support %llu/%llu/%llu\n",
                    point->frequency_hz, point->relative_db,
                    point->confidence,
                    (unsigned long long)point->observation_count,
                    (unsigned long long)point->pitch_cell_count,
                    (unsigned long long)point->harmonic_count) < 0) {
            return -1;
        }
    }
    return 0;
}

int hwa_body_envelope_report_text(FILE *stream,
                                  const HWABodyEnvelopeResult *result)
{
    HWANumericLocale locale;
    size_t index;
    int status = -1;

    if (stream == NULL || result == NULL ||
        hwa_c_numeric_locale_begin(&locale) != 0) {
        return -1;
    }
    if (fputs("Pitch-conditioned radiated envelope\n"
              "====================================\n"
              "The curve has zero mean and zero log-frequency slope.\n"
              "Room, microphone, strings, and playing remain in the result.\n\n",
              stream) == EOF ||
        hwa_body_report_estimate_text(
            stream, "Reference", &result->reference) != 0) {
        goto cleanup;
    }
    if (result->model_present) {
        if (fputc('\n', stream) == EOF ||
            hwa_body_report_estimate_text(
                stream, "Model", &result->model) != 0 ||
            fprintf(stream,
                    "\nComparison: %s\n"
                    "  shape RMSE: %.3f dB\n"
                    "  shape correlation: %.4f\n"
                    "  confidence: %.3f\n"
                    "  model minus reference:\n",
                    result->comparison_valid ? "valid" : "low-support",
                    result->shape_rmse_db, result->shape_correlation,
                    result->comparison_confidence) < 0) {
            goto cleanup;
        }
        for (index = 0U; index < result->gap_count; ++index) {
            const HWABodyEnvelopeGap *gap = &result->gaps[index];
            if (gap->valid &&
                fprintf(stream,
                        "  %8.2f Hz  %+8.3f dB  confidence %.3f\n",
                        gap->frequency_hz,
                        gap->model_minus_reference_db,
                        gap->confidence) < 0) {
                goto cleanup;
            }
        }
    }
    status = 0;

cleanup:
    if (hwa_c_numeric_locale_end(&locale) != 0) status = -1;
    return status;
}

static int hwa_body_report_estimate_json(
    FILE *stream,
    const HWABodyEnvelopeEstimate *estimate)
{
    size_t index;

    if (fputs("{\"path\":", stream) == EOF ||
        hwa_body_json_string(stream,
                             estimate->path != NULL ? estimate->path : "") != 0 ||
        fprintf(stream,
                ",\"status\":\"%s\",\"confidence\":%.17g,"
                "\"frames_seen\":%llu,\"frames_used\":%llu,"
                "\"frames_rejected_pitch\":%llu,"
                "\"pitch_min_hz\":%.17g,\"pitch_max_hz\":%.17g,"
                "\"observation_count\":%llu,\"points\":[",
                hwa_body_status_name(estimate->status), estimate->confidence,
                (unsigned long long)estimate->frames_seen,
                (unsigned long long)estimate->frames_used,
                (unsigned long long)estimate->frames_rejected_pitch,
                estimate->pitch_min_hz, estimate->pitch_max_hz,
                (unsigned long long)estimate->observation_count) < 0) {
        return -1;
    }
    for (index = 0U; index < estimate->point_count; ++index) {
        const HWABodyEnvelopePoint *point = &estimate->points[index];
        if (index != 0U && fputc(',', stream) == EOF) return -1;
        if (fprintf(stream,
                    "{\"frequency_hz\":%.17g,\"relative_db\":%.17g,"
                    "\"residual_spread_db\":%.17g,\"confidence\":%.17g,"
                    "\"observation_count\":%llu,\"pitch_cell_count\":%llu,"
                    "\"harmonic_count\":%llu,\"quality_flags\":%u,"
                    "\"valid\":%s}",
                    point->frequency_hz, point->relative_db,
                    point->residual_spread_db, point->confidence,
                    (unsigned long long)point->observation_count,
                    (unsigned long long)point->pitch_cell_count,
                    (unsigned long long)point->harmonic_count,
                    (unsigned)point->quality_flags,
                    point->valid ? "true" : "false") < 0) {
            return -1;
        }
    }
    return fputs("]}", stream) == EOF ? -1 : 0;
}

int hwa_body_envelope_report_json(FILE *stream,
                                  const HWABodyEnvelopeResult *result)
{
    HWANumericLocale locale;
    size_t index;
    int status = -1;

    if (stream == NULL || result == NULL ||
        hwa_c_numeric_locale_begin(&locale) != 0) {
        return -1;
    }
    if (fputs("{\"schema_version\":1,"
              "\"command\":\"body-envelope\","
              "\"method\":\"crossed-harmonic-response-1\","
              "\"shape_constraints\":[\"zero-mean\","
              "\"zero-log-frequency-slope\"],\"reference\":",
              stream) == EOF ||
        hwa_body_report_estimate_json(stream, &result->reference) != 0) {
        goto cleanup;
    }
    if (result->model_present) {
        if (fputs(",\"model\":", stream) == EOF ||
            hwa_body_report_estimate_json(stream, &result->model) != 0 ||
            fprintf(stream,
                    ",\"comparison\":{\"valid\":%s,"
                    "\"shape_rmse_db\":%.17g,"
                    "\"shape_correlation\":%.17g,"
                    "\"confidence\":%.17g,\"gaps\":[",
                    result->comparison_valid ? "true" : "false",
                    result->shape_rmse_db, result->shape_correlation,
                    result->comparison_confidence) < 0) {
            goto cleanup;
        }
        for (index = 0U; index < result->gap_count; ++index) {
            const HWABodyEnvelopeGap *gap = &result->gaps[index];
            if (index != 0U && fputc(',', stream) == EOF) goto cleanup;
            if (fprintf(stream,
                        "{\"frequency_hz\":%.17g,"
                        "\"model_minus_reference_db\":%.17g,"
                        "\"confidence\":%.17g,\"valid\":%s}",
                        gap->frequency_hz,
                        gap->model_minus_reference_db,
                        gap->confidence,
                        gap->valid ? "true" : "false") < 0) {
                goto cleanup;
            }
        }
        if (fputs("]}", stream) == EOF) goto cleanup;
    }
    if (fprintf(stream,
                ",\"fit_evaluations\":%llu,"
                "\"retained_work_bytes\":%llu}\n",
                (unsigned long long)result->fit_evaluations,
                (unsigned long long)result->retained_work_bytes) < 0) {
        goto cleanup;
    }
    status = 0;

cleanup:
    if (hwa_c_numeric_locale_end(&locale) != 0) status = -1;
    return status;
}
