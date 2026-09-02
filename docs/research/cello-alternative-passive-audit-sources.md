# Alternative cello passive-audit sources

Checked on 2026-08-31. The RWC Instruments Database supplies three cello
maker/player variations for the passive-decay work. Its 2026 open release
postdates the earlier source search. Variation 2 supplied the G2 fit note,
variation 3 supplied its validation note, and variation 1 supplied the first
four-string audit. Those uses exhaust RWC's three cello players and
instruments. RWC has no fourth variation for a fresh audit.

Keep the 14.2 GB archive, extracted source WAVE files, work WAVE files, and all
analyzer output outside Git. Fix each split before comparing it to a model.
Do not use the variation 1 final audit to choose a parameter value.

## Fresh-audit decision after the bridge-peak pass

No checked source now supplies a scientifically independent, admissible
four-open-string audit. Do not compare the new bridge-peak candidate with a
new recording yet.

- RWC has no unused maker/player variation. Changing dynamic within variation
  1, 2, or 3 changes the recording but reuses a player, instrument, and likely
  session that has already influenced this work.
- The Internet Cello Society has a separate public four-file set. Its C2, G2,
  and D3 files pass the raw-tail gate after an exact 8-to-16-bit format
  conversion, but A3 fails. The source also names no player, instrument,
  session, A4 reference, or pitch-work policy.
- OrchideaSOL remains the best named independent set. Its exact metadata rows
  say natural, unretuned open-string pizzicato, but the official raw audio
  still needs an IRCAM account. Raw hashes and frame spans cannot be pinned
  without that archive.
- The Freesound Old Cello pack is another separate set with clear CC BY 4.0
  terms. Its four original AIFF downloads still need a Freesound login. The
  public MP3 previews cannot supply source hashes or lossless spans.

An authorized OrchideaSOL archive or the four logged-in Freesound originals
would reopen this step. Keep either set outside Git, pin every raw hash and
whole-file frame span, and run the current input gate before exposing the
candidate to it.

### Why another RWC dynamic is not independent

