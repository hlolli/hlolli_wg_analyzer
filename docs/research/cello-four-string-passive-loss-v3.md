# Cello four-string passive-loss sweep

Run on 2026-08-28 and checked again on 2026-08-31. This report records one
scalar passive-loss sweep for each open cello string. Raw recordings, work
WAVE files, bundles, rendered WAVE files, and selection JSON stay outside Git.

The original Iowa sweep produced this provisional vector:

| String | Selected loss time | Fixed-model value |
|---|---:|---:|
| C2 | 1.00 s | 0.25 s |
| G2 | 1.00 s | 0.25 s |
| D3 | 0.75 s | 0.25 s |
| A3 | 1.00 s | 0.25 s |

These values have not been merged or written to the fixed cello model. The
fixed model remains `violin-derived`. The four validation takes affected the
rank, so they are not a final untouched audit set.

The later RWC-backed G2 fit changed only G2, from 1.00 to 1.50 seconds. Its
final four-string candidate was C2 1.00, G2 1.50, D3 0.75, and A3 1.00
seconds. A new RWC variation-1 audit rejected that vector, so none of these
values has entered the fixed model.

## Inputs

The fit notes come from the University of Iowa 2012 single-pitch session. The
validation notes come from the first open-string note in the Iowa 2001 range
files. The sessions use different players, cellos, mics, and channel layouts.
The [source and trim receipt](cello-gda-passive-loss-sources.md) records the
official pages, raw files, rights, formats, and exact sample spans for G2, D3,
and A3. The [C2 adapter receipt](../../adapters/hlolli_wg_cello/README.md)
records the same facts for C2.

| String | Fit input SHA-256 | Validation input SHA-256 | Fit T60 | Validation T60 |
|---|---|---|---:|---:|
| C2 | `94dbd2259de5162cc16a4d5a8d9ef54590cdf7d3a9b3a37276c4483292714daa` | `f61a03f01cf32cb8c60d2e9b3a98011deaace77f2fcd8b122530fc5f92f73d2a` | 5.2464 s | 8.4415 s |
| G2 | `d0ed42947553acff74b795da3316e80731375bde08f5023a8734cbe22c23a72e` | `d5eb0f46d53790bfe3a14b23befeb0202e9c41f757caa96bbc054f2ef3118f36` | 5.2855 s | 12.4263 s |
| D3 | `d27fbe2e24317c694a9997c4045ce004e2aa37ed5e4ffd96904ea3a14a32c40e` | `2be3c37bbe67fc911302b0a0edfc9cb42df540e7a2943243ae331781650d60df` | 3.7397 s | 12.4340 s |
| A3 | `c60aecbe28fbdc2c7c837ffdb542d10473fcf2483f45ce0c9ae54ab8f5a23e02` | `2c36a4366ddf86a2c7da7ae1eb7ff03a00b65dd14e954b3e4cd77ccc8d404148` | 7.8205 s | 4.3863 s |

The G2 validation source has 16 full-scale samples at its attack. The decay
measure starts after the attack and finds one clean tail, but this clipping
still limits the result. None of the files states pluck force, pluck position,
string make, or session A4.

## Checked method

Each target uses one seven-level scalar experiment at 0.08, 0.125, 0.25,
0.50, 0.75, 1.00, and 1.50 seconds. One fit case and one validation case give
14 jobs per string and 56 jobs in all. This avoids a four-parameter
one-at-a-time result that could select only one changed value.

The generic probe renders the named open string at its measured 2012 pitch.
It holds the pizzicato gate high, leaves the body renderer off, and prevents
note-off damping from hiding passive string loss. Each model file is 12
seconds, stereo PCM24, 44.1 kHz, and 529,200 frames.

`passive-decay-v3`:

- finds the attack with 20 ms RMS windows at 5 ms hops;
- starts the tail 40 ms after the attack peak;
- rejects a second onset, a non-decay, less than 20 dB of range, or more than
  5 dB of straight-line residual;
- removes initial gain and compares the whole reference support; and
- holds a model at its checked final level if its support ends first, so a
  fast decay cannot avoid the late reference tail.

The v3 test set includes a clean two-rate string tail that exceeds the old
2.5 dB residual limit, a wide-range irregular tail that exceeds the new 5 dB
limit, and a rank check where a short fast candidate must lose to a closer
longer candidate. The longer probe also covers the 6.72-second G2 and
5.52-second D3 validation supports.

