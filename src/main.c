#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "alignment_file.h"
#include "alignment_report.h"
#include "basic_pitch_audio_provider.h"
#include "basic_pitch_provider.h"
#include "body_envelope_report.h"
#include "event_analysis.h"
#include "event_bundle.h"
#include "experiment.h"
#include "experiment_process.h"
#include "experiment_report.h"
#include "file_output.h"
#include "gap_report.h"
#include "gap_report_output.h"
#include "harmonic_decay_report.h"
#include "inference_basic_pitch_onnx.h"
#include "inference_htdemucs_onnx.h"
#include "inference_onnx.h"
#include "inference_provider.h"
#include "htdemucs.h"
#include "internal.h"
#include "instrument_stem_provider.h"
#include "item_file.h"
#include "item_report.h"
#include "isolated_note_report.h"
#include "measure_file.h"
#include "measure_report.h"
#include "output.h"
#include "physical_file.h"
#include "physical_report.h"
#include "production_file.h"
#include "production_report.h"
#include "report.h"
#include "run_file.h"
#include "run_report.h"
#include "sha256.h"
#include "stem_note_derivation.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <signal.h>
#endif

typedef enum HWAExportKind {
    HWA_EXPORT_FRAMES = 1,
    HWA_EXPORT_SPECTROGRAM = 2
} HWAExportKind;

#define HWA_CLI_MAX_BINDINGS 4096U
#define HWA_CLI_EVENT_PATH_LIMIT 4096U

typedef struct HWACli {
    HWAAnalysisOptions options;
    HWAAlignmentOptions alignment_options;
    HWASegmentationOptions segmentation_options;
    HWAMeasurementOptions measurement_options;
    HWAProfileComparisonOptions comparison_options;
    HWAPhysicalOptions physical_options;
    HWAProductionOptions production_options;
    HWARunOptions run_options;
    HWAExperimentOptions experiment_options;
    HWAGapReportOptions gap_report_options;
    HWAIsolatedNoteOptions isolated_note_options;
    HWAHarmonicDecayOptions harmonic_decay_options;
    const char *positionals[5];
    size_t positional_count;
    const char *output_path;
    const char *score_path;
    const char *alignment_path;
    const char *labels_path;
    const char *amend_path;
    const char *items_path;
    const char *room_ir_path;
    const char *renderer_path;
    const char *resume_path;
    const char *model_path;
    const char *expected_model_sha256;
    const char *physical_bindings[HWA_CLI_MAX_BINDINGS];
    size_t physical_binding_count;
    HWABasicPitchDecoderOptions basic_pitch_options;
    uint64_t max_model_bytes;
    uint64_t inference_timeout_milliseconds;
    size_t max_note_events;
    HWAExportKind export_kind;
    int json;
    int replace;
    int analysis_clock_option_set;
    int frame_size_option_set;
    int hop_size_option_set;
    int silence_option_set;
    int analysis_only_option_set;
    int analysis_resource_option_set;
    int analysis_spectral_resource_option_set;
    int decode_block_option_set;
    int max_bytes_option_set;
    int input_frame_limit_set;
    int alignment_option_set;
    int segmentation_option_set;
    int measurement_option_set;
    int comparison_option_set;
    int physical_option_set;
    int production_option_set;
    int run_option_set;
    int experiment_option_set;
    int gap_report_option_set;
    int isolated_note_option_set;
    int isolated_note_expected_set;
    int isolated_note_metrics_set;
    int harmonic_decay_expected_set;
    int inference_option_set;
    int basic_pitch_option_set;
    int allow_run;
} HWACli;

static void hwa_print_usage(FILE *stream)
{
    (void)fprintf(
        stream,
        "Usage:\n"
        "  hlolli-wg-analyzer [OPTIONS] inspect INPUT.wav\n"
        "  hlolli-wg-analyzer [OPTIONS] compare REFERENCE.wav MODEL.wav\n"
        "  hlolli-wg-analyzer [OPTIONS] body-envelope REFERENCE.wav "
        "[MODEL.wav]\n"
        "  hlolli-wg-analyzer --json harmonic-decay REFERENCE.wav "
        "[MODEL.wav] --expected-hz HZ\n"
        "  hlolli-wg-analyzer isolated-note INPUT.wav --expected-hz HZ "
        "--metrics pitch|passive-decay|pitch,passive-decay\n"
        "  hlolli-wg-analyzer [OPTIONS] export INPUT.wav --kind KIND "
        "--output FILE.csv\n"
        "  hlolli-wg-analyzer [OPTIONS] align REFERENCE.wav TARGET.wav "
        "--output FILE.hwa-align\n"
        "  hlolli-wg-analyzer [OPTIONS] align --score SCORE.csv AUDIO.wav "
        "--output FILE.hwa-align\n"
        "  hlolli-wg-analyzer [OPTIONS] segment --alignment ALIGN.hwa-align "
        "AUDIO.wav --output FILE.hwa-items\n"
        "  hlolli-wg-analyzer [OPTIONS] measure --items ITEMS.hwa-items "
        "AUDIO.wav --output FILE.hwa-measures\n"
        "  hlolli-wg-analyzer [OPTIONS] compare-measures REFERENCE.hwa-measures "
        "MODEL.hwa-measures --output FILE.hwa-compare\n"
        "  hlolli-wg-analyzer [OPTIONS] check-physical REFERENCE.hwa-measures "
        "MODEL.hwa-measures --output FILE.hwa-physical\n"
        "  hlolli-wg-analyzer [OPTIONS] account-production "
        "REFERENCE.hwa-measures REFERENCE.wav MODEL.hwa-measures MODEL.wav "
        "[--room-ir ROOM.wav] --output FILE.hwa-production\n"
        "  hlolli-wg-analyzer [OPTIONS] analyze-run RUN.json "
        "--bind ID=PATH [--bind ID=PATH ...] --output FILE.hwa-run\n"
        "  hlolli-wg-analyzer [OPTIONS] experiment EXPERIMENT.json "
        "--renderer PATH --allow-run --bind ID=PATH [--bind ID=PATH ...] "
        "--output NEW_DIRECTORY\n"
        "  hlolli-wg-analyzer [OPTIONS] rank REPORT.json "
        "--bind ID=PATH [--bind ID=PATH ...]\n"
        "  hlolli-wg-analyzer [OPTIONS] excerpt REPORT.json "
        "--bind ID=PATH [--bind ID=PATH ...] --output NEW_DIRECTORY\n"
        "  hlolli-wg-analyzer [OPTIONS] report REPORT.json "
        "--bind ID=PATH [--bind ID=PATH ...] --output NEW_DIRECTORY\n"
        "  hlolli-wg-analyzer [--json] validate-event-bundle "
        "DIRECTORY.hwa-events\n"
        "  hlolli-wg-analyzer [ANALYSIS OPTIONS] analyze-events INPUT.wav "
        "--output NEW.hwa-events\n"
        "  hlolli-wg-analyzer infer-note-events INPUT.wav --model MODEL.onnx "
        "--output NEW.hwa-events\n"
        "  hlolli-wg-analyzer separate-instruments INPUT.wav "
        "--model MODEL.onnx --output NEW.hwa-events\n"
        "  hlolli-wg-analyzer infer-stem-note-events STEMS.hwa-events "
        "--model MODEL.onnx --output NEW.hwa-events\n"
        "  hlolli-wg-analyzer [--json] inference-capabilities\n");
    (void)fprintf(
        stream,
        "\n"
        "Commands that accept standard input use -. Segment and saved-artifact "
        "commands require named files.\n"
        "\n"
        "Output:\n"
        "  --json                      Write JSON for a report or summary.\n"
        "  --kind frames|spectrogram   Select the CSV export.\n"
        "  --output PATH               New artifact path; - for stdout.\n"
        "  --replace                   Permit replacing a regular output file.\n"
        "  --score PATH                Align an unfolded note manifest to audio.\n"
        "  --alignment PATH            Segment a score-to-audio alignment.\n"
        "  --labels PATH               Add typed event labels while segmenting.\n"
        "  --amend PATH                Apply locked anchors or item edits.\n"
        "  --items PATH                Measure one canonical item file.\n"
        "  --bind NAME=PATH            Add a manifest or evidence input.\n"
        "  --room-ir PATH              Add explicit room evidence.\n"
        "  --renderer PATH             Name the experiment renderer.\n"
        "  --resume-from DIRECTORY     Reuse matching checked experiment jobs.\n"
        "  --model PATH                ONNX model for the inference command.\n"
        "  --expect-model-sha256 HEX   Require these exact model bytes.\n"
        "  --allow-run                 Permit the named renderer to run.\n"
        "\n"
        "Channels and frames:\n"
        "  --channel N                 Analyze one 1-based input channel.\n"
        "  --mixdown                   Analyze an equal mono mix.\n"
        "  --block-frames N            Decode block size.\n"
        "  --frame-size N              Power-of-two analysis frame size.\n"
        "  --hop-size N                Analysis hop size.\n"
        "  --silence-threshold DBFS    Activity threshold.\n"
        "\n"
        "Work limits:\n"
        "  --max-bytes N               Maximum input bytes.\n"
        "  --max-frames N              Maximum decoded frames.\n"
        "  --max-work-bytes N          Maximum analysis heap work.\n"
        "  --max-transforms N          Maximum spectral transforms.\n"
        "  --max-track-points N        Maximum exported time points.\n"
        "  --max-spectrum-values N     Maximum stored spectrogram values.\n"
        "  --max-lag N                 Maximum stereo delay lag in samples.\n"
        "  --true-peak-oversample N    1 or 4.\n"
        "  --expected-hz HZ            Expected note fundamental.\n"
        "  --metrics LIST              Isolated-note metrics to check.\n"
        "  --max-note-evaluations N    Maximum note-analysis checks.\n"
        "  --onset-threshold RATIO     Basic Pitch onset cut (default 0.5).\n"
        "  --frame-threshold RATIO     Basic Pitch frame cut (default 0.3).\n"
        "  --minimum-note-frames N     Basic Pitch minimum (default 11).\n"
        "  --energy-tolerance-frames N Basic Pitch release gap (default 11).\n"
        "  --max-note-events N         Maximum inferred notes.\n"
        "  --max-model-bytes N         Maximum ONNX bytes (hard cap INT_MAX).\n"
        "  --inference-timeout-ms N    Provider work deadline (default 600000).\n"
        "\n");
    (void)fprintf(
        stream,
        "Alignment:\n"
        "  --alignment-step SECONDS    Fine feature step (default 0.05).\n"
        "  --coarse-step SECONDS       Coarse feature step (default 0.20).\n"
        "  --dtw-band SECONDS          Coarse path radius (default 45).\n"
        "  --fine-radius SECONDS       Fine path radius (default 1.5).\n"
        "  --refine-radius SECONDS     Local onset search (default 0.15).\n"
        "  --match-threshold RATIO     Low-confidence cut (default 0.45).\n"
        "  --max-dtw-cells N           Maximum visited DTW cells.\n"
        "  --max-alignment-work-bytes N  Maximum alignment work storage.\n"
        "  --max-alignment-points N    Maximum saved alignment points.\n"
        "  --max-score-events N        Maximum note-manifest rows.\n"
        "  --max-manual-anchors N      Maximum locked amend anchors.\n"
        "\n"
        "Segmentation:\n"
        "  --boundary-search SECONDS   Search around mapped bounds (default 0.15).\n"
        "  --tail-limit SECONDS        Residual-tail limit (default 1.5).\n"
        "  --min-phase SECONDS         Minimum attack or release (default 0.02).\n"
        "  --min-body SECONDS          Minimum body length (default 0.05).\n"
        "  --item-threshold RATIO      Low-confidence cut (default 0.45).\n"
        "  --max-segmentation-work-bytes N  Maximum segmentation work storage.\n"
        "  --max-boundary-evaluations N  Maximum boundary candidates checked.\n"
        "  --max-events N              Maximum loaded score events.\n"
        "  --max-items N               Maximum output items.\n"
        "  --max-item-members N        Maximum item membership rows.\n"
        "  --max-label-rows N          Maximum typed label rows.\n"
        "  --max-manual-items N        Maximum amended item rows.\n"
        "\n"
        "Measurement:\n"
        "  --measure-fft-size N        Measurement FFT size (default 4096).\n"
        "  --measure-hop-size N        Measurement hop size (default 256).\n"
        "  --pitch-confidence-floor X  Pitch confidence cut (default 0.30).\n"
        "  --spectral-floor-dbfs DBFS  Spectral floor (default -100).\n"
        "  --max-partials N            Maximum tracked partials (default 12).\n"
        "  --max-measurement-work-bytes N  Maximum measurement heap work.\n"
        "  --max-measurement-transforms N  Maximum measurement FFT count.\n"
        "  --max-measurement-series-points N  Maximum sampled series slots.\n"
        "  --max-measurement-evaluations N  Maximum item-frame checks.\n"
        "  --max-measurement-events N  Maximum loaded item events.\n"
        "  --max-measurement-items N   Maximum loaded item contexts.\n"
        "  --max-measurement-members N Maximum loaded item members.\n"
        "  --max-measurements N        Maximum scalar observations.\n"
        "  --max-measurement-groups N  Maximum role groups.\n"
        "  --max-measurement-group-members N  Maximum group memberships.\n"
        "  --max-measurement-statistics N  Maximum distribution rows.\n"
        "  --max-measurement-warnings N  Maximum measurement warnings.\n"
        "\n"
        "Profile comparison:\n"
        "  --max-comparison-work-bytes N  Maximum comparison heap work.\n"
        "  --max-comparison-contexts N Maximum loaded contexts per profile.\n"
        "  --max-comparison-measurements N  Maximum loaded observations.\n"
        "  --max-comparison-groups N   Maximum loaded or joined groups.\n"
        "  --max-comparison-group-members N  Maximum loaded memberships.\n"
        "  --max-comparison-statistics N  Maximum loaded statistics.\n"
        "  --max-comparison-warnings N Maximum loaded warnings.\n"
        "  --max-distributions N       Maximum compared distributions.\n"
        "  --max-gaps N                Maximum saved gap rows.\n"
        "\n");
    (void)fprintf(
        stream,
        "Physical checks:\n"
        "  --physical-fft-size N       Physical-check FFT size (default 8192).\n"
        "  --physical-hop-size N       Physical-check hop size (default 256).\n"
        "  --physical-floor-dbfs DBFS  Physical-check floor (default -100).\n"
        "  --max-physical-work-bytes N Maximum physical-check heap work.\n"
        "  --max-physical-transforms N Maximum physical-check FFT count.\n"
        "  --max-physical-evaluations N  Maximum profile and signal checks.\n"
        "  --max-physical-bindings N   Maximum explicit WAVE inputs.\n"
        "  --max-physical-modes N      Maximum mode indices per body case.\n"
        "  --max-physical-checks N     Maximum saved physical checks.\n"
        "  --max-physical-findings N   Maximum saved findings.\n"
        "  --max-physical-warnings N   Maximum saved warnings.\n"
        "\n"
        "Production account:\n"
        "  --max-production-ir-frames N  Maximum room impulse frames.\n"
        "  --max-production-work-bytes N  Maximum tracked production work.\n"
        "  --max-production-evaluations N  Maximum production work visits.\n"
        "  --max-production-spans N    Maximum matched spans.\n"
        "  --max-production-envelope-points N  Maximum envelope/IR points.\n"
        "  --max-production-fits N     Maximum saved fit rows.\n"
        "  --max-production-evaluation-rows N  Maximum saved evaluation rows.\n"
        "  --max-production-view-rows N  Maximum saved view rows.\n"
        "  --max-production-warnings N  Maximum saved warnings.\n"
        "\n"
        "Run analysis:\n"
        "  --max-run-manifest-bytes N Maximum manifest bytes.\n"
        "  --max-run-input-bytes N    Maximum bytes per bound input.\n"
        "  --max-run-input-frames N   Maximum frames per bound WAVE.\n"
        "  --max-run-probe-bytes N    Maximum bytes per probe.\n"
        "  --max-run-probe-values N   Maximum probe values.\n"
        "  --max-run-work-bytes N     Maximum tracked run work.\n"
        "  --max-run-evaluations N    Maximum run work visits.\n"
        "  --max-run-stems N          Maximum declared stems.\n"
        "  --max-run-probes N         Maximum declared probes.\n"
        "  --max-run-links N          Maximum declared links.\n"
        "  --max-run-json-depth N     Maximum manifest JSON depth.\n"
        "  --max-run-json-tokens N    Maximum manifest JSON tokens.\n"
        "  --max-run-result-rows N    Maximum saved run rows.\n"
        "  --max-run-warnings N       Maximum saved run warnings.\n"
        "\n"
        "Experiment execution:\n"
        "  --max-experiment-manifest-bytes N  Maximum experiment JSON bytes.\n"
        "  --max-experiment-input-bytes N  Maximum fixed input or renderer bytes.\n"
        "  --max-experiment-work-bytes N  Maximum tracked experiment work.\n"
        "  --max-experiment-bundle-bytes N  Maximum committed bundle bytes.\n"
        "  --max-experiment-output-file-bytes N  Maximum one rendered file.\n"
        "  --max-experiment-run-evaluations N  Maximum summed run-analysis visits.\n"
        "  --max-experiment-job-ms N  Maximum renderer time per job.\n"
        "  --max-experiment-total-ms N  Maximum total experiment time.\n"
        "  --max-experiment-parameters N  Maximum varied parameters.\n"
        "  --max-experiment-levels N  Maximum saved parameter levels.\n"
        "  --max-experiment-cases N   Maximum fit/check cases.\n"
        "  --max-experiment-responses N  Maximum selected responses.\n"
        "  --max-experiment-points N  Maximum parameter points.\n"
        "  --max-experiment-jobs N    Maximum rendered or reused jobs.\n"
        "  --max-experiment-replicates N  Maximum repeats per point and case.\n"
        "  --max-experiment-artifacts N  Maximum rendered artifacts.\n"
        "  --max-experiment-observations N  Maximum response observations.\n"
        "  --max-experiment-sensitivities N  Maximum sensitivity rows.\n"
        "  --max-experiment-interactions N  Maximum interaction rows.\n"
        "  --max-experiment-warnings N  Maximum experiment warnings.\n"
        "\n");
    (void)fprintf(
        stream,
        "Gap report:\n"
        "  --max-report-manifest-bytes N  Maximum report manifest bytes.\n"
        "  --max-report-input-bytes N  Maximum bytes per bound input.\n"
        "  --max-report-input-frames N  Maximum frames per bound WAVE.\n"
        "  --max-report-work-bytes N  Maximum tracked report work.\n"
        "  --max-report-evaluations N  Maximum report work visits.\n"
        "  --max-report-output-file-bytes N  Maximum one report file.\n"
        "  --max-report-bundle-bytes N  Maximum report bundle bytes.\n"
        "  --max-report-excerpt-frames N  Maximum frames per excerpt.\n"
        "  --max-report-total-excerpt-frames N  Maximum summed excerpt frames.\n"
        "  --max-report-sources N     Maximum source rows and bindings.\n"
        "  --max-report-labels N      Maximum label rows.\n"
        "  --max-report-candidates N  Maximum candidate rows.\n"
        "  --max-report-families N    Maximum linked families.\n"
        "  --max-report-groups N      Maximum linked groups.\n"
        "  --max-report-cases N       Maximum case rows.\n"
        "  --max-report-excerpts N    Maximum excerpt rows.\n"
        "  --max-report-warnings N    Maximum report warnings.\n"
        "  --max-report-json-depth N  Maximum manifest JSON depth.\n"
        "  --max-report-json-tokens N  Maximum manifest JSON tokens.\n"
        "\n"
        "  --help                      Show this help.\n"
        "  --version                   Show the program version.\n");
}

