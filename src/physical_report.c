#include "physical_report.h"

#include "alignment_file.h"
#include "numeric_locale.h"
#include "output.h"
#include "physical_check.h"
#include "physical_file.h"

#include <inttypes.h>
#include <stdio.h>

static int hwa_physical_json_number(FILE *stream, double value)
{
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_physical_json_optional_number(FILE *stream,
                                             double value,
                                             int valid)
{
    return valid ? hwa_physical_json_number(stream, value) :
                   (fputs("null", stream) == EOF ? -1 : 0);
}

static int hwa_physical_json_optional_u64(FILE *stream,
                                          uint64_t value,
                                          int valid)
{
    return valid ? (fprintf(stream, "%" PRIu64, value) < 0 ? -1 : 0) :
                   (fputs("null", stream) == EOF ? -1 : 0);
}

static int hwa_physical_json_path(FILE *stream,
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

static int hwa_physical_json_build(FILE *stream)
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

static int hwa_physical_json_profile_limits(
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

static int hwa_physical_json_options(FILE *stream,
                                     const HWAPhysicalOptions *options)
{
    if (fprintf(
            stream,
            "{\"decode_block_frames\":%zu,\"fft_size\":%zu,"
            "\"hop_size\":%zu,\"spectral_floor_dbfs\":%.17g,"
            "\"max_wave_bytes\":%" PRIu64 ",\"max_wave_frames\":%" PRIu64
            ",\"max_work_bytes\":%" PRIu64
            ",\"max_pair_evaluations\":%" PRIu64
            ",\"max_bindings\":%zu,\"max_transforms\":%zu,"
            "\"max_modes\":%zu,\"max_checks\":%zu,"
            "\"max_findings\":%zu,\"max_warnings\":%zu,"
            "\"profile_limits\":",
            options->decode_block_frames, options->fft_size,
            options->hop_size, options->spectral_floor_dbfs,
            options->max_wave_bytes, options->max_wave_frames,
            options->max_work_bytes, options->max_pair_evaluations,
            options->max_bindings, options->max_transforms,
            options->max_modes, options->max_checks,
            options->max_findings, options->max_warnings) < 0 ||
        hwa_physical_json_profile_limits(stream,
                                         &options->profile_limits) != 0 ||
        fputc('}', stream) == EOF) return -1;
    return 0;
}

static int hwa_physical_json_format(FILE *stream, const HWAFormat *format)
{
    return fputs("{\"container\":", stream) == EOF ||
           hwa_json_write_string(stream,
                                 hwa_container_name(format->container)) != 0 ||
           fputs(",\"encoding\":", stream) == EOF ||
           hwa_json_write_string(stream,
                                 hwa_encoding_name(format->encoding)) != 0 ||
           fprintf(stream,
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

static int hwa_physical_json_source(FILE *stream,
                                    const HWAPhysicalSource *source)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"role\":", source->id) < 0 ||
        hwa_json_write_string(stream, source->role) != 0 ||
        fputs(",\"input\":", stream) == EOF ||
        hwa_physical_json_path(stream, source->path, source->sha256) != 0 ||
        fprintf(stream, ",\"is_wave\":%s,\"format\":",
                source->is_wave ? "true" : "false") < 0) return -1;
    if (source->is_wave) {
        if (hwa_physical_json_format(stream, &source->format) != 0) return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    return fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_physical_json_check(FILE *stream,
                                   const HWAPhysicalCheck *check)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"scope\":", check->id) < 0 ||
        hwa_json_write_string(stream, check->scope) != 0 ||
        fputs(",\"case_id\":", stream) == EOF ||
        hwa_json_write_string(stream, check->case_id) != 0 ||
        fputs(",\"element\":", stream) == EOF ||
        hwa_json_write_string(stream, check->element) != 0 ||
        fputs(",\"kind\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_physical_check_kind_name(check->kind)) != 0 ||
        fprintf(stream, ",\"index\":%" PRIu32 ",\"unit\":", check->index) < 0 ||
        hwa_json_write_string(stream,
                              hwa_physical_unit_name(check->unit)) != 0 ||
        fputs(",\"availability\":", stream) == EOF ||
        hwa_json_write_string(
            stream,
            hwa_physical_availability_name(check->availability)) != 0 ||
        fputs(",\"reference_value\":", stream) == EOF ||
        hwa_physical_json_optional_number(
            stream, check->reference_value, check->reference_valid) != 0 ||
        fputs(",\"model_value\":", stream) == EOF ||
        hwa_physical_json_optional_number(
            stream, check->model_value, check->model_valid) != 0 ||
        fputs(",\"delta\":", stream) == EOF ||
        hwa_physical_json_optional_number(
            stream, check->delta, check->delta_valid) != 0 ||
        fputs(",\"confidence\":", stream) == EOF ||
        hwa_physical_json_number(stream, check->confidence) != 0 ||
        fprintf(stream,
                ",\"evidence_flags\":%" PRIu32
                ",\"quality_flags\":%" PRIu32
                ",\"reference_valid\":%s,\"model_valid\":%s,"
                "\"delta_valid\":%s}",
                check->evidence_flags, check->quality_flags,
                check->reference_valid ? "true" : "false",
                check->model_valid ? "true" : "false",
                check->delta_valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_physical_json_finding(FILE *stream,
                                     const HWAPhysicalFinding *finding)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"class\":", finding->id) < 0 ||
        hwa_json_write_string(
            stream,
            hwa_physical_finding_class_name(finding->finding_class)) != 0 ||
        fputs(",\"severity\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_physical_severity_name(finding->severity)) != 0 ||
        fputs(",\"code\":", stream) == EOF ||
        hwa_json_write_string(stream, finding->code) != 0 ||
        fputs(",\"message\":", stream) == EOF ||
        hwa_json_write_string(stream, finding->message) != 0 ||
        fputs(",\"check_id\":", stream) == EOF ||
        hwa_physical_json_optional_u64(
            stream, finding->check_id, finding->check_id_valid) != 0 ||
        fputs(",\"score\":", stream) == EOF ||
        hwa_physical_json_optional_number(
            stream, finding->score, finding->score_valid) != 0 ||
        fprintf(stream,
                ",\"rank\":%zu,\"check_id_valid\":%s,"
                "\"score_valid\":%s}",
                finding->rank,
                finding->check_id_valid ? "true" : "false",
                finding->score_valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_physical_json_warning(FILE *stream,
                                     const HWAPhysicalWarning *warning)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"code\":", warning->id) < 0 ||
        hwa_json_write_string(stream, warning->code) != 0 ||
        fputs(",\"message\":", stream) == EOF ||
        hwa_json_write_string(stream, warning->message) != 0 ||
        fputs(",\"source_id\":", stream) == EOF ||
        hwa_physical_json_optional_u64(
            stream, warning->source_id, warning->source_id_valid) != 0 ||
        fputs(",\"check_id\":", stream) == EOF ||
        hwa_physical_json_optional_u64(
            stream, warning->check_id, warning->check_id_valid) != 0 ||
        fprintf(stream,
                ",\"source_id_valid\":%s,\"check_id_valid\":%s}",
                warning->source_id_valid ? "true" : "false",
                warning->check_id_valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_physical_text_bytes(FILE *stream, const char *text)
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

static int hwa_physical_text_value(FILE *stream, double value, int valid)
{
    return valid ? hwa_physical_json_number(stream, value) :
                   (fputs("n/a", stream) == EOF ? -1 : 0);
}

static int hwa_physical_text_finding(FILE *stream,
                                     const HWAPhysicalCheckSet *set,
                                     const HWAPhysicalFinding *finding,
                                     size_t display_index)
{
    const HWAPhysicalCheck *check = finding->check_id_valid
        ? &set->checks[(size_t)(finding->check_id - 1U)] : NULL;
    if (fprintf(stream, "%zu. [%s] ", display_index,
                hwa_physical_severity_name(finding->severity)) < 0 ||
        hwa_physical_text_bytes(stream, finding->code) != 0 ||
        fputs(": ", stream) == EOF ||
        hwa_physical_text_bytes(stream, finding->message) != 0) return -1;
    if (finding->score_valid) {
        if (fprintf(stream, " (score %.6g)", finding->score) < 0) return -1;
    } else if (fputs(" (score n/a)", stream) == EOF) {
        return -1;
    }
    if (check == NULL) {
        return fputs("\n   check n/a kind=n/a index=n/a scope=n/a case=n/a element=n/a"
                     "\n   reference=n/a model=n/a delta=n/a\n",
                     stream) == EOF ? -1 : 0;
    }
    if (fprintf(stream,
                "\n   check #%" PRIu64 " kind=%s index=%" PRIu32 " scope=",
                check->id, hwa_physical_check_kind_name(check->kind),
                check->index) < 0 ||
        hwa_physical_text_bytes(stream, check->scope) != 0 ||
        fputs(" case=", stream) == EOF ||
        hwa_physical_text_bytes(stream, check->case_id) != 0 ||
        fputs(" element=", stream) == EOF ||
        hwa_physical_text_bytes(stream, check->element) != 0 ||
        fputs("\n   reference=", stream) == EOF ||
        hwa_physical_text_value(
            stream, check->reference_value, check->reference_valid) != 0 ||
        fputs(" model=", stream) == EOF ||
        hwa_physical_text_value(
            stream, check->model_value, check->model_valid) != 0 ||
        fputs(" delta=", stream) == EOF ||
        hwa_physical_text_value(stream, check->delta, check->delta_valid) != 0 ||
        fputc('\n', stream) == EOF) return -1;
    return 0;
}

static int hwa_report_physical_text_impl(FILE *stream,
                                         const HWAPhysicalCheckSet *set)
{
    size_t index;
    size_t available = 0U;
    size_t unavailable = 0U;
    size_t insufficient = 0U;
    size_t scored_shown = 0U;
    size_t missing_shown = 0U;
    size_t display_index = 0U;
    char error[HWA_ERROR_SIZE];
    if (stream == NULL ||
        hwa_physical_check_set_validate(set, error, sizeof(error)) != 0) {
        return -1;
    }
    for (index = 0U; index < set->check_count; ++index) {
        if (set->checks[index].availability == HWA_PHYSICAL_AVAILABLE) {
            available++;
        } else if (set->checks[index].availability ==
                   HWA_PHYSICAL_UNAVAILABLE) {
            unavailable++;
        } else {
            insufficient++;
        }
    }
    if (fputs("Physical checks\nReference: ", stream) == EOF ||
        hwa_physical_text_bytes(stream, set->reference_measures_path) != 0 ||
        fputs("\nModel: ", stream) == EOF ||
        hwa_physical_text_bytes(stream, set->model_measures_path) != 0 ||
        fprintf(stream,
                "\nSources: %zu\nChecks: %zu"
                " (available %zu, unavailable %zu, insufficient %zu)\n"
                "Findings: %zu\nWarnings: %zu\n"
                "Pair evaluations: %" PRIu64 "\nTransforms: %zu\n",
                set->source_count, set->check_count, available,
                unavailable, insufficient, set->finding_count,
                set->warning_count, set->pair_evaluations,
                set->transform_count) < 0) return -1;
    if (set->finding_count != 0U &&
        fputs("Top findings:\n", stream) == EOF) return -1;
    for (index = 0U; index < set->finding_count; ++index) {
        const HWAPhysicalFinding *finding = &set->findings[index];
        if (finding->score_valid) {
            if (scored_shown == 10U) continue;
            scored_shown++;
        } else {
            if (finding->finding_class != HWA_PHYSICAL_FINDING_UNAVAILABLE ||
                missing_shown == 10U) continue;
            missing_shown++;
        }
        display_index++;
        if (hwa_physical_text_finding(
                stream, set, finding, display_index) != 0) return -1;
    }
    return 0;
}

static int hwa_report_physical_json_impl(FILE *stream,
                                         const HWAPhysicalCheckSet *set)
{
    size_t index;
    char error[HWA_ERROR_SIZE];
    if (stream == NULL ||
        hwa_physical_check_set_validate(set, error, sizeof(error)) != 0) {
        return -1;
    }
    if (fputs("{\"schema_version\":7,\"command\":\"check-physical\","
              "\"physical_schema_version\":1,\"tool_version\":",
              stream) == EOF ||
        hwa_json_write_string(stream, HWA_VERSION) != 0 ||
        fputs(",\"method_version\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              HWA_PHYSICAL_CHECK_METHOD_VERSION) != 0 ||
        fputs(",\"build\":", stream) == EOF ||
        hwa_physical_json_build(stream) != 0 ||
        fputs(",\"options\":", stream) == EOF ||
        hwa_physical_json_options(stream, &set->options) != 0 ||
        fputs(",\"profiles\":{\"reference\":", stream) == EOF ||
        hwa_physical_json_path(stream, set->reference_measures_path,
                               set->reference_measures_sha256) != 0 ||
        fputs(",\"model\":", stream) == EOF ||
        hwa_physical_json_path(stream, set->model_measures_path,
                               set->model_measures_sha256) != 0 ||
        fprintf(stream,
                "},\"work\":{\"retained_work_bytes\":%" PRIu64
                ",\"pair_evaluations\":%" PRIu64
                ",\"transform_count\":%zu},\"sources\":[",
                set->retained_work_bytes, set->pair_evaluations,
                set->transform_count) < 0) return -1;
    for (index = 0U; index < set->source_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_physical_json_source(stream, &set->sources[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"checks\":[", stream) == EOF) return -1;
    for (index = 0U; index < set->check_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_physical_json_check(stream, &set->checks[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"findings\":[", stream) == EOF) return -1;
    for (index = 0U; index < set->finding_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_physical_json_finding(stream, &set->findings[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"warnings\":[", stream) == EOF) return -1;
    for (index = 0U; index < set->warning_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_physical_json_warning(stream, &set->warnings[index]) != 0) {
            return -1;
        }
    }
    return fputs("]}\n", stream) == EOF ? -1 : 0;
}

int hwa_report_physical_text(FILE *stream,
                             const HWAPhysicalCheckSet *set)
{
    HWANumericLocale locale;
    int status;
    if (hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    status = hwa_report_physical_text_impl(stream, set);
    if (hwa_c_numeric_locale_end(&locale) != 0) return -1;
    return status;
}

int hwa_report_physical_json(FILE *stream,
                             const HWAPhysicalCheckSet *set)
{
    HWANumericLocale locale;
    int status;
    if (hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    status = hwa_report_physical_json_impl(stream, set);
    if (hwa_c_numeric_locale_end(&locale) != 0) return -1;
    return status;
}