The Stage 8 whole-file RMS and band gaps reach their cap and stay flat. They
serve only as process checks here. Selection uses the gain-independent decay
curves from each raw model WAVE.

## Full score table

Loss is decay-curve RMSE divided by the manifest's 3 dB scale. Score is fit
loss plus validation loss. The validation split also applies the saved harm
gate. Rows marked no failed that gate.

| String | Loss time | Model T60 | Fit loss | Validation loss | Score | Eligible |
|---|---:|---:|---:|---:|---:|---|
| C2 | 0.080 | 0.5200 | 6.2700 | 5.4649 | 11.7349 | no |
| C2 | 0.125 | 0.7966 | 5.8037 | 5.0950 | 10.8987 | no |
| C2 | 0.250 | 1.5476 | 4.8322 | 4.3910 | 9.2232 | yes |
| C2 | 0.500 | 3.0034 | 3.1064 | 3.2192 | 6.3255 | yes |
| C2 | 0.750 | 4.4264 | 1.4270 | 2.1611 | 3.5880 | yes |
| C2 | 1.000 | 5.8152 | 0.5141 | 1.1808 | **1.6949** | yes |
| C2 | 1.500 | 8.5046 | 1.8594 | 0.9426 | 2.8021 | yes |
| G2 | 0.080 | 0.5020 | 4.3019 | 4.5614 | 8.8633 | no |
| G2 | 0.125 | 0.7570 | 3.9336 | 4.4197 | 8.3533 | no |
| G2 | 0.250 | 1.4277 | 2.9970 | 3.9497 | 6.9466 | yes |
| G2 | 0.500 | 2.6158 | 1.6418 | 3.1866 | 4.8284 | yes |
| G2 | 0.750 | 3.7046 | 0.8469 | 2.5433 | 3.3903 | yes |
| G2 | 1.000 | 4.7192 | 1.0398 | 1.9543 | **2.9941** | yes |
| G2 | 1.500 | 6.6187 | 2.2301 | 0.9562 | 3.1863 | yes |
| D3 | 0.080 | 0.5099 | 6.3996 | 3.7373 | 10.1370 | no |
| D3 | 0.125 | 0.7755 | 5.8021 | 3.4576 | 9.2597 | no |
| D3 | 0.250 | 1.4647 | 4.4031 | 2.8183 | 7.2214 | yes |
| D3 | 0.500 | 2.7008 | 2.1140 | 1.9147 | 4.0287 | yes |
| D3 | 0.750 | 3.8308 | 0.8124 | 1.3641 | **2.1765** | yes |
| D3 | 1.000 | 4.8909 | 1.1550 | 1.0636 | 2.2186 | yes |
| D3 | 1.500 | 6.8678 | 1.9365 | 1.3205 | 3.2570 | yes |
| A3 | 0.080 | 0.4691 | 4.0556 | 4.6240 | 8.6796 | no |
| A3 | 0.125 | 0.6971 | 3.7739 | 4.2755 | 8.0493 | no |
| A3 | 0.250 | 1.2674 | 3.1382 | 3.3831 | 6.5213 | yes |
| A3 | 0.500 | 2.2639 | 2.5588 | 2.0034 | 4.5622 | yes |
| A3 | 0.750 | 3.1841 | 2.1418 | 0.9536 | 3.0954 | yes |
| A3 | 1.000 | 4.0246 | 1.7732 | 0.3588 | **2.1320** | yes |
| A3 | 1.500 | 5.6043 | 1.3032 | 1.2147 | 2.5179 | yes |

The fit-only minima are C2 1.00, G2 0.75, D3 0.75, and A3 1.50 seconds.
Adding validation loss changes G2 and A3 to 1.00 second. A later final test
must use recordings that did not affect either the grid or this rank.

## Passive-decay-v4 rescore

On 2026-08-31 the live decay tool moved to `passive-decay-v4`. It keeps the
v3 range, residual, support, and full-reference-span rules. It adds a waveform
change check so slow modal beating does not look like a second pluck.

The four saved experiments were rescored without rendering again. C2, G2,
and A3 scores stayed exact. D3 validation onset handling changed some scores,
but D3 still chose 0.75 seconds. The full vector therefore stayed C2 1.00,
G2 1.00, D3 0.75, and A3 1.00 seconds.