static int hwa_finish_stream(FILE *stream, const char *name)
{
    int flush_result = fflush(stream);

    if (flush_result != 0 || ferror(stream)) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: cannot write %s\n", name);
        return -1;
    }
    return 0;
}

static int hwa_parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static int hwa_parse_size(const char *text, size_t *value)
{
    uint64_t parsed;

    if (hwa_parse_u64(text, &parsed) != 0 || parsed > (uint64_t)SIZE_MAX) {
        return -1;
    }
    *value = (size_t)parsed;
    return 0;
}

static int hwa_parse_double(const char *text, double *value)
{
    char *end = NULL;
    double parsed;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(parsed)) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int hwa_parse_isolated_note_metrics(const char *text,
                                           uint32_t *mask)
{
    if (strcmp(text, "pitch") == 0) {
        *mask = HWA_ISOLATED_NOTE_PITCH;
    } else if (strcmp(text, "passive-decay") == 0) {
        *mask = HWA_ISOLATED_NOTE_PASSIVE_DECAY;
    } else if (strcmp(text, "pitch,passive-decay") == 0 ||
               strcmp(text, "passive-decay,pitch") == 0) {
        *mask = HWA_ISOLATED_NOTE_PITCH |
                HWA_ISOLATED_NOTE_PASSIVE_DECAY;
    } else {
        return -1;
    }
    return 0;
}

static int hwa_need_value(int argc,
                          char **argv,
                          int *argument,
                          const char *option,
                          const char **value)
{
    if (*argument + 1 >= argc) {
        (void)fprintf(stderr,
                      "hlolli-wg-analyzer: %s needs a value\n",
                      option);
        return -1;
    }
    *argument += 1;
    *value = argv[*argument];
    return 0;
}

