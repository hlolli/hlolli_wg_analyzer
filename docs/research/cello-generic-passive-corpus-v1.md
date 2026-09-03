# Generic cello passive-string corpus fit

Checked on 2026-09-03. This work replaces one-recording-per-split parameter
selection with source-balanced development evidence from four different
cellos, players, sessions, and microphone chains. It does not change the fixed
cello model and does not claim that one physical parameter vector can reproduce
every cello.

## Corpus and isolation

The source-level folds are:

| Development fold | Source | Player/cello identity | Dynamics offered |
|---|---|---|---|
| A | University of Iowa 2012 | Yoo-Jung Chang; 1923 Charles Quenoil cello | `pp`, `mf`, `ff` |
| A | RWC cello variation 2 | RWC professional player/maker variation 2 | piano, mezzo, forte |
| B | University of Iowa 2001 | Jean Montes; site-labelled 1736 cello | `pp`, `mf`, `ff` |
| B | RWC cello variation 3 | RWC professional player/maker variation 3 | piano, mezzo, forte |

A player, cello, session, and played take never cross folds. Both folds are
development data: after designing corpus intake and aggregation against these
sources, fold B is not misrepresented as untouched validation. The only
held-out generalization evidence is the separately reserved fifth player,
cello, and session.

RWC variation 1 and OrchideaSOL remain consumed final-audit sources. None of
their audio, metrics, residuals, or prior candidate outcomes enters this fit.
The Internet Cello Society files and lossy Freesound previews remain diagnostic
only.

The source plan named 48 open-string pizzicato candidates: three dynamics on
four strings from each source identity. Model-blind pitch, one-event decay, and
harmonic-support checks admitted 28:

| String | Admitted references | Source identities |
|---|---:|---:|
| C2 | 5 | 4 |
| G2 | 8 | 4 |
| D3 | 8 | 4 |
| A3 | 7 | 4 |

Rejected rows stay rejected. Typical reasons are a second onset, less than the
required decay range, an irregular tail, pitch outside 60 cents, or fewer than
three valid reference harmonics. No model result can restore a rejected row.
Raw audio, work audio, renders, and selections stay outside Git.

## Intake correction

The first model-blind intake treated each 60-pluck RWC file as one chromatic
group. The files actually contain four ascending string groups, whose open
strings are events 0, 13, 26, and 39. A later check also found that the generic
Iowa range extractor could select a later low-frequency event instead of the
already receipted first open note. All corpus-v1 model comparisons were
therefore invalidated before use.

The final intake uses the existing exact Iowa 2001 `ff` spans, source-specific
silence separation for the remaining Iowa ranges, and the four RWC group
ordinals above. Its model-blind prepared-corpus receipt SHA-256 is
`0ab0f7bc5305d1f3529e9ab34d978815161137ff33b52cc3c19ba6df713b1d82`.
The final predeclaration SHA-256 is
`4a4df1cc997be558088ccd667ed0d7b669d096d6453a4464e265a68523db95e2`.
The execution freeze SHA-256 is
`ff2fbabcf0acbc8faa264be9eb35bf10d4141dede6054fb44a2a77cfda18e894`.

## Source-balanced selection

For each string and objective kind, every source identity gets equal total
weight. Multiple admitted dynamics divide their source's weight instead of
letting the largest source dominate. Both development folds rank candidates.
The primary rank is their combined source-balanced mean, followed by the worst
source mean and canonical point ID.

A candidate source mean must not exceed 3.0 or worsen by more than 0.25 from
that source's baseline. Fold B may not worsen in aggregate. Each reference
still needs at least three model/reference harmonic overlaps, T60 ratio from
one third through three, support ratio at least one third, mean harmonic error
at most 1.5 octaves, and maximum harmonic error at most 3 octaves. These wider
per-take bounds recognize real between-cello variation; the source-mean gate,
not a demand to imitate every take, controls generic quality.

The implementation adds checked corpus plans, source-conditioned render
geometry, source-balanced objective weights, source-mean evidence, and
source-mean gates. Sixty-five selector and cello-adapter tests pass.

## Results

D3 passes the original 24-point frequency-loss grid. Point 20 chooses the
existing 7086.471 Hz bridge cutoff, 0.75-second loss time, and 8 kHz nut
cutoff. Its source-balanced fold A/B losses are 2.143 and 1.136, down from a
6.425 total baseline score. Ten points pass every rule.

The scalar A3 grid fails because its best one-second point leaves only one and
two valid harmonic overlaps for the RWC variation-3 forte and mezzo takes. A
predeclared 24-point existing-control follow-up raises termination cutoffs
without adding DSP. It passes at point 11: 12 kHz bridge cutoff, 1.0-second
loss time, and the existing 12 kHz nut cutoff. Its fold A/B losses are 1.573
and 1.344, for a 2.918 score versus the 8.048 baseline. Five points pass.