| Item | SHA-256 |
|---|---|
| first v4 selector | `f2177e793885bfab90d0fca57dfa83635d49cd70b9004a7b4d9ac3aece0142cd` |
| C2 v4 selection | `3e47c3d04da7cf49c7bf2d478252e0d7fadd077a409cb1617b9c7c9669c40550` |
| G2 v4 selection | `b492f902483395b04ae91ab184d2289c7c66cd2224c78a61239884361ea60294` |
| D3 v4 selection | `69ee608e46031af2b27d67a58bd77ebd9b81f0cc06c3ab54f75ff153abf43f06` |
| A3 v4 selection | `958714b4b45ded37e7ad69b566ecba7c0de71158f24db4b2b3948bbd59639ef5` |

### Joint-ready v4 rescore

The joint check binds the whole selector file, including its version 2 gate
code. The four scalar experiments were therefore scored once more after that
code stopped changing. This did not render audio again and kept the same
vector.

| String | Loss time | Fit loss | Validation loss | Score | Selection SHA-256 |
|---|---:|---:|---:|---:|---|
| C2 | 1.00 s | 0.5141 | 1.1808 | 1.6949 | `7732c8dd7770c73017024dbc7fca40aeb0c22119741d6d74ccea3534cfd3b11e` |
| G2 | 1.00 s | 1.0398 | 1.9543 | 2.9941 | `5a24837e95f8893c189f9972245c70cefd5bc77ea7f4d85439f5dfcc4bffad1e` |
| D3 | 0.75 s | 0.8124 | 1.2903 | 2.1028 | `695b3dbb13ae6ee7c62eb03163a50c631bab74f09cd008796fe6da678427b9a1` |
| A3 | 1.00 s | 1.7732 | 0.3588 | 2.1320 | `1e523ca79b5ed8359574745bc8c7990e41e4f26bc3683b65ee9bab3662a58401` |

The selector SHA-256 for these four files is
`84fcde397de6ec44e7438e7b6b2f081f5da9af8d76dd1e2ab02d486f833e84a9`.

## Joint candidate contract run

The checked joint path freezes the four selected values into one two-point
experiment. Point 0 keeps all four loss times at 0.25 seconds. Point 1 uses
C2 1.00, G2 1.00, D3 0.75, and A3 1.00 seconds. The first contract run below
used eight fit and validation cases, 16 renders, and the matching validation
render for each audit goal. The current adapter writes 12 true fit,
validation, and audit cases and 24 renders. Each audit recording now has its
own frozen case and model render. The saved hashes below still describe the
first 16-render run.

The builder requires eight distinct scalar recordings and four more distinct
audit recordings. It rejects bundle and audit paths under either source
repository. It creates the final bundle directory and files without replacing
an existing path.

At the time of this first contract run no third four-string recording set was
on disk. The run therefore used four new synthetic mono PCM16 files outside
Git. Each has 44,100 Hz sample rate, 529,200 frames, and 0.1 seconds of leading
zeroes. The tail is an exponential times a three-part sine sum with relative
amplitudes 1, 0.08, and 0.03 and phases 0, 0.31, and 0.73 radians, divided by
1.11. Samples use signed 16-bit rounding. This input is only a code-path test.

| Target | Frequency | Decay constant | Gain | Synthetic SHA-256 |
|---|---:|---:|---:|---|
| C2 | 64.918422 Hz | 0.84 s | 0.43 | `390adfc3fe8266ca7c86b4cda6d552e2074ecb357e7a78c46f888206b54d1a6f` |
| G2 | 98.259777 Hz | 0.68 s | 0.41 | `356e85fe102984449d1849126b92f8069f5feeda1589680b7da7643b21f132b6` |
| D3 | 146.768088 Hz | 0.55 s | 0.39 | `13e360ab0716f39a3b410813fb6df2b923248193a49ef38fe8c684a6fa406394` |
| A3 | 220.178095 Hz | 0.58 s | 0.37 | `3070ae6c85db811b9def40a508b4daa325be21571f68a3810a0d5cfec8a0cf2e` |

All 16 Release renders completed. Each model file is finite, non-silent,
unclipped stereo PCM24 at 44.1 kHz with 529,200 frames. The run made eight
unique model hashes: one baseline and one candidate render per string. Stage
8 reported eight flat whole-file diagnostic responses, as expected; the fit
tool scored the raw WAVE decay curves.

