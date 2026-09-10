#include <hlolli_wg_analyzer.h>

#include <stdint.h>
#include <string.h>

typedef struct ClientBytes {
    const unsigned char *data;
    size_t size;
} ClientBytes;

static int client_read_at(void *context,
                          uint64_t offset,
                          unsigned char *destination,
                          size_t size)
{
    ClientBytes *bytes = (ClientBytes *)context;
    if (offset > (uint64_t)bytes->size ||
        (uint64_t)size > (uint64_t)bytes->size - offset) return -1;
    memcpy(destination, bytes->data + (size_t)offset, size);
    return 0;
}

int main(void)
{
    static const unsigned char truncated_wave[12] = {
        'R', 'I', 'F', 'F', 4U, 0U, 0U, 0U,
        'W', 'A', 'V', 'E'
    };
    ClientBytes bytes = {truncated_wave, sizeof(truncated_wave)};
    HWAByteSource source = {
        &bytes, "public-client.wav", sizeof(truncated_wave), client_read_at
    };
    HWAAnalysis analysis;
    char error[HWA_ERROR_SIZE];
    if (strcmp(HWA_VERSION, "1.1.0") != 0 ||
        hwa_analyze_wav_source(&source, NULL, &analysis,
                               error, sizeof(error)) == 0 ||
        error[0] == '\0' || analysis.path != NULL ||
        analysis.channels != NULL || analysis.tracks != NULL ||
        analysis.spectrogram_db != NULL) {
        return 1;
    }
    hwa_analysis_free(&analysis);
    {
        HWASeparationEvaluation evaluation;
        HWASeparationEvalOptions options;
        hwa_separation_eval_options_default(&options);
        if (hwa_evaluate_separation_samples(NULL, NULL, NULL, 0U, 0U,
                &options, &evaluation, error, sizeof(error)) == 0 ||
            hwa_evaluate_separation_wav(NULL, NULL, NULL, &options,
                &evaluation, error, sizeof(error)) == 0) return 1;
    }
    {
        HWAEventScoreOptions score_options;
        hwa_event_score_options_default(&score_options);
        if (score_options.kind != HWA_EVENT_SCORE_CSOUND || score_options.tempo_bpm != 0U ||
            hwa_event_score_write(NULL, NULL, 0U, &score_options, error, sizeof(error)) == 0 ||
            error[0] == '\0') return 1;
    }
    {
        HWANotePhaseOptions options;
        HWANotePhaseResult phases;
        hwa_note_phase_options_default(&options);
        memset(&phases, 0xA5, sizeof(phases));
        if (hwa_analyze_note_phases_wav(NULL, &options, &phases,
                                        error, sizeof(error)) == 0 ||
            phases.path != NULL || error[0] == '\0') return 1;
        hwa_note_phase_result_free(&phases);
        hwa_note_phase_result_free(&phases);
        {
            HWANotePhaseSpectraResult spectra;
            memset(&spectra, 0xA5, sizeof(spectra));
            if (hwa_analyze_note_phase_spectra_wav(NULL, &options, &spectra,
                    error, sizeof(error)) == 0 || spectra.series.envelope.summary.path != NULL ||
                    spectra.series.frames != NULL || spectra.bin_powers != NULL ||
                    spectra.bin_count != 0U || error[0] == '\0') return 1;
            hwa_note_phase_spectra_result_free(&spectra);
            hwa_note_phase_spectra_result_free(&spectra);
            if (hwa_analyze_note_phase_spectra_wav(NULL, &options, NULL,
                    error, sizeof(error)) == 0 || error[0] == '\0') return 1;
            hwa_note_phase_spectra_result_free(NULL);
        }
        {
            HWANotePhaseFramesResult frames;
            memset(&frames, 0xA5, sizeof(frames));
            if (hwa_analyze_note_phase_frames_wav(NULL, &options, &frames,
                    error, sizeof(error)) == 0 || frames.envelope.summary.path != NULL ||
                    frames.frames != NULL || frames.frame_count != 0U || error[0] == '\0') return 1;
            hwa_note_phase_frames_result_free(&frames);
            hwa_note_phase_frames_result_free(&frames);
            if (hwa_analyze_note_phase_frames_wav(NULL, &options, NULL,
                    error, sizeof(error)) == 0 || error[0] == '\0') return 1;
            hwa_note_phase_frames_result_free(NULL);
        }
        {
            HWANotePhaseEnvelopeResult envelope;
            memset(&envelope, 0xA5, sizeof(envelope));
            if (hwa_analyze_note_phase_envelope_wav(NULL, &options, &envelope,
                    error, sizeof(error)) == 0 || envelope.summary.path != NULL ||
                    envelope.attack_envelope_bins != 0U || error[0] == '\0') return 1;
            hwa_note_phase_envelope_result_free(&envelope);
            hwa_note_phase_envelope_result_free(&envelope);
            if (hwa_analyze_note_phase_envelope_wav(NULL, &options, NULL,
                    error, sizeof(error)) == 0 || error[0] == '\0') return 1;
        }
    }
    return 0;
}
