# Cello third passive-audit source receipt

Checked on 2026-08-31. This first search found no complete third set. A later
check found the new 2026 RWC online release and ran two real joint audits; see
[the replacement receipt](cello-alternative-passive-audit-sources.md). This
note still pins four OrchideaSOL files as a later source, but the official
audio download needs an IRCAM Forum login. That gate blocks local raw hashes,
archive-member checks, frame counts, exact whole-file spans, pitch checks, and
`passive-decay-v4` results.

Do not use these four files for fitting or rank changes. Keep the archive,
source WAVE files, work files, and analyzer output outside Git. The fixed model
has not changed.

## Pinned OrchideaSOL set

The [official OrchideaSOL 2.0 record](https://zenodo.org/records/3740399)
describes 13,265 isolated notes recorded at IRCAM from 1996 through 1999. It
states that the files are mono 16-bit WAVE at 44.1 kHz and last two to ten
seconds. Zenodo hosts the metadata but not the audio.

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

Both URLs redirected to IRCAM sign-in when checked without a session. The API
reported `authorized_download: false`. The four metadata paths above are
exact, but the archive member prefix cannot be checked without the ZIP. Do not
guess that prefix in a manifest.

Zenodo says that the audio uses the
[IRCAM Forum License](https://forum.ircam.fr/legal/contrat-de-licence-forum-ircam/),
while the metadata uses CC BY 4.0. The current Forum terms give an individual
free account personal use of free items. The base grant is personal,
non-transferable, non-exclusive, and non-commercial; commercial use needs
other terms. The terms also limit copying. This receipt therefore does not
treat the audio as redistributable. Each user must accept the IRCAM terms and
keep the audio outside Git.

### Blocked audit receipt

| String | Raw WAVE SHA-256 | Exact source span | Work SHA-256 | v4 result |
|---|---|---|---|---|
| C2 | blocked by sign-in | frame count not read | not made | not run |
| G2 | blocked by sign-in | frame count not read | not made | not run |
| D3 | blocked by sign-in | frame count not read | not made | not run |
| A3 | blocked by sign-in | frame count not read | not made | not run |

Once an authorized user supplies one official ZIP outside Git:

1. Check its byte count and MD5 against the matching asset above, then record
   its SHA-256.
2. List the ZIP and bind the real archive members to the four exact metadata
   paths. Reject a missing or duplicate member.
3. Extract only those members outside Git. Check that each is mono PCM16 WAVE
   at 44.1 kHz, record its byte and frame counts, and record its SHA-256.
4. Prefer no trim. If each file contains one isolated pluck as the record says,
   use the exact half-open span `[0, source_frame_count)`. A format-only WAVE
   rewrite must preserve every PCM sample. Record its SHA-256. Reject unrelated
   sound or a second pluck instead of editing the file until it passes.
5. Run the bounded pitch check around C2, G2, D3, and A3. Report measured pitch
   and cents from A4=440; do not resample or correct pitch.
6. Run the current `passive-decay-v4` check and record the fit tool's SHA-256.
   Keep its 20 dB minimum decay range and 5 dB maximum line residual. Do not
   raise a limit.
7. Admit the set only if all four raw tails pass. Keep it in the audit split and
   do not use its result to pick new parameter values.

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

Pin the four OrchideaSOL metadata rows for the third audit, but keep the audit
gate closed until an authorized official archive supplies all four raw files.
TinySOL and Philharmonia lack the needed recordings. CORA does not name
pizzicato or prove that its files are unretuned. The Freesound previews show
suitable decay shapes but do not meet the raw, tuning, or untouched-source
rules. No public, anonymous-download set checked here meets every condition.