| Split | Baseline mean loss | Candidate mean loss |
|---|---:|---:|
| fit | 3.8426 | 1.0349 |
| validation | 3.6920 | 1.1961 |
| synthetic audit | 4.7195 | 0.9136 |
| total | 12.2541 | 3.1446 |

The candidate still failed one fixed absolute gate. Its G2 validation model
has a 4.7192-second T60 and 2.6240 seconds of support, while the reference has
a 12.4263-second T60 and 6.7197 seconds of support. The ratios are 0.3798 and
0.3905. Both fall below the named 0.5 floors. The curve loss, 1.9543, stays
under the 2.0 cap, and all other goals pass. The fit command wrote a failed
result with no chosen point. The profile writer then rejected it and created
neither a promoted profile nor a write receipt. Do not lower these limits just
to accept the current vector.

| Joint item | SHA-256 |
|---|---|
| adapter source | `431faead346c7022f9c9a4a97538855e7f8f3012fee4dc0010cc0b005d9e0578` |
| analyzer binary | `51785b0c117570fdf251f08dbbeaa107067556a79ddf41a64a5c351d0a5204da` |
| frozen renderer | `2ad7d0607f63efea0033dadd309d991837e17dad7582668e89ba0872f52f4d2b` |
| bundle receipt | `18809b1b345711b776eb4c31f6a79def5c9f01235358d041bb42aecabc8be50c` |
| experiment manifest | `de1ad18383e42788c60f23c6601748a07d181f4f556f550cb1e8d7eb271be9e3` |
| fit manifest | `584da1d44cc2a4a6228faeebe22d0e71f36fb1d21612a75ca3468df6c5407113` |
| binding manifest | `cdb8328d4130f53b7118192f34aafb391c146afe8dfef594174413114c5e1a8f` |
| candidate profile | `1643ed51a7420795a771359cb38dc40236fcc75a1d33cd7b1806ced8f8a2da65` |
| experiment result | `54f8354518d6621adb395b9fbf88a7effb3c763a54af0b610d623c3f705fbfda` |
| failed fit result | `dc9f9d5ad47206594533b265633864182a97b72c0045015bb28f5368c8b4a97f` |

## Real RWC audit runs

The RWC Instruments Database was released online under CC BY-NC 4.0 in
February 2026. The [source receipt](cello-alternative-passive-audit-sources.md)
records the official row, archive member hashes, exact lossless splits, pitch,
and raw-tail checks.

The first real run used four mezzo open strings from cello variation 2. The
candidate cut mean audit loss from 5.0570 to 1.5961. It still failed the fixed
2.0 curve-loss cap on D3 at 2.1284 and G2 at 2.7073. The Iowa G2 validation
ratios also failed as before.

A second run used variation 3 mezzo C2, G2, and D3 plus forte A3. Every RWC
audit goal passed. Mean audit loss fell from 5.5081 to 1.3194. The complete
candidate still failed only the Iowa G2 validation row: its model/reference
T60 ratio was 0.3798 and its support ratio was 0.3905. No chosen point or
profile was written.

| Run | Bundle receipt | Experiment result | Failed selection |
|---|---|---|---|
| RWC variation 2 | `8a43e4e53395c46c537e058c613d5a8afa48429b65ea2433c68023e1d7dc0e07` | `845e6339f6def3de835f5dc8c5ebce13458b1a2afe482f83d6c2249eda9730ec` | `bef91ec5ba7016f48d702bdfd3147e052e3aaf39d6c442586d3d9e315bd63457` |
| RWC variation 3 | `68f28876366d638a98f52441feaae5db693f761de020edd649f6cae05cd63fd2` | `4906f1f9b55618432a962e05a13d17d08e8ba86b90013540c87243ef82268b93` | `4d866a670a2b3da78b1e86e941d8fd0af17a5e2cf5ae95365fff465186d1d1ae` |

## G2 boundary sweep

The G2 grid then added 1.20, 1.25, 1.30, 1.35, 1.40, and 1.45 seconds. Its
26 Release jobs show that a missed coarse point did not cause the failure.

