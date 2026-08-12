#include "measure_report.h"

#include "alignment_file.h"
#include "measure_compare.h"
#include "measure_file.h"
#include "output.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

static int hwa_measure_json_number(FILE *stream, double value)
{
    if (!isfinite(value)) return fputs("null", stream) == EOF ? -1 : 0;
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_measure_json_optional_number(FILE *stream,
                                            double value,
                                            int valid)
{
    return valid ? hwa_measure_json_number(stream, value) :
                   (fputs("null", stream) == EOF ? -1 : 0);
}

static int hwa_measure_json_path(FILE *stream,
                                 const char *path,
                                 const char *sha256)
{
    return stream == NULL || path == NULL || sha256 == NULL ||
           fputs("{\"path\":", stream) == EOF ||
           hwa_json_write_string(stream, path) != 0 ||
           fputs(",\"path_encoding\":\"utf8_with_invalid_bytes_as_u00xx\","
                 "\"path_bytes_hex\":", stream) == EOF ||
           hwa_json_write_byte_hex(stream, path) != 0 ||
           fputs(",\"sha256\":", stream) == EOF ||
           hwa_json_write_string(stream, sha256) != 0 ||
           fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_measure_json_options(FILE *stream,
                                    const HWAMeasurementOptions *options)
{
    return fprintf(
        stream,
        "{\"decode_block_frames\":%zu,\"fft_size\":%zu,"
        "\"hop_size\":%zu,\"pitch_confidence_floor\":%.17g,"
        "\"spectral_floor_dbfs\":%.17g,\"max_partials\":%zu,"
        "\"max_input_bytes\":%" PRIu64 ",\"max_input_frames\":%" PRIu64
        ",\"max_work_bytes\":%" PRIu64 ",\"max_transforms\":%zu,"
        "\"max_series_points\":%zu,\"max_item_frame_evaluations\":%" PRIu64
        ",\"max_events\":%zu,\"max_items\":%zu,"
        "\"max_item_members\":%zu,\"max_measurements\":%zu,"
        "\"max_groups\":%zu,\"max_group_members\":%zu,"
        "\"max_statistics\":%zu,\"max_warnings\":%zu}",
        options->decode_block_frames, options->fft_size, options->hop_size,
        options->pitch_confidence_floor, options->spectral_floor_dbfs,
        options->max_partials, options->max_input_bytes,
        options->max_input_frames, options->max_work_bytes,
        options->max_transforms, options->max_series_points,
        options->max_item_frame_evaluations, options->max_events,
        options->max_items, options->max_item_members,
        options->max_measurements, options->max_groups,
        options->max_group_members, options->max_statistics,
        options->max_warnings) < 0 ? -1 : 0;
}

static int hwa_compare_json_options(FILE *stream,
                                    const HWAProfileComparisonOptions *options)
{
    return fprintf(
        stream,
        "{\"max_input_bytes\":%" PRIu64 ",\"max_work_bytes\":%" PRIu64
        ",\"max_contexts\":%zu,\"max_measurements\":%zu,"
        "\"max_groups\":%zu,\"max_group_members\":%zu,"
        "\"max_statistics\":%zu,\"max_warnings\":%zu,"
        "\"max_distributions\":%zu,\"max_gaps\":%zu}",
        options->max_input_bytes, options->max_work_bytes,
        options->max_contexts, options->max_measurements,
        options->max_groups, options->max_group_members,
        options->max_statistics, options->max_warnings,
        options->max_distributions, options->max_gaps) < 0 ? -1 : 0;
}

static int hwa_measure_json_nullable_path(FILE *stream,
                                          const char *path,
                                          const char *sha256)
{
    return path == NULL ? (fputs("null", stream) == EOF ? -1 : 0) :
                          hwa_measure_json_path(stream, path, sha256);
}

static int hwa_measure_json_group(FILE *stream,
                                  const HWAMeasureGroup *group)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"key\":", group->id) < 0 ||
        hwa_json_write_string(stream, group->key) != 0 ||
        fputs(",\"item_kind\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_measure_item_kind_name(group->item_kind)) != 0 ||
        fputs(",\"item_role\":", stream) == EOF ||
        hwa_json_write_string(stream, group->item_role) != 0 ||
        fputs(",\"selector\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_measure_group_selector_name(group->selector)) != 0 ||
        fputs(",\"value\":", stream) == EOF ||
        hwa_json_write_string(stream, group->value) != 0 ||
        fprintf(stream, ",\"member_count\":%zu}", group->member_count) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_measure_json_context(FILE *stream,
                                    const HWAMeasureItemContext *context)
{
    const char *names[] = {
        "pitch", "register", "dynamic", "articulation", "part",
        "physical_element", "controller", "technique", "score_section",
        "transition", "gesture"
    };
    const char *values[] = {
        context->labels.pitch, context->labels.register_name,
        context->labels.dynamic, context->labels.articulation,
        context->labels.part, context->labels.physical_element,
        context->labels.controller, context->labels.technique,
        context->labels.score_section, context->labels.transition,
        context->labels.gesture
    };
    size_t index;
    if (fprintf(stream, "{\"item_id\":%" PRIu64 ",\"item_key\":",
                context->item_id) < 0 ||
        hwa_json_write_string(stream, context->item_key) != 0 ||
        fputs(",\"item_kind\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_measure_item_kind_name(context->item_kind)) != 0 ||
        fputs(",\"item_role\":", stream) == EOF ||
        hwa_json_write_string(stream, context->item_role) != 0 ||
        fprintf(stream,
                ",\"start_sample\":%" PRIu64 ",\"end_sample\":%" PRIu64
                ",\"labels\":{",
                context->start_sample, context->end_sample) < 0) return -1;
    for (index = 0U; index < 11U; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_json_write_string(stream, names[index]) != 0 ||
            fputc(':', stream) == EOF) return -1;
        if (values[index] != NULL) {
            if (hwa_json_write_string(stream, values[index]) != 0) return -1;
        } else if (fputs("null", stream) == EOF) return -1;
    }
    return fprintf(stream,
                   "},\"label_override_flags\":%" PRIu32
                   ",\"source_event_count\":%zu,\"item_confidence\":%.17g,"
                   "\"item_quality_flags\":%" PRIu32 ",\"excluded\":%s}",
                   context->labels.override_flags,
                   context->source_event_count, context->item_confidence,
                   context->item_quality_flags,
                   context->excluded ? "true" : "false") < 0 ? -1 : 0;
}

static int hwa_measure_json_build(FILE *stream)
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

static int hwa_measure_json_statistics(FILE *stream,
                                       const HWAMeasureStatistics *statistics)
{
    if (fprintf(stream,
                "{\"total_count\":%zu,\"valid_count\":%zu,"
                "\"missing_count\":%zu,\"minimum\":",
                statistics->total_count, statistics->valid_count,
                statistics->missing_count) < 0 ||
        hwa_measure_json_optional_number(stream, statistics->minimum,
                                         statistics->valid) != 0 ||
        fputs(",\"q05\":", stream) == EOF ||
        hwa_measure_json_optional_number(stream, statistics->q05,
                                         statistics->valid) != 0 ||
        fputs(",\"q25\":", stream) == EOF ||
        hwa_measure_json_optional_number(stream, statistics->q25,
                                         statistics->valid) != 0 ||
        fputs(",\"q50\":", stream) == EOF ||
        hwa_measure_json_optional_number(stream, statistics->q50,
                                         statistics->valid) != 0 ||
        fputs(",\"q75\":", stream) == EOF ||
        hwa_measure_json_optional_number(stream, statistics->q75,
                                         statistics->valid) != 0 ||
        fputs(",\"q95\":", stream) == EOF ||
        hwa_measure_json_optional_number(stream, statistics->q95,
                                         statistics->valid) != 0 ||
        fputs(",\"maximum\":", stream) == EOF ||
        hwa_measure_json_optional_number(stream, statistics->maximum,
                                         statistics->valid) != 0 ||
        fputs(",\"mean\":", stream) == EOF ||
        hwa_measure_json_optional_number(stream, statistics->mean,
                                         statistics->valid) != 0 ||
        fputs(",\"population_sd\":", stream) == EOF ||
        hwa_measure_json_optional_number(stream, statistics->population_sd,
                                         statistics->valid) != 0 ||
        fputs(",\"confidence\":", stream) == EOF ||
        hwa_measure_json_optional_number(stream, statistics->confidence,
                                         statistics->valid) != 0 ||
        fprintf(stream, ",\"valid\":%s}",
                statistics->valid ? "true" : "false") < 0) {
        return -1;
    }
    return 0;
}

static int hwa_measure_json_warning(FILE *stream,
                                    const HWAMeasureWarning *warning)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"code\":", warning->id) < 0 ||
        hwa_json_write_string(stream, warning->code) != 0 ||
        fputs(",\"message\":", stream) == EOF ||
        hwa_json_write_string(stream, warning->message) != 0 ||
        fputs(",\"item_id\":", stream) == EOF) return -1;
    if (warning->item_id_valid) {
        if (fprintf(stream, "%" PRIu64, warning->item_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) return -1;
    if (fputs(",\"observation_id\":", stream) == EOF) return -1;
    if (warning->observation_id_valid) {
        if (fprintf(stream, "%" PRIu64, warning->observation_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) return -1;
    return fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_compare_json_warning(FILE *stream,
                                    const HWAProfileWarning *warning)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"code\":", warning->id) < 0 ||
        hwa_json_write_string(stream, warning->code) != 0 ||
        fputs(",\"message\":", stream) == EOF ||
        hwa_json_write_string(stream, warning->message) != 0 ||
        fputs(",\"group_id\":", stream) == EOF) return -1;
    if (warning->group_id_valid) {
        if (fprintf(stream, "%" PRIu64, warning->group_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) return -1;
    if (fputs(",\"distribution_id\":", stream) == EOF) return -1;
    if (warning->distribution_id_valid) {
        if (fprintf(stream, "%" PRIu64, warning->distribution_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) return -1;
    return fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_measure_text_path(FILE *stream, const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;
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

int hwa_report_measurement_text(FILE *stream,
                                const HWAMeasurementSet *set)
{
    if (stream == NULL || set == NULL || set->items_path == NULL ||
        set->audio_path == NULL ||
        fputs("Measurement profile\nItems: ", stream) == EOF ||
        hwa_measure_text_path(stream, set->items_path) != 0 ||
        fputs("\nAudio: ", stream) == EOF ||
        hwa_measure_text_path(stream, set->audio_path) != 0 ||
        fprintf(stream,
                "\nContexts: %zu\nMeasurements: %zu\n"
                "Role groups: %zu\nStatistics: %zu\nWarnings: %zu\n"
                "Frame evaluations: %" PRIu64 "\n",
                set->context_count, set->measurement_count,
                set->group_count, set->statistic_count, set->warning_count,
                set->item_frame_evaluations) < 0) {
        return -1;
    }
    return 0;
}

int hwa_report_measurement_json(FILE *stream,
                                const HWAMeasurementSet *set)
{
    size_t index;
    if (stream == NULL || set == NULL || set->items_path == NULL ||
        set->audio_path == NULL || set->alignment_path == NULL ||
        set->source_score_path == NULL ||
        (set->context_count != 0U && set->contexts == NULL) ||
        (set->measurement_count != 0U && set->measurements == NULL) ||
        (set->group_count != 0U && set->groups == NULL) ||
        (set->group_member_count != 0U && set->group_members == NULL) ||
        (set->statistic_count != 0U && set->statistics == NULL) ||
        (set->warning_count != 0U && set->warnings == NULL) ||
        fputs("{\"schema_version\":5,\"command\":\"measure\","
              "\"measures_schema_version\":1,\"tool_version\":",
              stream) == EOF ||
        hwa_json_write_string(stream, HWA_VERSION) != 0 ||
        fputs(",\"method_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_MEASUREMENT_METHOD_VERSION) != 0 ||
        fputs(",\"build\":", stream) == EOF ||
        hwa_measure_json_build(stream) != 0 ||
        fputs(",\"options\":", stream) == EOF ||
        hwa_measure_json_options(stream, &set->options) != 0 ||
        fputs(",\"inputs\":{\"items\":", stream) == EOF ||
        hwa_measure_json_path(stream, set->items_path, set->items_sha256) != 0 ||
        fputs(",\"audio\":", stream) == EOF ||
        hwa_measure_json_path(stream, set->audio_path, set->audio_sha256) != 0 ||
        fputs("},\"sources\":{\"alignment\":", stream) == EOF ||
        hwa_measure_json_path(stream, set->alignment_path,
                              set->alignment_sha256) != 0 ||
        fputs(",\"labels\":", stream) == EOF ||
        hwa_measure_json_nullable_path(stream, set->labels_path,
                                       set->labels_sha256) != 0 ||
        fputs(",\"amendment\":", stream) == EOF ||
        hwa_measure_json_nullable_path(stream, set->amendment_path,
                                       set->amendment_sha256) != 0 ||
        fputs(",\"score\":", stream) == EOF ||
        hwa_measure_json_path(stream, set->source_score_path,
                              set->source_score_sha256) != 0 ||
        fprintf(stream,
                "},\"audio\":{\"sample_rate_hz\":%" PRIu32
                ",\"frames\":%" PRIu64 ",\"duration_seconds\":%.17g,"
                "\"channels\":%u,\"container\":",
                set->audio_format.sample_rate_hz, set->audio_format.frames,
                set->audio_format.duration_seconds,
                (unsigned)set->audio_format.channels) < 0 ||
        hwa_json_write_string(
            stream, hwa_container_name(set->audio_format.container)) != 0 ||
        fputs(",\"encoding\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_encoding_name(set->audio_format.encoding)) != 0 ||
        fprintf(stream,
                ",\"bits_per_sample\":%u,\"valid_bits_per_sample\":%u,"
                "\"block_align\":%u,\"channel_mask\":%" PRIu32
                ",\"data_bytes\":%" PRIu64 "},"
                "\"summary\":{\"context_count\":%zu,"
                "\"measurement_count\":%zu,\"group_count\":%zu,"
                "\"group_member_count\":%zu,\"statistic_count\":%zu,"
                "\"warning_count\":%zu,\"item_frame_evaluations\":%" PRIu64
                ",\"level_reference_dbfs\":",
                (unsigned)set->audio_format.bits_per_sample,
                (unsigned)set->audio_format.valid_bits_per_sample,
                (unsigned)set->audio_format.block_align,
                set->audio_format.channel_mask, set->audio_format.data_bytes,
                set->context_count, set->measurement_count, set->group_count,
                set->group_member_count, set->statistic_count,
                set->warning_count, set->item_frame_evaluations) < 0) {
        return -1;
    }
    if (hwa_measure_json_optional_number(
            stream, set->level_reference_dbfs,
            set->level_reference_valid) != 0 ||
        fprintf(stream,
                ",\"level_reference_valid\":%s,"
                "\"level_reference_item_count\":%zu,"
                "\"capability_flags\":%" PRIu32
                ",\"transform_count\":%zu,\"retained_work_bytes\":%" PRIu64
                "},\"contexts\":[",
                set->level_reference_valid ? "true" : "false",
                set->level_reference_item_count, set->capability_flags,
                set->transform_count, set->retained_work_bytes) < 0) {
        return -1;
    }
    for (index = 0U; index < set->context_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_measure_json_context(stream, &set->contexts[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"measurements\":[", stream) == EOF) return -1;
    for (index = 0U; index < set->measurement_count; ++index) {
        const HWAMeasureObservation *observation = &set->measurements[index];
        if ((index != 0U && fputc(',', stream) == EOF) ||
            fprintf(stream,
                    "{\"id\":%" PRIu64 ",\"item_id\":%" PRIu64
                    ",\"kind\":",
                    observation->id, observation->item_id) < 0 ||
            hwa_json_write_string(stream,
                                  hwa_measure_kind_name(observation->kind)) != 0 ||
            fprintf(stream, ",\"index\":%" PRIu32 ",\"unit\":",
                    observation->index) < 0 ||
            hwa_json_write_string(stream,
                                  hwa_measure_unit_name(observation->unit)) != 0 ||
            fputs(",\"view\":", stream) == EOF ||
            hwa_json_write_string(stream,
                                  hwa_measure_view_name(observation->view)) != 0 ||
            fputs(",\"status\":", stream) == EOF ||
            hwa_json_write_string(stream,
                                  hwa_measure_status_name(observation->status)) != 0 ||
            fputs(",\"value\":", stream) == EOF ||
            hwa_measure_json_optional_number(
                stream, observation->value,
                observation->status == HWA_MEASURE_STATUS_VALID) != 0 ||
            fputs(",\"confidence\":", stream) == EOF ||
            hwa_measure_json_number(stream, observation->confidence) != 0 ||
            fprintf(stream,
                    ",\"evidence_flags\":%" PRIu32
                    ",\"quality_flags\":%" PRIu32 "}",
                    observation->evidence_flags,
                    observation->quality_flags) < 0) {
            return -1;
        }
    }
    if (fputs("],\"groups\":[", stream) == EOF) return -1;
    for (index = 0U; index < set->group_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_measure_json_group(stream, &set->groups[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"group_members\":[", stream) == EOF) return -1;
    for (index = 0U; index < set->group_member_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            fprintf(stream,
                    "{\"group_id\":%" PRIu64 ",\"item_id\":%" PRIu64 "}",
                    set->group_members[index].group_id,
                    set->group_members[index].item_id) < 0) return -1;
    }
    if (fputs("],\"statistics\":[", stream) == EOF) return -1;
    for (index = 0U; index < set->statistic_count; ++index) {
        const HWAMeasureStatistic *statistic = &set->statistics[index];
        if ((index != 0U && fputc(',', stream) == EOF) ||
            fprintf(stream,
                    "{\"id\":%" PRIu64 ",\"group_id\":%" PRIu64
                    ",\"kind\":",
                    statistic->id, statistic->group_id) < 0 ||
            hwa_json_write_string(stream,
                                  hwa_measure_kind_name(statistic->kind)) != 0 ||
            fprintf(stream, ",\"index\":%" PRIu32 ",\"unit\":",
                    statistic->index) < 0 ||
            hwa_json_write_string(stream,
                                  hwa_measure_unit_name(statistic->unit)) != 0 ||
            fputs(",\"view\":", stream) == EOF ||
            hwa_json_write_string(stream,
                                  hwa_measure_view_name(statistic->view)) != 0 ||
            fputs(",\"distribution\":", stream) == EOF ||
            hwa_measure_json_statistics(stream, &statistic->statistics) != 0 ||
            fprintf(stream, ",\"quality_flags\":%" PRIu32 "}",
                    statistic->quality_flags) < 0) return -1;
    }
    if (fputs("],\"warnings\":[", stream) == EOF) return -1;
    for (index = 0U; index < set->warning_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_measure_json_warning(stream, &set->warnings[index]) != 0) {
            return -1;
        }
    }
    return fputs("]}", stream) == EOF ? -1 : 0;
}

int hwa_report_profile_comparison_text(
    FILE *stream,
    const HWAProfileComparisonSet *set)
{
    size_t ranked = 0U;
    size_t index;
    if (stream == NULL || set == NULL || set->reference_path == NULL ||
        set->model_path == NULL) return -1;
    for (index = 0U; index < set->gap_count; ++index) {
        if (set->gaps[index].rank != 0U) ranked++;
    }
    if (fputs("Profile comparison\nReference: ", stream) == EOF ||
        hwa_measure_text_path(stream, set->reference_path) != 0 ||
        fputs("\nModel: ", stream) == EOF ||
        hwa_measure_text_path(stream, set->model_path) != 0 ||
        fprintf(stream,
                "\nRole groups: %zu\nDistributions: %zu\n"
                "Ranked descriptive gaps: %zu\nWarnings: %zu\n",
                set->group_count, set->distribution_count,
                ranked, set->warning_count) < 0) return -1;
    return 0;
}

int hwa_report_profile_comparison_json(
    FILE *stream,
    const HWAProfileComparisonSet *set)
{
    size_t index;
    if (stream == NULL || set == NULL || set->reference_path == NULL ||
        set->model_path == NULL ||
        (set->group_count != 0U && set->groups == NULL) ||
        (set->distribution_count != 0U && set->distributions == NULL) ||
        (set->gap_count != 0U && set->gaps == NULL) ||
        (set->warning_count != 0U && set->warnings == NULL) ||
        fputs("{\"schema_version\":6,\"command\":\"compare-measures\","
              "\"comparison_schema_version\":1,\"tool_version\":",
              stream) == EOF ||
        hwa_json_write_string(stream, HWA_VERSION) != 0 ||
        fputs(",\"method_version\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              HWA_PROFILE_COMPARISON_METHOD_VERSION) != 0 ||
        fputs(",\"build\":", stream) == EOF ||
        hwa_measure_json_build(stream) != 0 ||
        fputs(",\"options\":", stream) == EOF ||
        hwa_compare_json_options(stream, &set->options) != 0 ||
        fputs(",\"inputs\":{\"reference\":", stream) == EOF ||
        hwa_measure_json_path(stream, set->reference_path,
                              set->reference_sha256) != 0 ||
        fputs(",\"model\":", stream) == EOF ||
        hwa_measure_json_path(stream, set->model_path,
                              set->model_sha256) != 0 ||
        fprintf(stream,
                "},\"summary\":{\"group_count\":%zu,"
                "\"distribution_count\":%zu,\"gap_count\":%zu,"
                "\"warning_count\":%zu,\"retained_work_bytes\":%" PRIu64
                "},\"groups\":[",
                set->group_count, set->distribution_count,
                set->gap_count, set->warning_count,
                set->retained_work_bytes) < 0) return -1;
    for (index = 0U; index < set->group_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_measure_json_group(stream, &set->groups[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"distributions\":[", stream) == EOF) return -1;
    for (index = 0U; index < set->distribution_count; ++index) {
        const HWAProfileDistribution *distribution =
            &set->distributions[index];
        if ((index != 0U && fputc(',', stream) == EOF) ||
            fprintf(stream,
                    "{\"id\":%" PRIu64 ",\"group_id\":%" PRIu64
                    ",\"kind\":",
                    distribution->id, distribution->group_id) < 0 ||
            hwa_json_write_string(
                stream, hwa_measure_kind_name(distribution->kind)) != 0 ||
            fprintf(stream, ",\"index\":%" PRIu32 ",\"unit\":",
                    distribution->index) < 0 ||
            hwa_json_write_string(
                stream, hwa_measure_unit_name(distribution->unit)) != 0 ||
            fputs(",\"view\":", stream) == EOF ||
            hwa_json_write_string(
                stream, hwa_measure_view_name(distribution->view)) != 0 ||
            fputs(",\"reference\":", stream) == EOF ||
            hwa_measure_json_statistics(
                stream, &distribution->reference_statistics) != 0 ||
            fputs(",\"model\":", stream) == EOF ||
            hwa_measure_json_statistics(
                stream, &distribution->model_statistics) != 0 ||
            fprintf(stream,
                    ",\"reference_valid\":%s,\"model_valid\":%s}",
                    distribution->reference_valid ? "true" : "false",
                    distribution->model_valid ? "true" : "false") < 0) {
            return -1;
        }
    }
    if (fputs("],\"gaps\":[", stream) == EOF) return -1;
    for (index = 0U; index < set->gap_count; ++index) {
        const HWAProfileGap *gap = &set->gaps[index];
        if ((index != 0U && fputc(',', stream) == EOF) ||
            fprintf(stream,
                    "{\"id\":%" PRIu64 ",\"distribution_id\":%" PRIu64
                    ",\"mean_delta\":",
                    gap->id, gap->distribution_id) < 0 ||
            hwa_measure_json_optional_number(stream, gap->mean_delta,
                                             gap->mean_delta_valid) != 0 ||
            fputs(",\"median_delta\":", stream) == EOF ||
            hwa_measure_json_optional_number(stream, gap->median_delta,
                                             gap->median_delta_valid) != 0 ||
            fputs(",\"quantile_distance\":", stream) == EOF ||
            hwa_measure_json_optional_number(stream, gap->quantile_distance,
                                             gap->quantile_distance_valid) != 0 ||
            fputs(",\"standardized_mean_shift\":", stream) == EOF ||
            hwa_measure_json_optional_number(
                stream, gap->standardized_mean_shift,
                gap->standardized_mean_shift_valid) != 0 ||
            fputs(",\"valid_coverage\":", stream) == EOF ||
            hwa_measure_json_optional_number(stream, gap->valid_coverage,
                                             gap->valid_coverage_valid) != 0 ||
            fputs(",\"gap_score\":", stream) == EOF ||
            hwa_measure_json_optional_number(stream, gap->gap_score,
                                             gap->gap_score_valid) != 0 ||
            fprintf(stream,
                    ",\"rank\":%zu,\"quality_flags\":%" PRIu32 "}",
                    gap->rank, gap->quality_flags) < 0) return -1;
    }
    if (fputs("],\"warnings\":[", stream) == EOF) return -1;
    for (index = 0U; index < set->warning_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_compare_json_warning(stream, &set->warnings[index]) != 0) {
            return -1;
        }
    }
    return fputs("]}", stream) == EOF ? -1 : 0;
}
