# Audio Evidence

This context names the audio, musical, and inferred facts that the analyzer
records without tying them to one score format or model.

## Language

**Source recording**:
The unchanged audio supplied for analysis and the clock against which exact
sample positions are stated.
_Avoid_: Original stem, clean source

**Derived audio**:
Audio produced from a source recording by separation, cleanup, rendering, or
another stated process.
_Avoid_: Clean audio, true source

**Instrument stem**:
Derived audio estimated to contain one named instrument or instrument group.
_Avoid_: Isolated truth, clean instrument

**Score event**:
A musical event supplied by score material rather than found in audio.
_Avoid_: Ground truth note

**Performance event**:
A bounded musical occurrence found or measured in a source recording or
instrument stem.
_Avoid_: Score note, frame

**Event candidate**:
One possible label or reading for a performance event, paired with its score.
_Avoid_: Measurement, fact

**Trace**:
An ordered series of values tied to a source clock, such as pitch, level, or
spectral change.
_Avoid_: Event, summary

**Observation**:
A value measured from stated audio over stated sample bounds.
_Avoid_: Guess, label

**Inference**:
A value estimated by a rule or trained model rather than measured directly.
_Avoid_: Measurement, truth

**Event bundle**:
A complete set of musical events, traces, audio links, and supporting facts
derived from one or more source recordings.
_Avoid_: Score, database, report

**Inference provider**:
Code or a model adapter that accepts named inputs and returns one event bundle.
_Avoid_: Analyzer, model runtime

**Inference task**:
One named job that a provider starts, checks, and frees.
_Avoid_: Thread, process

**Source clock**:
The source recording's sample rate and frame positions used by all linked
events, traces, and derived audio.
_Avoid_: Seconds timeline, score time
