#include "run_report.h"

#include "alignment_file.h"
#include "numeric_locale.h"
#include "output.h"
#include "run.h"

#include <inttypes.h>
#include <stdio.h>

#define HWA_RUN_TEXT_MAX_DETAILS 12U

static int hwa_run_json_number(FILE *stream, double value)
{
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_run_json_optional_number(FILE *stream,
                                        double value,
                                        int valid)
{
    return valid ? hwa_run_json_number(stream, value) :
                   (fputs("null", stream) == EOF ? -1 : 0);
}

static int hwa_run_json_optional_u64(FILE *stream,
                                     uint64_t value,
                                     int valid)
{
    return valid ? (fprintf(stream, "%" PRIu64, value) < 0 ? -1 : 0) :
                   (fputs("null", stream) == EOF ? -1 : 0);
}

static int hwa_run_json_path(FILE *stream,
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

static int hwa_run_json_build(FILE *stream)
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

static int hwa_run_json_options(FILE *stream, const HWARunOptions *options)
{
    return fprintf(
        stream,
        "{\"decode_block_frames\":%zu,\"max_manifest_bytes\":%" PRIu64
        ",\"max_input_bytes\":%" PRIu64
        ",\"max_input_frames\":%" PRIu64
        ",\"max_probe_bytes\":%" PRIu64
        ",\"max_probe_values\":%" PRIu64
        ",\"max_work_bytes\":%" PRIu64
        ",\"max_evaluations\":%" PRIu64
        ",\"max_stems\":%zu,\"max_probes\":%zu,\"max_links\":%zu"
        ",\"max_json_depth\":%zu,\"max_json_tokens\":%zu"
        ",\"max_result_rows\":%zu,\"max_warnings\":%zu}",
        options->decode_block_frames, options->max_manifest_bytes,
        options->max_input_bytes, options->max_input_frames,
        options->max_probe_bytes, options->max_probe_values,
        options->max_work_bytes, options->max_evaluations,
        options->max_stems, options->max_probes, options->max_links,
        options->max_json_depth, options->max_json_tokens,
        options->max_result_rows, options->max_warnings) < 0 ? -1 : 0;
}

static int hwa_run_json_format(FILE *stream, const HWAFormat *format)
{
    return fputs("{\"container\":", stream) == EOF ||
           hwa_json_write_string(stream,
                                 hwa_container_name(format->container)) != 0 ||
           fputs(",\"encoding\":", stream) == EOF ||
           hwa_json_write_string(stream,
                                 hwa_encoding_name(format->encoding)) != 0 ||
           fprintf(
               stream,
               ",\"channels\":%u,\"sample_rate_hz\":%" PRIu32
               ",\"bits_per_sample\":%u,\"valid_bits_per_sample\":%u"
               ",\"block_align\":%u,\"channel_mask\":%" PRIu32
               ",\"frames\":%" PRIu64 ",\"data_bytes\":%" PRIu64
               ",\"duration_seconds\":%.17g}",
               (unsigned)format->channels, format->sample_rate_hz,
               (unsigned)format->bits_per_sample,
               (unsigned)format->valid_bits_per_sample,
               (unsigned)format->block_align, format->channel_mask,
               format->frames, format->data_bytes,
               format->duration_seconds) < 0 ? -1 : 0;
}

static int hwa_run_json_source(FILE *stream, const HWARunSource *source)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"binding_id\":", source->id) < 0 ||
        hwa_json_write_string(stream, source->binding_id) != 0 ||
        fputs(",\"input\":", stream) == EOF ||
        hwa_run_json_path(stream, source->path, source->sha256) != 0 ||
        fputs(",\"kind\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_run_source_kind_name(source->kind)) != 0 ||
        fputs(",\"side\":", stream) == EOF ||
        hwa_json_write_string(stream, hwa_run_side_name(source->side)) != 0 ||
        fputs(",\"role\":", stream) == EOF) return -1;
    if (source->kind == HWA_RUN_SOURCE_STEM) {
        if (hwa_json_write_string(
                stream, hwa_run_stem_role_name(source->role)) != 0) return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    if (fputs(",\"probe_format\":", stream) == EOF) return -1;
    if (source->kind == HWA_RUN_SOURCE_PROBE) {
        if (hwa_json_write_string(
                stream, hwa_run_probe_format_name(source->probe_format)) != 0) {
            return -1;
        }
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    if (fputs(",\"probe_name\":", stream) == EOF) return -1;
    if (source->kind == HWA_RUN_SOURCE_PROBE) {
        if (hwa_json_write_string(stream, source->probe_name) != 0) return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    if (fputs(",\"unit\":", stream) == EOF) return -1;
    if (source->kind == HWA_RUN_SOURCE_PROBE) {
        if (hwa_json_write_string(stream, source->unit) != 0) return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    if (fputs(",\"format\":", stream) == EOF) return -1;
    if (source->kind == HWA_RUN_SOURCE_STEM) {
        if (hwa_run_json_format(stream, &source->format) != 0) return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    return fprintf(
        stream,
        ",\"start_sample\":%" PRId64 ",\"gain_db\":%.17g"
        ",\"rate_numerator\":%" PRIu64
        ",\"rate_denominator\":%" PRIu64
        ",\"file_bytes\":%" PRIu64
        ",\"value_count\":%" PRIu64 "}",
        source->start_sample, source->gain_db,
        source->rate_numerator, source->rate_denominator,
        source->file_bytes, source->value_count) < 0 ? -1 : 0;
}

static int hwa_run_json_clock(FILE *stream, const HWARunClock *clock)
{
    return fprintf(stream,
                   "{\"id\":%" PRIu64 ",\"role\":", clock->id) < 0 ||
           hwa_json_write_string(stream,
                                 hwa_run_stem_role_name(clock->role)) != 0 ||
           fprintf(stream,
                   ",\"reference_source_id\":%" PRIu64
                   ",\"model_source_id\":%" PRIu64 ",\"availability\":",
                   clock->reference_source_id,
                   clock->model_source_id) < 0 ||
           hwa_json_write_string(
               stream, hwa_run_availability_name(clock->availability)) != 0 ||
           fprintf(stream,
                   ",\"start_offset_samples\":%" PRId64
                   ",\"end_offset_samples\":%" PRId64
                   ",\"drift_samples\":%" PRId64
                   ",\"overlap_frames\":%" PRIu64
                   ",\"drift_ppm\":%.17g,\"quality_flags\":%" PRIu32 "}",
                   clock->start_offset_samples, clock->end_offset_samples,
                   clock->drift_samples, clock->overlap_frames,
                   clock->drift_ppm, clock->quality_flags) < 0 ? -1 : 0;
}

static int hwa_run_json_feature(FILE *stream, const HWARunFeature *feature)
{
    if (fprintf(stream,
                "{\"id\":%" PRIu64 ",\"clock_id\":%" PRIu64
                ",\"role\":",
                feature->id, feature->clock_id) < 0 ||
        hwa_json_write_string(
            stream, hwa_run_stem_role_name(feature->role)) != 0 ||
        fputs(",\"kind\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_run_feature_kind_name(feature->kind)) != 0 ||
        fprintf(stream, ",\"index\":%" PRIu32 ",\"unit\":",
                feature->index) < 0 ||
        hwa_json_write_string(stream, hwa_run_unit_name(feature->unit)) != 0 ||
        fputs(",\"availability\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_run_availability_name(feature->availability)) != 0 ||
        fputs(",\"reference_value\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, feature->reference_value,
                                     feature->reference_valid) != 0 ||
        fputs(",\"model_value\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, feature->model_value,
                                     feature->model_valid) != 0 ||
        fputs(",\"delta\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, feature->delta,
                                     feature->delta_valid) != 0 ||
        fputs(",\"normalized_gap\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, feature->normalized_gap,
                                     feature->gap_valid) != 0 ||
        fprintf(stream, ",\"quality_flags\":%" PRIu32 "}",
                feature->quality_flags) < 0) return -1;
    return 0;
}

static int hwa_run_json_stage(FILE *stream, const HWARunStage *stage)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"from_role\":", stage->id) < 0 ||
        hwa_json_write_string(
            stream, hwa_run_stem_role_name(stage->from_role)) != 0 ||
        fputs(",\"to_role\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_run_stem_role_name(stage->to_role)) != 0 ||
        fputs(",\"availability\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_run_availability_name(stage->availability)) != 0 ||
        fputs(",\"prior_gap\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, stage->prior_gap,
                                     stage->gap_valid) != 0 ||
        fputs(",\"current_gap\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, stage->current_gap,
                                     stage->gap_valid) != 0 ||
        fputs(",\"added_gap\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, stage->added_gap,
                                     stage->gap_valid) != 0 ||
        fprintf(stream, ",\"rank\":%zu,\"quality_flags\":%" PRIu32 "}",
                stage->rank, stage->quality_flags) < 0) return -1;
    return 0;
}

static int hwa_run_json_probe(FILE *stream, const HWARunProbe *probe)
{
    if (fprintf(stream,
                "{\"id\":%" PRIu64 ",\"source_id\":%" PRIu64
                ",\"availability\":",
                probe->id, probe->source_id) < 0 ||
        hwa_json_write_string(
            stream, hwa_run_availability_name(probe->availability)) != 0 ||
        fprintf(stream, ",\"value_count\":%" PRIu64 ",\"minimum\":",
                probe->value_count) < 0 ||
        hwa_run_json_optional_number(stream, probe->minimum,
                                     probe->statistics_valid) != 0 ||
        fputs(",\"maximum\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, probe->maximum,
                                     probe->statistics_valid) != 0 ||
        fputs(",\"mean\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, probe->mean,
                                     probe->statistics_valid) != 0 ||
        fputs(",\"population_sd\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, probe->population_sd,
                                     probe->statistics_valid) != 0 ||
        fputc('}', stream) == EOF) return -1;
    return 0;
}

static int hwa_run_json_link(FILE *stream, const HWARunLink *link)
{
    if (fprintf(stream,
                "{\"id\":%" PRIu64
                ",\"stem_source_id\":%" PRIu64
                ",\"probe_source_id\":%" PRIu64 ",\"feature\":",
                link->id, link->stem_source_id,
                link->probe_source_id) < 0 ||
        hwa_json_write_string(
            stream, hwa_run_feature_kind_name(link->feature)) != 0 ||
        fprintf(stream, ",\"feature_index\":%" PRIu32
                ",\"availability\":", link->feature_index) < 0 ||
        hwa_json_write_string(
            stream, hwa_run_availability_name(link->availability)) != 0 ||
        fprintf(stream,
                ",\"lag_hops\":%" PRId64
                ",\"lag_samples\":%" PRId64 ",\"correlation\":",
                link->lag_hops, link->lag_samples) < 0 ||
        hwa_run_json_optional_number(stream, link->correlation,
                                     link->fit_valid) != 0 ||
        fputs(",\"slope\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, link->slope,
                                     link->fit_valid) != 0 ||
        fputs(",\"intercept\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, link->intercept,
                                     link->fit_valid) != 0 ||
        fputs(",\"r_squared\":", stream) == EOF ||
        hwa_run_json_optional_number(stream, link->r_squared,
                                     link->fit_valid) != 0 ||
        fprintf(stream,
                ",\"point_count\":%zu,\"coverage\":%.17g"
                ",\"quality_flags\":%" PRIu32 "}",
                link->point_count, link->coverage,
                link->quality_flags) < 0) return -1;
    return 0;
}

static int hwa_run_json_warning(FILE *stream, const HWARunWarning *warning)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"code\":", warning->id) < 0 ||
        hwa_json_write_string(stream, warning->code) != 0 ||
        fputs(",\"message\":", stream) == EOF ||
        hwa_json_write_string(stream, warning->message) != 0 ||
        fputs(",\"source_id\":", stream) == EOF ||
        hwa_run_json_optional_u64(stream, warning->source_id,
                                  warning->source_id_valid) != 0 ||
        fputs(",\"clock_id\":", stream) == EOF ||
        hwa_run_json_optional_u64(stream, warning->clock_id,
                                  warning->clock_id_valid) != 0 ||
        fputs(",\"stage_id\":", stream) == EOF ||
        hwa_run_json_optional_u64(stream, warning->stage_id,
                                  warning->stage_id_valid) != 0 ||
        fputs(",\"link_id\":", stream) == EOF ||
        hwa_run_json_optional_u64(stream, warning->link_id,
                                  warning->link_id_valid) != 0 ||
        fputc('}', stream) == EOF) return -1;
    return 0;
}

static int hwa_run_text_bytes(FILE *stream, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != 0U) {
        unsigned char byte = *cursor++;
        if (byte >= 0x20U && byte <= 0x7eU && byte != '\\') {
            if (fputc((int)byte, stream) == EOF) return -1;
        } else if (byte == '\\') {
            if (fputs("\\\\", stream) == EOF) return -1;
        } else if (fprintf(stream, "\\x%02x", (unsigned)byte) < 0) {
            return -1;
        }
    }
    return 0;
}

static const char *hwa_run_source_binding(const HWARunResult *result,
                                          uint64_t source_id)
{
    if (source_id == 0U || source_id > result->source_count) return NULL;
    return result->sources[(size_t)(source_id - UINT64_C(1))].binding_id;
}

static int hwa_run_text_link(FILE *stream,
                             const HWARunResult *result,
                             const HWARunLink *link)
{
    const char *stem = hwa_run_source_binding(result, link->stem_source_id);
    const char *probe = hwa_run_source_binding(result, link->probe_source_id);
    if (stem == NULL || probe == NULL ||
        fprintf(stream, "  link %" PRIu64 " [%s] stem=", link->id,
                hwa_run_availability_name(link->availability)) < 0 ||
        hwa_run_text_bytes(stream, stem) != 0 ||
        fputs("; probe=", stream) == EOF ||
        hwa_run_text_bytes(stream, probe) != 0) return -1;
    if (link->fit_valid) {
        return fprintf(stream,
                       ": lag=%" PRId64 " hops / %" PRId64
                       " samples; r=%+.6g; slope=%+.6g; intercept=%+.6g; "
                       "R2=%.6g; points=%zu; coverage=%.3f\n",
                       link->lag_hops, link->lag_samples,
                       link->correlation, link->slope, link->intercept,
                       link->r_squared, link->point_count,
                       link->coverage) < 0 ? -1 : 0;
    }
    return fprintf(stream,
                   ": fit unavailable; lag=%" PRId64 " hops / %" PRId64
                   " samples; points=%zu; coverage=%.3f; flags=%" PRIu32
                   "\n",
                   link->lag_hops, link->lag_samples, link->point_count,
                   link->coverage, link->quality_flags) < 0 ? -1 : 0;
}

static int hwa_report_run_text_impl(FILE *stream, const HWARunResult *result)
{
    size_t index;
    size_t shown = 0U;
    size_t available_clocks = 0U;
    size_t available_features = 0U;
    size_t available_links = 0U;
    char error[HWA_ERROR_SIZE];
    if (stream == NULL ||
        hwa_run_result_validate(result, error, sizeof(error)) != 0) return -1;
    for (index = 0U; index < result->clock_count; ++index) {
        if (result->clocks[index].availability == HWA_RUN_AVAILABLE) {
            available_clocks++;
        }
    }
    for (index = 0U; index < result->feature_count; ++index) {
        if (result->features[index].availability == HWA_RUN_AVAILABLE) {
            available_features++;
        }
    }
    for (index = 0U; index < result->link_count; ++index) {
        if (result->links[index].availability == HWA_RUN_AVAILABLE) {
            available_links++;
        }
    }
    if (fprintf(stream,
                "Run analysis (%s)\n"
                "Clock: %" PRIu32 " Hz\n"
                "Sources: %zu; clocks: %zu available of %zu; "
                "features: %zu available of %zu\n"
                "Stages: %zu; probes: %zu; links: %zu available of %zu; "
                "warnings: %zu\n"
                "Work: %" PRIu64 " retained bytes; %" PRIu64
                " evaluations\n",
                HWA_RUN_METHOD_VERSION, result->clock_rate_hz,
                result->source_count, available_clocks, result->clock_count,
                available_features, result->feature_count,
                result->stage_count, result->probe_count,
                available_links, result->link_count, result->warning_count,
                result->retained_work_bytes,
                result->evaluation_count) < 0) return -1;
    shown = 0U;
    for (index = 0U; index < result->clock_count &&
                     shown < HWA_RUN_TEXT_MAX_DETAILS; ++index) {
        const HWARunClock *clock = &result->clocks[index];
        if (clock->start_offset_samples == 0 && clock->drift_samples == 0) {
            continue;
        }
        if (shown == 0U && fputs("Clock checks that need review:\n", stream) == EOF) {
            return -1;
        }
        if (fprintf(stream,
                    "  %s: start %" PRId64 " samples; end drift %" PRId64
                    " samples (%+.6g ppm)\n",
                    hwa_run_stem_role_name(clock->role),
                    clock->start_offset_samples, clock->drift_samples,
                    clock->drift_ppm) < 0) return -1;
        shown++;
    }
    if (result->link_count != 0U) {
        size_t limit = result->link_count < HWA_RUN_TEXT_MAX_DETAILS
                           ? result->link_count
                           : HWA_RUN_TEXT_MAX_DETAILS;
        if (fputs("Probe links:\n"
                  "  These fits describe association; they do not prove cause.\n",
                  stream) == EOF) return -1;
        for (index = 0U; index < limit; ++index) {
            if (hwa_run_text_link(stream, result, &result->links[index]) != 0) {
                return -1;
            }
        }
        if (result->link_count > limit &&
            fprintf(stream, "  ... %zu more links\n",
                    result->link_count - limit) < 0) return -1;
    }
    shown = 0U;
    for (index = 0U; index < result->stage_count; ++index) {
        if (result->stages[index].gap_valid) shown++;
    }
    if (shown != 0U) {
        if (fputs("Measured stage changes (positive means the saved gap grew):\n",
                  stream) == EOF) return -1;
        shown = 0U;
        for (index = 0U; index < result->stage_count &&
                         shown < HWA_RUN_TEXT_MAX_DETAILS; ++index) {
            const HWARunStage *stage = &result->stages[index];
            if (!stage->gap_valid) continue;
            if (fprintf(stream, "  #%zu %s -> %s: %+.6g\n", stage->rank,
                        hwa_run_stem_role_name(stage->from_role),
                        hwa_run_stem_role_name(stage->to_role),
                        stage->added_gap) < 0) return -1;
            shown++;
        }
    } else if (fputs("Stage changes: unavailable unless source, body, wet, and final each have all 12 valid features.\n",
                     stream) == EOF) {
        return -1;
    }
    if (result->warning_count != 0U) {
        size_t limit = result->warning_count < HWA_RUN_TEXT_MAX_DETAILS ?
            result->warning_count : HWA_RUN_TEXT_MAX_DETAILS;
        if (fputs("Warning details:\n", stream) == EOF) return -1;
        for (index = 0U; index < limit; ++index) {
            if (fputs("  ", stream) == EOF ||
                hwa_run_text_bytes(stream, result->warnings[index].code) != 0 ||
                fputs(": ", stream) == EOF ||
                hwa_run_text_bytes(stream,
                                   result->warnings[index].message) != 0 ||
                fputc('\n', stream) == EOF) return -1;
        }
        if (result->warning_count > limit &&
            fprintf(stream, "  ... %zu more warnings\n",
                    result->warning_count - limit) < 0) return -1;
    }
    return 0;
}

static int hwa_report_run_json_impl(FILE *stream, const HWARunResult *result)
{
    size_t index;
    char error[HWA_ERROR_SIZE];
    if (stream == NULL ||
        hwa_run_result_validate(result, error, sizeof(error)) != 0) return -1;
    if (fputs("{\"schema_version\":9,\"command\":\"analyze-run\","
              "\"run_schema_version\":1,\"tool_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_VERSION) != 0 ||
        fputs(",\"method_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_RUN_METHOD_VERSION) != 0 ||
        fputs(",\"build\":", stream) == EOF ||
        hwa_run_json_build(stream) != 0 ||
        fputs(",\"options\":", stream) == EOF ||
        hwa_run_json_options(stream, &result->options) != 0 ||
        fputs(",\"manifest\":", stream) == EOF ||
        hwa_run_json_path(stream, result->manifest_path,
                          result->manifest_sha256) != 0 ||
        fprintf(stream,
                ",\"clock_rate_hz\":%" PRIu32
                ",\"work\":{\"retained_work_bytes\":%" PRIu64
                ",\"evaluation_count\":%" PRIu64 "},\"sources\":[",
                result->clock_rate_hz, result->retained_work_bytes,
                result->evaluation_count) < 0) return -1;
#define HWA_RUN_JSON_ARRAY(field, count, writer)                            \
    do {                                                                    \
        for (index = 0U; index < result->count; ++index) {                  \
            if ((index != 0U && fputc(',', stream) == EOF) ||               \
                writer(stream, &result->field[index]) != 0) return -1;      \
        }                                                                   \
    } while (0)
    HWA_RUN_JSON_ARRAY(sources, source_count, hwa_run_json_source);
    if (fputs("],\"clocks\":[", stream) == EOF) return -1;
    HWA_RUN_JSON_ARRAY(clocks, clock_count, hwa_run_json_clock);
    if (fputs("],\"features\":[", stream) == EOF) return -1;
    HWA_RUN_JSON_ARRAY(features, feature_count, hwa_run_json_feature);
    if (fputs("],\"stages\":[", stream) == EOF) return -1;
    HWA_RUN_JSON_ARRAY(stages, stage_count, hwa_run_json_stage);
    if (fputs("],\"probes\":[", stream) == EOF) return -1;
    HWA_RUN_JSON_ARRAY(probes, probe_count, hwa_run_json_probe);
    if (fputs("],\"links\":[", stream) == EOF) return -1;
    HWA_RUN_JSON_ARRAY(links, link_count, hwa_run_json_link);
    if (fputs("],\"warnings\":[", stream) == EOF) return -1;
    HWA_RUN_JSON_ARRAY(warnings, warning_count, hwa_run_json_warning);
#undef HWA_RUN_JSON_ARRAY
    return fputs("]}\n", stream) == EOF ? -1 : 0;
}

int hwa_report_run_text(FILE *stream, const HWARunResult *result)
{
    HWANumericLocale locale;
    int status;
    if (hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    status = hwa_report_run_text_impl(stream, result);
    if (hwa_c_numeric_locale_end(&locale) != 0) return -1;
    return status;
}

int hwa_report_run_json(FILE *stream, const HWARunResult *result)
{
    HWANumericLocale locale;
    int status;
    if (hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    status = hwa_report_run_json_impl(stream, result);
    if (hwa_c_numeric_locale_end(&locale) != 0) return -1;
    return status;
}
