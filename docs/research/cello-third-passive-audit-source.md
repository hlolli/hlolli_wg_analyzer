# Cello third passive-audit source receipt

First checked on 2026-08-31 and completed on 2026-09-02 from an
authorized copy of the official general ZIP. The audio may be analyzed for
this research but may not be redistributed. The archive, source WAVE files,
and analyzer output remain outside Git.

These four files form the untouched third audit set. They may accept or reject
the already fixed candidate, but they must not tune it or choose another
parameter value. The fixed model has not changed.

## Pinned OrchideaSOL set

The [official OrchideaSOL 2.0 record](https://zenodo.org/records/3740399)
describes 13,265 isolated notes recorded at IRCAM from 1996 through 1999. It
states that the files are mono 16-bit WAVE at 44.1 kHz and last two to ten
seconds. The checked 2020 general ZIP instead stores these four members as
mono 24-bit PCM WAVE at 44.1 kHz; the byte, ZIP, and decoded format checks
below take precedence for this audit. Zenodo hosts the metadata but not the
audio.

The official metadata file is
[`OrchideaSOL_metadata.csv`](https://zenodo.org/api/records/3740399/files/OrchideaSOL_metadata.csv/content).
It has 1,918,409 bytes, MD5
`a917eedb9e2ec8875cbc691a14c98069`, and SHA-256
`5ad06eff8c8c62ab0ba764e54f61db26e1ca40e4a92d519dd5cbe4bd14666259`.
It has 1,593 cello rows, including 309 rows whose named technique contains
`pizz`. Use the no-mute, mezzo-forte `pizzicato_l_vib` rows below. Their string
IDs select the open strings, not the alternate D3 and A3 fingerings on lower
strings.

| String | Official logical WAVE path | MIDI | String ID | Retuned | Fold |
|---|---|---:|---:|---|---:|
| C2 | `Strings/Violoncello/pizzicato_l_vib/Vc-pizz_lv-C2-mf-4c-N.wav` | 36 | 4 | no | 3 |
| G2 | `Strings/Violoncello/pizzicato_l_vib/Vc-pizz_lv-G2-mf-3c-N.wav` | 43 | 3 | no | 3 |
| D3 | `Strings/Violoncello/pizzicato_l_vib/Vc-pizz_lv-D3-mf-2c-N.wav` | 50 | 2 | no | 3 |
| A3 | `Strings/Violoncello/pizzicato_l_vib/Vc-pizz_lv-A3-mf-1c-N.wav` | 57 | 1 | no | 0 |

The record defines pitch names by the A4=440 equal-tempered convention. It
defines the final `N` as natural: the file is distributed as recorded, without
digital pitch shift. The metadata also sets `Needed digital retuning` to
`False` for all four rows. These facts do not prove that a measured pitch will
equal the A4=440 target. Measure each file and report the gap; do not tune it.

### Audio asset and access gate

The [official IRCAM project page](https://forum.ircam.fr/projects/detail/orchideasol/)
lists release `_OrchideaSOL2020_release`. Its public API names these assets:

| Asset | Exact official URL | Bytes | Official MD5 |
|---|---|---:|---|
| General ZIP | [`/asset/108/None`](https://forum.ircam.fr/asset/108/None) | 8,371,284,936 | `697ca9f9d6f0bcaceb8b3d2e3e0206ac` |
| Named Windows ZIP | [`_OrchideaSOL2020_release_win.zip`](https://forum.ircam.fr/asset/1601/_OrchideaSOL2020_release_win.zip) | 8,381,264,325 | `eb0ade68d7a191f57f28a03665c226df` |

Anonymous requests still redirect to IRCAM sign-in. The authorized research
copy checked on 2026-09-02 is the general ZIP named above. Its byte count is
`8371284936`, its MD5 matches the official `697ca9f9d6f0bcaceb8b3d2e3e0206ac`,
and its SHA-256 is
`983d56381290202f11acf08186002b9e2531eda69c91444b690fe0065362fdb7`.
The exact member prefix is `_OrchideaSOL2020_release/OrchideaSOL2020/`.

Zenodo says that the audio uses the
[IRCAM Forum License](https://forum.ircam.fr/legal/contrat-de-licence-forum-ircam/),
while the metadata uses CC BY 4.0. The local authorization permits analysis for
this research but not redistribution. No archive member, decoded sample, or
private path belongs in Git or a release artifact.

### Completed archive and raw-file receipt

Each extracted WAVE matched one unique ZIP member by path, uncompressed size,
and CRC-32. It also matched the MD5 in the official
`OrchideaSOL2020.md5.txt` shipped with the release.

| String | Raw bytes | ZIP bytes | CRC-32 | Official MD5 | Raw SHA-256 |
|---|---:|---:|---|---|---|
| C2 | 840,782 | 466,831 | `b4090e2e` | `24c9e0ba7a1a732ce9d52ca594eb0f79` | `d7e2a83cafdcfa5d59d50603850f129fbdc407ecdafb5c1a9b61bbb805344bb2` |
| G2 | 1,073,504 | 631,357 | `dbaa0038` | `8af99aa238fd997503f17dd7130f0c1d` | `628e6b86975ed2c50d04cca304cb8f0d7d761d3bdd1f1735da5efc49995e38fd` |
| D3 | 975,240 | 557,637 | `bbe07974` | `52a51ac71d0afe79f1957aa0ef53ef00` | `6fcb6b1b1bf8bb9ccc791bb479e52b41467af073c5cf99f45a64181418f86577` |
| A3 | 643,616 | 380,527 | `b0bd5db2` | `82fe25456dc3e701e2fcf0d88a52466b` | `349f99894e337aba12303f9f483fb59f3b78ebcdc9ef64ae8eb4a59b4a856fda` |

All four files are mono PCM24 WAVE at 44.1 kHz. Each contains one isolated
pluck and needs no trim or format rewrite, so the audit uses the whole raw
half-open frame span.

| String | Exact source frames | Measured pitch | Cents from A4=440 | T60 | Range | Support | Line residual |
|---|---:|---:|---:|---:|---:|---:|---:|
| C2 | `[0,280234)` | 64.8310 Hz | -15.30 | 3.2181 s | 35.034 dB | 1.7460 s | 0.739 dB |
| G2 | `[0,357808)` | 97.6298 Hz | -6.53 | 5.6572 s | 35.141 dB | 2.2449 s | 1.916 dB |
| D3 | `[0,325053)` | 146.2847 Hz | -6.47 | 4.1983 s | 35.189 dB | 1.5964 s | 2.916 dB |
| A3 | `[0,214512)` | 220.7779 Hz | +6.11 | 3.2325 s | 35.036 dB | 1.8358 s | 1.064 dB |

The pitch check used `bounded_pitch.py` SHA-256
`6ca7aadf25e3f4b4787a0e89c156ab5f5125b3e4e04e6fe36769e7826a6d622d`.
The raw-tail check used `passive-decay-v4` from `instrument_fit.py` SHA-256
`f8952be56bb854f5ccbc61a2c90abab11fbecf6b01e5724fdea31df942acbf91`.
Its local public-safe result hashes to
`64d8cbcd58c2ba3074f0b42d89927fed3c86c20537051922dfd2ffa664892556`.
Every tail exceeds the fixed 20 dB range minimum and stays below the 5 dB line
residual maximum. The complete set therefore passes the predeclared input gate
and is admitted only as the held-out audit split.

## Held-out candidate result

The input gate above completed before the frozen seven-value candidate was
rendered against this set. A fresh Linux rerun reproduced the staged scalar
values: C2 1.00 s, D3 0.75 s, A3 1.00 s, and the G2 point with 2.50 s loss,
18 kHz bridge cutoff, 100 Hz peak bandwidth, and 0.02 peak loss. The G2 grid
again selected point 71 and reduced its fit/validation score from 7.6647 to
2.8248.

The joint candidate improved the total and every split mean, but the fixed
per-goal gate rejected it. C2, D3, and G2 exceeded the 2.0 audit loss cap, and
D3 became 1.4271 worse than its baseline, above the 0.25 harm cap.

| Measure | Baseline | Candidate |
|---|---:|---:|
| Total score | 11.9223 | 4.7043 |
| Fit mean | 4.9483 | 1.2586 |
| Validation mean | 3.8438 | 1.0590 |
| Audit mean | 3.1302 | 2.3867 |

| Audit goal | Baseline loss | Candidate loss | T60 ratio | Support ratio | Result |
|---|---:|---:|---:|---:|---|
| A3 | 3.6468 | 0.6183 | 1.2450 | 1.1766 | pass |
| C2 | 3.2065 | 2.6091 | 1.8070 | 1.8143 | fail: loss above 2.0 |
| D3 | 2.1357 | 3.5628 | 0.9168 | 1.3625 | fail: loss above 2.0 and harm above 0.25 |
| G2 | 3.5317 | 2.7567 | 1.4671 | 1.9778 | fail: loss above 2.0 |

All T60 and support ratios passed. All fit and validation objectives passed.
Those aggregate successes do not override one failed held-out goal.
`instrument_fit.py select` therefore exited with status 2 and wrote a checked
failed result; no model profile or profile receipt was written.

| Evidence | SHA-256 |
|---|---|
| G2 experiment | `c94356a2da2914cd45059a848d1909d20c5bd0799cc347c222d05819c45bfddc` |
| G2 selection | `ff2a9321087f87da172784646cb2f2a5dc4e2a40ad10a56c1d28dddaedba57d7` |
| Joint bundle receipt | `43dcc8f74d84c927db73ae96574a484bb5fd9b4f0438da50a870bc8792d27c1e` |
| Frozen candidate profile | `31d99c799d5831e51411c1bf16b966f1f7f16da17391c1792c8c46399e5fb828` |
| Joint experiment | `1619aa361b624a2cb6f436f852fdc1c515cc63b3c1fcb198d5c7d9ff35f5c281` |
| Failed joint selection | `82c2de7a117efa638a23ea6b75a6695964115166cf79bfb88e6fcb222b11615c` |

The frozen candidate is rejected. The fixed profile remains violin-derived.
This held-out set must not now be used to tune replacement parameter values;
a materially changed model needs fit/validation evidence and another untouched
audit before a profile write.

## TinySOL does not contain pizzicato

The [official TinySOL 6.0 record](https://zenodo.org/records/3685367) was the
first choice because its full audio archive is anonymous and CC BY 4.0. The
record says it contains only the ordinary, no-mute technique. A check of the
official metadata found 291 cello rows and no cello pizzicato row.

The official `TinySOL.tar.gz` URL is
[`https://zenodo.org/api/records/3685367/files/TinySOL.tar.gz/content`](https://zenodo.org/api/records/3685367/files/TinySOL.tar.gz/content).
Zenodo reports 1,026,917,185 bytes and MD5
`36030a7fe389da86c3419e5ee48e3b7f`. The metadata has 317,576 bytes, MD5
`a86c9bb115f69e61f2f25872e397fc4a`, and SHA-256
`925727f1036cb3d574856470be6b04af5c9961102902a23cc8eafc7907d040da`.
The metadata alone rules this set out, so the 1 GB audio archive was not
downloaded.

## Public fallback checks

### Philharmonia

The [Philharmonia sound-sample page](https://philharmonia.co.uk/resources/sound-samples/)
says its musicians recorded the files and permits use, including in commercial
work, but bars selling or sharing the sounds as samples. Its official
[`Strings.zip`](https://philharmonia-assets.s3-eu-west-1.amazonaws.com/uploads/2020/02/12112005/Strings.zip)
has 171,467,548 bytes and SHA-256
`19157323ba3c5fe7970a9deadbff00165352996901be72d220319f0ff8e7aee5`.

The archive has only these cello paths containing `pizz`:

```text
Strings/cello/cello_C2_phrase_forte_snap-pizz.mp3
Strings/cello/cello_C2_phrase_fortissimo_pizz-normal.mp3
Strings/cello/cello_C2_phrase_piano_pizz-normal.mp3
```

They are multi-note phrases. The archive has no separate cello pizzicato C2,
G2, D3, and A3 files. It cannot supply four clean, named open-string tails and
was not run through the decay gate.

### Good-sounds on CORA

The official [Good-sounds dataset V1](https://doi.org/10.34810/DATA2314) on
CORA is public and CC BY 4.0. Its unrestricted
[`good-sounds.zip`](https://dataverse.csuc.cat/api/access/datafile/280585) has
2,168,063,959 bytes and official MD5
`bb47a9a675b8af7d6c7d0260ac6f7145`. The official
[`sounds.json`](https://dataverse.csuc.cat/api/access/datafile/280588) has SHA-256
`445db6151e8029b27c598f156f6de9234f61e2342bd2ff105949b3ea074ca19b`
and 948 cello rows.

Pack 22, `cello_margarita_reference`, does name one mezzo-forte reference row
on each open string. Official
[`takes.json`](https://dataverse.csuc.cat/api/access/datafile/280584) maps each
played note to AKG and Neumann mic files:

| String | Sound ID | String ID | AKG archive member | Neumann archive member |
|---|---:|---:|---|---|
| C2 | 1940 | 4 | `sound_files/cello_margarita_reference/akg/0000.wav` | `sound_files/cello_margarita_reference/neumann/0000.wav` |
| G2 | 1947 | 3 | `sound_files/cello_margarita_reference/akg/0007.wav` | `sound_files/cello_margarita_reference/neumann/0007.wav` |
| D3 | 1954 | 2 | `sound_files/cello_margarita_reference/akg/0014.wav` | `sound_files/cello_margarita_reference/neumann/0014.wav` |
| A3 | 1961 | 1 | `sound_files/cello_margarita_reference/akg/0021.wav` | `sound_files/cello_margarita_reference/neumann/0021.wav` |

This is not a pizzicato audit set. The row schema has no articulation field,
and neither the pack nor these rows says pizzicato. Other cello rows name bow
speed and bow position. The four public Neumann preview pages call these only
“single notes”; their IDs are 247467, 247474, 247481, and 247488. A current v4
check of the decoded previews rejected all four as too irregular for one
passive tail. That preview check does not replace a raw-file check.

The current README says A440, while all four rows say `pitch_reference: 442`.
The metadata has no field that proves the files avoided digital pitch work.
Thus CORA supplies exact string IDs, public access, and clear rights, but it
does not prove pizzicato, one passive tail, or unretuned source audio. Do not
use it for this audit. The metadata settled the check, so the 2.17 GB archive
was not downloaded.

### Freesound old-cello pack

The uploader's official
[Old Cello-pizzicato pack](https://freesound.org/people/tim.kahn/packs/2680/)
has separate C2, G2, D3, and A3 labels. Each sound page describes one cello
pizzicato multisample recorded through Zoom H4 built-in mics. Each original is
a 44.1 kHz, stereo, 16-bit AIFF under CC BY 4.0. The source states no A4 or
measured tuning. Original downloads need a Freesound login.

| Note | Sound and original download | Public HQ preview | Preview SHA-256 |
|---|---|---|---|
| C2 | [42245 AIFF](https://freesound.org/people/tim.kahn/sounds/42245/download/42245__timkahn__c_s-cello-c2.aiff) | [MP3](https://cdn.freesound.org/previews/42/42245_7037-hq.mp3) | `1a5fd7149ebb8f65cbc89abba9432074f3f8ba56771e3c9874fbf1e05a12d015` |
| G2 | [42258 AIFF](https://freesound.org/people/tim.kahn/sounds/42258/download/42258__timkahn__c_s-cello-g2.aiff) | [MP3](https://cdn.freesound.org/previews/42/42258_7037-hq.mp3) | `a0c2ae55e6134ce321166f39ac68d5cc1b9dbf146e7c39cd811a89b4b8d755d0` |
| D3 | [42250 AIFF](https://freesound.org/people/tim.kahn/sounds/42250/download/42250__timkahn__c_s-cello-d3.aiff) | [MP3](https://cdn.freesound.org/previews/42/42250_7037-hq.mp3) | `1275ea602cf4fd32e804572c0918bc383cab9689d1d5811dc6cda378e0d90b86` |
| A3 | [42239 AIFF](https://freesound.org/people/tim.kahn/sounds/42239/download/42239__timkahn__c_s-cello-a3.aiff) | [MP3](https://cdn.freesound.org/previews/42/42239_7037-hq.mp3) | `b602bac3a40b6765c44a19184e76ee4f51786c9476c20b05c696ef0587c9e270` |

FFmpeg 8.1.2 decoded the public previews to stereo PCM16 WAVE at 44.1 kHz
without a rate, channel, or gain change. No lead trim was possible: the first
20 ms RMS window already met the attack floor in every file. The work span is
therefore every decoded frame. These bounds preserve the decoded PCM samples,
but MP3 decoding cannot preserve the unavailable source AIFF samples.

| Note | Decoded frames kept | Decoded WAVE SHA-256 |
|---|---:|---|
| C2 | `[0, 261747)` | `fc8c87eaa50dbae6fb98223311fc60b17596166bfca5ccc7254e8f4cb3f47ec5` |
| G2 | `[0, 238574)` | `a426fa447557a2f23ebd6de7ad130aaf6869ba9435810c21810f7ad8410616ed` |
| D3 | `[0, 194491)` | `0ec4780d65f3710f1d461ae45a4e6c2c680bc77405a45ce444506185d4628099` |
| A3 | `[0, 140310)` | `f138caeaa78eca548c0a25d3f1f616ce9f93075f68e1fdcfca577179dfea3302` |

The public previews all pass the current v4 input gate. This replay used
20 ms RMS windows at 5 ms hops, started 40 ms after the first peak, stopped at
35 dB of decay, required at least 20 dB of range, and allowed at most 5 dB of
line residual.

| Note | T60 | Range | Support | Line residual | v4 result |
|---|---:|---:|---:|---:|---|
| C2 | 5.8139 s | 35.026 dB | 2.8186 s | 2.230 dB | pass |
| G2 | 12.4776 s | 34.754 dB | 5.2780 s | 4.547 dB | pass |
| D3 | 6.8397 s | 35.139 dB | 3.1429 s | 2.067 dB | pass |
| A3 | 3.5412 s | 35.332 dB | 1.9306 s | 0.573 dB | pass |

A bounded preview pitch check measured C2 at 63.1977 Hz (-59.47 cents from
its label), G2 at 96.9947 Hz (-17.83 cents), and D3 at 145.0064 Hz (-21.66
cents). The A3 search stuck at its search edge and gave no valid A3 measure.
These checks do not prove the pitch of the source AIFF files.

The lossy previews, missing raw hashes, missing tuning claim, failed A3 pitch
check, and prior analysis rule this pack out as an untouched audit set. Keep
its v4 passes only as decay-gate diagnostics.

## Decision

The authorized official archive supplied all four raw files, and the completed
joint audit rejected the frozen candidate. This OrchideaSOL set is now consumed
as held-out evidence and cannot be reused to tune or audit a replacement.
TinySOL and Philharmonia lack the needed recordings. CORA does not name
pizzicato or prove that its files are unretuned. The Freesound previews show
suitable decay shapes but do not meet the raw, tuning, or untouched-source
rules. No public, anonymous-download set checked here meets every condition.