The checked `01_instruments.csv` has only cello rows 171, 172, and 173. The
checked `02_instruments_details_en.csv` assigns each one forte, mezzo, and
piano pizzicato members. The [official legacy table](https://staff.aist.go.jp/m.goto/RWC-MDB/rwc-mdb-i-all.html)
shows the same nine members.

| Variation | Pizzicato members | Prior role | Fresh-audit result |
|---|---|---|---|
| 1 | `171VCPZF.WAV`, `171VCPZM.WAV`, `171VCPZP.WAV` | Forte supplied the failed final audit | Mezzo or piano would reuse the same maker/player variation and observed session. |
| 2 | `172VCPZF.WAV`, `172VCPZM.WAV`, `172VCPZP.WAV` | Mezzo supplied fit; forte supplied a same-player check | No member is independent of fit. |
| 3 | `173VCPZF.WAV`, `173VCPZM.WAV`, `173VCPZP.WAV` | Mezzo and forte supplied validation | No member is independent of validation. |

Variation 1 mezzo is a fully pinned correlated fallback, not a final audit.
It comes from the same official archive and license listed below. Do not run
it against the bridge-peak candidate as an untouched test.

| Item | Value |
|---|---|
| Exact member | `RWC-I/171/171VCPZM.WAV` |
| Member bytes | 15,421,644 |
| Compressed bytes | 4,924,670 |
| ZIP CRC-32 | `ff1f47ad` |
| Member MD5 | `13a777e24a30bcd00392286e85ed0593` |
| Member SHA-256 | `ddce0929ebd69dd3bee516d8e560341f528c1026faae2bc8378012d01a1d8a0c` |
| Audio | mono PCM16 WAVE, 44.1 kHz, 7,710,800 frames |

The following exact source slices and work hashes finish its receipt without
making it independent. A decoded-sample comparison found each work file as
one exact and unique source slice.

| String | Exact source frames `[start,end)` | Work frames | Work SHA-256 |
|---|---:|---:|---|
| C2 | `[0,127890)` | 127,890 | `f5569228a8a6a8125266a6144e14d06f35f7574a39a36350221650f5a28b2a76` |
| G2 | `[1697850,1865430)` | 167,580 | `b06d6b51bded3a620b56409d01adbc8f27c713267d42508189d41a01e70ca19a` |
| D3 | `[3272220,3435390)` | 163,170 | `291e11b1d5036be6c8f0297158f6cb84732c4a7eca748df8f56722ab41c9a8b6` |
| A3 | `[4833360,4983300)` | 149,940 | `ad09a92e6fd0f0de18f87de1091d5bb8a3f5e35568f754d135edb000d3ca7cf2` |

### Internet Cello Society four-file check

The [Cello Open Strings page](https://www.cello.org/Cello_Introduction/B.html)
links separate C, G, D, and A string pizzicato WAVE files. It labels every
string and the articulation. The page identifies an ICS collection and
carries a 1995 copyright; the files also differ from RWC in format, length,
and content. That is strong evidence of a separate published source, but it
does not prove a different player or cello. The page does not identify its
player, instrument, recording date, or session.

The [ICS copyright terms](https://www.cello.org/Copyright.html) allow only
individual, non-commercial use and prohibit unapproved duplication,
distribution, modification, and commercial use. This is not an open-data
license. A user may check the files locally only when that use falls within
the personal-use grant. Never add the audio or converted work files to Git.

The URLs below returned the named source bytes without login. The server ETag
for each file equals its checked MD5. Each file is mono unsigned 8-bit PCM
WAVE at 22,050 Hz. Each source already holds one pluck, so its lossless source
span is the full half-open frame range.

| String | Direct source | Bytes | Source frames `[start,end)` | MD5 | SHA-256 |
|---|---|---:|---:|---|---|
| C2 | [`Cstringpizz.wav`](https://www.cello.org/Sounds/Cstringpizz.wav) | 24,492 | `[0,24448)` | `97a25f4d26d9a84c67ec5b251e8b3df7` | `091a2cb63b2da107cb1588625462a63d2d6d86a061ce7885ea9c10c752af8c35` |
| G2 | [`Gstringpizz.wav`](https://www.cello.org/Sounds/Gstringpizz.wav) | 29,996 | `[0,29952)` | `cd9716024d33011cdb95abe4e18db622` | `e1a69afb410511b0f3a48649f83db599deb12015edfe00a590a49bafdebcb8e9` |
| D3 | [`Dstringpizz.wav`](https://www.cello.org/Sounds/Dstringpizz.wav) | 26,924 | `[0,26880)` | `1092e73b8e4abc8d6ece1a76d5c0958f` | `90c0b1f35811ec9bc79352363e11a2829fe32ae48f5962d16f37aa1b1f2d0784` |
| A3 | [`Astringpizz.wav`](https://www.cello.org/Sounds/Astringpizz.wav) | 24,940 | `[0,24896)` | `d521da98e00b996ee5be30b15309695b` | `3914dc4a63b29fce3bf9a906a8c5d43e8d2e91467e4033acc64d7fe6ae9dcd1d` |

`passive-decay-v4` reads only signed 16- or 24-bit PCM. FFmpeg 8.1.2 made a
format-only PCM16 WAVE for each file without changing sample rate, channels,
frame count, or timing. A sample-by-sample check confirmed that every signed
work sample equals `(source_unsigned_8_bit - 128) * 256`. That mapping is
one-to-one and adds no signal data.

| String | PCM16 work SHA-256 | T60 | Range | Support | Line residual | v4 result |
|---|---|---:|---:|---:|---:|---|
| C2 | `7c791c7add230c78fc35af65a2eb1b16bfa7ea7268ee417388d0b28f1ff2ffc7` | 3.7596 s | 22.213 dB | 0.9828 s | 2.286 dB | pass |
| G2 | `d4a514336c1f91bd753f946b828ea58e07b3f446666c45d755f77cb626c5ac68` | 4.4668 s | 23.263 dB | 1.2472 s | 2.237 dB | pass |
| D3 | `518e692fe26d4b41a6d80ebe9eaa17c6f2ab5df1a4ab5cf64eedf175d5991461` | 2.7031 s | 22.900 dB | 1.1125 s | 2.952 dB | pass |
| A3 | `c2a0c54660ef25f95fece8f5da0dd9db91977cf2b691caefd7789cb2bb2d5f5b` | -- | -- | -- | -- | fail: noisy or under 20 dB range |

This replay used `passive-decay-v4` from `tools/instrument_fit.py` SHA-256
`f8952be56bb854f5ccbc61a2c90abab11fbecf6b01e5724fdea31df942acbf91`.
Do not trim or relax the gate to admit A3. Because one of four tails fails,
the ICS set cannot audit the candidate.

## Earlier decision: use the 2026 RWC release

The [official RWC Instruments record](https://zenodo.org/records/17170844) was
published on 2026-02-01. It re-releases the original RWC-MDB-I-2001 instrument
sounds under CC BY-NC 4.0. The record says that each variation normally uses a
different maker and professional player. It also says that string sounds cover
each string, that notes occur in ascending half-step order, and that mute gaps
separate the notes.

The official metadata names cello variation 2, `PIZZICATO`, mezzo dynamic,
pitch range C2 through F5, and member `172VCPZM.WAV`. The legacy
[official instrument table](https://staff.aist.go.jp/m.goto/RWC-MDB/rwc-mdb-i-all.html)
gives the same row. The archive's cover identifies `PZ` as pizzicato, `M` as
mezzo, and variation 2 as one maker/player variation. This is a different
source and performance from both Iowa sessions.

### Published archive and exact member

| Item | Value |
|---|---|
| Record | `https://zenodo.org/records/17170844` |
| Direct archive | `https://zenodo.org/api/records/17170844/files/RWC-I.zip/content` |
| Archive name | `RWC-I.zip` |
| Published bytes | 14,173,869,024 |
| Published MD5 | `fb5789335fe68abdc09929618e9f0403` |
| License | CC BY-NC 4.0; credit the RWC authors and release |
| Exact member | `RWC-I/172/172VCPZM.WAV` |
| Member bytes | 14,218,238 |
| Member ZIP CRC-32 | `3b2e1101` |
| Member MD5 | `df1dd02ab463e6cfd458d970b96b6e83` |
| Member SHA-256 | `14594b4693b48481435cf280c15b2718d14e6d459e586345ffbfda187d67c66f` |
| Audio | mono PCM16 WAVE, 44.1 kHz, 7,109,097 frames |

Zenodo publishes the archive MD5, but not the member hashes. The member MD5,
SHA-256, CRC-32, size, and audio facts above were checked after an anonymous
ranged read of the official archive. Python `remotezip` can extract only this
member, so a user need not fetch the full archive. A full download and ordinary
ZIP extraction must produce the same member bytes.

The archive also contains `RWC-I/01_instruments.csv` and
`RWC-I/02_instruments_details_en.csv`. Their checked SHA-256 values are
`b8780ee6d3ae8595b4b8e321436eb17af661a2c349ae4ebce6b3accda5d3e5f6`
and `68360cd8cfae92166b5958c321a1e2058006c287fafaec7d41fdfdb4d788d317`.
The second file contains the exact cello row.

### Lossless four-note split

The source contains 60 plucks. The four open strings begin four ascending
string groups. The source-span limits below fall inside the mute gaps. FFmpeg
8.1.2 copied each PCM span to a new PCM16 WAVE without a rate, channel, gain,
or sample change. A byte comparison of decoded PCM confirmed that every output
sample equals the named source slice.

| String | Exact source frames `[start,end)` | Work frames | Work bytes | Work SHA-256 |
|---|---:|---:|---:|---|
| C2 | `[0,110250)` | 110,250 | 220,578 | `6eead97777e4f7d78d02bcc7dc493e22f98451682521b5e5fec1292572362eb1` |
| G2 | `[1587600,1728720)` | 141,120 | 282,318 | `e7cc4024989597cb8bd1a3127d20fa6481c96403352cf75570b11efbac5853b3` |
| D3 | `[3175200,3307500)` | 132,300 | 264,678 | `38841a28803e1d38283686678d9467cffc49ddbaffd0dde54907d203abc6d43a` |
| A3 | `[4740750,4868640)` | 127,890 | 255,858 | `308cb7dd2fca7a0a7f307687f20e152d2cd2936bf405457edb29d449b17e619d` |

Use `atrim=start_sample=START:end_sample=END,asetpts=PTS-STARTPTS` and
`-c:a pcm_s16le` for each row. Write the outputs outside both repositories.
Reject the source if its SHA-256 differs before splitting. Record the exact
FFmpeg version and confirm sample equality after the rewrite.

Use these manifest IDs for all four rows:

- `source_id`: `rwc-i-2026-17170844-172vcpzm`
- `performance_id`: `rwc-cello-variation2-mezzo-pizzicato`

The four work hashes differ, so the joint adapter accepts them as distinct
audit inputs even though they name one checked source sequence.

### Pitch and decay checks

The bounded pitch check used A4=440 targets only as test bounds. It did not
retune, resample, or change the work files.

| String | Measured pitch | Cents from target | A4 equivalent |
|---|---:|---:|---:|
| C2 | 64.3150 Hz | -29.13 | 432.66 Hz |
| G2 | 96.9577 Hz | -18.49 | 435.33 Hz |
| D3 | 144.7113 Hz | -25.19 | 433.64 Hz |
| A3 | 219.1000 Hz | -7.10 | 438.20 Hz |

All four raw tails pass the fixed `passive-decay-v4` limits: at least 20 dB of
range and no more than 5 dB line residual. The fit tool SHA-256 for this replay
was `84fcde397de6ec44e7438e7b6b2f081f5da9af8d76dd1e2ab02d486f833e84a9`.

| String | T60 | Range | Support | Line residual | Result |
|---|---:|---:|---:|---:|---|
| C2 | 5.1868 s | 39.760 dB | 1.7111 s | 1.502 dB | pass |
| G2 | 5.8686 s | 44.466 dB | 1.6213 s | 3.630 dB | pass |
| D3 | 3.7071 s | 35.179 dB | 1.6263 s | 3.474 dB | pass |
| A3 | 3.4014 s | 35.008 dB | 1.5166 s | 2.386 dB | pass |

RWC does not publish an A4 reference or an explicit statement that it avoided
digital pitch correction. Do not invent either claim. The release describes
these as the original 2001 DVD-ROM WAVE recordings, and the measured offsets
show that the open notes do not sit exactly on A4=440 targets. That is evidence,
not proof, of untouched pitch. The current passive-decay audit does not require
an exact A4 or use pitch to rank the loss values. If the project adds a strict
published no-retune requirement, this set must remain blocked until RWC's
owners supply that statement.

## Other primary-source checks

| Source | Current result |
|---|---|
| [OrchideaSOL](https://zenodo.org/records/3740399) | Its metadata gives exact natural, no-retune open-string pizzicato rows. The official 8.37 GB audio asset still redirects anonymous users to IRCAM sign-in. |
| [FullSOL and Studio On Line](https://forum.ircam.fr/projects/detail/fullsol/) | IRCAM Forum still controls the audio download. FullSOL needs an active premium account; it is not an anonymous fallback. |
| [University of Iowa](https://theremin.music.uiowa.edu/MIScello2012.html) | It has exact open-string pizzicato audio, but the 2012 files already form the fit split and the 2001 files already form the validation split. Reusing either is not a third audit. |
| [Freesound Old Cello-pizzicato](https://freesound.org/people/tim.kahn/packs/2680/) | It names C2, G2, D3, and A3 pizzicato AIFF files, but original downloads still require login. Anonymous files are lossy MP3 previews, and the source gives no tuning or no-retune statement. |
| [Internet Cello Society](https://www.cello.org/Cello_Introduction/B.html) | It has four direct PCM WAVE files under personal-use-only terms. C2, G2, and D3 pass v4 after an exact bit-depth conversion; A3 fails the fixed input gate. The page names no player, cello, session, tuning, or pitch-work policy. |
| [Philharmonia samples](https://philharmonia.co.uk/resources/sound-samples/) | The official strings archive has only three cello pizzicato files, all multi-note C2 phrases. It has no separate four-string set. |
| [Good-sounds](https://doi.org/10.34810/DATA2314) | Its public archive names all four open strings, but the rows do not name pizzicato and do not state that the recordings avoided pitch work. The public previews also failed the fixed tail-shape check. |
| [TinySOL](https://zenodo.org/records/3685367) | Its open CC BY archive has no cello pizzicato row. |
| [McGill MUMS](https://www.mcgill.ca/music/resources/mums/html/index.htm) | The former official MUMS URL now returns 404. No current first-party anonymous audio archive or rights grant was found. |
| [Karoryfer/Bigcat cello](https://github.com/sfzinstruments/karoryfer-bigcat.cello) | This CC0 sampler library has direct plucked WAVE samples only on A, C, E-flat, and G-flat roots. It has no raw G2 or D3 member and does not identify four open strings. |
| [VCSL](https://github.com/sgossner/VCSL) | This CC0 sampler project permits contributed recordings to be tuned, equalized, or noise-reduced. Its current first-party tree exposes no solo-cello four-open-string pizzicato set. |
| [CC0 plucked-cello samples](https://github.com/cleary/samples-cello-plucked) | This pack says its recordings are tuned to C with several attacks and octaves. It does not supply C2, G2, D3, and A3 open-string IDs. |
| [Single Sound Clarity](https://zenodo.org/records/30599) | It includes a plucked-cello programme item, not four files with pitch and string IDs. Its record also has no machine-readable license. |
| [Vibrato Dataset](https://zenodo.org/records/1016050) | Its violoncello subset covers vibrato and non-vibrato tones, not pizzicato open strings. |
| [Cello bow-kinematics data](https://zenodo.org/records/10696680) | Its score uses open strings, but the recordings compare human and robot bowing. They are not passive pizzicato tails. |

## Joint checks

The predeclared joint gate ran with the four variation 2 work files. All 16
Release renders passed their file checks. The fixed candidate cut mean audit
loss from 5.0570 to 1.5961, but two audit curves missed the fixed 2.0 loss
cap: D3 scored 2.1284 and G2 scored 2.7073. The older Iowa G2 validation row
also failed its T60 and support ratios. The selector wrote a failed result and
no model file was written.

| Joint item | SHA-256 |
|---|---|
| Bundle receipt | `8a43e4e53395c46c537e058c613d5a8afa48429b65ea2433c68023e1d7dc0e07` |
| Experiment result | `845e6339f6def3de835f5dc8c5ebce13458b1a2afe482f83d6c2249eda9730ec` |
| Failed selection | `bef91ec5ba7016f48d702bdfd3147e052e3aaf39d6c442586d3d9e315bd63457` |

The same archive also has cello variation 3. A second audit used mezzo C2,
G2, and D3 from `173VCPZM.WAV`, plus forte A3 from `173VCPZF.WAV` because
the mezzo A3 tail failed the fixed raw-tail check. This mixed-dynamic set is
not as clean a design as variation 2, but every fixed candidate audit row
passed. Mean audit loss fell from 5.5081 to 1.3194. The full candidate still
failed only the older Iowa G2 validation ratios, so this run also wrote no
model.

| Member | Bytes | MD5 | SHA-256 |
|---|---:|---|---|
| `RWC-I/173/173VCPZM.WAV` | 16,525,752 | `f006747ec511087cd4dfb74d9763fcd9` | `5f94d982e14e2b1bbdca14f6b48c33fc551ec087a0fb02b88aa0ada737828149` |
| `RWC-I/173/173VCPZF.WAV` | 16,480,592 | `8021653e06f3247ffa2d85ad5861d7eb` | `f9acb70ecce66f3a66d0b9407504fa778d9902c3dc8d9b8dcc1ca95e891bc237` |

| String | Dynamic | Exact source frames `[start,end)` | Work SHA-256 | Audit loss |
|---|---|---:|---|---:|
| C2 | mezzo | `[61890,217205)` | `80204e2c13a9d9380e8fdb3abf8c374d3eaa5d25a051381f79df10507814b273` | 1.3424 |
| G2 | mezzo | `[1916159,2064287)` | `90081ed3a28788e2c7d0e004e2654b895ce2cc01a3fb84120a718005fdbab3c0` | 0.6592 |
| D3 | mezzo | `[3788974,3933816)` | `78665e1ac6f4f18155f4e04717940c07322fe42a78670c0c3c7762df24d400bd` | 1.4973 |
| A3 | forte | `[5440160,5583940)` | `f8a642b5f4d3b787913b19126d8fd0a5eb5ccc45c2daee46e34e94df8af38176` | 1.7790 |

The variation 3 work files measured C2 64.5517 Hz, G2 97.5819 Hz, D3
145.2142 Hz, and A3 218.3931 Hz. Their raw T60 values were 6.9058, 4.4240,
5.3074, and 4.5549 seconds. Decoded work samples matched the named source
spans exactly.

| Joint item | SHA-256 |
|---|---|
| Bundle receipt | `68f28876366d638a98f52441feaae5db693f761de020edd649f6cae05cd63fd2` |
| Experiment result | `4906f1f9b55618432a962e05a13d17d08e8ba86b90013540c87243ef82268b93` |
| Failed selection | `4d866a670a2b3da78b1e86e941d8fd0af17a5e2cf5ae95365fff465186d1d1ae` |

## G2 refinement result

A new G2-only grid added 1.20, 1.25, 1.30, 1.35, 1.40, and 1.45 seconds
between the old 1.00 and 1.50 points. It rendered 26 real model jobs. No
point met all fixed fit and validation rules. At 1.35 seconds the fit loss was
1.9301, but the Iowa validation T60 and support ratios were 0.4884 and
0.4981. At 1.40 seconds those ratios passed at 0.5021 and 0.5071, but fit
loss rose to 2.0362, just above the 2.0 cap. The ordinary combined score still
selected 1.00 second.

| Refine item | SHA-256 |
|---|---|
| Bundle receipt | `1069bc0e23b840ea134ab67b0b9c7a006619811ef16afe380187d92ebd90bb55` |
| Experiment result | `e53206ec5a3a86bc9ecb79e24024c7f2e65e1b0ee4b77297d409941c758fd4e1` |
| Selection | `a7a71957128be14fcbc6187b707b536be3f007146a76b1a35009778b265aa931` |

Do not weaken the gate or copy any provisional value into the fixed model.

## RWC G2 fit and validation split

The same variation 2 sequence also has forte and piano members. They were
fetched by ranged reads before choosing the later split.

| Member | Bytes | Compressed bytes | ZIP CRC-32 | SHA-256 |
|---|---:|---:|---|---|
| `RWC-I/172/172VCPZF.WAV` | 15,734,562 | 5,874,918 | `6306bff0` | `e2348ea8ecfe393edc8eebe9c83fa6093dbe03f4ccfae6fbecb152a56876f283` |
| `RWC-I/172/172VCPZP.WAV` | 16,215,242 | 3,555,864 | `5b3134e0` | `457547a7664e20ee185ead4a509f8bac655a314b4a4117298b0e52b5342c62e9` |

The forte G2 slice keeps source frames `[1720252,1878778)`. Its 158,526
samples match the source exactly and hash to
`d36fef14862d6aff0e413026dc066144d1959cde9d22fa6f58bdbd049c96c8b0`.
It measures 97.5691 Hz, or -7.61 cents from G2, and passes the v4 tail gate
with 4.8138-second T60 and 3.980 dB residual. It serves as a same-player
replication check, not the final validation source.

The piano G2 slice keeps `[1843156,1966627)`. Its 123,471 exact samples hash
to `0ea90bb78fd5e358e820bbadaef9ddec7a38ee17116ce4873da3b4d55d10a1aa`.
It passes the tail gate, but bounded pitch measures 102.8267 Hz, or +83.25
cents from G2. It is not an open-G validation note and was excluded before
model comparison.

The final G2 scalar fit uses variation 2 mezzo as fit and variation 3 mezzo as
validation. These name different RWC maker/player variations. The renderer
uses the fit note's measured 96.9577320733 Hz pitch and 435.3254981715 Hz A4
equivalent. The 19-point grid covers 0.08 through 3.0 seconds and gives 38
Release renders. Scalar eligibility now applies the same absolute rules as
the joint gate: loss at most 2.0, T60 ratio from 0.5 through 2.0, and support
ratio at least 0.5 for every fit and validation goal.

The unconstrained score preferred 1.20 seconds, but its fit loss was 2.3248,
so it failed the fixed loss cap. Values through 1.45 seconds failed the same
cap. The first passing minimum was 1.50 seconds: fit loss 1.9923, validation
loss 1.3101, and score 3.3024. The next point, 1.60 seconds, also passed but
scored 3.3358. This fixed G2 at 1.50 seconds before the final audit.

| G2 item | SHA-256 |
|---|---|
| Bundle receipt | `1c5e3767f67d352a8116cff40e67abef90e8197186e1a38491007ea8f9495b1e` |
| Experiment result | `9f8773f46bb288362967f6f23cfecaa483225d1008430a7491c931d94d08abff` |
| Selection | `4eae4a639c18e4eb0f74ca4cf24588ea2fbd0faaf7036a1929ea59a71500a60d` |

## Variation 1 forte final audit

The candidate was fixed at C2 1.00, G2 1.50, D3 0.75, and A3 1.00 seconds
before fetching `RWC-I/171/171VCPZF.WAV`. The official member has 15,356,306
bytes, ZIP CRC-32 `e8c787b9`, and SHA-256
`82456d69ca81235e1ba5fab75edf61aa44aaed164474acad8db08aa90db8acd3`.
It is mono PCM16 at 44.1 kHz. The four work files preserve every named source
sample.

| String | Exact source frames `[start,end)` | Work frames | Work SHA-256 |
|---|---:|---:|---|
| C2 | `[0,119685)` | 119,685 | `a0afae6f024af19e0850a77a2780794fea1d58730994d3f4813dae6841788994` |
| G2 | `[1631492,1782122)` | 150,630 | `6e04ec595c079820ebbd5d2429a85d20aad08a1aab2230a3bd24979ec19c969e` |
| D3 | `[3233775,3388626)` | 154,851 | `8f5fadd8d4143b315ea1ccaf961402059b99e238d1b4b96040bb292ac2f54a5c` |
| A3 | `[4817735,4953276)` | 135,541 | `95584bf06ca16fc2c14c14e51c74fb8413ccba9c940e28783eef17e0c0c21844` |

All four raw tails pass v4 before comparison. Measured pitches are C2
64.5780 Hz (-22.07 cents), G2 97.1869 Hz (-14.40), D3 146.0007 Hz (-9.83),
and A3 219.6922 Hz (-2.42). Raw T60 values are 5.4234, 2.8394, 5.4456, and
4.6673 seconds. Line residuals are 2.601, 2.903, 2.787, and 3.164 dB.

The 16-job joint run failed, so no profile was written. C2 and A3 passed.
D3 curve error was 7.5126 dB, above the 6 dB cap. G2 curve error was 9.9005
dB, its model/reference T60 ratio was 2.3385, and the candidate was worse than
the baseline on that audit goal. The split means and total score improved, but
one failed goal is enough to reject the vector.

| Final audit item | SHA-256 |
|---|---|
| Joint bundle receipt | `8858d1504351c3340b2112037c76e2a03587dd4619620316172a4525b5e3ca34` |
| Candidate profile in rejected bundle | `ce6099bd26b046612845250c62eae2b4155838c01a152223d7e92c5555105b14` |
| Experiment result | `d68d9b0a1db8ada18d074fa2f30ba0923471de237707159249dd6275e806972e` |
| Failed selection | `f89292ea9270786813eca9e9b7e7facf05df1fb795d6d712f6152a77ceef6449` |

One loss time per string does not cover these players and dynamics. This
failure prompted the later frequency-shaped loss and bridge-peak work. Once a
candidate passes its fit and validation gates, the current blocker is the
fresh audit stated at the top of this note. Do not substitute another RWC
dynamic for that audit. OrchideaSOL remains the first choice if an authorized
archive becomes available.