C2 fails both the original 48-point grid and its predeclared 48-point
existing-control follow-up. In the follow-up, point 4 has source means from
0.851 through 2.354 and a 3.648 development score, but the RWC variation-3
mezzo reference has valid harmonics 1, 3, and 8 while the model overlaps only
harmonics 3 and 8. The model fundamental is too short, yet harmonics 3 and 8
are already 1.439 and 1.565 octaves too long. Higher nut/bridge cutoffs and
loss times through 2.0 seconds never produce the required three overlaps.
Existing one-pole terminations cannot independently preserve the fundamental
while damping those upper partials, so no generic C2 profile is staged.

G2 passes the 96-point bridge-peak grid. Point 88 chooses an 18 kHz bridge
cutoff, 200 Hz peak bandwidth, 0.02 peak loss, and 3.0-second broadband loss
time. Its fold A/B losses are 1.770 and 1.393, for a 3.163 score versus the
9.097 baseline. Four points pass every rule. The generic result is wider and
longer-lived than the earlier one-cello candidate (100 Hz and 2.5 seconds).

| Target | Bundle receipt | Experiment | Selection | Staged profile |
|---|---|---|---|---|
| C2 follow-up, fail | `488c052b986c37284c3847decfa0ac5760509b766018e4db0554836a1ecd3126` | `30c6de00c6e55cb86f0899841ae1e8025ad7e723b36b04225100728d4dc6d617` | `e6a8bc850d18d9a0fe971915d43c967fb392ce1dd9754f62a29d69f313d5f3a9` | -- |
| G2, pass | `3a9960ab8e31de7bcf4be009a0418f1159a71a4c27eb4fbaa287f5633e91eead` | `9d8bfeca6814fc55c2d5f31b60f0379f91ae689b2b2914f3ed14ff10b51ffed6` | `2932885cf5d328fba02ef39fa26b4c4e13f8aef262d9f9b2c4dc8a2c90c8d821` | `25506d0659b8b4aef623dde978707c35b2b2a0ff16616d0b97bc058ce869ef04` |
| D3, pass | `c8c480824b8987c012e0258d1ddc35215caa373edcfd51bc3c729b6c4d3178a1` | `a329db16064ea1538c3f77a142e253ed0f5eb906cb78ed308fb91d20f2b33a3b` | `f129d55cd31f53c22f26d0868078c0bc1550bd3aed54a066bfb64f237cc55482` | `54a92d941884c6f5e53cbbc0a240babd46efb63f9c25cbab7c149b6ba32c62af` |
| A3 follow-up, pass | `65eda5b0b81b74ef7180962708d79df45a8eec8efe62f14bc68fb5a4baf57a55` | `cbd660e1099860a4a0a206034834547eb6f1b3f25b2244e7b0919846c06221ed` | `5f0c58b12ca430a306f210d7dd5f45f953bfe61e46dd1bbbe148745ff2abc607` | `94b7676e69258b6aa15fc3204af6a337fc48c029d9e9e1944fc1bfb6730dab4d` |

The final external summary is `result-summary-v4.json` in the private run
directory. A timestamp correction receipt, SHA-256
`fceea3390377d9e6da941c2b490390ecc465c4fc4a5af7c09f53aabfe3e38212`,
invalidates approximate wall-clock labels in earlier immutable receipts without
changing their hashes, ordering, inputs, or scientific results.

## Audit reservation and model state

Audit reservation SHA-256
`62896655d0855db1ab729da4f906fbf756fbc5a0584c76bcd4cfcb29ce3b521a`
requires a new player, cello, and session with C2, G2, D3, and A3 ordinary
pizzicato at three dynamics and three replicates. The audit audio must not be
opened or measured until a four-string candidate and its gates are frozen. It
may only accept or reject that candidate.

The fixed profile SHA-256 remains
`af2fcd8a7bafa2dd40ef5883f5f5ca85c4fa9f096d213ee902ef515eded763ac`.
It still has 0.25-second loss time and zero bridge-peak loss on every string,
and its evidence status remains `violin-derived`. Staged per-string profiles
are not an authorized fixed model.

For later bowed-string work, apply the same source-level rule to all eligible
Iowa dynamics and quality-good Good-sounds cello rows, keeping player and pack
identities in one fold. TinySOL and the public Bach probes remain held-out
note/piece checks. More files from one cello are useful for dynamics; they do
not substitute for more independent cellos.