| Loss time | Fit loss | Validation loss | Validation T60 ratio | Validation support ratio |
|---:|---:|---:|---:|---:|
| 1.00 | 1.0398 | 1.9543 | 0.3798 | 0.3905 |
| 1.20 | 1.5726 | 1.5094 | 0.4426 | 0.4536 |
| 1.25 | 1.6984 | 1.4141 | 0.4576 | 0.4662 |
| 1.30 | 1.8176 | 1.3186 | 0.4728 | 0.4811 |
| 1.35 | 1.9301 | 1.2158 | 0.4884 | 0.4981 |
| 1.40 | 2.0362 | 1.1236 | 0.5021 | 0.5071 |
| 1.45 | 2.1360 | 1.0336 | 0.5168 | 0.5204 |
| 1.50 | 2.2301 | 0.9562 | 0.5326 | 0.5390 |

The fixed rules require curve loss at most 2.0 and both ratios at least 0.5.
No row passes all three. The source grid now keeps these boundary points so a
future rerun cannot hide this gap.

| Refine item | SHA-256 |
|---|---|
| Adapter source used to build the grid | `86ced5d1841633b98d29c84c07f5c71aaf3e1522dd05c3fe55064cc314e3bb35` |
| Bundle receipt | `1069bc0e23b840ea134ab67b0b9c7a006619811ef16afe380187d92ebd90bb55` |
| Experiment result | `e53206ec5a3a86bc9ecb79e24024c7f2e65e1b0ee4b77297d409941c758fd4e1` |
| Selection | `a7a71957128be14fcbc6187b707b536be3f007146a76b1a35009778b265aa931` |

## RWC-backed scalar gate and final audit

The old Iowa G2 validation note was clipped and had a 12.4263-second T60 with
a near-limit straight-line residual. A replacement scalar split uses RWC
variation 2 mezzo for fit and variation 3 mezzo for validation. The renderer
uses the fit note's measured 96.9577320733 Hz pitch. G2 now has 19 levels from
0.08 through 3.0 seconds.

The scalar selector now applies the joint gate's absolute rules before rank:
each fit and validation goal needs loss at most 2.0, T60 ratio from 0.5 through
2.0, and support ratio at least 0.5. This rule also rechecked C2, D3, and A3.
Fresh Release runs kept C2 1.00, D3 0.75, and A3 1.00 seconds. G2 chose 1.50
seconds. The lower-scoring 1.20 through 1.45 points failed the 2.0 fit-loss
cap, while the passing 1.60 point scored worse than 1.50.

| String | Chosen loss time | Fit loss | Validation loss | Selection SHA-256 |
|---|---:|---:|---:|---|
| C2 | 1.00 s | 0.5141 | 1.1808 | `267cf7bf7c3d329baafa81042d587a0d42ba5ddcba2305c6fc8285bf7e151588` |
| G2 | 1.50 s | 1.9923 | 1.3101 | `4eae4a639c18e4eb0f74ca4cf24588ea2fbd0faaf7036a1929ea59a71500a60d` |
| D3 | 0.75 s | 0.8124 | 1.2903 | `cbdec5ad024f2964c8fcd7ce2ad70c7c88127bf5b9be5303907d120e96b09ada` |
| A3 | 1.00 s | 1.7732 | 0.3588 | `d6fa75baaad8947b2f515c552b1efbd3e0daff554fe4f923b798ea984eb8edaa` |

The final audit used RWC cello variation 1 forte. The candidate was frozen
before that member was fetched or split. All four source tails passed the v4
input gate. C2 and A3 passed the model gate. D3 failed with 7.5126 dB curve
error. G2 failed with 9.9005 dB curve error and a 2.3385 model/reference T60
ratio. The fit tool wrote a failed result with no chosen fields, and no
profile writer ran.

| Final item | SHA-256 |
|---|---|
| Current adapter | `ed3dfb872451e0538e07ac4093bdd4fa13078e2922072d2128368a05c513789b` |
| Current selector | `ab2013359408bd449cc0bd1d5c7069002581937237891f7ae432b950f8d33d52` |
| Joint bundle receipt | `8858d1504351c3340b2112037c76e2a03587dd4619620316172a4525b5e3ca34` |
| Joint experiment | `d68d9b0a1db8ada18d074fa2f30ba0923471de237707159249dd6275e806972e` |
| Failed selection | `f89292ea9270786813eca9e9b7e7facf05df1fb795d6d712f6152a77ceef6449` |