static int hwa_parse_option_with_value(HWACli *cli,
                                       const char *option,
                                       const char *value)
{
    size_t size_value;
    uint64_t u64_value;

    if (strcmp(option, "--channel") == 0) {
        if (hwa_parse_u64(value, &u64_value) != 0 ||
            u64_value == 0U || u64_value > UINT16_MAX ||
            cli->options.channel_mode == HWA_CHANNEL_MIX) {
            return -1;
        }
        cli->options.channel_mode = HWA_CHANNEL_SELECT;
        cli->options.selected_channel = (uint16_t)u64_value;
        cli->analysis_clock_option_set = 1;
    } else if (strcmp(option, "--block-frames") == 0) {
        if (hwa_parse_size(value, &size_value) != 0 || size_value == 0U) {
            return -1;
        }
        cli->options.decode_block_frames = size_value;
        cli->isolated_note_options.decode_block_frames = size_value;
        cli->harmonic_decay_options.decode_block_frames = size_value;
        cli->measurement_options.decode_block_frames = size_value;
        cli->physical_options.decode_block_frames = size_value;
        cli->production_options.decode_block_frames = size_value;
        cli->run_options.decode_block_frames = size_value;
        cli->gap_report_options.decode_block_frames = size_value;
        cli->decode_block_option_set = 1;
    } else if (strcmp(option, "--frame-size") == 0) {
        if (hwa_parse_size(value, &size_value) != 0 || size_value == 0U) {
            return -1;
        }
        cli->options.frame_size = size_value;
        cli->analysis_clock_option_set = 1;
        cli->frame_size_option_set = 1;
    } else if (strcmp(option, "--hop-size") == 0) {
        if (hwa_parse_size(value, &size_value) != 0 || size_value == 0U) {
            return -1;
        }
        cli->options.hop_size = size_value;
        cli->analysis_clock_option_set = 1;
        cli->hop_size_option_set = 1;
    } else if (strcmp(option, "--silence-threshold") == 0) {
        if (hwa_parse_double(value, &cli->options.silence_threshold_dbfs) != 0) {
            return -1;
        }
        cli->analysis_clock_option_set = 1;
        cli->silence_option_set = 1;
    } else if (strcmp(option, "--max-bytes") == 0) {
        if (hwa_parse_u64(value, &cli->options.max_input_bytes) != 0 ||
            cli->options.max_input_bytes == 0U) {
            return -1;
        }
        cli->measurement_options.max_input_bytes =
            cli->options.max_input_bytes;
        cli->comparison_options.max_input_bytes = cli->options.max_input_bytes;
        cli->physical_options.max_wave_bytes = cli->options.max_input_bytes;
        cli->physical_options.profile_limits.max_input_bytes =
            cli->options.max_input_bytes;
        cli->production_options.max_input_bytes = cli->options.max_input_bytes;
        cli->production_options.profile_limits.max_input_bytes =
            cli->options.max_input_bytes;
        cli->run_options.max_input_bytes = cli->options.max_input_bytes;
        cli->gap_report_options.max_input_bytes =
            cli->options.max_input_bytes;
        cli->isolated_note_options.max_input_bytes =
            cli->options.max_input_bytes;
        cli->harmonic_decay_options.max_input_bytes =
            cli->options.max_input_bytes;
        cli->max_bytes_option_set = 1;
    } else if (strcmp(option, "--max-frames") == 0) {
        if (hwa_parse_u64(value, &cli->options.max_input_frames) != 0 ||
            cli->options.max_input_frames == 0U) {
            return -1;
        }
        cli->measurement_options.max_input_frames =
            cli->options.max_input_frames;
        cli->physical_options.max_wave_frames =
            cli->options.max_input_frames;
        cli->production_options.max_input_frames =
            cli->options.max_input_frames;
        cli->run_options.max_input_frames = cli->options.max_input_frames;
        cli->gap_report_options.max_input_frames =
            cli->options.max_input_frames;
        cli->isolated_note_options.max_input_frames =
            cli->options.max_input_frames;
        cli->harmonic_decay_options.max_input_frames =
            cli->options.max_input_frames;
        cli->input_frame_limit_set = 1;
    } else if (strcmp(option, "--max-work-bytes") == 0) {
        if (hwa_parse_u64(value, &cli->options.max_work_bytes) != 0 ||
            cli->options.max_work_bytes == 0U) {
            return -1;
        }
        cli->isolated_note_options.max_work_bytes =
            cli->options.max_work_bytes;
        cli->harmonic_decay_options.max_work_bytes =
            cli->options.max_work_bytes;
        cli->analysis_resource_option_set = 1;
    } else if (strcmp(option, "--expected-hz") == 0) {
        if (hwa_parse_double(value,
                             &cli->isolated_note_options.expected_hz) != 0) {
            return -1;
        }
        cli->harmonic_decay_options.expected_hz =
            cli->isolated_note_options.expected_hz;
        cli->isolated_note_option_set = 1;
        cli->isolated_note_expected_set = 1;
        cli->harmonic_decay_expected_set = 1;
    } else if (strcmp(option, "--metrics") == 0) {
        if (hwa_parse_isolated_note_metrics(
                value, &cli->isolated_note_options.metric_mask) != 0) {
            return -1;
        }
        cli->isolated_note_option_set = 1;
        cli->isolated_note_metrics_set = 1;
    } else if (strcmp(option, "--max-note-evaluations") == 0) {
        if (hwa_parse_u64(
                value, &cli->isolated_note_options.max_evaluations) != 0 ||
            cli->isolated_note_options.max_evaluations == 0U) {
            return -1;
        }
        cli->harmonic_decay_options.max_evaluations =
            cli->isolated_note_options.max_evaluations;
        cli->isolated_note_option_set = 1;
    } else if (strcmp(option, "--onset-threshold") == 0) {
        if (hwa_parse_double(
                value, &cli->basic_pitch_options.onset_threshold) != 0 ||
            cli->basic_pitch_options.onset_threshold < 0.0 ||
            cli->basic_pitch_options.onset_threshold > 1.0)
            return -1;
        cli->basic_pitch_option_set = 1;
    } else if (strcmp(option, "--frame-threshold") == 0) {
        if (hwa_parse_double(
                value, &cli->basic_pitch_options.frame_threshold) != 0 ||
            cli->basic_pitch_options.frame_threshold < 0.0 ||
            cli->basic_pitch_options.frame_threshold > 1.0)
            return -1;
        cli->basic_pitch_option_set = 1;
    } else if (strcmp(option, "--minimum-note-frames") == 0) {
        if (hwa_parse_size(
                value, &cli->basic_pitch_options.minimum_note_frames) != 0 ||
            cli->basic_pitch_options.minimum_note_frames == 0U)
            return -1;
        cli->basic_pitch_option_set = 1;
    } else if (strcmp(option, "--energy-tolerance-frames") == 0) {
        if (hwa_parse_size(
                value,
                &cli->basic_pitch_options.energy_tolerance_frames) != 0 ||
            cli->basic_pitch_options.energy_tolerance_frames == 0U)
            return -1;
        cli->basic_pitch_option_set = 1;
    } else if (strcmp(option, "--max-note-events") == 0) {
        if (hwa_parse_size(value, &cli->max_note_events) != 0 ||
            cli->max_note_events == 0U)
            return -1;
        cli->basic_pitch_option_set = 1;
    } else if (strcmp(option, "--max-model-bytes") == 0) {
        if (hwa_parse_u64(value, &cli->max_model_bytes) != 0 ||
            cli->max_model_bytes == 0U)
            return -1;
        cli->inference_option_set = 1;
    } else if (strcmp(option, "--inference-timeout-ms") == 0) {
        if (hwa_parse_u64(
                value, &cli->inference_timeout_milliseconds) != 0 ||
            cli->inference_timeout_milliseconds == 0U)
            return -1;
        cli->inference_option_set = 1;
    } else if (strcmp(option, "--max-transforms") == 0) {
        if (hwa_parse_size(value, &cli->options.max_transforms) != 0 ||
            cli->options.max_transforms == 0U) {
            return -1;
        }
        cli->analysis_resource_option_set = 1;
        cli->analysis_spectral_resource_option_set = 1;
    } else if (strcmp(option, "--max-track-points") == 0) {
        if (hwa_parse_size(value, &cli->options.max_track_points) != 0 ||
            cli->options.max_track_points == 0U) {
            return -1;
        }
        cli->analysis_resource_option_set = 1;
        cli->analysis_spectral_resource_option_set = 1;
    } else if (strcmp(option, "--max-spectrum-values") == 0) {
        if (hwa_parse_size(value, &cli->options.max_spectrum_values) != 0 ||
            cli->options.max_spectrum_values == 0U) {
            return -1;
        }
        cli->analysis_only_option_set = 1;
    } else if (strcmp(option, "--max-lag") == 0) {
        if (hwa_parse_size(value, &cli->options.max_lag_samples) != 0) {
            return -1;
        }
        cli->analysis_only_option_set = 1;
    } else if (strcmp(option, "--true-peak-oversample") == 0) {
        if (hwa_parse_u64(value, &u64_value) != 0 ||
            (u64_value != 1U && u64_value != 4U)) {
            return -1;
        }
        cli->options.true_peak_oversample = (unsigned)u64_value;
        cli->analysis_only_option_set = 1;
    } else if (strcmp(option, "--output") == 0) {
        cli->output_path = value;
    } else if (strcmp(option, "--model") == 0) {
        cli->model_path = value;
        cli->inference_option_set = 1;
    } else if (strcmp(option, "--expect-model-sha256") == 0) {
        cli->expected_model_sha256 = value;
        cli->inference_option_set = 1;
    } else if (strcmp(option, "--score") == 0) {
        cli->score_path = value;
    } else if (strcmp(option, "--alignment") == 0) {
        cli->alignment_path = value;
    } else if (strcmp(option, "--labels") == 0) {
        cli->labels_path = value;
    } else if (strcmp(option, "--amend") == 0) {
        cli->amend_path = value;
    } else if (strcmp(option, "--items") == 0) {
        cli->items_path = value;
    } else if (strcmp(option, "--room-ir") == 0) {
        cli->room_ir_path = value;
    } else if (strcmp(option, "--renderer") == 0) {
        cli->renderer_path = value;
    } else if (strcmp(option, "--resume-from") == 0) {
        cli->resume_path = value;
    } else if (strcmp(option, "--bind") == 0) {
        if (cli->physical_binding_count >= HWA_CLI_MAX_BINDINGS ||
            strchr(value, '=') == NULL) {
            return -1;
        }
        cli->physical_bindings[cli->physical_binding_count++] = value;
    } else if (strcmp(option, "--alignment-step") == 0) {
        if (hwa_parse_double(value,
                             &cli->alignment_options.alignment_step_seconds) != 0) {
            return -1;
        }
        cli->alignment_option_set = 1;
    } else if (strcmp(option, "--coarse-step") == 0) {
        if (hwa_parse_double(value,
                             &cli->alignment_options.coarse_step_seconds) != 0) {
            return -1;
        }
        cli->alignment_option_set = 1;
    } else if (strcmp(option, "--dtw-band") == 0) {
        if (hwa_parse_double(value,
                             &cli->alignment_options.dtw_band_seconds) != 0) {
            return -1;
        }
        cli->alignment_option_set = 1;
    } else if (strcmp(option, "--fine-radius") == 0) {
        if (hwa_parse_double(value,
                             &cli->alignment_options.fine_radius_seconds) != 0) {
            return -1;
        }
        cli->alignment_option_set = 1;
    } else if (strcmp(option, "--refine-radius") == 0) {
        if (hwa_parse_double(value,
                             &cli->alignment_options.refine_radius_seconds) != 0) {
            return -1;
        }
        cli->alignment_option_set = 1;
    } else if (strcmp(option, "--match-threshold") == 0) {
        if (hwa_parse_double(value,
                             &cli->alignment_options.match_threshold) != 0) {
            return -1;
        }
        cli->alignment_option_set = 1;
    } else if (strcmp(option, "--max-dtw-cells") == 0) {
        if (hwa_parse_u64(value, &cli->alignment_options.max_dtw_cells) != 0 ||
            cli->alignment_options.max_dtw_cells == 0U) {
            return -1;
        }
        cli->alignment_option_set = 1;
    } else if (strcmp(option, "--max-alignment-work-bytes") == 0) {
        if (hwa_parse_u64(
                value, &cli->alignment_options.max_alignment_work_bytes) != 0 ||
            cli->alignment_options.max_alignment_work_bytes == 0U) {
            return -1;
        }
        cli->alignment_option_set = 1;
    } else if (strcmp(option, "--max-alignment-points") == 0) {
        if (hwa_parse_size(value,
                           &cli->alignment_options.max_alignment_points) != 0 ||
            cli->alignment_options.max_alignment_points == 0U) {
            return -1;
        }
        cli->alignment_option_set = 1;
    } else if (strcmp(option, "--max-score-events") == 0) {
        if (hwa_parse_size(value,
                           &cli->alignment_options.max_score_events) != 0 ||
            cli->alignment_options.max_score_events == 0U) {
            return -1;
        }
        cli->alignment_option_set = 1;
    } else if (strcmp(option, "--max-manual-anchors") == 0) {
        if (hwa_parse_size(value,
                           &cli->alignment_options.max_manual_anchors) != 0 ||
            cli->alignment_options.max_manual_anchors == 0U) {
            return -1;
        }
        cli->alignment_option_set = 1;
    } else if (strcmp(option, "--boundary-search") == 0) {
        if (hwa_parse_double(
                value, &cli->segmentation_options.boundary_search_seconds) != 0) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--tail-limit") == 0) {
        if (hwa_parse_double(
                value, &cli->segmentation_options.tail_limit_seconds) != 0) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--min-phase") == 0) {
        if (hwa_parse_double(
                value, &cli->segmentation_options.min_phase_seconds) != 0) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--min-body") == 0) {
        if (hwa_parse_double(
                value, &cli->segmentation_options.min_body_seconds) != 0) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--item-threshold") == 0) {
        if (hwa_parse_double(
                value, &cli->segmentation_options.item_confidence_threshold) != 0) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--max-segmentation-work-bytes") == 0) {
        if (hwa_parse_u64(
                value,
                &cli->segmentation_options.max_segmentation_work_bytes) != 0 ||
            cli->segmentation_options.max_segmentation_work_bytes == 0U) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--max-boundary-evaluations") == 0) {
        if (hwa_parse_u64(
                value,
                &cli->segmentation_options.max_boundary_evaluations) != 0 ||
            cli->segmentation_options.max_boundary_evaluations == 0U) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--max-events") == 0) {
        if (hwa_parse_size(value, &cli->segmentation_options.max_events) != 0 ||
            cli->segmentation_options.max_events == 0U) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--max-items") == 0) {
        if (hwa_parse_size(value, &cli->segmentation_options.max_items) != 0 ||
            cli->segmentation_options.max_items == 0U) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--max-item-members") == 0) {
        if (hwa_parse_size(
                value, &cli->segmentation_options.max_item_members) != 0 ||
            cli->segmentation_options.max_item_members == 0U) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--max-label-rows") == 0) {
        if (hwa_parse_size(
                value, &cli->segmentation_options.max_label_rows) != 0 ||
            cli->segmentation_options.max_label_rows == 0U) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--max-manual-items") == 0) {
        if (hwa_parse_size(
                value, &cli->segmentation_options.max_manual_items) != 0 ||
            cli->segmentation_options.max_manual_items == 0U) {
            return -1;
        }
        cli->segmentation_option_set = 1;
    } else if (strcmp(option, "--measure-fft-size") == 0) {
        if (hwa_parse_size(value, &cli->measurement_options.fft_size) != 0 ||
            cli->measurement_options.fft_size == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--measure-hop-size") == 0) {
        if (hwa_parse_size(value, &cli->measurement_options.hop_size) != 0 ||
            cli->measurement_options.hop_size == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--pitch-confidence-floor") == 0) {
        if (hwa_parse_double(
                value,
                &cli->measurement_options.pitch_confidence_floor) != 0) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--spectral-floor-dbfs") == 0) {
        if (hwa_parse_double(
                value, &cli->measurement_options.spectral_floor_dbfs) != 0) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-partials") == 0) {
        if (hwa_parse_size(value, &cli->measurement_options.max_partials) != 0 ||
            cli->measurement_options.max_partials == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurement-work-bytes") == 0) {
        if (hwa_parse_u64(value,
                          &cli->measurement_options.max_work_bytes) != 0 ||
            cli->measurement_options.max_work_bytes == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurement-transforms") == 0) {
        if (hwa_parse_size(value,
                           &cli->measurement_options.max_transforms) != 0 ||
            cli->measurement_options.max_transforms == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurement-series-points") == 0) {
        if (hwa_parse_size(value,
                           &cli->measurement_options.max_series_points) != 0 ||
            cli->measurement_options.max_series_points == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurement-evaluations") == 0) {
        if (hwa_parse_u64(
                value,
                &cli->measurement_options.max_item_frame_evaluations) != 0 ||
            cli->measurement_options.max_item_frame_evaluations == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurement-events") == 0) {
        if (hwa_parse_size(value, &cli->measurement_options.max_events) != 0 ||
            cli->measurement_options.max_events == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurement-items") == 0) {
        if (hwa_parse_size(value, &cli->measurement_options.max_items) != 0 ||
            cli->measurement_options.max_items == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurement-members") == 0) {
        if (hwa_parse_size(
                value, &cli->measurement_options.max_item_members) != 0 ||
            cli->measurement_options.max_item_members == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurements") == 0) {
        if (hwa_parse_size(
                value, &cli->measurement_options.max_measurements) != 0 ||
            cli->measurement_options.max_measurements == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurement-groups") == 0) {
        if (hwa_parse_size(value, &cli->measurement_options.max_groups) != 0 ||
            cli->measurement_options.max_groups == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurement-group-members") == 0) {
        if (hwa_parse_size(
                value, &cli->measurement_options.max_group_members) != 0 ||
            cli->measurement_options.max_group_members == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurement-statistics") == 0) {
        if (hwa_parse_size(
                value, &cli->measurement_options.max_statistics) != 0 ||
            cli->measurement_options.max_statistics == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-measurement-warnings") == 0) {
        if (hwa_parse_size(value,
                           &cli->measurement_options.max_warnings) != 0 ||
            cli->measurement_options.max_warnings == 0U) {
            return -1;
        }
        cli->measurement_option_set = 1;
    } else if (strcmp(option, "--max-comparison-work-bytes") == 0) {
        if (hwa_parse_u64(value,
                          &cli->comparison_options.max_work_bytes) != 0 ||
            cli->comparison_options.max_work_bytes == 0U) {
            return -1;
        }
        cli->comparison_option_set = 1;
    } else if (strcmp(option, "--max-comparison-contexts") == 0) {
        if (hwa_parse_size(value,
                           &cli->comparison_options.max_contexts) != 0 ||
            cli->comparison_options.max_contexts == 0U) {
            return -1;
        }
        cli->comparison_option_set = 1;
    } else if (strcmp(option, "--max-comparison-measurements") == 0) {
        if (hwa_parse_size(value,
                           &cli->comparison_options.max_measurements) != 0 ||
            cli->comparison_options.max_measurements == 0U) {
            return -1;
        }
        cli->comparison_option_set = 1;
    } else if (strcmp(option, "--max-comparison-groups") == 0) {
        if (hwa_parse_size(value, &cli->comparison_options.max_groups) != 0 ||
            cli->comparison_options.max_groups == 0U) {
            return -1;
        }
        cli->comparison_option_set = 1;
    } else if (strcmp(option, "--max-comparison-group-members") == 0) {
        if (hwa_parse_size(
                value, &cli->comparison_options.max_group_members) != 0 ||
            cli->comparison_options.max_group_members == 0U) {
            return -1;
        }
        cli->comparison_option_set = 1;
    } else if (strcmp(option, "--max-comparison-statistics") == 0) {
        if (hwa_parse_size(
                value, &cli->comparison_options.max_statistics) != 0 ||
            cli->comparison_options.max_statistics == 0U) {
            return -1;
        }
        cli->comparison_option_set = 1;
    } else if (strcmp(option, "--max-comparison-warnings") == 0) {
        if (hwa_parse_size(value,
                           &cli->comparison_options.max_warnings) != 0 ||
            cli->comparison_options.max_warnings == 0U) {
            return -1;
        }
        cli->comparison_option_set = 1;
    } else if (strcmp(option, "--max-distributions") == 0) {
        if (hwa_parse_size(value,
                           &cli->comparison_options.max_distributions) != 0 ||
            cli->comparison_options.max_distributions == 0U) {
            return -1;
        }
        cli->comparison_option_set = 1;
    } else if (strcmp(option, "--max-gaps") == 0) {
        if (hwa_parse_size(value, &cli->comparison_options.max_gaps) != 0 ||
            cli->comparison_options.max_gaps == 0U) {
            return -1;
        }
        cli->comparison_option_set = 1;
    } else if (strcmp(option, "--physical-fft-size") == 0) {
        if (hwa_parse_size(value, &cli->physical_options.fft_size) != 0 ||
            cli->physical_options.fft_size == 0U) {
            return -1;
        }
        cli->physical_option_set = 1;
    } else if (strcmp(option, "--physical-hop-size") == 0) {
        if (hwa_parse_size(value, &cli->physical_options.hop_size) != 0 ||
            cli->physical_options.hop_size == 0U) {
            return -1;
        }
        cli->physical_option_set = 1;
    } else if (strcmp(option, "--physical-floor-dbfs") == 0) {
        if (hwa_parse_double(value,
                             &cli->physical_options.spectral_floor_dbfs) != 0) {
            return -1;
        }
        cli->physical_option_set = 1;
    } else if (strcmp(option, "--max-physical-work-bytes") == 0) {
        if (hwa_parse_u64(value, &cli->physical_options.max_work_bytes) != 0 ||
            cli->physical_options.max_work_bytes == 0U) {
            return -1;
        }
        cli->physical_option_set = 1;
    } else if (strcmp(option, "--max-physical-transforms") == 0) {
        if (hwa_parse_size(value, &cli->physical_options.max_transforms) != 0 ||
            cli->physical_options.max_transforms == 0U) {
            return -1;
        }
        cli->physical_option_set = 1;
    } else if (strcmp(option, "--max-physical-evaluations") == 0) {
        if (hwa_parse_u64(
                value, &cli->physical_options.max_pair_evaluations) != 0 ||
            cli->physical_options.max_pair_evaluations == 0U) {
            return -1;
        }
        cli->physical_option_set = 1;
    } else if (strcmp(option, "--max-physical-bindings") == 0) {
        if (hwa_parse_size(value, &cli->physical_options.max_bindings) != 0 ||
            cli->physical_options.max_bindings == 0U) {
            return -1;
        }
        cli->physical_option_set = 1;
    } else if (strcmp(option, "--max-physical-modes") == 0) {
        if (hwa_parse_size(value, &cli->physical_options.max_modes) != 0 ||
            cli->physical_options.max_modes == 0U) {
            return -1;
        }
        cli->physical_option_set = 1;
    } else if (strcmp(option, "--max-physical-checks") == 0) {
        if (hwa_parse_size(value, &cli->physical_options.max_checks) != 0 ||
            cli->physical_options.max_checks == 0U) {
            return -1;
        }
        cli->physical_option_set = 1;
    } else if (strcmp(option, "--max-physical-findings") == 0) {
        if (hwa_parse_size(value, &cli->physical_options.max_findings) != 0 ||
            cli->physical_options.max_findings == 0U) {
            return -1;
        }
        cli->physical_option_set = 1;
    } else if (strcmp(option, "--max-physical-warnings") == 0) {
        if (hwa_parse_size(value, &cli->physical_options.max_warnings) != 0 ||
            cli->physical_options.max_warnings == 0U) {
            return -1;
        }
        cli->physical_option_set = 1;
    } else if (strcmp(option, "--max-production-ir-frames") == 0) {
        if (hwa_parse_u64(value, &cli->production_options.max_ir_frames) != 0 ||
            cli->production_options.max_ir_frames == 0U) {
            return -1;
        }
        cli->production_option_set = 1;
    } else if (strcmp(option, "--max-production-work-bytes") == 0) {
        if (hwa_parse_u64(value, &cli->production_options.max_work_bytes) != 0 ||
            cli->production_options.max_work_bytes == 0U) {
            return -1;
        }
        cli->production_option_set = 1;
    } else if (strcmp(option, "--max-production-evaluations") == 0) {
        if (hwa_parse_u64(value, &cli->production_options.max_evaluations) != 0 ||
            cli->production_options.max_evaluations == 0U) {
            return -1;
        }
        cli->production_option_set = 1;
    } else if (strcmp(option, "--max-production-spans") == 0) {
        if (hwa_parse_size(value, &cli->production_options.max_spans) != 0 ||
            cli->production_options.max_spans == 0U) {
            return -1;
        }
        cli->production_option_set = 1;
    } else if (strcmp(option, "--max-production-envelope-points") == 0) {
        if (hwa_parse_size(
                value, &cli->production_options.max_envelope_points) != 0 ||
            cli->production_options.max_envelope_points == 0U) {
            return -1;
        }
        cli->production_option_set = 1;
    } else if (strcmp(option, "--max-production-fits") == 0) {
        if (hwa_parse_size(value, &cli->production_options.max_fits) != 0 ||
            cli->production_options.max_fits == 0U) {
            return -1;
        }
        cli->production_option_set = 1;
    } else if (strcmp(option, "--max-production-evaluation-rows") == 0) {
        if (hwa_parse_size(
                value, &cli->production_options.max_evaluation_rows) != 0 ||
            cli->production_options.max_evaluation_rows == 0U) {
            return -1;
        }
        cli->production_option_set = 1;
    } else if (strcmp(option, "--max-production-view-rows") == 0) {
        if (hwa_parse_size(value, &cli->production_options.max_view_rows) != 0 ||
            cli->production_options.max_view_rows == 0U) {
            return -1;
        }
        cli->production_option_set = 1;
    } else if (strcmp(option, "--max-production-warnings") == 0) {
        if (hwa_parse_size(value, &cli->production_options.max_warnings) != 0 ||
            cli->production_options.max_warnings == 0U) {
            return -1;
        }
        cli->production_option_set = 1;
    } else if (strcmp(option, "--max-run-manifest-bytes") == 0) {
        if (hwa_parse_u64(value, &cli->run_options.max_manifest_bytes) != 0 ||
            cli->run_options.max_manifest_bytes == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-input-bytes") == 0) {
        if (hwa_parse_u64(value, &cli->run_options.max_input_bytes) != 0 ||
            cli->run_options.max_input_bytes == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-input-frames") == 0) {
        if (hwa_parse_u64(value, &cli->run_options.max_input_frames) != 0 ||
            cli->run_options.max_input_frames == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-probe-bytes") == 0) {
        if (hwa_parse_u64(value, &cli->run_options.max_probe_bytes) != 0 ||
            cli->run_options.max_probe_bytes == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-probe-values") == 0) {
        if (hwa_parse_u64(value, &cli->run_options.max_probe_values) != 0 ||
            cli->run_options.max_probe_values == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-work-bytes") == 0) {
        if (hwa_parse_u64(value, &cli->run_options.max_work_bytes) != 0 ||
            cli->run_options.max_work_bytes == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-evaluations") == 0) {
        if (hwa_parse_u64(value, &cli->run_options.max_evaluations) != 0 ||
            cli->run_options.max_evaluations == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-stems") == 0) {
        if (hwa_parse_size(value, &cli->run_options.max_stems) != 0 ||
            cli->run_options.max_stems == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-probes") == 0) {
        if (hwa_parse_size(value, &cli->run_options.max_probes) != 0 ||
            cli->run_options.max_probes == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-links") == 0) {
        if (hwa_parse_size(value, &cli->run_options.max_links) != 0 ||
            cli->run_options.max_links == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-json-depth") == 0) {
        if (hwa_parse_size(value, &cli->run_options.max_json_depth) != 0 ||
            cli->run_options.max_json_depth == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-json-tokens") == 0) {
        if (hwa_parse_size(value, &cli->run_options.max_json_tokens) != 0 ||
            cli->run_options.max_json_tokens == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-result-rows") == 0) {
        if (hwa_parse_size(value, &cli->run_options.max_result_rows) != 0 ||
            cli->run_options.max_result_rows == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-run-warnings") == 0) {
        if (hwa_parse_size(value, &cli->run_options.max_warnings) != 0 ||
            cli->run_options.max_warnings == 0U) {
            return -1;
        }
        cli->run_option_set = 1;
    } else if (strcmp(option, "--max-experiment-manifest-bytes") == 0) {
        if (hwa_parse_u64(value,
                          &cli->experiment_options.max_manifest_bytes) != 0 ||
            cli->experiment_options.max_manifest_bytes == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-input-bytes") == 0) {
        if (hwa_parse_u64(value,
                          &cli->experiment_options.max_input_bytes) != 0 ||
            cli->experiment_options.max_input_bytes == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-work-bytes") == 0) {
        if (hwa_parse_u64(value,
                          &cli->experiment_options.max_work_bytes) != 0 ||
            cli->experiment_options.max_work_bytes == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-bundle-bytes") == 0) {
        if (hwa_parse_u64(value,
                          &cli->experiment_options.max_bundle_bytes) != 0 ||
            cli->experiment_options.max_bundle_bytes == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-output-file-bytes") == 0) {
        if (hwa_parse_u64(
                value, &cli->experiment_options.max_output_file_bytes) != 0 ||
            cli->experiment_options.max_output_file_bytes == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-run-evaluations") == 0) {
        if (hwa_parse_u64(
                value,
                &cli->experiment_options.max_total_run_evaluations) != 0 ||
            cli->experiment_options.max_total_run_evaluations == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-job-ms") == 0) {
        if (hwa_parse_u64(
                value, &cli->experiment_options.max_job_milliseconds) != 0 ||
            cli->experiment_options.max_job_milliseconds == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-total-ms") == 0) {
        if (hwa_parse_u64(
                value, &cli->experiment_options.max_total_milliseconds) != 0 ||
            cli->experiment_options.max_total_milliseconds == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-parameters") == 0) {
        if (hwa_parse_size(value,
                           &cli->experiment_options.max_parameters) != 0 ||
            cli->experiment_options.max_parameters == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-levels") == 0) {
        if (hwa_parse_size(value, &cli->experiment_options.max_levels) != 0 ||
            cli->experiment_options.max_levels == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-cases") == 0) {
        if (hwa_parse_size(value, &cli->experiment_options.max_cases) != 0 ||
            cli->experiment_options.max_cases == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-responses") == 0) {
        if (hwa_parse_size(value,
                           &cli->experiment_options.max_responses) != 0 ||
            cli->experiment_options.max_responses == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-points") == 0) {
        if (hwa_parse_size(value, &cli->experiment_options.max_points) != 0 ||
            cli->experiment_options.max_points == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-jobs") == 0) {
        if (hwa_parse_size(value, &cli->experiment_options.max_jobs) != 0 ||
            cli->experiment_options.max_jobs == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-replicates") == 0) {
        if (hwa_parse_size(value,
                           &cli->experiment_options.max_replicates) != 0 ||
            cli->experiment_options.max_replicates == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-artifacts") == 0) {
        if (hwa_parse_size(value,
                           &cli->experiment_options.max_artifacts) != 0 ||
            cli->experiment_options.max_artifacts == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-observations") == 0) {
        if (hwa_parse_size(value,
                           &cli->experiment_options.max_observations) != 0 ||
            cli->experiment_options.max_observations == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-sensitivities") == 0) {
        if (hwa_parse_size(value,
                           &cli->experiment_options.max_sensitivities) != 0 ||
            cli->experiment_options.max_sensitivities == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-interactions") == 0) {
        if (hwa_parse_size(value,
                           &cli->experiment_options.max_interactions) != 0 ||
            cli->experiment_options.max_interactions == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-experiment-warnings") == 0) {
        if (hwa_parse_size(value,
                           &cli->experiment_options.max_warnings) != 0 ||
            cli->experiment_options.max_warnings == 0U) return -1;
        cli->experiment_option_set = 1;
    } else if (strcmp(option, "--max-report-manifest-bytes") == 0) {
        if (hwa_parse_u64(value,
                          &cli->gap_report_options.max_manifest_bytes) != 0 ||
            cli->gap_report_options.max_manifest_bytes == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-input-bytes") == 0) {
        if (hwa_parse_u64(value,
                          &cli->gap_report_options.max_input_bytes) != 0 ||
            cli->gap_report_options.max_input_bytes == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-input-frames") == 0) {
        if (hwa_parse_u64(value,
                          &cli->gap_report_options.max_input_frames) != 0 ||
            cli->gap_report_options.max_input_frames == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-work-bytes") == 0) {
        if (hwa_parse_u64(value,
                          &cli->gap_report_options.max_work_bytes) != 0 ||
            cli->gap_report_options.max_work_bytes == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-evaluations") == 0) {
        if (hwa_parse_u64(value,
                          &cli->gap_report_options.max_evaluations) != 0 ||
            cli->gap_report_options.max_evaluations == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-output-file-bytes") == 0) {
        if (hwa_parse_u64(
                value, &cli->gap_report_options.max_output_file_bytes) != 0 ||
            cli->gap_report_options.max_output_file_bytes == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-bundle-bytes") == 0) {
        if (hwa_parse_u64(value,
                          &cli->gap_report_options.max_bundle_bytes) != 0 ||
            cli->gap_report_options.max_bundle_bytes == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-excerpt-frames") == 0) {
        if (hwa_parse_u64(
                value, &cli->gap_report_options.max_excerpt_frames) != 0 ||
            cli->gap_report_options.max_excerpt_frames == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-total-excerpt-frames") == 0) {
        if (hwa_parse_u64(
                value,
                &cli->gap_report_options.max_total_excerpt_frames) != 0 ||
            cli->gap_report_options.max_total_excerpt_frames == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-sources") == 0) {
        if (hwa_parse_size(value,
                           &cli->gap_report_options.max_sources) != 0 ||
            cli->gap_report_options.max_sources == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-labels") == 0) {
        if (hwa_parse_size(value,
                           &cli->gap_report_options.max_labels) != 0 ||
            cli->gap_report_options.max_labels == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-candidates") == 0) {
        if (hwa_parse_size(value,
                           &cli->gap_report_options.max_candidates) != 0 ||
            cli->gap_report_options.max_candidates == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-groups") == 0) {
        if (hwa_parse_size(value,
                           &cli->gap_report_options.max_groups) != 0 ||
            cli->gap_report_options.max_groups == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-families") == 0) {
        if (hwa_parse_size(value,
                           &cli->gap_report_options.max_families) != 0 ||
            cli->gap_report_options.max_families == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-cases") == 0) {
        if (hwa_parse_size(value,
                           &cli->gap_report_options.max_cases) != 0 ||
            cli->gap_report_options.max_cases == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-excerpts") == 0) {
        if (hwa_parse_size(value,
                           &cli->gap_report_options.max_excerpts) != 0 ||
            cli->gap_report_options.max_excerpts == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-warnings") == 0) {
        if (hwa_parse_size(value,
                           &cli->gap_report_options.max_warnings) != 0 ||
            cli->gap_report_options.max_warnings == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-json-depth") == 0) {
        if (hwa_parse_size(value,
                           &cli->gap_report_options.max_json_depth) != 0 ||
            cli->gap_report_options.max_json_depth == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--max-report-json-tokens") == 0) {
        if (hwa_parse_size(value,
                           &cli->gap_report_options.max_json_tokens) != 0 ||
            cli->gap_report_options.max_json_tokens == 0U) return -1;
        cli->gap_report_option_set = 1;
    } else if (strcmp(option, "--kind") == 0) {
        if (strcmp(value, "frames") == 0) {
            cli->export_kind = HWA_EXPORT_FRAMES;
        } else if (strcmp(value, "spectrogram") == 0) {
            cli->export_kind = HWA_EXPORT_SPECTROGRAM;
        } else {
            return -1;
        }
    } else {
        return -1;
    }
    return 0;
}

static int hwa_parse_cli(int argc, char **argv, HWACli *cli)
{
    int end_options = 0;
    int argument;

    memset(cli, 0, sizeof(*cli));
    hwa_analysis_options_default(&cli->options);
    hwa_alignment_options_default(&cli->alignment_options);
    hwa_segmentation_options_default(&cli->segmentation_options);
    hwa_measurement_options_default(&cli->measurement_options);
    hwa_profile_comparison_options_default(&cli->comparison_options);
    hwa_physical_options_default(&cli->physical_options);
    hwa_production_options_default(&cli->production_options);
    hwa_run_options_default(&cli->run_options);
    hwa_experiment_options_default(&cli->experiment_options);
    hwa_gap_report_options_default(&cli->gap_report_options);
    hwa_isolated_note_options_default(&cli->isolated_note_options);
    hwa_harmonic_decay_options_default(&cli->harmonic_decay_options);
    hwa_basic_pitch_decoder_options_default(&cli->basic_pitch_options);
    cli->max_model_bytes = UINT64_C(268435456);
    cli->inference_timeout_milliseconds = UINT64_C(600000);
    cli->max_note_events = 1000000U;
    for (argument = 1; argument < argc; ++argument) {
        const char *current = argv[argument];

        if (!end_options && strcmp(current, "--") == 0) {
            end_options = 1;
        } else if (!end_options && strcmp(current, "--help") == 0) {
            hwa_print_usage(stdout);
            return 1;
        } else if (!end_options && strcmp(current, "--version") == 0) {
            (void)printf("hlolli-wg-analyzer %s\n", HWA_VERSION);
            return 1;
        } else if (!end_options && strcmp(current, "--json") == 0) {
            cli->json = 1;
        } else if (!end_options && strcmp(current, "--replace") == 0) {
            cli->replace = 1;
        } else if (!end_options && strcmp(current, "--allow-run") == 0) {
            cli->allow_run = 1;
        } else if (!end_options && strcmp(current, "--mixdown") == 0) {
            if (cli->options.channel_mode == HWA_CHANNEL_SELECT) {
                (void)fputs("hlolli-wg-analyzer: --mixdown and --channel conflict\n",
                            stderr);
                return -1;
            }
            cli->options.channel_mode = HWA_CHANNEL_MIX;
            cli->analysis_clock_option_set = 1;
        } else if (!end_options && current[0] == '-' && current[1] != '\0') {
            const char *value;
            if (hwa_need_value(argc, argv, &argument, current, &value) != 0 ||
                hwa_parse_option_with_value(cli, current, value) != 0) {
                (void)fprintf(stderr,
                              "hlolli-wg-analyzer: invalid %s value\n",
                              current);
                return -1;
            }
        } else if (cli->positional_count <
                   sizeof(cli->positionals) / sizeof(cli->positionals[0])) {
            cli->positionals[cli->positional_count++] = current;
        } else {
            (void)fputs("hlolli-wg-analyzer: too many arguments\n", stderr);
            return -1;
        }
    }
    return 0;
}

static int hwa_analyze_or_report(const char *path,
                                 const HWAAnalysisOptions *options,
                                 HWAAnalysis *analysis)
{
    char error[HWA_ERROR_SIZE] = {0};

    if (hwa_analyze_wav_with_options(path, options, analysis,
                                     error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error : "analysis failed");
        return -1;
    }
    return 0;
}

static int hwa_run_inspect(const HWACli *cli)
{
    HWAAnalysis analysis;
    int result = 1;

    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "inspect") != 0 ||
        cli->output_path != NULL || cli->replace ||
        cli->export_kind != 0 || cli->score_path != NULL ||
        cli->alignment_path != NULL || cli->labels_path != NULL ||
        cli->amend_path != NULL || cli->items_path != NULL ||
        cli->room_ir_path != NULL ||
        cli->alignment_option_set || cli->segmentation_option_set ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set ||
        cli->physical_binding_count != 0U) {
        return -1;
    }
    if (hwa_analyze_or_report(cli->positionals[1], &cli->options, &analysis) != 0) {
        return 1;
    }
    if (cli->json) {
        if (fputs("{\"schema_version\":2,\"command\":\"inspect\",\"file\":",
                  stdout) != EOF &&
            hwa_report_analysis_json(stdout, &analysis) == 0 &&
            fputs("}\n", stdout) != EOF &&
            hwa_finish_stream(stdout, "standard output") == 0) {
            result = 0;
        }
    } else if (hwa_report_analysis_text(stdout, &analysis) == 0 &&
               hwa_finish_stream(stdout, "standard output") == 0) {
        result = 0;
    }
    hwa_analysis_free(&analysis);
    return result;
}

static int hwa_run_compare(const HWACli *cli)
{
    HWAAnalysis reference;
    HWAAnalysis model;
    int result = 1;

    if (cli->positional_count != 3U ||
        strcmp(cli->positionals[0], "compare") != 0 ||
        cli->output_path != NULL || cli->replace ||
        cli->export_kind != 0 || cli->score_path != NULL ||
        cli->alignment_path != NULL || cli->labels_path != NULL ||
        cli->amend_path != NULL || cli->items_path != NULL ||
        cli->room_ir_path != NULL ||
        cli->alignment_option_set || cli->segmentation_option_set ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set ||
        cli->physical_binding_count != 0U) {
        return -1;
    }
    if (strcmp(cli->positionals[1], "-") == 0 &&
        strcmp(cli->positionals[2], "-") == 0) {
        (void)fputs("hlolli-wg-analyzer: compare accepts only one stdin input\n",
                    stderr);
        return 2;
    }
    if (hwa_analyze_or_report(cli->positionals[1], &cli->options, &reference) != 0) {
        return 1;
    }
    if (hwa_analyze_or_report(cli->positionals[2], &cli->options, &model) != 0) {
        hwa_analysis_free(&reference);
        return 1;
    }
    if (cli->json) {
        if (fputs("{\"schema_version\":2,\"command\":\"compare\","
                  "\"reference\":",
                  stdout) != EOF &&
            hwa_report_analysis_json(stdout, &reference) == 0 &&
            fputs(",\"model\":", stdout) != EOF &&
            hwa_report_analysis_json(stdout, &model) == 0 &&
            fputs(",\"delta\":", stdout) != EOF &&
            hwa_report_compare_json(stdout, &reference, &model) == 0 &&
            fputs("}\n", stdout) != EOF &&
            hwa_finish_stream(stdout, "standard output") == 0) {
            result = 0;
        }
    } else if (fputs("Reference\n=========\n", stdout) != EOF &&
               hwa_report_analysis_text(stdout, &reference) == 0 &&
               fputs("\nModel\n=====\n", stdout) != EOF &&
               hwa_report_analysis_text(stdout, &model) == 0 &&
               fputc('\n', stdout) != EOF &&
               hwa_report_compare_text(stdout, &reference, &model) == 0 &&
               hwa_finish_stream(stdout, "standard output") == 0) {
        result = 0;
    }
    hwa_analysis_free(&model);
    hwa_analysis_free(&reference);
    return result;
}

static int hwa_run_body_envelope(const HWACli *cli)
{
    HWABodyEnvelopeOptions options;
    HWABodyEnvelopeResult body;
    const char *model_path = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    int report_result;
    int result = 1;

    memset(&body, 0, sizeof(body));
    if ((cli->positional_count != 2U && cli->positional_count != 3U) ||
        strcmp(cli->positionals[0], "body-envelope") != 0 ||
        cli->output_path != NULL || cli->replace || cli->export_kind != 0 ||
        cli->score_path != NULL || cli->alignment_path != NULL ||
        cli->labels_path != NULL || cli->amend_path != NULL ||
        cli->items_path != NULL || cli->room_ir_path != NULL ||
        cli->renderer_path != NULL || cli->resume_path != NULL ||
        cli->allow_run || cli->alignment_option_set ||
        cli->segmentation_option_set || cli->measurement_option_set ||
        cli->comparison_option_set || cli->physical_option_set ||
        cli->production_option_set || cli->run_option_set ||
        cli->experiment_option_set || cli->gap_report_option_set ||
        cli->physical_binding_count != 0U) {
        return -1;
    }
    if (cli->positional_count == 3U) {
        model_path = cli->positionals[2];
        if (strcmp(cli->positionals[1], "-") == 0 &&
            strcmp(model_path, "-") == 0) {
            (void)fputs(
                "hlolli-wg-analyzer: body-envelope accepts one stdin input\n",
                stderr);
            return 2;
        }
    }
    hwa_body_envelope_options_default(&options);
    options.analysis.channel_mode = cli->options.channel_mode;
    options.analysis.selected_channel = cli->options.selected_channel;
    options.analysis.max_input_bytes = cli->options.max_input_bytes;
    options.analysis.max_input_frames = cli->options.max_input_frames;
    if (cli->decode_block_option_set) {
        options.analysis.decode_block_frames =
            cli->options.decode_block_frames;
    }
    if (cli->frame_size_option_set) {
        options.analysis.frame_size = cli->options.frame_size;
    }
    if (cli->hop_size_option_set) {
        options.analysis.hop_size = cli->options.hop_size;
    }
    if (cli->silence_option_set) {
        options.analysis.silence_threshold_dbfs =
            cli->options.silence_threshold_dbfs;
    }
    if (cli->analysis_resource_option_set) {
        options.analysis.max_work_bytes = cli->options.max_work_bytes;
        options.analysis.max_transforms = cli->options.max_transforms;
        options.analysis.max_track_points = cli->options.max_track_points;
    }
    if (cli->analysis_only_option_set) {
        options.analysis.max_spectrum_values =
            cli->options.max_spectrum_values;
    }
    if (hwa_body_envelope_wavs(
            cli->positionals[1], model_path, &options, &body,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "body-envelope analysis failed");
        hwa_body_envelope_result_free(&body);
        return 1;
    }
    report_result = cli->json
                        ? hwa_body_envelope_report_json(stdout, &body)
                        : hwa_body_envelope_report_text(stdout, &body);
    if (report_result == 0 &&
        hwa_finish_stream(stdout, "standard output") == 0) {
        result = 0;
    } else {
        (void)fputs("hlolli-wg-analyzer: cannot write standard output\n",
                    stderr);
    }
    hwa_body_envelope_result_free(&body);
    return result;
}

static int hwa_run_isolated_note(const HWACli *cli)
{
    HWAIsolatedNoteResult note;
    char error[HWA_ERROR_SIZE] = {0};
    int result = 1;

    memset(&note, 0, sizeof(note));
    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "isolated-note") != 0 ||
        !cli->isolated_note_expected_set ||
        !cli->isolated_note_metrics_set ||
        cli->output_path != NULL || cli->replace || cli->export_kind != 0 ||
        cli->score_path != NULL || cli->alignment_path != NULL ||
        cli->labels_path != NULL || cli->amend_path != NULL ||
        cli->items_path != NULL || cli->room_ir_path != NULL ||
        cli->renderer_path != NULL || cli->resume_path != NULL ||
        cli->allow_run || cli->analysis_clock_option_set ||
        cli->analysis_only_option_set || cli->alignment_option_set ||
        cli->segmentation_option_set || cli->measurement_option_set ||
        cli->comparison_option_set || cli->physical_option_set ||
        cli->production_option_set || cli->run_option_set ||
        cli->experiment_option_set || cli->gap_report_option_set ||
        cli->physical_binding_count != 0U) {
        return -1;
    }
    if (hwa_analyze_isolated_note_wav(
            cli->positionals[1], &cli->isolated_note_options, &note,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "isolated-note analysis failed");
        hwa_isolated_note_result_free(&note);
        return 1;
    }
    if (hwa_isolated_note_report_json(stdout, &note) == 0 &&
        hwa_finish_stream(stdout, "standard output") == 0) {
        result = 0;
    } else {
        (void)fputs("hlolli-wg-analyzer: cannot write standard output\n",
                    stderr);
    }
    hwa_isolated_note_result_free(&note);
    return result;
}

static int hwa_run_harmonic_decay(const HWACli *cli)
{
    HWAHarmonicDecayResult decay;
    const char *model_path = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    int result = 1;

    memset(&decay, 0, sizeof(decay));
    if ((cli->positional_count != 2U && cli->positional_count != 3U) ||
        strcmp(cli->positionals[0], "harmonic-decay") != 0 ||
        !cli->json || !cli->harmonic_decay_expected_set ||
        cli->isolated_note_metrics_set ||
        cli->output_path != NULL || cli->replace || cli->export_kind != 0 ||
        cli->score_path != NULL || cli->alignment_path != NULL ||
        cli->labels_path != NULL || cli->amend_path != NULL ||
        cli->items_path != NULL || cli->room_ir_path != NULL ||
        cli->renderer_path != NULL || cli->resume_path != NULL ||
        cli->allow_run || cli->analysis_clock_option_set ||
        cli->analysis_only_option_set ||
        cli->analysis_spectral_resource_option_set ||
        cli->alignment_option_set || cli->segmentation_option_set ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set || cli->experiment_option_set ||
        cli->gap_report_option_set || cli->physical_binding_count != 0U) {
        return -1;
    }
    if (cli->positional_count == 3U) {
        model_path = cli->positionals[2];
        if (strcmp(cli->positionals[1], "-") == 0 &&
            strcmp(model_path, "-") == 0) {
            (void)fputs(
                "hlolli-wg-analyzer: harmonic-decay accepts one stdin input\n",
                stderr);
            return 2;
        }
    }
    if (hwa_harmonic_decay_wavs(
            cli->positionals[1], model_path,
            &cli->harmonic_decay_options, &decay,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "harmonic-decay analysis failed");
        hwa_harmonic_decay_result_free(&decay);
        return 1;
    }
    if (hwa_harmonic_decay_report_json(stdout, &decay) == 0 &&
        hwa_finish_stream(stdout, "standard output") == 0) {
        result = 0;
    } else {
        (void)fputs("hlolli-wg-analyzer: cannot write standard output\n",
                    stderr);
    }
    hwa_harmonic_decay_result_free(&decay);
    return result;
}

static int hwa_run_export(HWACli *cli)
{
    HWAAnalysis analysis;
    HWAFileOutput output;
    const char *protected_paths[1];
    char error[HWA_ERROR_SIZE] = {0};
    int write_result;
    int result = 1;

    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "export") != 0 ||
        cli->output_path == NULL || cli->export_kind == 0 || cli->json ||
        cli->score_path != NULL || cli->alignment_path != NULL ||
        cli->labels_path != NULL || cli->amend_path != NULL ||
        cli->items_path != NULL || cli->room_ir_path != NULL ||
        cli->alignment_option_set ||
        cli->segmentation_option_set || cli->measurement_option_set ||
        cli->physical_option_set ||
        cli->production_option_set || cli->run_option_set ||
        cli->physical_binding_count != 0U ||
        (cli->replace && strcmp(cli->output_path, "-") == 0)) {
        return -1;
    }
    cli->options.collect_tracks = 1;
    cli->options.collect_spectrogram =
        cli->export_kind == HWA_EXPORT_SPECTROGRAM;
    if (hwa_analyze_or_report(cli->positionals[1], &cli->options, &analysis) != 0) {
        return 1;
    }
    protected_paths[0] = cli->positionals[1];
    if (hwa_file_output_open(&output, cli->output_path,
                             protected_paths, 1U, cli->replace,
                             error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        hwa_analysis_free(&analysis);
        return 1;
    }
    write_result = cli->export_kind == HWA_EXPORT_FRAMES
                       ? hwa_report_frames_csv(
                             hwa_file_output_stream(&output), &analysis)
                       : hwa_report_spectrogram_csv(
                             hwa_file_output_stream(&output), &analysis);
    if (write_result != 0) {
        (void)fputs("hlolli-wg-analyzer: cannot write CSV output\n", stderr);
        hwa_file_output_abort(&output);
    } else if (hwa_file_output_finish(&output, "CSV output",
                                      error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
    } else {
        result = 0;
    }
    hwa_analysis_free(&analysis);
    return result;
}

static int hwa_alignment_input_hashes(
    HWAAlignmentMode mode,
    const char *reference_path,
    const char *target_path,
    const char *score_path,
    uint64_t max_bytes,
    char reference_sha256[HWA_SHA256_HEX_SIZE],
    char target_sha256[HWA_SHA256_HEX_SIZE],
    char score_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    reference_sha256[0] = '\0';
    target_sha256[0] = '\0';
    score_sha256[0] = '\0';
    if (mode == HWA_ALIGNMENT_AUDIO_TO_AUDIO) {
        return hwa_sha256_file(reference_path, max_bytes,
                               reference_sha256, error, error_size) != 0 ||
                       hwa_sha256_file(target_path, max_bytes,
                                       target_sha256, error, error_size) != 0
                   ? -1
                   : 0;
    }
    return hwa_sha256_file(score_path, max_bytes,
                           score_sha256, error, error_size) != 0 ||
                   hwa_sha256_file(target_path, max_bytes,
                                   target_sha256, error, error_size) != 0
               ? -1
               : 0;
}

static int hwa_run_align(HWACli *cli)
{
    HWAAlignment alignment;
    HWAAlignmentLockedSet locked;
    HWAFileOutput output;
    HWAAlignmentMode mode;
    const HWAAlignmentAnchor *locked_anchors = NULL;
    size_t locked_count = 0U;
    const char *reference_path = NULL;
    const char *target_path;
    const char *protected_paths[4];
    size_t protected_count = 0U;
    char reference_sha256[HWA_SHA256_HEX_SIZE];
    char target_sha256[HWA_SHA256_HEX_SIZE];
    char score_sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    int align_result;
    int write_result;
    int result = 1;

    memset(&alignment, 0, sizeof(alignment));
    memset(&locked, 0, sizeof(locked));
    if (cli->export_kind != 0 || cli->output_path == NULL ||
        cli->alignment_path != NULL || cli->labels_path != NULL ||
        cli->items_path != NULL || cli->room_ir_path != NULL ||
        cli->segmentation_option_set ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set ||
        cli->physical_binding_count != 0U ||
        (strcmp(cli->output_path, "-") == 0 && (cli->json || cli->replace))) {
        return -1;
    }
    if (cli->score_path == NULL) {
        if (cli->positional_count != 3U ||
            strcmp(cli->positionals[0], "align") != 0) {
            return -1;
        }
        mode = HWA_ALIGNMENT_AUDIO_TO_AUDIO;
        reference_path = cli->positionals[1];
        target_path = cli->positionals[2];
    } else {
        if (cli->positional_count != 2U ||
            strcmp(cli->positionals[0], "align") != 0) {
            return -1;
        }
        mode = HWA_ALIGNMENT_SCORE_TO_AUDIO;
        target_path = cli->positionals[1];
    }
    if ((reference_path != NULL && strcmp(reference_path, "-") == 0) ||
        strcmp(target_path, "-") == 0 ||
        (cli->score_path != NULL && strcmp(cli->score_path, "-") == 0) ||
        (cli->amend_path != NULL && strcmp(cli->amend_path, "-") == 0)) {
        (void)fputs("hlolli-wg-analyzer: align needs named regular inputs\n",
                    stderr);
        return 2;
    }
    cli->alignment_options.analysis = cli->options;
    cli->alignment_options.analysis.collect_tracks = 1;
    cli->alignment_options.analysis.collect_spectrogram = 0;
    cli->alignment_options.analysis.true_peak_oversample = 1U;

    if (cli->amend_path != NULL) {
        uint64_t amend_max_bytes =
            cli->alignment_options.max_alignment_work_bytes / 2U;

        if (amend_max_bytes > cli->options.max_input_bytes) {
            amend_max_bytes = cli->options.max_input_bytes;
        }
        if (hwa_alignment_file_read_locked(
                cli->amend_path, amend_max_bytes,
                cli->alignment_options.max_manual_anchors,
                &locked, error, sizeof(error)) != 0 ||
            hwa_alignment_input_hashes(
                mode, reference_path, target_path, cli->score_path,
                cli->options.max_input_bytes, reference_sha256,
                target_sha256, score_sha256, error, sizeof(error)) != 0 ||
            hwa_alignment_locked_set_matches(
                &locked, mode, reference_sha256, target_sha256,
                score_sha256, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                          error[0] != '\0' ? error :
                                               "cannot load amended alignment");
            hwa_alignment_locked_set_free(&locked);
            return 1;
        }
        locked_anchors = locked.anchors;
        locked_count = locked.anchor_count;
    }
    align_result = mode == HWA_ALIGNMENT_AUDIO_TO_AUDIO
                       ? hwa_align_audio_wav(
                             reference_path, target_path,
                             &cli->alignment_options, locked_anchors,
                             locked_count, &alignment, error, sizeof(error))
                       : hwa_align_score_manifest_wav(
                             cli->score_path, target_path,
                             &cli->alignment_options, locked_anchors,
                             locked_count, &alignment, error, sizeof(error));
    hwa_alignment_locked_set_free(&locked);
    if (align_result != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error : "alignment failed");
        hwa_alignment_free(&alignment);
        return 1;
    }
    if (reference_path != NULL) {
        protected_paths[protected_count++] = reference_path;
    }
    protected_paths[protected_count++] = target_path;
    if (cli->score_path != NULL) {
        protected_paths[protected_count++] = cli->score_path;
    }
    if (cli->amend_path != NULL) {
        protected_paths[protected_count++] = cli->amend_path;
    }
    if (hwa_file_output_open(&output, cli->output_path,
                             protected_paths, protected_count, cli->replace,
                             error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        hwa_alignment_free(&alignment);
        return 1;
    }
    write_result = hwa_alignment_file_write(
        hwa_file_output_stream(&output), &alignment, error, sizeof(error));
    if (write_result != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error :
                                           "cannot write alignment output");
        hwa_file_output_abort(&output);
    } else if (hwa_file_output_finish(&output, "alignment output",
                                      error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
    } else if (strcmp(cli->output_path, "-") == 0) {
        result = 0;
    } else {
        int report_result = cli->json
                                ? hwa_report_alignment_json(stdout, &alignment)
                                : hwa_report_alignment_text(stdout, &alignment);
        if (report_result == 0 &&
            hwa_finish_stream(stdout, "standard output") == 0) {
            result = 0;
        }
    }
    hwa_alignment_free(&alignment);
    return result;
}

static int hwa_run_segment(HWACli *cli)
{
    HWAItemSet items;
    HWAFileOutput output;
    const char *protected_paths[4];
    size_t protected_count = 0U;
    char error[HWA_ERROR_SIZE];
    int write_result;
    int result = 1;

    memset(&items, 0, sizeof(items));
    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "segment") != 0 ||
        cli->alignment_path == NULL || cli->output_path == NULL ||
        cli->score_path != NULL || cli->export_kind != 0 ||
        cli->alignment_option_set || cli->analysis_clock_option_set ||
        cli->analysis_only_option_set || cli->items_path != NULL ||
        cli->room_ir_path != NULL ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set ||
        cli->physical_binding_count != 0U ||
        (strcmp(cli->output_path, "-") == 0 && (cli->json || cli->replace))) {
        return -1;
    }
    if (strcmp(cli->alignment_path, "-") == 0 ||
        strcmp(cli->positionals[1], "-") == 0 ||
        (cli->labels_path != NULL && strcmp(cli->labels_path, "-") == 0) ||
        (cli->amend_path != NULL && strcmp(cli->amend_path, "-") == 0)) {
        (void)fputs("hlolli-wg-analyzer: segment needs named regular inputs\n",
                    stderr);
        return 2;
    }
    cli->segmentation_options.decode_block_frames =
        cli->options.decode_block_frames;
    cli->segmentation_options.max_input_bytes = cli->options.max_input_bytes;
    cli->segmentation_options.max_input_frames = cli->options.max_input_frames;
    cli->segmentation_options.max_analysis_work_bytes =
        cli->options.max_work_bytes;
    cli->segmentation_options.max_transforms = cli->options.max_transforms;
    cli->segmentation_options.max_track_points = cli->options.max_track_points;

    if (hwa_segment_score_alignment_wav(
            cli->alignment_path, cli->positionals[1], cli->labels_path,
            cli->amend_path,
            &cli->segmentation_options, NULL, 0U,
            &items, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error : "segmentation failed");
        hwa_item_set_free(&items);
        return 1;
    }

    protected_paths[protected_count++] = cli->alignment_path;
    protected_paths[protected_count++] = cli->positionals[1];
    if (cli->labels_path != NULL) {
        protected_paths[protected_count++] = cli->labels_path;
    }
    if (cli->amend_path != NULL) {
        protected_paths[protected_count++] = cli->amend_path;
    }
    if (hwa_file_output_open(&output, cli->output_path,
                             protected_paths, protected_count, cli->replace,
                             error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        hwa_item_set_free(&items);
        return 1;
    }
    write_result = hwa_item_file_write(
        hwa_file_output_stream(&output), &items, error, sizeof(error));
    if (write_result != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error : "cannot write item output");
        (void)hwa_file_output_abort(&output);
    } else if (strcmp(cli->output_path, "-") == 0) {
        if (hwa_file_output_finish(&output, "item output",
                                   error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        } else {
            result = 0;
        }
    } else {
        int report_result = cli->json
                                ? hwa_report_items_json(stdout, &items)
                                : hwa_report_items_text(stdout, &items);
        if (report_result != 0) {
            (void)fputs(
                "hlolli-wg-analyzer: cannot write standard output\n",
                stderr);
            (void)hwa_file_output_abort(&output);
        } else if (hwa_finish_stream(stdout, "standard output") == 0) {
            if (hwa_file_output_finish(&output, "item output",
                                       error, sizeof(error)) != 0) {
                (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
            } else {
                result = 0;
            }
        } else {
            (void)hwa_file_output_abort(&output);
        }
    }
    hwa_item_set_free(&items);
    return result;
}

static int hwa_run_measure(HWACli *cli)
{
    HWAMeasurementSet measures;
    HWAFileOutput output;
    const char *protected_paths[2];
    char error[HWA_ERROR_SIZE];
    int write_result;
    int result = 1;

    memset(&measures, 0, sizeof(measures));
    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "measure") != 0 ||
        cli->items_path == NULL || cli->output_path == NULL ||
        cli->score_path != NULL || cli->alignment_path != NULL ||
        cli->labels_path != NULL || cli->amend_path != NULL ||
        cli->room_ir_path != NULL ||
        cli->export_kind != 0 || cli->alignment_option_set ||
        cli->segmentation_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set ||
        cli->physical_binding_count != 0U ||
        cli->analysis_clock_option_set || cli->analysis_only_option_set ||
        cli->analysis_resource_option_set ||
        (strcmp(cli->output_path, "-") == 0 && (cli->json || cli->replace))) {
        return -1;
    }
    if (strcmp(cli->items_path, "-") == 0 ||
        strcmp(cli->positionals[1], "-") == 0) {
        (void)fputs(
            "hlolli-wg-analyzer: measure needs named regular inputs\n",
            stderr);
        return 2;
    }
    if (hwa_measure_item_file_wav(
            cli->items_path, cli->positionals[1],
            &cli->measurement_options, &measures,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error : "measurement failed");
        hwa_measurement_set_free(&measures);
        return 1;
    }
    protected_paths[0] = cli->items_path;
    protected_paths[1] = cli->positionals[1];
    if (hwa_file_output_open(&output, cli->output_path,
                             protected_paths, 2U, cli->replace,
                             error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        hwa_measurement_set_free(&measures);
        return 1;
    }
    write_result = hwa_measure_file_write(
        hwa_file_output_stream(&output), &measures, error, sizeof(error));
    if (write_result != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error :
                                                "cannot write measurement output");
        (void)hwa_file_output_abort(&output);
    } else if (strcmp(cli->output_path, "-") == 0) {
        if (hwa_file_output_finish(&output, "measurement output",
                                   error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        } else {
            result = 0;
        }
    } else {
        int report_result = cli->json
                                ? hwa_report_measurement_json(stdout, &measures)
                                : hwa_report_measurement_text(stdout, &measures);
        if (report_result != 0) {
            (void)fputs(
                "hlolli-wg-analyzer: cannot write standard output\n",
                stderr);
            (void)hwa_file_output_abort(&output);
        } else if (hwa_finish_stream(stdout, "standard output") == 0) {
            if (hwa_file_output_finish(&output, "measurement output",
                                       error, sizeof(error)) != 0) {
                (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
            } else {
                result = 0;
            }
        } else {
            (void)hwa_file_output_abort(&output);
        }
    }
    hwa_measurement_set_free(&measures);
    return result;
}

static int hwa_run_compare_measures(const HWACli *cli)
{
    HWAProfileComparisonSet comparison;
    HWAFileOutput output;
    const char *protected_paths[2];
    char error[HWA_ERROR_SIZE];
    int write_result;
    int result = 1;

    memset(&comparison, 0, sizeof(comparison));
    if (cli->positional_count != 3U ||
        strcmp(cli->positionals[0], "compare-measures") != 0 ||
        cli->output_path == NULL || cli->items_path != NULL ||
        cli->score_path != NULL || cli->alignment_path != NULL ||
        cli->labels_path != NULL || cli->amend_path != NULL ||
        cli->room_ir_path != NULL ||
        cli->export_kind != 0 || cli->alignment_option_set ||
        cli->segmentation_option_set || cli->measurement_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set ||
        cli->physical_binding_count != 0U ||
        cli->analysis_clock_option_set || cli->analysis_only_option_set ||
        cli->analysis_resource_option_set || cli->decode_block_option_set ||
        cli->input_frame_limit_set ||
        (strcmp(cli->output_path, "-") == 0 && (cli->json || cli->replace))) {
        return -1;
    }
    if (strcmp(cli->positionals[1], "-") == 0 ||
        strcmp(cli->positionals[2], "-") == 0) {
        (void)fputs(
            "hlolli-wg-analyzer: compare-measures needs named regular inputs\n",
            stderr);
        return 2;
    }
    if (hwa_compare_measure_files(
            cli->positionals[1], cli->positionals[2],
            &cli->comparison_options, &comparison,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error :
                                                "profile comparison failed");
        hwa_profile_comparison_set_free(&comparison);
        return 1;
    }
    protected_paths[0] = cli->positionals[1];
    protected_paths[1] = cli->positionals[2];
    if (hwa_file_output_open(&output, cli->output_path,
                             protected_paths, 2U, cli->replace,
                             error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        hwa_profile_comparison_set_free(&comparison);
        return 1;
    }
    write_result = hwa_profile_comparison_file_write(
        hwa_file_output_stream(&output), &comparison, error, sizeof(error));
    if (write_result != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error :
                                                "cannot write comparison output");
        (void)hwa_file_output_abort(&output);
    } else if (strcmp(cli->output_path, "-") == 0) {
        if (hwa_file_output_finish(&output, "comparison output",
                                   error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        } else {
            result = 0;
        }
    } else {
        int report_result = cli->json
            ? hwa_report_profile_comparison_json(stdout, &comparison)
            : hwa_report_profile_comparison_text(stdout, &comparison);
        if (report_result != 0) {
            (void)fputs(
                "hlolli-wg-analyzer: cannot write standard output\n",
                stderr);
            (void)hwa_file_output_abort(&output);
        } else if (hwa_finish_stream(stdout, "standard output") == 0) {
            if (hwa_file_output_finish(&output, "comparison output",
                                       error, sizeof(error)) != 0) {
                (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
            } else {
                result = 0;
            }
        } else {
            (void)hwa_file_output_abort(&output);
        }
    }
    hwa_profile_comparison_set_free(&comparison);
    return result;
}

static void hwa_free_physical_inputs(HWAPhysicalInput *inputs,
                                     size_t input_count)
{
    size_t index;

    if (inputs == NULL) {
        return;
    }
    for (index = 0U; index < input_count; ++index) {
        free((void *)inputs[index].role);
    }
    free(inputs);
}

static int hwa_make_physical_inputs(const HWACli *cli,
                                    HWAPhysicalInput **out_inputs)
{
    HWAPhysicalInput *inputs;
    size_t index;

    *out_inputs = NULL;
    if (cli->physical_binding_count == 0U) {
        return 0;
    }
    inputs = (HWAPhysicalInput *)calloc(cli->physical_binding_count,
                                        sizeof(*inputs));
    if (inputs == NULL) {
        (void)fputs("hlolli-wg-analyzer: cannot allocate bindings\n", stderr);
        return -1;
    }
    for (index = 0U; index < cli->physical_binding_count; ++index) {
        const char *text = cli->physical_bindings[index];
        const char *separator = strchr(text, '=');
        size_t role_size;
        char *role;

        if (separator == NULL || separator == text || separator[1] == '\0') {
            (void)fputs("hlolli-wg-analyzer: invalid --bind value\n", stderr);
            hwa_free_physical_inputs(inputs, cli->physical_binding_count);
            return -1;
        }
        role_size = (size_t)(separator - text);
        if (role_size == SIZE_MAX) {
            hwa_free_physical_inputs(inputs, cli->physical_binding_count);
            return -1;
        }
        role = (char *)malloc(role_size + 1U);
        if (role == NULL) {
            (void)fputs("hlolli-wg-analyzer: cannot allocate binding role\n",
                        stderr);
            hwa_free_physical_inputs(inputs, cli->physical_binding_count);
            return -1;
        }
        memcpy(role, text, role_size);
        role[role_size] = '\0';
        inputs[index].role = role;
        inputs[index].path = separator + 1;
    }
    *out_inputs = inputs;
    return 0;
}

static int hwa_run_check_physical(HWACli *cli)
{
    HWAPhysicalCheckSet checks;
    HWAPhysicalInput *inputs = NULL;
    HWAFileOutput output;
    const char **protected_paths = NULL;
    size_t protected_count;
    size_t index;
    char error[HWA_ERROR_SIZE];
    int write_result;
    int result = 1;

    memset(&checks, 0, sizeof(checks));
    if (cli->positional_count != 3U ||
        strcmp(cli->positionals[0], "check-physical") != 0 ||
        cli->output_path == NULL || cli->items_path != NULL ||
        cli->score_path != NULL || cli->alignment_path != NULL ||
        cli->labels_path != NULL || cli->amend_path != NULL ||
        cli->room_ir_path != NULL || cli->production_option_set ||
        cli->run_option_set ||
        cli->export_kind != 0 || cli->alignment_option_set ||
        cli->segmentation_option_set || cli->measurement_option_set ||
        cli->analysis_clock_option_set || cli->analysis_only_option_set ||
        cli->analysis_resource_option_set ||
        (strcmp(cli->output_path, "-") == 0 && (cli->json || cli->replace))) {
        return -1;
    }
    if (strcmp(cli->positionals[1], "-") == 0 ||
        strcmp(cli->positionals[2], "-") == 0) {
        (void)fputs(
            "hlolli-wg-analyzer: check-physical needs named regular inputs\n",
            stderr);
        return 2;
    }
    if (hwa_make_physical_inputs(cli, &inputs) != 0) {
        return 1;
    }
    for (index = 0U; index < cli->physical_binding_count; ++index) {
        if (strcmp(inputs[index].path, "-") == 0) {
            (void)fputs(
                "hlolli-wg-analyzer: check-physical bindings must be named\n",
                stderr);
            hwa_free_physical_inputs(inputs, cli->physical_binding_count);
            return 2;
        }
    }
    cli->physical_options.profile_limits = cli->comparison_options;
    if (hwa_check_physical_files(
            cli->positionals[1], cli->positionals[2], inputs,
            cli->physical_binding_count, &cli->physical_options, &checks,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error : "physical check failed");
        hwa_physical_check_set_free(&checks);
        hwa_free_physical_inputs(inputs, cli->physical_binding_count);
        return 1;
    }
    protected_count = 2U + cli->physical_binding_count;
    protected_paths = (const char **)calloc(protected_count,
                                             sizeof(*protected_paths));
    if (protected_paths == NULL) {
        (void)fputs("hlolli-wg-analyzer: cannot allocate protected paths\n",
                    stderr);
        hwa_physical_check_set_free(&checks);
        hwa_free_physical_inputs(inputs, cli->physical_binding_count);
        return 1;
    }
    protected_paths[0] = cli->positionals[1];
    protected_paths[1] = cli->positionals[2];
    for (index = 0U; index < cli->physical_binding_count; ++index) {
        protected_paths[2U + index] = inputs[index].path;
    }
    if (hwa_file_output_open(&output, cli->output_path, protected_paths,
                             protected_count, cli->replace,
                             error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        free(protected_paths);
        hwa_physical_check_set_free(&checks);
        hwa_free_physical_inputs(inputs, cli->physical_binding_count);
        return 1;
    }
    write_result = hwa_physical_file_write(
        hwa_file_output_stream(&output), &checks, error, sizeof(error));
    if (write_result != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error :
                                               "cannot write physical output");
        (void)hwa_file_output_abort(&output);
    } else if (strcmp(cli->output_path, "-") == 0) {
        if (hwa_file_output_finish(&output, "physical output",
                                   error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        } else {
            result = 0;
        }
    } else {
        int report_result = cli->json
                                ? hwa_report_physical_json(stdout, &checks)
                                : hwa_report_physical_text(stdout, &checks);
        if (report_result != 0) {
            (void)fputs(
                "hlolli-wg-analyzer: cannot write standard output\n", stderr);
            (void)hwa_file_output_abort(&output);
        } else if (hwa_finish_stream(stdout, "standard output") == 0) {
            if (hwa_file_output_finish(&output, "physical output",
                                       error, sizeof(error)) != 0) {
                (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
            } else {
                result = 0;
            }
        } else {
            (void)hwa_file_output_abort(&output);
        }
    }
    free(protected_paths);
    hwa_physical_check_set_free(&checks);
    hwa_free_physical_inputs(inputs, cli->physical_binding_count);
    return result;
}

static int hwa_run_account_production(HWACli *cli)
{
    HWAProductionInputs inputs;
    HWAProductionResult production;
    HWAFileOutput output;
    const char *protected_paths[5];
    size_t protected_count = 4U;
    char error[HWA_ERROR_SIZE];
    int write_result;
    int result = 1;

    memset(&inputs, 0, sizeof(inputs));
    memset(&production, 0, sizeof(production));
    if (cli->positional_count != 5U ||
        strcmp(cli->positionals[0], "account-production") != 0 ||
        cli->output_path == NULL || cli->items_path != NULL ||
        cli->score_path != NULL || cli->alignment_path != NULL ||
        cli->labels_path != NULL || cli->amend_path != NULL ||
        cli->export_kind != 0 || cli->alignment_option_set ||
        cli->segmentation_option_set || cli->measurement_option_set ||
        cli->physical_option_set || cli->physical_binding_count != 0U ||
        cli->run_option_set ||
        cli->analysis_clock_option_set || cli->analysis_only_option_set ||
        cli->analysis_resource_option_set ||
        (strcmp(cli->output_path, "-") == 0 && (cli->json || cli->replace))) {
        return -1;
    }
    if (strcmp(cli->positionals[1], "-") == 0 ||
        strcmp(cli->positionals[2], "-") == 0 ||
        strcmp(cli->positionals[3], "-") == 0 ||
        strcmp(cli->positionals[4], "-") == 0 ||
        (cli->room_ir_path != NULL && strcmp(cli->room_ir_path, "-") == 0)) {
        (void)fputs(
            "hlolli-wg-analyzer: account-production needs named regular inputs\n",
            stderr);
        return 2;
    }

    inputs.reference_measures_path = cli->positionals[1];
    inputs.reference_audio_path = cli->positionals[2];
    inputs.model_measures_path = cli->positionals[3];
    inputs.model_audio_path = cli->positionals[4];
    inputs.room_ir_path = cli->room_ir_path;
    cli->production_options.profile_limits = cli->comparison_options;
    if (hwa_account_production_files(
            &inputs, &cli->production_options, &production,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error :
                                               "production correction failed");
        hwa_production_result_free(&production);
        return 1;
    }

    protected_paths[0] = inputs.reference_measures_path;
    protected_paths[1] = inputs.reference_audio_path;
    protected_paths[2] = inputs.model_measures_path;
    protected_paths[3] = inputs.model_audio_path;
    if (inputs.room_ir_path != NULL) {
        protected_paths[protected_count++] = inputs.room_ir_path;
    }
    if (hwa_file_output_open(&output, cli->output_path, protected_paths,
                             protected_count, cli->replace,
                             error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        hwa_production_result_free(&production);
        return 1;
    }
    write_result = hwa_production_file_write(
        hwa_file_output_stream(&output), &production, error, sizeof(error));
    if (write_result != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error :
                                           "cannot write production output");
        (void)hwa_file_output_abort(&output);
    } else if (strcmp(cli->output_path, "-") == 0) {
        if (hwa_file_output_finish(&output, "production output",
                                   error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        } else {
            result = 0;
        }
    } else {
        int report_result = cli->json
                                ? hwa_report_production_json(stdout, &production)
                                : hwa_report_production_text(stdout, &production);
        if (report_result != 0) {
            (void)fputs(
                "hlolli-wg-analyzer: cannot write standard output\n", stderr);
            (void)hwa_file_output_abort(&output);
        } else if (hwa_finish_stream(stdout, "standard output") == 0) {
            if (hwa_file_output_finish(&output, "production output",
                                       error, sizeof(error)) != 0) {
                (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
            } else {
                result = 0;
            }
        } else {
            (void)hwa_file_output_abort(&output);
        }
    }
    hwa_production_result_free(&production);
    return result;
}

static void hwa_free_run_bindings(HWARunBinding *bindings,
                                  size_t binding_count)
{
    size_t index;

    if (bindings == NULL) {
        return;
    }
    for (index = 0U; index < binding_count; ++index) {
        free((void *)bindings[index].id);
    }
    free(bindings);
}

static int hwa_make_run_bindings(const HWACli *cli,
                                 HWARunBinding **out_bindings)
{
    HWARunBinding *bindings;
    size_t index;

    *out_bindings = NULL;
    if (cli->physical_binding_count == 0U) {
        return 0;
    }
    bindings = (HWARunBinding *)calloc(cli->physical_binding_count,
                                       sizeof(*bindings));
    if (bindings == NULL) {
        (void)fputs("hlolli-wg-analyzer: cannot allocate bindings\n", stderr);
        return -1;
    }
    for (index = 0U; index < cli->physical_binding_count; ++index) {
        const char *text = cli->physical_bindings[index];
        const char *separator = strchr(text, '=');
        size_t id_size;
        char *id;

        if (separator == NULL || separator == text || separator[1] == '\0') {
            (void)fputs("hlolli-wg-analyzer: invalid --bind value\n", stderr);
            hwa_free_run_bindings(bindings, cli->physical_binding_count);
            return -1;
        }
        id_size = (size_t)(separator - text);
        if (id_size == SIZE_MAX) {
            hwa_free_run_bindings(bindings, cli->physical_binding_count);
            return -1;
        }
        id = (char *)malloc(id_size + 1U);
        if (id == NULL) {
            (void)fputs(
                "hlolli-wg-analyzer: cannot allocate binding ID\n", stderr);
            hwa_free_run_bindings(bindings, cli->physical_binding_count);
            return -1;
        }
        memcpy(id, text, id_size);
        id[id_size] = '\0';
        bindings[index].id = id;
        bindings[index].path = separator + 1;
    }
    *out_bindings = bindings;
    return 0;
}

static int hwa_run_analyze_run(HWACli *cli)
{
    HWARunBinding *bindings = NULL;
    HWARunResult run;
    HWAFileOutput output;
    const char **protected_paths = NULL;
    size_t protected_count;
    size_t index;
    char error[HWA_ERROR_SIZE];
    int write_result;
    int result = 1;

    memset(&run, 0, sizeof(run));
    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "analyze-run") != 0 ||
        cli->output_path == NULL || cli->score_path != NULL ||
        cli->alignment_path != NULL || cli->labels_path != NULL ||
        cli->amend_path != NULL || cli->items_path != NULL ||
        cli->room_ir_path != NULL || cli->export_kind != 0 ||
        cli->alignment_option_set || cli->segmentation_option_set ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->analysis_clock_option_set || cli->analysis_only_option_set ||
        cli->analysis_resource_option_set ||
        (strcmp(cli->output_path, "-") == 0 && (cli->json || cli->replace))) {
        return -1;
    }
    if (strcmp(cli->positionals[1], "-") == 0) {
        (void)fputs(
            "hlolli-wg-analyzer: analyze-run needs named regular inputs\n",
            stderr);
        return 2;
    }
    if (hwa_make_run_bindings(cli, &bindings) != 0) {
        return 1;
    }
    for (index = 0U; index < cli->physical_binding_count; ++index) {
        if (strcmp(bindings[index].path, "-") == 0) {
            (void)fputs(
                "hlolli-wg-analyzer: analyze-run bindings must be named\n",
                stderr);
            hwa_free_run_bindings(bindings, cli->physical_binding_count);
            return 2;
        }
    }
    if (hwa_analyze_run_files(
            cli->positionals[1], bindings, cli->physical_binding_count,
            &cli->run_options, &run, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error : "run analysis failed");
        hwa_run_result_free(&run);
        hwa_free_run_bindings(bindings, cli->physical_binding_count);
        return 1;
    }

    protected_count = cli->physical_binding_count + 1U;
    protected_paths = (const char **)calloc(protected_count,
                                             sizeof(*protected_paths));
    if (protected_paths == NULL) {
        (void)fputs("hlolli-wg-analyzer: cannot allocate protected paths\n",
                    stderr);
        hwa_run_result_free(&run);
        hwa_free_run_bindings(bindings, cli->physical_binding_count);
        return 1;
    }
    protected_paths[0] = cli->positionals[1];
    for (index = 0U; index < cli->physical_binding_count; ++index) {
        protected_paths[index + 1U] = bindings[index].path;
    }
    if (hwa_file_output_open(&output, cli->output_path, protected_paths,
                             protected_count, cli->replace,
                             error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        free(protected_paths);
        hwa_run_result_free(&run);
        hwa_free_run_bindings(bindings, cli->physical_binding_count);
        return 1;
    }
    write_result = hwa_run_file_write(
        hwa_file_output_stream(&output), &run, error, sizeof(error));
    if (write_result != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error : "cannot write run output");
        (void)hwa_file_output_abort(&output);
    } else if (strcmp(cli->output_path, "-") == 0) {
        if (hwa_file_output_finish(&output, "run output",
                                   error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        } else {
            result = 0;
        }
    } else {
        int report_result = cli->json
                                ? hwa_report_run_json(stdout, &run)
                                : hwa_report_run_text(stdout, &run);
        if (report_result != 0) {
            (void)fputs(
                "hlolli-wg-analyzer: cannot write standard output\n", stderr);
            (void)hwa_file_output_abort(&output);
        } else if (hwa_finish_stream(stdout, "standard output") == 0) {
            if (hwa_file_output_finish(&output, "run output",
                                       error, sizeof(error)) != 0) {
                (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
            } else {
                result = 0;
            }
        } else {
            (void)hwa_file_output_abort(&output);
        }
    }
    free(protected_paths);
    hwa_run_result_free(&run);
    hwa_free_run_bindings(bindings, cli->physical_binding_count);
    return result;
}

static int hwa_run_experiment(HWACli *cli)
{
    HWARunBinding *bindings = NULL;
    HWAExperimentProcessRenderer process_renderer;
    HWAExperimentRenderer renderer;
    HWAExperimentResult experiment;
    size_t index;
    char error[HWA_ERROR_SIZE] = {0};
    int report_result;
    int result = 1;

    memset(&process_renderer, 0, sizeof(process_renderer));
    memset(&renderer, 0, sizeof(renderer));
    memset(&experiment, 0, sizeof(experiment));
    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "experiment") != 0 ||
        cli->output_path == NULL || cli->renderer_path == NULL ||
        !cli->allow_run || cli->replace ||
        strcmp(cli->output_path, "-") == 0 ||
        cli->score_path != NULL || cli->alignment_path != NULL ||
        cli->labels_path != NULL || cli->amend_path != NULL ||
        cli->items_path != NULL || cli->room_ir_path != NULL ||
        cli->export_kind != 0 || cli->alignment_option_set ||
        cli->segmentation_option_set || cli->measurement_option_set ||
        cli->physical_option_set ||
        cli->production_option_set || cli->analysis_clock_option_set ||
        cli->analysis_only_option_set || cli->analysis_resource_option_set) {
        return -1;
    }
    if (strcmp(cli->positionals[1], "-") == 0 ||
        strcmp(cli->renderer_path, "-") == 0 ||
        (cli->resume_path != NULL && strcmp(cli->resume_path, "-") == 0)) {
        (void)fputs(
            "hlolli-wg-analyzer: experiment needs named paths\n", stderr);
        return 2;
    }
    if (hwa_make_run_bindings(cli, &bindings) != 0) return 1;
    for (index = 0U; index < cli->physical_binding_count; ++index) {
        if (strcmp(bindings[index].path, "-") == 0) {
            (void)fputs(
                "hlolli-wg-analyzer: experiment bindings must be named\n",
                stderr);
            hwa_free_run_bindings(bindings, cli->physical_binding_count);
            return 2;
        }
    }
    if (hwa_experiment_process_renderer_open(
            &process_renderer, cli->renderer_path,
            cli->experiment_options.max_input_bytes,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        hwa_free_run_bindings(bindings, cli->physical_binding_count);
        return 1;
    }
    renderer.id = "process";
    renderer.sha256 = process_renderer.sha256;
    renderer.context = &process_renderer;
    renderer.render = hwa_experiment_process_renderer_render;
    cli->experiment_options.run = cli->run_options;
    if (hwa_execute_experiment_files(
            cli->positionals[1], bindings, cli->physical_binding_count,
            cli->output_path, cli->resume_path, &renderer,
            &cli->experiment_options, &experiment,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error : "experiment failed");
        hwa_experiment_result_free(&experiment);
        hwa_experiment_process_renderer_close(&process_renderer);
        hwa_free_run_bindings(bindings, cli->physical_binding_count);
        return 1;
    }
    report_result = cli->json
                        ? hwa_report_experiment_json(stdout, &experiment)
                        : hwa_report_experiment_text(stdout, &experiment);
    if (report_result == 0 &&
        hwa_finish_stream(stdout, "standard output") == 0) {
        result = 0;
    } else {
        char remove_error[HWA_ERROR_SIZE] = {0};

        (void)fputs(
            "hlolli-wg-analyzer: cannot write standard output\n", stderr);
        if (hwa_experiment_output_remove(
                &experiment, remove_error, sizeof(remove_error)) != 0) {
            (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", remove_error);
        }
    }
    hwa_experiment_result_free(&experiment);
    hwa_experiment_process_renderer_close(&process_renderer);
    hwa_free_run_bindings(bindings, cli->physical_binding_count);
    return result;
}

static int hwa_run_gap_report(HWACli *cli)
{
    HWARunBinding *bindings = NULL;
    HWAGapReportResult report;
    HWAGapReportMode mode;
    const char *command;
    size_t index;
    char error[HWA_ERROR_SIZE] = {0};
    int report_result;
    int result = 1;

    memset(&report, 0, sizeof(report));
    if (cli->positional_count != 2U) return -1;
    command = cli->positionals[0];
    if (strcmp(command, "rank") == 0) {
        mode = HWA_GAP_REPORT_RANK;
        if (cli->output_path != NULL) return -1;
    } else if (strcmp(command, "excerpt") == 0) {
        mode = HWA_GAP_REPORT_EXCERPTS;
        if (cli->output_path == NULL || strcmp(cli->output_path, "-") == 0)
            return -1;
    } else if (strcmp(command, "report") == 0) {
        mode = HWA_GAP_REPORT_FULL;
        if (cli->output_path == NULL || strcmp(cli->output_path, "-") == 0)
            return -1;
    } else {
        return -1;
    }
    if (cli->replace || cli->score_path != NULL ||
        cli->alignment_path != NULL || cli->labels_path != NULL ||
        cli->amend_path != NULL || cli->items_path != NULL ||
        cli->room_ir_path != NULL || cli->renderer_path != NULL ||
        cli->resume_path != NULL || cli->allow_run ||
        cli->export_kind != 0 || cli->alignment_option_set ||
        cli->segmentation_option_set || cli->measurement_option_set ||
        cli->physical_option_set ||
        cli->analysis_clock_option_set ||
        cli->analysis_only_option_set || cli->analysis_resource_option_set) {
        return -1;
    }
    if (strcmp(cli->positionals[1], "-") == 0) {
        (void)fputs(
            "hlolli-wg-analyzer: Stage 9 needs a named manifest\n", stderr);
        return 2;
    }
    if (hwa_make_run_bindings(cli, &bindings) != 0) return 1;
    for (index = 0U; index < cli->physical_binding_count; ++index) {
        if (strcmp(bindings[index].path, "-") == 0) {
            (void)fputs(
                "hlolli-wg-analyzer: Stage 9 bindings must be named\n",
                stderr);
            hwa_free_run_bindings(bindings, cli->physical_binding_count);
            return 2;
        }
    }
    cli->production_options.profile_limits = cli->comparison_options;
    cli->experiment_options.run = cli->run_options;
    cli->gap_report_options.measurement = cli->comparison_options;
    cli->gap_report_options.production = cli->production_options;
    cli->gap_report_options.run = cli->run_options;
    cli->gap_report_options.experiment = cli->experiment_options;
    if (hwa_build_gap_report_files(
            cli->positionals[1], bindings, cli->physical_binding_count,
            cli->output_path, mode, &cli->gap_report_options, &report,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error : "gap report failed");
        hwa_gap_report_result_free(&report);
        hwa_free_run_bindings(bindings, cli->physical_binding_count);
        return 1;
    }
    report_result = cli->json ? hwa_gap_report_json(stdout, &report)
                              : hwa_gap_report_text(stdout, &report);
    if (report_result == 0 &&
        hwa_finish_stream(stdout, "standard output") == 0) {
        result = 0;
    } else {
        (void)fputs(
            "hlolli-wg-analyzer: cannot write standard output\n", stderr);
        if (mode != HWA_GAP_REPORT_RANK) {
            char remove_error[HWA_ERROR_SIZE] = {0};
            if (hwa_gap_report_output_remove(
                    &report, remove_error, sizeof(remove_error)) != 0) {
                (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                              remove_error);
            }
        }
    }
    hwa_gap_report_result_free(&report);
    hwa_free_run_bindings(bindings, cli->physical_binding_count);
    return result;
}

static int hwa_event_bundle_child_counts(const HWAEventBundle *bundle,
                                         size_t *value_count,
                                         size_t *trace_ref_count)
{
    size_t values = 0U;
    size_t trace_refs = 0U;
    size_t index;
    if (bundle == NULL || value_count == NULL || trace_ref_count == NULL)
        return -1;
    for (index = 0U; index < bundle->event_count; ++index) {
        if (SIZE_MAX - values < bundle->events[index].value_count ||
            SIZE_MAX - trace_refs < bundle->events[index].trace_ref_count)
            return -1;
        values += bundle->events[index].value_count;
        trace_refs += bundle->events[index].trace_ref_count;
    }
    *value_count = values;
    *trace_ref_count = trace_refs;
    return 0;
}

static int hwa_report_event_bundle_json(FILE *stream,
                                        const HWAEventBundle *bundle)
{
    size_t value_count;
    size_t trace_ref_count;
    if (stream == NULL || bundle == NULL ||
        hwa_event_bundle_child_counts(bundle, &value_count,
                                      &trace_ref_count) != 0 ||
        fputs("{\"schema\":\"hwa-event-bundle-validation\","
              "\"schema_version\":1,"
              "\"command\":\"validate-event-bundle\","
              "\"valid\":true,\"valid_means\":\"schema conformance\","
              "\"bundle_schema\":\"" HWA_EVENT_BUNDLE_SCHEMA "\","
              "\"bundle_schema_version\":", stream) == EOF ||
        fprintf(stream, "%u,\"directory\":",
                HWA_EVENT_BUNDLE_SCHEMA_VERSION) < 0 ||
        hwa_json_write_string(stream,
                              bundle->directory != NULL
                                  ? bundle->directory : "") != 0 ||
        fputs(",\"manifest_sha256\":", stream) == EOF ||
        hwa_json_write_string(stream, bundle->manifest_sha256) != 0 ||
        fprintf(stream,
                ",\"counts\":{\"providers\":%zu,\"audio\":%zu,"
                "\"events\":%zu,\"values\":%zu,\"traces\":%zu,"
                "\"trace_refs\":%zu,\"warnings\":%zu},"
                "\"retained_work_bytes\":%" PRIu64 ","
                "\"total_file_bytes\":%" PRIu64 "}\n",
                bundle->provider_count, bundle->audio_count,
                bundle->event_count, value_count, bundle->trace_count,
                trace_ref_count, bundle->warning_count,
                bundle->retained_work_bytes,
                bundle->total_file_bytes) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_report_event_bundle_text(FILE *stream,
                                        const HWAEventBundle *bundle)
{
    size_t value_count;
    size_t trace_ref_count;
    if (stream == NULL || bundle == NULL ||
        hwa_event_bundle_child_counts(bundle, &value_count,
                                      &trace_ref_count) != 0 ||
        fprintf(stream,
                "Event bundle validation passed\n"
                "Schema: %s %u\n"
                "Directory: %s\n"
                "Manifest SHA-256: %s\n"
                "Counts: %zu provider%s, %zu audio, %zu event%s, "
                "%zu value%s, %zu trace%s, %zu trace reference%s, "
                "%zu warning%s\n"
                "Retained work bytes: %" PRIu64 "\n"
                "Total file bytes: %" PRIu64 "\n"
                "Valid means schema conformance.\n",
                HWA_EVENT_BUNDLE_SCHEMA, HWA_EVENT_BUNDLE_SCHEMA_VERSION,
                bundle->directory != NULL ? bundle->directory : "",
                bundle->manifest_sha256,
                bundle->provider_count,
                bundle->provider_count == 1U ? "" : "s",
                bundle->audio_count,
                bundle->event_count,
                bundle->event_count == 1U ? "" : "s",
                value_count, value_count == 1U ? "" : "s",
                bundle->trace_count,
                bundle->trace_count == 1U ? "" : "s",
                trace_ref_count, trace_ref_count == 1U ? "" : "s",
                bundle->warning_count,
                bundle->warning_count == 1U ? "" : "s",
                bundle->retained_work_bytes,
                bundle->total_file_bytes) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_run_validate_event_bundle(const HWACli *cli)
{
    HWAEventBundleLimits limits;
    HWAEventBundle bundle;
    char error[HWA_ERROR_SIZE] = {0};
    int result = 1;
    memset(&bundle, 0, sizeof(bundle));
    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "validate-event-bundle") != 0 ||
        cli->output_path != NULL || cli->replace ||
        cli->export_kind != 0 || cli->score_path != NULL ||
        cli->alignment_path != NULL || cli->labels_path != NULL ||
        cli->amend_path != NULL || cli->items_path != NULL ||
        cli->room_ir_path != NULL || cli->renderer_path != NULL ||
        cli->resume_path != NULL || cli->allow_run ||
        cli->physical_binding_count != 0U ||
        cli->analysis_clock_option_set || cli->analysis_only_option_set ||
        cli->analysis_resource_option_set ||
        cli->analysis_spectral_resource_option_set ||
        cli->frame_size_option_set || cli->hop_size_option_set ||
        cli->silence_option_set || cli->decode_block_option_set ||
        cli->max_bytes_option_set || cli->input_frame_limit_set ||
        cli->alignment_option_set || cli->segmentation_option_set ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set || cli->experiment_option_set ||
        cli->gap_report_option_set || cli->isolated_note_option_set ||
        cli->isolated_note_expected_set || cli->isolated_note_metrics_set ||
        cli->harmonic_decay_expected_set) {
        return -1;
    }
    if (strcmp(cli->positionals[1], "-") == 0) {
        (void)fputs(
            "hlolli-wg-analyzer: validate-event-bundle needs a named directory\n",
            stderr);
        return 2;
    }
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_read(cli->positionals[1], &limits, &bundle,
                              error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "event bundle validation failed");
        hwa_event_bundle_free(&bundle);
        return 1;
    }
    if ((cli->json
             ? hwa_report_event_bundle_json(stdout, &bundle)
             : hwa_report_event_bundle_text(stdout, &bundle)) == 0 &&
        hwa_finish_stream(stdout, "standard output") == 0) {
        result = 0;
    }
    hwa_event_bundle_free(&bundle);
    return result;
}

static int hwa_run_analyze_events(const HWACli *cli)
{
    HWAEventAnalysisOptions options;
    char error[HWA_ERROR_SIZE] = {0};
    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "analyze-events") != 0 ||
        cli->output_path == NULL || cli->output_path[0] == '\0' ||
        strcmp(cli->positionals[1], "-") == 0 ||
        strcmp(cli->output_path, "-") == 0 || cli->json || cli->replace ||
        cli->export_kind != 0 || cli->score_path != NULL ||
        cli->alignment_path != NULL || cli->labels_path != NULL ||
        cli->amend_path != NULL || cli->items_path != NULL ||
        cli->room_ir_path != NULL || cli->renderer_path != NULL ||
        cli->resume_path != NULL || cli->allow_run ||
        cli->physical_binding_count != 0U || cli->analysis_only_option_set ||
        cli->alignment_option_set || cli->segmentation_option_set ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set || cli->experiment_option_set ||
        cli->gap_report_option_set || cli->isolated_note_option_set ||
        cli->harmonic_decay_expected_set) {
        return -1;
    }
    hwa_event_analysis_options_default(&options);
    options.analysis = cli->options;
    options.analysis.collect_tracks = 1;
    options.analysis.collect_spectrogram = 0;
    if (hwa_event_analysis_options_validate(
            &options, error, sizeof(error)) != 0)
        return -1;
    if (hwa_analyze_events_wav(
            cli->positionals[1], cli->output_path, &options,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "event analysis failed");
        return 1;
    }
    return 0;
}

static int hwa_run_infer_note_events(const HWACli *cli)
{
    HWAWavReader reader;
    HWAByteSource source;
    HWAInferenceInput input;
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    HWABasicPitchRunner runner;
    HWAInferencePollState state = HWA_INFERENCE_PENDING;
    const HWAInferenceOutput *output = NULL;
    void *task = NULL;
    char *settings_json = NULL;
    char source_sha256[HWA_SHA256_HEX_SIZE];
    char model_sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    int reader_open = 0;
    int provider_open = 0;
    int result = 1;
    memset(&reader, 0, sizeof(reader));
    memset(&source, 0, sizeof(source));
    memset(&input, 0, sizeof(input));
    memset(&request, 0, sizeof(request));
    memset(&provider, 0, sizeof(provider));
    memset(&runner, 0, sizeof(runner));
    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "infer-note-events") != 0 ||
        cli->model_path == NULL || cli->model_path[0] == '\0' ||
        cli->output_path == NULL || cli->output_path[0] == '\0' ||
        strcmp(cli->positionals[1], "-") == 0 ||
        strcmp(cli->model_path, "-") == 0 ||
        strcmp(cli->output_path, "-") == 0 || cli->json || cli->replace ||
        cli->export_kind != 0 || cli->score_path != NULL ||
        cli->alignment_path != NULL || cli->labels_path != NULL ||
        cli->amend_path != NULL || cli->items_path != NULL ||
        cli->room_ir_path != NULL || cli->renderer_path != NULL ||
        cli->resume_path != NULL || cli->allow_run ||
        cli->physical_binding_count != 0U ||
        cli->analysis_clock_option_set || cli->analysis_only_option_set ||
        cli->analysis_spectral_resource_option_set ||
        cli->alignment_option_set || cli->segmentation_option_set ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set || cli->experiment_option_set ||
        cli->gap_report_option_set || cli->isolated_note_option_set ||
        cli->harmonic_decay_expected_set || cli->decode_block_option_set)
        return -1;
    if (hwa_basic_pitch_decoder_options_validate(
            &cli->basic_pitch_options, error, sizeof(error)) != 0)
        return -1;
    if (!hwa_inference_basic_pitch_onnx_available()) {
        (void)fputs(
            "hlolli-wg-analyzer: ONNX Runtime support is not compiled\n",
            stderr);
        return 1;
    }
    if (hwa_wav_reader_open(
            &reader, cli->positionals[1], cli->options.max_input_bytes,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        goto cleanup;
    }
    reader_open = 1;
    if (reader.format.sample_rate_hz != HWA_BASIC_PITCH_SAMPLE_RATE ||
        reader.format.channels != 1U) {
        (void)fputs(
            "hlolli-wg-analyzer: Basic Pitch v1 needs mono 22050 Hz WAVE input\n",
            stderr);
        goto cleanup;
    }
    if (reader.format.frames > cli->options.max_input_frames) {
        (void)fputs(
            "hlolli-wg-analyzer: Basic Pitch input exceeds the frame limit\n",
            stderr);
        goto cleanup;
    }
    source = reader.source;
    source.name = cli->positionals[1];
    if (hwa_inference_byte_source_sha256(
            &source, cli->options.max_input_bytes, source_sha256,
            error, sizeof(error)) != 0 ||
        hwa_basic_pitch_task_settings_build(
            &cli->basic_pitch_options, &settings_json,
            error, sizeof(error)) != 0 ||
        hwa_inference_basic_pitch_onnx_runner_open(
            cli->model_path, cli->expected_model_sha256,
            cli->max_model_bytes, &runner, model_sha256,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error : "cannot open inference inputs");
        goto cleanup;
    }
    if (hwa_basic_pitch_provider_init(
            &provider, model_sha256, HWA_BASIC_PITCH_ADAPTER_SHA256,
            &cli->basic_pitch_options, cli->options.max_work_bytes,
            &runner, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "cannot open Basic Pitch provider");
        goto cleanup;
    }
    provider_open = 1;
    memset(&runner, 0, sizeof(runner));
    input.id = "source";
    input.role = "source-recording";
    input.media_type = "audio/wav";
    input.sha256 = source_sha256;
    input.bytes = source;
    request.task = HWA_BASIC_PITCH_TASK_NAME;
    request.settings_json = settings_json;
    request.expected_provider_name = provider.name;
    request.expected_provider_version = provider.version;
    request.expected_model_sha256 = provider.model_sha256;
    request.seed = UINT64_C(0);
    request.source_recording_id = UINT64_C(1);
    request.source_input_id = input.id;
    request.inputs = &input;
    request.input_count = 1U;
    request.source_format = reader.format;
    request.max_input_file_bytes = cli->options.max_input_bytes;
    request.max_input_bytes = cli->options.max_input_bytes;
    request.timeout_milliseconds = cli->inference_timeout_milliseconds;
    hwa_event_bundle_limits_default(&request.output_limits);
    request.output_limits.max_events = cli->max_note_events;
    request.output_limits.max_values = cli->max_note_events;
    request.output_limits.max_work_bytes = cli->options.max_work_bytes;
    if (provider.start(
            provider.context, &request, &task,
            error, sizeof(error)) != 0 || task == NULL ||
        provider.poll(
            provider.context, task, &state, &output,
            error, sizeof(error)) != 0 ||
        state != HWA_INFERENCE_READY || output == NULL ||
        hwa_inference_output_validate_for_request(
            &provider, &request, output, error, sizeof(error)) != 0 ||
        hwa_inference_output_write(
            cli->output_path, output, &request.output_limits,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "Basic Pitch inference failed");
        goto cleanup;
    }
    result = 0;
cleanup:
    if (task != NULL && provider.task_free != NULL)
        provider.task_free(provider.context, task);
    if (provider_open) hwa_inference_provider_destroy(&provider);
    if (runner.context != NULL && runner.destroy != NULL)
        runner.destroy(runner.context);
    if (reader_open) hwa_wav_reader_close(&reader);
    free(settings_json);
    return result;
}

static int hwa_run_separate_instruments(const HWACli *cli)
{
    HWAWavReader reader;
    HWAByteSource source;
    HWAInferenceInput input;
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    HWAHTDemucsModelRunner model_runner;
    HWAInstrumentStemRunner stem_runner;
    HWAInferencePollState state = HWA_INFERENCE_PENDING;
    const HWAInferenceOutput *output = NULL;
    void *task = NULL;
    char source_sha256[HWA_SHA256_HEX_SIZE];
    char model_sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    int reader_open = 0;
    int provider_open = 0;
    int result = 1;
    memset(&reader, 0, sizeof(reader));
    memset(&source, 0, sizeof(source));
    memset(&input, 0, sizeof(input));
    memset(&request, 0, sizeof(request));
    memset(&provider, 0, sizeof(provider));
    memset(&model_runner, 0, sizeof(model_runner));
    memset(&stem_runner, 0, sizeof(stem_runner));
    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "separate-instruments") != 0 ||
        cli->model_path == NULL || cli->model_path[0] == '\0' ||
        cli->output_path == NULL || cli->output_path[0] == '\0' ||
        strcmp(cli->positionals[1], "-") == 0 ||
        strcmp(cli->model_path, "-") == 0 ||
        strcmp(cli->output_path, "-") == 0 || cli->json || cli->replace ||
        cli->export_kind != 0 || cli->score_path != NULL ||
        cli->alignment_path != NULL || cli->labels_path != NULL ||
        cli->amend_path != NULL || cli->items_path != NULL ||
        cli->room_ir_path != NULL || cli->renderer_path != NULL ||
        cli->resume_path != NULL || cli->allow_run ||
        cli->physical_binding_count != 0U ||
        cli->analysis_clock_option_set || cli->analysis_only_option_set ||
        cli->analysis_spectral_resource_option_set ||
        cli->alignment_option_set || cli->segmentation_option_set ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set || cli->experiment_option_set ||
        cli->gap_report_option_set || cli->isolated_note_option_set ||
        cli->harmonic_decay_expected_set || cli->decode_block_option_set)
        return -1;
    if (!hwa_inference_htdemucs_onnx_available()) {
        (void)fputs(
            "hlolli-wg-analyzer: ONNX Runtime support is not compiled\n",
            stderr);
        return 1;
    }
    if (hwa_wav_reader_open(
            &reader, cli->positionals[1], cli->options.max_input_bytes,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        goto cleanup;
    }
    reader_open = 1;
    if (reader.format.sample_rate_hz != HWA_HTDEMUCS_SAMPLE_RATE ||
        (reader.format.channels != 1U && reader.format.channels != 2U)) {
        (void)fputs(
            "hlolli-wg-analyzer: HTDemucs v1 needs mono or stereo "
            "44100 Hz WAVE input\n",
            stderr);
        goto cleanup;
    }
    if (reader.format.frames > cli->options.max_input_frames) {
        (void)fputs(
            "hlolli-wg-analyzer: HTDemucs input exceeds the frame limit\n",
            stderr);
        goto cleanup;
    }
    source = reader.source;
    source.name = cli->positionals[1];
    if (hwa_inference_byte_source_sha256(
            &source, cli->options.max_input_bytes, source_sha256,
            error, sizeof(error)) != 0 ||
        hwa_inference_htdemucs_onnx_runner_open(
            cli->model_path, cli->expected_model_sha256,
            cli->max_model_bytes, &model_runner, model_sha256,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "cannot open separation inputs");
        goto cleanup;
    }
    if (hwa_htdemucs_instrument_runner_init(
            &stem_runner, cli->options.max_work_bytes, &model_runner,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "cannot open HTDemucs adapter");
        goto cleanup;
    }
    memset(&model_runner, 0, sizeof(model_runner));
    if (hwa_instrument_stem_provider_init(
            &provider, model_sha256, HWA_HTDEMUCS_ADAPTER_SHA256,
            cli->options.max_work_bytes, &stem_runner,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "cannot open instrument stem provider");
        goto cleanup;
    }
    provider_open = 1;
    memset(&stem_runner, 0, sizeof(stem_runner));
    input.id = "source";
    input.role = "source-recording";
    input.media_type = "audio/wav";
    input.sha256 = source_sha256;
    input.bytes = source;
    request.task = HWA_INSTRUMENT_STEM_TASK_NAME;
    request.settings_json = "{}";
    request.expected_provider_name = provider.name;
    request.expected_provider_version = provider.version;
    request.expected_model_sha256 = provider.model_sha256;
    request.seed = UINT64_C(0);
    request.source_recording_id = UINT64_C(1);
    request.source_input_id = input.id;
    request.inputs = &input;
    request.input_count = 1U;
    request.source_format = reader.format;
    request.max_input_file_bytes = cli->options.max_input_bytes;
    request.max_input_bytes = cli->options.max_input_bytes;
    request.timeout_milliseconds = cli->inference_timeout_milliseconds;
    hwa_event_bundle_limits_default(&request.output_limits);
    request.output_limits.max_audio_files = HWA_HTDEMUCS_STEM_COUNT + 1U;
    request.output_limits.max_events = HWA_HTDEMUCS_STEM_COUNT;
    request.output_limits.max_values = HWA_HTDEMUCS_STEM_COUNT;
    request.output_limits.max_traces = 1U;
    request.output_limits.max_trace_refs = 1U;
    request.output_limits.max_providers = 1U;
    request.output_limits.max_warnings = 1U;
    request.output_limits.max_work_bytes = cli->options.max_work_bytes;
    if (provider.start(
            provider.context, &request, &task,
            error, sizeof(error)) != 0 || task == NULL ||
        provider.poll(
            provider.context, task, &state, &output,
            error, sizeof(error)) != 0 ||
        state != HWA_INFERENCE_READY || output == NULL ||
        hwa_inference_output_validate_for_request(
            &provider, &request, output, error, sizeof(error)) != 0 ||
        hwa_inference_output_write(
            cli->output_path, output, &request.output_limits,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "instrument separation failed");
        goto cleanup;
    }
    result = 0;
cleanup:
    if (task != NULL && provider.task_free != NULL)
        provider.task_free(provider.context, task);
    if (provider_open) hwa_inference_provider_destroy(&provider);
    if (stem_runner.context != NULL && stem_runner.destroy != NULL)
        stem_runner.destroy(stem_runner.context);
    if (model_runner.context != NULL && model_runner.destroy != NULL)
        model_runner.destroy(model_runner.context);
    if (reader_open) hwa_wav_reader_close(&reader);
    return result;
}

static int hwa_cli_event_format_equal(const HWAFormat *left,
                                      const HWAFormat *right)
{
    return left != NULL && right != NULL &&
           left->container == right->container &&
           left->encoding == right->encoding &&
           left->channels == right->channels &&
           left->sample_rate_hz == right->sample_rate_hz &&
           left->bits_per_sample == right->bits_per_sample &&
           left->valid_bits_per_sample == right->valid_bits_per_sample &&
           left->block_align == right->block_align &&
           left->channel_mask == right->channel_mask &&
           left->frames == right->frames &&
           left->data_bytes == right->data_bytes &&
           left->duration_seconds == right->duration_seconds;
}

static int hwa_cli_event_payload_path(const char *directory,
                                      const char *relative_path,
                                      char path[HWA_CLI_EVENT_PATH_LIMIT])
{
    int length;
    if (directory == NULL || directory[0] == '\0' ||
        relative_path == NULL || relative_path[0] == '\0' || path == NULL)
        return -1;
    length = snprintf(path, HWA_CLI_EVENT_PATH_LIMIT, "%s/%s",
                      directory, relative_path);
    return length < 0 || (size_t)length >= HWA_CLI_EVENT_PATH_LIMIT
               ? -1 : 0;
}

typedef struct HWACliStemPathSource HWACliStemPathSource;

typedef struct HWACliStemSourcePool {
    HWAWavReader reader;
    const HWACliStemPathSource *active;
    uint64_t max_input_bytes;
    int reader_open;
} HWACliStemSourcePool;

struct HWACliStemPathSource {
    HWACliStemSourcePool *pool;
    const char *directory;
    const char *relative_path;
    uint64_t expected_size;
};

static int hwa_cli_stem_source_read_at(void *context,
                                       uint64_t offset,
                                       unsigned char *destination,
                                       size_t size)
{
    HWACliStemPathSource *source = (HWACliStemPathSource *)context;
    HWACliStemSourcePool *pool;
    char path[HWA_CLI_EVENT_PATH_LIMIT];
    char ignored[HWA_ERROR_SIZE] = {0};
    if (source == NULL || source->pool == NULL || destination == NULL ||
        offset > source->expected_size ||
        (uint64_t)size > source->expected_size - offset)
        return -1;
    pool = source->pool;
    if (pool->active != source) {
        if (pool->reader_open) {
            hwa_wav_reader_close(&pool->reader);
            pool->reader_open = 0;
            pool->active = NULL;
        }
        if (hwa_cli_event_payload_path(
                source->directory, source->relative_path, path) != 0 ||
            hwa_wav_reader_open(
                &pool->reader, path, pool->max_input_bytes,
                ignored, sizeof(ignored)) != 0)
            return -1;
        pool->reader_open = 1;
        if (pool->reader.source.size != source->expected_size) {
            hwa_wav_reader_close(&pool->reader);
            pool->reader_open = 0;
            return -1;
        }
        pool->active = source;
    }
    return pool->reader.source.read_at(
        pool->reader.source.context, offset, destination, size);
}

static int hwa_run_infer_stem_note_events(const HWACli *cli)
{
    HWAEventBundle stem_bundle;
    HWAEventBundleLimits limits;
    HWAStemNoteDerivationOptions derivation_options;
    HWAStemNoteDerivation *derivation = NULL;
    HWAStemNoteAudioSource *sources = NULL;
    HWACliStemPathSource *path_sources = NULL;
    HWACliStemSourcePool source_pool;
    HWABasicPitchRunner runner;
    HWAInferenceProvider basic_pitch_provider;
    HWAInferenceProvider audio_note_provider;
    char *settings_json = NULL;
    char model_sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    size_t stem_count = 0U;
    size_t audio_index;
    int basic_pitch_open = 0;
    int audio_note_open = 0;
    int result = 1;
    memset(&stem_bundle, 0, sizeof(stem_bundle));
    memset(&runner, 0, sizeof(runner));
    memset(&source_pool, 0, sizeof(source_pool));
    memset(&basic_pitch_provider, 0, sizeof(basic_pitch_provider));
    memset(&audio_note_provider, 0, sizeof(audio_note_provider));
    if (cli->positional_count != 2U ||
        strcmp(cli->positionals[0], "infer-stem-note-events") != 0 ||
        cli->model_path == NULL || cli->model_path[0] == '\0' ||
        cli->output_path == NULL || cli->output_path[0] == '\0' ||
        strcmp(cli->positionals[1], "-") == 0 ||
        strcmp(cli->model_path, "-") == 0 ||
        strcmp(cli->output_path, "-") == 0 ||
        strcmp(cli->positionals[1], cli->output_path) == 0 ||
        cli->json || cli->replace || cli->export_kind != 0 ||
        cli->score_path != NULL || cli->alignment_path != NULL ||
        cli->labels_path != NULL || cli->amend_path != NULL ||
        cli->items_path != NULL || cli->room_ir_path != NULL ||
        cli->renderer_path != NULL || cli->resume_path != NULL ||
        cli->allow_run || cli->physical_binding_count != 0U ||
        cli->analysis_clock_option_set || cli->analysis_only_option_set ||
        cli->analysis_spectral_resource_option_set ||
        cli->alignment_option_set || cli->segmentation_option_set ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set || cli->experiment_option_set ||
        cli->gap_report_option_set || cli->isolated_note_option_set ||
        cli->harmonic_decay_expected_set || cli->decode_block_option_set)
        return -1;
    if (hwa_basic_pitch_decoder_options_validate(
            &cli->basic_pitch_options, error, sizeof(error)) != 0)
        return -1;
    if (!hwa_inference_basic_pitch_onnx_available()) {
        (void)fputs(
            "hlolli-wg-analyzer: ONNX Runtime support is not compiled\n",
            stderr);
        return 1;
    }
    if (hwa_event_bundle_output_path_validate(
            cli->output_path, cli->positionals[1],
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n", error);
        return 1;
    }
    hwa_event_bundle_limits_default(&limits);
    if (limits.max_payload_file_bytes > cli->options.max_input_bytes)
        limits.max_payload_file_bytes = cli->options.max_input_bytes;
    limits.max_work_bytes = cli->options.max_work_bytes;
    if (hwa_event_bundle_read(
            cli->positionals[1], &limits, &stem_bundle,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "cannot read instrument stems");
        goto cleanup;
    }
    for (audio_index = 0U; audio_index < stem_bundle.audio_count;
         ++audio_index) {
        const HWAEventAudio *audio = &stem_bundle.audio[audio_index];
        if (audio->kind == HWA_EVENT_SOURCE_RECORDING &&
            audio->format.frames > cli->options.max_input_frames) {
            (void)fputs(
                "hlolli-wg-analyzer: stem source exceeds the frame limit\n",
                stderr);
            goto cleanup;
        }
        if (audio->kind == HWA_EVENT_INSTRUMENT_STEM) stem_count++;
    }
    if (stem_count == 0U || stem_count > SIZE_MAX / sizeof(*sources) ||
        stem_count > SIZE_MAX / sizeof(*path_sources)) {
        (void)fputs(
            "hlolli-wg-analyzer: instrument stem count is invalid\n",
            stderr);
        goto cleanup;
    }
    sources = (HWAStemNoteAudioSource *)calloc(
        stem_count, sizeof(*sources));
    path_sources = (HWACliStemPathSource *)calloc(
        stem_count, sizeof(*path_sources));
    if (sources == NULL || path_sources == NULL) {
        (void)fputs(
            "hlolli-wg-analyzer: cannot allocate stem input rows\n", stderr);
        goto cleanup;
    }
    source_pool.max_input_bytes = cli->options.max_input_bytes;
    stem_count = 0U;
    for (audio_index = 0U; audio_index < stem_bundle.audio_count;
         ++audio_index) {
        const HWAEventAudio *audio = &stem_bundle.audio[audio_index];
        HWAWavReader reader;
        if (audio->kind != HWA_EVENT_INSTRUMENT_STEM) continue;
        memset(&reader, 0, sizeof(reader));
        path_sources[stem_count].pool = &source_pool;
        path_sources[stem_count].directory = cli->positionals[1];
        path_sources[stem_count].relative_path = audio->relative_path;
        path_sources[stem_count].expected_size = audio->file_bytes;
        sources[stem_count].audio_id = audio->id;
        sources[stem_count].bytes.context = &path_sources[stem_count];
        sources[stem_count].bytes.name = audio->relative_path;
        sources[stem_count].bytes.size = audio->file_bytes;
        sources[stem_count].bytes.read_at = hwa_cli_stem_source_read_at;
        if (hwa_wav_reader_open_source(
                &reader, &sources[stem_count].bytes,
                cli->options.max_input_bytes, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                          error[0] != '\0' ? error
                                            : "invalid stem payload path");
            goto cleanup;
        }
        if (!hwa_cli_event_format_equal(
                &reader.format, &audio->format) ||
            audio->format.encoding != HWA_ENCODING_IEEE_FLOAT ||
            audio->format.channels != 2U ||
            audio->format.sample_rate_hz != 44100U ||
            audio->format.bits_per_sample != 32U ||
            audio->format.valid_bits_per_sample != 32U ||
            audio->format.block_align != 8U ||
            audio->format.frames > cli->options.max_input_frames) {
            (void)fputs(
                "hlolli-wg-analyzer: stem note inference needs stereo "
                "float32 44100 Hz stems within the frame limit\n",
                stderr);
            hwa_wav_reader_close(&reader);
            goto cleanup;
        }
        hwa_wav_reader_close(&reader);
        stem_count++;
    }
    if (hwa_basic_pitch_task_settings_build(
            &cli->basic_pitch_options, &settings_json,
            error, sizeof(error)) != 0 ||
        hwa_inference_basic_pitch_onnx_runner_open(
            cli->model_path, cli->expected_model_sha256,
            cli->max_model_bytes, &runner, model_sha256,
            error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "cannot open note inference inputs");
        goto cleanup;
    }
    if (hwa_basic_pitch_provider_init(
            &basic_pitch_provider, model_sha256,
            HWA_BASIC_PITCH_ADAPTER_SHA256,
            &cli->basic_pitch_options, cli->options.max_work_bytes,
            &runner, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "cannot open Basic Pitch provider");
        goto cleanup;
    }
    basic_pitch_open = 1;
    memset(&runner, 0, sizeof(runner));
    if (hwa_basic_pitch_audio_provider_init(
            &audio_note_provider, &basic_pitch_provider,
            HWA_BASIC_PITCH_AUDIO_ADAPTER_SHA256,
            cli->options.max_work_bytes, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "cannot open raw-audio note provider");
        goto cleanup;
    }
    audio_note_open = 1;
    hwa_stem_note_derivation_options_default(&derivation_options);
    derivation_options.note_task = HWA_BASIC_PITCH_AUDIO_TASK_NAME;
    derivation_options.note_settings_json = settings_json;
    derivation_options.max_input_file_bytes = cli->options.max_input_bytes;
    derivation_options.max_input_bytes = cli->options.max_input_bytes;
    derivation_options.timeout_milliseconds =
        cli->inference_timeout_milliseconds;
    derivation_options.max_note_events = cli->max_note_events;
    derivation_options.output_limits = limits;
    if (hwa_stem_note_derivation_run(
            &stem_bundle, sources, stem_count, &audio_note_provider,
            &derivation_options, &derivation,
            error, sizeof(error)) != 0 || derivation == NULL ||
        hwa_inference_output_write(
            cli->output_path,
            hwa_stem_note_derivation_output(derivation),
            &limits, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hlolli-wg-analyzer: %s\n",
                      error[0] != '\0' ? error
                                        : "stem note inference failed");
        goto cleanup;
    }
    result = 0;
cleanup:
    hwa_stem_note_derivation_free(derivation);
    if (audio_note_open)
        hwa_inference_provider_destroy(&audio_note_provider);
    if (basic_pitch_open)
        hwa_inference_provider_destroy(&basic_pitch_provider);
    if (runner.context != NULL && runner.destroy != NULL)
        runner.destroy(runner.context);
    if (source_pool.reader_open)
        hwa_wav_reader_close(&source_pool.reader);
    free(path_sources);
    free(sources);
    hwa_event_bundle_free(&stem_bundle);
    free(settings_json);
    return result;
}

static int hwa_run_inference_capabilities(const HWACli *cli)
{
    const char *onnx_version = hwa_inference_onnx_runtime_version();
    int onnx_available = hwa_inference_onnx_available();
    int htdemucs_available = hwa_inference_htdemucs_onnx_available();
    if (cli->positional_count != 1U ||
        strcmp(cli->positionals[0], "inference-capabilities") != 0 ||
        cli->output_path != NULL || cli->replace ||
        cli->export_kind != 0 || cli->score_path != NULL ||
        cli->alignment_path != NULL || cli->labels_path != NULL ||
        cli->amend_path != NULL || cli->items_path != NULL ||
        cli->room_ir_path != NULL || cli->renderer_path != NULL ||
        cli->resume_path != NULL || cli->allow_run ||
        cli->physical_binding_count != 0U ||
        cli->analysis_clock_option_set || cli->analysis_only_option_set ||
        cli->analysis_resource_option_set || cli->decode_block_option_set ||
        cli->max_bytes_option_set || cli->input_frame_limit_set ||
        cli->alignment_option_set || cli->segmentation_option_set ||
        cli->measurement_option_set || cli->comparison_option_set ||
        cli->physical_option_set || cli->production_option_set ||
        cli->run_option_set || cli->experiment_option_set ||
        cli->gap_report_option_set || cli->isolated_note_option_set ||
        cli->harmonic_decay_expected_set) {
        return -1;
    }
    if (cli->json) {
        if (fputs("{\"schema\":\"hwa-inference-capabilities\","
                  "\"schema_version\":1,"
                  "\"built_in_event_analysis\":true,"
                  "\"onnx_runtime\":{\"compiled\":",
                  stdout) == EOF ||
            fputs(onnx_available ? "true,\"version\":"
                                 : "false,\"version\":null",
                  stdout) == EOF ||
            (onnx_available && hwa_json_write_string(stdout, onnx_version) != 0) ||
            fputs("},\"polyphonic_note_task\":{"
                  "\"contract\":\"org.hlolli.polyphonic-note-events-v1\","
                  "\"provider\":\"org.hlolli.basic-pitch-onnx\","
                  "\"implemented\":true,\"available\":",
                  stdout) == EOF) {
            return 1;
        }
        if (fputs(onnx_available ? "true" : "false", stdout) == EOF ||
            fputs("},\"instrument_stem_task\":{"
                  "\"contract\":\"" HWA_INSTRUMENT_STEM_TASK_NAME "\","
                  "\"provider\":\"" HWA_INSTRUMENT_STEM_PROVIDER_NAME "\","
                  "\"implemented\":true,\"available\":",
                  stdout) == EOF ||
            fputs(htdemucs_available ? "true" : "false", stdout) == EOF ||
            fputs("},\"stem_note_workflow\":{"
                  "\"contract\":\"org.hlolli.stem-note-events-v1\","
                  "\"provider\":\"" HWA_BASIC_PITCH_AUDIO_PROVIDER_NAME
                  "\",\"implemented\":true,\"available\":",
                  stdout) == EOF ||
            fputs(onnx_available ? "true" : "false", stdout) == EOF ||
            fputs("}}\n", stdout) == EOF)
            return 1;
    } else if (fprintf(
                   stdout,
                   "Inference capabilities\n"
                   "Built-in event analysis: available\n"
                   "ONNX Runtime: %s%s%s%s\n"
                   "Polyphonic note task: Basic Pitch adapter %s\n"
                   "Instrument stem task: HTDemucs six-stem adapter %s\n"
                   "Stem note workflow: Basic Pitch per-stem adapter %s\n",
                   onnx_available ? "available" : "not compiled",
                   onnx_available ? " (" : "",
                   onnx_available ? onnx_version : "",
                   onnx_available ? ")" : "",
                   onnx_available ? "available" : "not available",
                   htdemucs_available ? "available" : "not available",
                   onnx_available ? "available" : "not available") < 0) {
        return 1;
    }
    return hwa_finish_stream(stdout, "standard output") == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    HWACli cli;
    int parse_result;
    int result;

#if !defined(_WIN32)
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        (void)fputs("hlolli-wg-analyzer: cannot ignore SIGPIPE\n", stderr);
        return 1;
    }
#endif
    parse_result = hwa_parse_cli(argc, argv, &cli);

    if (parse_result > 0) {
        return hwa_finish_stream(stdout, "standard output") == 0 ? 0 : 1;
    }
    if (parse_result < 0 || cli.positional_count == 0U) {
        hwa_print_usage(stderr);
        return 2;
    }
    if (strcmp(cli.positionals[0], "infer-note-events") != 0 &&
        strcmp(cli.positionals[0], "separate-instruments") != 0 &&
        strcmp(cli.positionals[0], "infer-stem-note-events") != 0 &&
        cli.inference_option_set) {
        result = -1;
    } else if (strcmp(cli.positionals[0], "infer-note-events") != 0 &&
        strcmp(cli.positionals[0], "infer-stem-note-events") != 0 &&
        cli.basic_pitch_option_set) {
        result = -1;
    } else if (strcmp(cli.positionals[0], "isolated-note") != 0 &&
        strcmp(cli.positionals[0], "harmonic-decay") != 0 &&
        cli.isolated_note_option_set) {
        result = -1;
    } else if (strcmp(cli.positionals[0], "experiment") != 0 &&
        strcmp(cli.positionals[0], "rank") != 0 &&
        strcmp(cli.positionals[0], "excerpt") != 0 &&
        strcmp(cli.positionals[0], "report") != 0 &&
        (cli.renderer_path != NULL || cli.resume_path != NULL ||
         cli.allow_run || cli.experiment_option_set)) {
        result = -1;
    } else if (strcmp(cli.positionals[0], "rank") != 0 &&
               strcmp(cli.positionals[0], "excerpt") != 0 &&
               strcmp(cli.positionals[0], "report") != 0 &&
               cli.gap_report_option_set) {
        result = -1;
    } else if (strcmp(cli.positionals[0], "inspect") == 0) {
        result = hwa_run_inspect(&cli);
    } else if (strcmp(cli.positionals[0], "compare") == 0) {
        result = hwa_run_compare(&cli);
    } else if (strcmp(cli.positionals[0], "body-envelope") == 0) {
        result = hwa_run_body_envelope(&cli);
    } else if (strcmp(cli.positionals[0], "isolated-note") == 0) {
        result = hwa_run_isolated_note(&cli);
    } else if (strcmp(cli.positionals[0], "harmonic-decay") == 0) {
        result = hwa_run_harmonic_decay(&cli);
    } else if (strcmp(cli.positionals[0], "export") == 0) {
        result = hwa_run_export(&cli);
    } else if (strcmp(cli.positionals[0], "align") == 0) {
        result = hwa_run_align(&cli);
    } else if (strcmp(cli.positionals[0], "segment") == 0) {
        result = hwa_run_segment(&cli);
    } else if (strcmp(cli.positionals[0], "measure") == 0) {
        result = hwa_run_measure(&cli);
    } else if (strcmp(cli.positionals[0], "compare-measures") == 0) {
        result = hwa_run_compare_measures(&cli);
    } else if (strcmp(cli.positionals[0], "check-physical") == 0) {
        result = hwa_run_check_physical(&cli);
    } else if (strcmp(cli.positionals[0], "account-production") == 0) {
        result = hwa_run_account_production(&cli);
    } else if (strcmp(cli.positionals[0], "analyze-run") == 0) {
        result = hwa_run_analyze_run(&cli);
    } else if (strcmp(cli.positionals[0], "experiment") == 0) {
        result = hwa_run_experiment(&cli);
    } else if (strcmp(cli.positionals[0], "rank") == 0 ||
               strcmp(cli.positionals[0], "excerpt") == 0 ||
               strcmp(cli.positionals[0], "report") == 0) {
        result = hwa_run_gap_report(&cli);
    } else if (strcmp(cli.positionals[0], "validate-event-bundle") == 0) {
        result = hwa_run_validate_event_bundle(&cli);
    } else if (strcmp(cli.positionals[0], "analyze-events") == 0) {
        result = hwa_run_analyze_events(&cli);
    } else if (strcmp(cli.positionals[0], "infer-note-events") == 0) {
        result = hwa_run_infer_note_events(&cli);
    } else if (strcmp(cli.positionals[0], "separate-instruments") == 0) {
        result = hwa_run_separate_instruments(&cli);
    } else if (strcmp(cli.positionals[0], "infer-stem-note-events") == 0) {
        result = hwa_run_infer_stem_note_events(&cli);
    } else if (strcmp(cli.positionals[0], "inference-capabilities") == 0) {
        result = hwa_run_inference_capabilities(&cli);
    } else {
        result = -1;
    }
    if (result < 0) {
        (void)fputs("hlolli-wg-analyzer: invalid command options\n", stderr);
        hwa_print_usage(stderr);
        return 2;
    }
    return result;
}
