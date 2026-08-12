#include "production_report.h"

#include "alignment_file.h"
#include "measure_compare.h"
#include "numeric_locale.h"
#include "output.h"
#include "production.h"
#include "production_file.h"

#include <inttypes.h>
#include <stdio.h>

#define HWA_PRODUCTION_TEXT_MAX_SURVIVORS 16U
#define HWA_PRODUCTION_TEXT_MAX_WARNINGS 8U

static int hwa_production_json_number(FILE *stream, double value)
{
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_production_json_optional_number(FILE *stream,
                                               double value,
                                               int valid)
{
    return valid ? hwa_production_json_number(stream, value) :
                   (fputs("null", stream) == EOF ? -1 : 0);
}

static int hwa_production_json_optional_u64(FILE *stream,
                                            uint64_t value,
                                            int valid)
{
    return valid ? (fprintf(stream, "%" PRIu64, value) < 0 ? -1 : 0) :
                   (fputs("null", stream) == EOF ? -1 : 0);
}

static int hwa_production_json_path(FILE *stream,
                                    const char *path,
                                    const char *sha256)
{
    return fputs("{\"path\":", stream) == EOF ||
           hwa_json_write_string(stream, path) != 0 ||
           fputs(",\"path_encoding\":\"utf8_with_invalid_bytes_as_u00xx\","
                 "\"path_bytes_hex\":", stream) == EOF ||
           hwa_json_write_byte_hex(stream, path) != 0 ||
           fputs(",\"sha256\":", stream) == EOF ||
           hwa_json_write_string(stream, sha256) != 0 ||
           fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_production_json_build(FILE *stream)
{
    return fputs("{\"compiler_family\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_compiler_family()) != 0 ||
           fputs(",\"compiler_version\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_compiler_version()) != 0 ||
           fputs(",\"c_standard\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_c_standard()) != 0 ||
           fputs(",\"target_os\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_target_os()) != 0 ||
           fprintf(stream, ",\"pointer_bits\":%u,\"endianness\":",
                   hwa_build_pointer_bits()) < 0 ||
           hwa_json_write_string(stream, hwa_build_endianness()) != 0 ||
           fputs(",\"mode\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_mode()) != 0 ||
           fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_production_json_profile_limits(
    FILE *stream,
    const HWAProfileComparisonOptions *limits)
{
    return fprintf(
        stream,
        "{\"max_input_bytes\":%" PRIu64 ",\"max_work_bytes\":%" PRIu64
        ",\"max_contexts\":%zu,\"max_measurements\":%zu,"
        "\"max_groups\":%zu,\"max_group_members\":%zu,"
        "\"max_statistics\":%zu,\"max_warnings\":%zu,"
        "\"max_distributions\":%zu,\"max_gaps\":%zu}",
        limits->max_input_bytes, limits->max_work_bytes,
        limits->max_contexts, limits->max_measurements,
        limits->max_groups, limits->max_group_members,
        limits->max_statistics, limits->max_warnings,
        limits->max_distributions, limits->max_gaps) < 0 ? -1 : 0;
}

static int hwa_production_json_options(
    FILE *stream,
    const HWAProductionOptions *options)
{
    if (fprintf(
        stream,
            "{\"decode_block_frames\":%zu,"
            "\"max_input_bytes\":%" PRIu64
            ",\"max_input_frames\":%" PRIu64
            ",\"max_ir_frames\":%" PRIu64
            ",\"max_work_bytes\":%" PRIu64
            ",\"max_evaluations\":%" PRIu64 ",\"max_spans\":%zu,"
            "\"max_envelope_points\":%zu,\"max_fits\":%zu,"
            "\"max_evaluation_rows\":%zu,\"max_view_rows\":%zu,"
            "\"max_warnings\":%zu,\"profile_limits\":",
            options->decode_block_frames,
            options->max_input_bytes, options->max_input_frames,
            options->max_ir_frames, options->max_work_bytes,
            options->max_evaluations, options->max_spans,
            options->max_envelope_points,
            options->max_fits, options->max_evaluation_rows,
            options->max_view_rows, options->max_warnings) < 0 ||
        hwa_production_json_profile_limits(
            stream, &options->profile_limits) != 0 ||
        fputc('}', stream) == EOF) return -1;
    return 0;
}

static int hwa_production_json_profile_method(
    FILE *stream,
    const HWAProductionProfileMethod *method)
{
    return fprintf(
        stream,
        "{\"measurement_method_version\":\"%s\","
        "\"fft_size\":%zu,\"hop_size\":%zu,"
        "\"pitch_confidence_floor\":%.17g,"
        "\"spectral_floor_dbfs\":%.17g,\"max_partials\":%zu}",
        HWA_PRODUCTION_SOURCE_MEASUREMENT_METHOD_VERSION, method->fft_size,
        method->hop_size, method->pitch_confidence_floor,
        method->spectral_floor_dbfs, method->max_partials) < 0 ? -1 : 0;
}

static int hwa_production_json_format(FILE *stream,
                                      const HWAFormat *format)
{
    return fputs("{\"container\":", stream) == EOF ||
           hwa_json_write_string(
               stream, hwa_container_name(format->container)) != 0 ||
           fputs(",\"encoding\":", stream) == EOF ||
           hwa_json_write_string(
               stream, hwa_encoding_name(format->encoding)) != 0 ||
           fprintf(
               stream,
               ",\"channels\":%u,\"sample_rate_hz\":%" PRIu32
               ",\"bits_per_sample\":%u,\"valid_bits_per_sample\":%u,"
               "\"block_align\":%u,\"channel_mask\":%" PRIu32
               ",\"frames\":%" PRIu64 ",\"data_bytes\":%" PRIu64
               ",\"duration_seconds\":%.17g}",
               (unsigned)format->channels, format->sample_rate_hz,
               (unsigned)format->bits_per_sample,
               (unsigned)format->valid_bits_per_sample,
               (unsigned)format->block_align, format->channel_mask,
               format->frames, format->data_bytes,
               format->duration_seconds) < 0 ? -1 : 0;
}

static int hwa_production_json_source(
    FILE *stream,
    const HWAProductionSource *source)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"role\":", source->id) < 0 ||
        hwa_json_write_string(stream, source->role) != 0 ||
        fputs(",\"input\":", stream) == EOF ||
        hwa_production_json_path(
            stream, source->path, source->sha256) != 0 ||
        fprintf(stream, ",\"is_wave\":%s,\"format\":",
                source->is_wave ? "true" : "false") < 0) return -1;
    if (source->is_wave) {
        if (hwa_production_json_format(
                stream, &source->format) != 0) return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    return fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_production_json_span(FILE *stream,
                                    const HWAProductionSpan *span)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"split\":", span->id) < 0 ||
        hwa_json_write_string(
            stream, hwa_production_split_name(span->split)) != 0 ||
        fputs(",\"item_key\":", stream) == EOF ||
        hwa_json_write_string(stream, span->item_key) != 0 ||
        fputs(",\"item_kind\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_measure_item_kind_name(span->item_kind)) != 0 ||
        fputs(",\"item_role\":", stream) == EOF ||
        hwa_json_write_string(stream, span->item_role) != 0 ||
        fprintf(
            stream,
            ",\"reference_item_id\":%" PRIu64
            ",\"reference_start_sample\":%" PRIu64
            ",\"reference_end_sample\":%" PRIu64
            ",\"model_item_id\":%" PRIu64
            ",\"model_start_sample\":%" PRIu64
            ",\"model_end_sample\":%" PRIu64
            ",\"eligibility_flags\":%" PRIu32 "}",
            span->reference_item_id, span->reference_start_sample,
            span->reference_end_sample, span->model_item_id,
            span->model_start_sample, span->model_end_sample,
            span->eligibility_flags) < 0) return -1;
    return 0;
}

static int hwa_production_json_fit(FILE *stream,
                                   const HWAProductionFit *fit)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"scope\":", fit->id) < 0 ||
        hwa_json_write_string(
            stream, hwa_production_scope_name(fit->scope)) != 0 ||
        fputs(",\"kind\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_production_fit_kind_name(fit->kind)) != 0 ||
        fprintf(stream, ",\"index\":%" PRIu32 ",\"unit\":", fit->index) < 0 ||
        hwa_json_write_string(
            stream, hwa_production_unit_name(fit->unit)) != 0 ||
        fputs(",\"availability\":", stream) == EOF ||
        hwa_json_write_string(
            stream,
            hwa_production_availability_name(fit->availability)) != 0 ||
        fputs(",\"estimate\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, fit->estimate, fit->estimate_valid) != 0 ||
        fputs(",\"q05\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, fit->q05, fit->uncertainty_valid) != 0 ||
        fputs(",\"q95\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, fit->q95, fit->uncertainty_valid) != 0 ||
        fprintf(
            stream,
            ",\"span_count\":%zu,\"point_count\":%zu,"
            "\"quality_flags\":%" PRIu32
            ",\"estimate_valid\":%s,\"uncertainty_valid\":%s}",
            fit->span_count, fit->point_count, fit->quality_flags,
            fit->estimate_valid ? "true" : "false",
            fit->uncertainty_valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_production_json_evaluation(
    FILE *stream,
    const HWAProductionEvaluation *row)
{
    if (fprintf(
            stream,
            "{\"id\":%" PRIu64 ",\"span_id\":%" PRIu64 ",\"view\":",
            row->id, row->span_id) < 0 ||
        hwa_json_write_string(
            stream, hwa_production_view_name(row->view)) != 0 ||
        fputs(",\"kind\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_production_metric_kind_name(row->kind)) != 0 ||
        fprintf(stream, ",\"index\":%" PRIu32 ",\"unit\":", row->index) < 0 ||
        hwa_json_write_string(
            stream, hwa_production_unit_name(row->unit)) != 0 ||
        fputs(",\"availability\":", stream) == EOF ||
        hwa_json_write_string(
            stream,
            hwa_production_availability_name(row->availability)) != 0 ||
        fputs(",\"reference_value\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, row->reference_value, row->reference_valid) != 0 ||
        fputs(",\"model_value\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, row->model_value, row->model_valid) != 0 ||
        fputs(",\"delta\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, row->delta, row->delta_valid) != 0 ||
        fputs(",\"confidence\":", stream) == EOF ||
        hwa_production_json_number(stream, row->confidence) != 0 ||
        fprintf(
            stream,
            ",\"evidence_flags\":%" PRIu32
            ",\"quality_flags\":%" PRIu32
            ",\"reference_valid\":%s,\"model_valid\":%s,"
            "\"delta_valid\":%s}",
            row->evidence_flags, row->quality_flags,
            row->reference_valid ? "true" : "false",
            row->model_valid ? "true" : "false",
            row->delta_valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_production_json_statistics(
    FILE *stream,
    const HWAProductionStatistics *statistics)
{
    if (fprintf(
            stream,
            "{\"total_count\":%zu,\"valid_count\":%zu,"
            "\"missing_count\":%zu,\"minimum\":",
            statistics->total_count, statistics->valid_count,
            statistics->missing_count) < 0 ||
        hwa_production_json_optional_number(
            stream, statistics->minimum, statistics->valid) != 0 ||
        fputs(",\"q05\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, statistics->q05, statistics->valid) != 0 ||
        fputs(",\"q25\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, statistics->q25, statistics->valid) != 0 ||
        fputs(",\"q50\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, statistics->q50, statistics->valid) != 0 ||
        fputs(",\"q75\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, statistics->q75, statistics->valid) != 0 ||
        fputs(",\"q95\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, statistics->q95, statistics->valid) != 0 ||
        fputs(",\"maximum\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, statistics->maximum, statistics->valid) != 0 ||
        fputs(",\"mean\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, statistics->mean, statistics->valid) != 0 ||
        fputs(",\"population_sd\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, statistics->population_sd, statistics->valid) != 0 ||
        fputs(",\"confidence\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, statistics->confidence, statistics->valid) != 0 ||
        fprintf(stream, ",\"valid\":%s}",
                statistics->valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_production_json_view(FILE *stream,
                                    const HWAProductionViewRow *row)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"split\":", row->id) < 0 ||
        hwa_json_write_string(
            stream, hwa_production_split_name(row->split)) != 0 ||
        fputs(",\"view\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_production_view_name(row->view)) != 0 ||
        fputs(",\"kind\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_production_metric_kind_name(row->kind)) != 0 ||
        fprintf(stream, ",\"index\":%" PRIu32 ",\"unit\":", row->index) < 0 ||
        hwa_json_write_string(
            stream, hwa_production_unit_name(row->unit)) != 0 ||
        fputs(",\"availability\":", stream) == EOF ||
        hwa_json_write_string(
            stream,
            hwa_production_availability_name(row->availability)) != 0 ||
        fputs(",\"reference_statistics\":", stream) == EOF ||
        hwa_production_json_statistics(
            stream, &row->reference_statistics) != 0 ||
        fputs(",\"model_statistics\":", stream) == EOF ||
        hwa_production_json_statistics(
            stream, &row->model_statistics) != 0 ||
        fputs(",\"median_delta\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, row->median_delta, row->gap_valid) != 0 ||
        fputs(",\"quantile_distance\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, row->quantile_distance, row->gap_valid) != 0 ||
        fputs(",\"gap_score\":", stream) == EOF ||
        hwa_production_json_optional_number(
            stream, row->gap_score, row->gap_valid) != 0 ||
        fputs(",\"raw_gap_score\":", stream) == EOF ||
        hwa_production_json_number(stream, row->raw_gap_score) != 0 ||
        fprintf(
            stream,
            ",\"quality_flags\":%" PRIu32
            ",\"survives\":%s,\"gap_valid\":%s}",
            row->quality_flags, row->survives ? "true" : "false",
            row->gap_valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_production_json_warning(
    FILE *stream,
    const HWAProductionWarning *warning)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"code\":", warning->id) < 0 ||
        hwa_json_write_string(stream, warning->code) != 0 ||
        fputs(",\"message\":", stream) == EOF ||
        hwa_json_write_string(stream, warning->message) != 0 ||
        fputs(",\"span_id\":", stream) == EOF ||
        hwa_production_json_optional_u64(
            stream, warning->span_id, warning->span_id_valid) != 0 ||
        fputs(",\"fit_id\":", stream) == EOF ||
        hwa_production_json_optional_u64(
            stream, warning->fit_id, warning->fit_id_valid) != 0 ||
        fprintf(
            stream,
            ",\"span_id_valid\":%s,\"fit_id_valid\":%s}",
            warning->span_id_valid ? "true" : "false",
            warning->fit_id_valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_production_text_bytes(FILE *stream, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != 0U) {
        if (*cursor >= 0x20U && *cursor <= 0x7eU &&
            *cursor != (unsigned char)'\\') {
            if (fputc((int)*cursor, stream) == EOF) return -1;
        } else if (*cursor == (unsigned char)'\\') {
            if (fputs("\\\\", stream) == EOF) return -1;
        } else if (fprintf(stream, "\\x%02x", (unsigned)*cursor) < 0) {
            return -1;
        }
        cursor++;
    }
    return 0;
}

static int hwa_report_production_text_impl(
    FILE *stream,
    const HWAProductionResult *result)
{
    size_t index;
    size_t train = 0U;
    size_t check = 0U;
    size_t available = 0U;
    size_t unavailable = 0U;
    size_t insufficient = 0U;
    size_t dry_available = 0U;
    size_t dry_survives = 0U;
    size_t room_available = 0U;
    size_t room_survives = 0U;
    size_t shown = 0U;
    char error[HWA_ERROR_SIZE];
    if (stream == NULL ||
        hwa_production_result_validate(
            result, error, sizeof(error)) != 0) return -1;
    for (index = 0U; index < result->span_count; ++index) {
        if (result->spans[index].split == HWA_PRODUCTION_TRAIN) train++;
        else check++;
    }
    for (index = 0U; index < result->fit_count; ++index) {
        if (result->fits[index].availability == HWA_PRODUCTION_AVAILABLE) {
            available++;
        } else if (result->fits[index].availability ==
                   HWA_PRODUCTION_UNAVAILABLE) {
            unavailable++;
        } else {
            insufficient++;
        }
    }
    for (index = 0U; index < result->view_row_count; ++index) {
        const HWAProductionViewRow *row = &result->view_rows[index];
        if (row->split != HWA_PRODUCTION_CHECK ||
            row->view == HWA_PRODUCTION_VIEW_RAW) continue;
        if (row->view == HWA_PRODUCTION_VIEW_DRY_LIKE) {
            if (row->availability == HWA_PRODUCTION_AVAILABLE) {
                dry_available++;
            }
            if (row->survives) dry_survives++;
        } else {
            if (row->availability == HWA_PRODUCTION_AVAILABLE) {
                room_available++;
            }
            if (row->survives) room_survives++;
        }
    }
    if (fputs("Production account\nReference audio: ", stream) == EOF ||
        hwa_production_text_bytes(
            stream, result->sources[1].path) != 0 ||
        fputs("\nModel audio: ", stream) == EOF ||
        hwa_production_text_bytes(
            stream, result->sources[3].path) != 0 ||
        fputs("\nRoom IR: ", stream) == EOF) return -1;
    if (result->source_count == 5U) {
        if (hwa_production_text_bytes(
                stream, result->sources[4].path) != 0) return -1;
    } else if (fputs("none", stream) == EOF) {
        return -1;
    }
    if (fprintf(
        stream,
        "\nSource profile method: %s (FFT %zu, hop %zu)\n"
        "Spans: %zu (train %zu, check %zu)\n"
        "Fits: %zu (available %zu, unavailable %zu, insufficient %zu)\n"
        "Evaluation rows: %zu\n"
        "Check dry-like: available %zu, survives %zu\n"
        "Check room-matched: available %zu, survives %zu\n"
        "Warnings: %zu\n"
        "Work: %" PRIu64 " evaluations, %" PRIu64 " retained bytes\n",
        HWA_PRODUCTION_SOURCE_MEASUREMENT_METHOD_VERSION,
        result->profile_method.fft_size, result->profile_method.hop_size,
        result->span_count, train, check, result->fit_count,
        available, unavailable, insufficient,
        result->evaluation_row_count, dry_available, dry_survives,
        room_available, room_survives, result->warning_count,
        result->evaluation_count, result->retained_work_bytes) < 0) return -1;
    for (index = 0U; index < result->view_row_count; ++index) {
        const HWAProductionViewRow *row = &result->view_rows[index];
        if (row->split != HWA_PRODUCTION_CHECK || !row->survives) continue;
        if (shown == 0U &&
            fputs("Surviving CHECK rows:\n", stream) == EOF) return -1;
        if (shown == HWA_PRODUCTION_TEXT_MAX_SURVIVORS) break;
        if (fprintf(
                stream, "  %s %s[%" PRIu32
                "]: raw gap %.6f, corrected gap %.6f\n",
                hwa_production_view_name(row->view),
                hwa_production_metric_kind_name(row->kind), row->index,
                row->raw_gap_score, row->gap_score) < 0) return -1;
        shown++;
    }
    if (dry_survives + room_survives > shown &&
        fprintf(stream, "  ... %zu more surviving rows\n",
                dry_survives + room_survives - shown) < 0) return -1;
    if (result->warning_count != 0U) {
        if (fputs("Warning details:\n", stream) == EOF) return -1;
        shown = result->warning_count < HWA_PRODUCTION_TEXT_MAX_WARNINGS ?
            result->warning_count : HWA_PRODUCTION_TEXT_MAX_WARNINGS;
        for (index = 0U; index < shown; ++index) {
            if (fputs("  ", stream) == EOF ||
                hwa_production_text_bytes(
                    stream, result->warnings[index].code) != 0 ||
                fputs(": ", stream) == EOF ||
                hwa_production_text_bytes(
                    stream, result->warnings[index].message) != 0 ||
                fputc('\n', stream) == EOF) return -1;
        }
        if (result->warning_count > shown &&
            fprintf(stream, "  ... %zu more warnings\n",
                    result->warning_count - shown) < 0) return -1;
    }
    return 0;
}

static int hwa_report_production_json_impl(
    FILE *stream,
    const HWAProductionResult *result)
{
    size_t index;
    char error[HWA_ERROR_SIZE];
    if (stream == NULL ||
        hwa_production_result_validate(
            result, error, sizeof(error)) != 0) return -1;
    if (fputs("{\"schema_version\":8,\"command\":\"account-production\","
              "\"production_schema_version\":1,\"tool_version\":",
              stream) == EOF ||
        hwa_json_write_string(stream, HWA_VERSION) != 0 ||
        fputs(",\"method_version\":", stream) == EOF ||
        hwa_json_write_string(
            stream, HWA_PRODUCTION_METHOD_VERSION) != 0 ||
        fputs(",\"build\":", stream) == EOF ||
        hwa_production_json_build(stream) != 0 ||
        fputs(",\"options\":", stream) == EOF ||
        hwa_production_json_options(stream, &result->options) != 0 ||
        fputs(",\"source_profile_method\":", stream) == EOF ||
        hwa_production_json_profile_method(
            stream, &result->profile_method) != 0 ||
        fprintf(
            stream,
            ",\"work\":{\"retained_work_bytes\":%" PRIu64
            ",\"evaluation_count\":%" PRIu64 "},\"sources\":[",
            result->retained_work_bytes,
            result->evaluation_count) < 0) return -1;
    for (index = 0U; index < result->source_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_production_json_source(
                stream, &result->sources[index]) != 0) return -1;
    }
    if (fputs("],\"spans\":[", stream) == EOF) return -1;
    for (index = 0U; index < result->span_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_production_json_span(
                stream, &result->spans[index]) != 0) return -1;
    }
    if (fputs("],\"fits\":[", stream) == EOF) return -1;
    for (index = 0U; index < result->fit_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_production_json_fit(
                stream, &result->fits[index]) != 0) return -1;
    }
    if (fputs("],\"evaluations\":[", stream) == EOF) return -1;
    for (index = 0U; index < result->evaluation_row_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_production_json_evaluation(
                stream, &result->evaluations[index]) != 0) return -1;
    }
    if (fputs("],\"views\":[", stream) == EOF) return -1;
    for (index = 0U; index < result->view_row_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_production_json_view(
                stream, &result->view_rows[index]) != 0) return -1;
    }
    if (fputs("],\"warnings\":[", stream) == EOF) return -1;
    for (index = 0U; index < result->warning_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_production_json_warning(
                stream, &result->warnings[index]) != 0) return -1;
    }
    return fputs("]}\n", stream) == EOF ? -1 : 0;
}

int hwa_report_production_text(FILE *stream,
                               const HWAProductionResult *result)
{
    HWANumericLocale locale;
    int status;
    if (hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    status = hwa_report_production_text_impl(stream, result);
    if (hwa_c_numeric_locale_end(&locale) != 0) return -1;
    return status;
}

int hwa_report_production_json(FILE *stream,
                               const HWAProductionResult *result)
{
    HWANumericLocale locale;
    int status;
    if (hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    status = hwa_report_production_json_impl(stream, result);
    if (hwa_c_numeric_locale_end(&locale) != 0) return -1;
    return status;
}
