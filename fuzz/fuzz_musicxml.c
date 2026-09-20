#include <hlolli_wg_analyzer.h>

#include <math.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    HWAMusicXMLOptions options;
    HWAMusicXMLScore score;
    char error[HWA_ERROR_SIZE];
    size_t i;
    hwa_musicxml_options_default(&options);
    options.max_input_bytes = UINT64_C(1048576);
    options.max_work_bytes = UINT64_C(16777216);
    options.max_nodes = 32768U;
    options.max_events = 4096U;
    options.max_measure_visits = 4096U;
    options.performance = (int)(size % 2U);
    if (hwa_musicxml_read(data, size, &options, &score, error, sizeof(error)) == 0) {
        if (!isfinite(score.duration_beats) || score.duration_beats <= 0.0) abort();
        for (i = 0U; i < score.event_count; i++) {
            const HWAMusicXMLEvent *event = &score.events[i];
            if (!isfinite(event->start_beats) || event->start_beats < 0.0 ||
                !isfinite(event->duration_beats) || event->duration_beats < 0.0 ||
                !isfinite(event->midi_pitch) || !isfinite(event->tempo_bpm) ||
                event->source_offset > score.xml_size || event->source_size > score.xml_size-event->source_offset ||
                (i != 0U && score.events[i-1U].start_beats > event->start_beats)) abort();
        }
    } else if (score.events != NULL || score.storage != NULL || score.event_count != 0U) abort();
    hwa_musicxml_score_free(&score);
    hwa_musicxml_score_free(&score);
    return 0;
}