The fixed profile and C source remain unchanged. This result rejects the
one-time-constant model; it does not justify weaker gates or a tuned value.

## Historical receipt hashes

| Item | SHA-256 |
|---|---|
| Adapter source | `1c83bef757054fcf29ad5cf16d2125c6e123b279e53dccc89ba97509bc244cfa` |
| v3 fit selector | `e4476bdfde0f44546cbae7b251f2784e257c72ab548e6057396b621e31a96c98` |
| Generic probe | `ac17b30bdddecaf23b4883b924a1af9ed7f6b01f0df47984853ff171db19ad48` |
| C2 manifest | `5e76750085717b7e72f022201fef59f0f86a5a3bc571309b9d84336bdde5d107` |
| G2 manifest | `a038f9ad6b161515824ee910172d305ec0ea0ddc07e209a54c986180083617f5` |
| D3 manifest | `1ffda8f86f1494091309a54f4dfc693f12fd423c15cf7fa3d2e079a28e91ad6a` |
| A3 manifest | `064c7ca0b24dadda89ede69327c493ba091e4595351fa0f9a509e34d489dc183` |
| Fixed model, unchanged | `14f81b089c9958e14a1c36b3826e076d0677ca550c33c13becf1411ec6f82835` |
| C source, unchanged | `d42c9c8cc77a211a3a64b840bb450c587ac1b19b3214389ab3947981cda4da42` |

| String | Bundle receipt | Experiment result | Selection result |
|---|---|---|---|
| C2 | `07ccbe459aacbf866c776a9a9b877d1f2de85feaca46e8f4f1cdb8356a067f90` | `ca9d42bb5182d7dd464372bcf2380ca33b4415255b28b579f7c796c7f3772674` | `676de59d965c3b94ed740c0ee5cfe62e43f619958dae6a48af294c19c96cf3d8` |
| G2 | `7d80ad665acb632ca42e8237117e9bded2f15e7d397665aa971adb82b7e5a1fa` | `735ecc6b656e8398ed91b1e9c4accbf7b6bfc55a293149d67127eac83a2549dc` | `b1a8623eb144eeeddf31a35ad8f0c98c491cf2061d1d3fef05f0bfa669200850` |
| D3 | `02c704ed00a9df3313364c211b7a519f11a8223bf94dc3a358e80a150a483d57` | `c5f05b7ec72447752776385ceb2fe16c91713b4e6b39a8466d7ffe7b72fb9858` | `0a3add0bf366013a83d765e9003e201edbaef9e687da965637e74c62e26cf6bf` |
| A3 | `719dc362f702942d6a1f255bf2ddf48453b2121d945fda29a353a1426fcbdd1d` | `a9687caa554bad66e512fa01c85f0a5369174169c2624500ac5782107bdc88ff` | `fde31ea9f1ab691d489ec246e5c8c80c530d1a89d0f1ba0d73a74b393e75f503` |

Every experiment has 14 artifacts and seven unique model hashes. Fit and
validation jobs at the same point have the same model hash. All 56 files are
finite, non-silent, unclipped stereo PCM24 with the stated rate and frame
count. Each bundle also checks the named Csound core and libsndfile paths
against the libraries loaded by the host before it freezes their hashes.

## Next gate

Do not copy the four values into the fixed model. Three RWC cello variations
cover G2 fit, validation, and the consumed final audit. The later OrchideaSOL
audit is also consumed and cannot rank another candidate.

A predeclared 2026-09-03 follow-up used only eligible Iowa fit and validation
data to test existing nut and bridge cutoffs with loss time on C2 and D3. Both
per-string selectors passed. `nut_loss_fraction` was not swept because its two
passive half-loop powers multiply back to the same full-loop gain. The
[C2/D3 frequency-loss report](cello-cd-frequency-loss-v1.md) records the grids,
metrics, and receipts. Those selections still need assembly with separately
passing G2 and A3 evidence and another untouched four-string audit.

No current result proves bow, bridge, body, release, or gesture data.

## Generic-corpus follow-up

The [generic passive-corpus run](cello-generic-passive-corpus-v1.md) balances
28 admitted takes across four source identities. G2, D3, and A3 pass, but C2
fails both the original and expanded existing-control grids. No four-string
candidate exists yet, and the newly reserved fifth player/cello/session must
remain untouched until C2 passes and candidate assembly is frozen.
