# Cello C2 and D3 frequency-dependent passive loss

Checked on 2026-09-03. This fit/validation run tests the existing nut and
bridge one-pole termination filters on C2 and D3. It does not change the fixed
cello model and does not authorize a four-string candidate.

## Separation and predeclaration

The run used only the Iowa fit and validation recordings already assigned to
C2 and D3. The consumed RWC variation 1 final audit and OrchideaSOL audit were
not read and did not set ranges, objectives, gates, ranking, tie-breaks, or
selected values.

The frozen predeclaration SHA-256 is
`b31c59135fdf8d6c44a0e711c0ae502e89753557bb622e50f7b5cba8c088bcfc`.
It names 72 grid points and 144 Release renders before either experiment ran.

| Input | Role | SHA-256 |
|---|---|---|
| C2 2012 pizzicato | fit | `e49471d0c8428c559995883d342c9ab1211a9a60a56508bb4f5f2fc85b8d66fb` |
| C2 2001 pizzicato | validation | `49018bc7c7991f49a475cad8725e381ef97604a83738eb0a388da96f3f5f29ad` |
| D3 2012 pizzicato | fit | `67603a93b66c8aa6bf5fbc609473fc54e6c7926a124e76e37bf9f04376aaaf4c` |
| D3 2001 pizzicato | validation | `ad0a53aa2bf254f3351f92caedf99e725739bba0baa0bee90dc05494814c285f` |

The selected scalar renders showed a consistent eligible-data residual. C2
fit harmonics 5 through 8 decayed too slowly by 10.791, 15.602, 14.825, and
11.518 dB on the T60-log scale. D3 fit harmonics 4 through 8 were too slow by
5.417, 2.873, 7.286, 7.824, and 3.154 dB. The diagnostic receipts are:

| Diagnostic | SHA-256 |
|---|---|
| C2 fit | `86e24633c4118a7f1806aa81f3ecce184051daa06d1a588e88834ee1f7a23046` |
| C2 validation | `3b9a3f40568114996ec985ecb03bf7ec1496c22930c38f19e2abdcfe4c4fd13e` |
| D3 fit | `ffd1e25612bb11cca4415071309cd8d1fbe1ab93c9f979637ebbf2fba9de65ed` |
| D3 validation | `52e3f27bfec0b9c3acc53c2176fbc51892246e2533a1e80e8ddb7bb9acc915d6` |

A3 was excluded because its fit and validation partial errors had conflicting
signs. G2 remains under its separate bridge-peak result. `nut_loss_fraction`
was also excluded: on a passive full loop its two gains multiply to
`pow(loop_gain, split) * pow(loop_gain, 1 - split) == loop_gain`. No eligible
residual identified a half-loop-level defect, so sweeping that value would add
points without testing the observed frequency dependence.

## Fixed grids and gate

C2 used 48 points:

- bridge cutoff: 1.8, 3.5, and 5.386995 kHz;
- loss time: 0.25, 0.75, 1.0, and 1.25 seconds; and
- nut cutoff: 1.5, 3, 6, and 20 kHz.

D3 used 24 points:

- bridge cutoff: 3.5 and 7.086471 kHz;
- loss time: 0.25, 0.5, 0.75, and 1.0 seconds; and
- nut cutoff: 4, 8, and 12 kHz.

Each split gives equal objective weight to whole-tail passive decay and eight
partial T60 comparisons. Every fit and validation objective needs loss at most
2.0. Passive comparisons also require a model/reference T60 ratio from 0.5
through 2.0 and support ratio at least 0.5. Harmonic comparisons require mean
error at most 0.75 octave and maximum error at most 1.5 octaves. Validation
mean loss may rise by at most 0.15 above baseline.

Validation is a binary gate, not a ranking input. `check_weight` is zero; among
points that pass every fit and validation rule, the selector ranks fit mean
loss and breaks an exact tie only by canonical point ID.

## Results

Both selectors exited zero.

| Target | Baseline fit | Baseline validation | Chosen fit | Chosen validation | Eligible points |
|---|---:|---:|---:|---:|---:|
| C2 | 10.4161 | 10.1955 | 0.5656 | 1.2427 | 14 |
| D3 | 3.2763 | 3.0434 | 0.8514 | 1.0230 | 6 |

C2 point 29 chose:

- bridge cutoff 3.5 kHz;
- loss time 1.0 second; and
- unchanged 20 kHz nut cutoff.

Its fit whole-tail RMSE is 1.7243 dB. The fit partial mean and maximum errors
are 0.2782 and 0.5029 octave across four valid partials. Validation whole-tail
RMSE is 4.3844 dB. Validation partial mean and maximum errors are 0.5120 and
0.7661 octave across five valid partials.

D3 point 12 chose:

- bridge cutoff 3.5 kHz;
- loss time 1.0 second; and
- nut cutoff 8 kHz.

Its fit whole-tail RMSE is 3.0684 dB. The fit partial mean and maximum errors
are 0.3400 and 0.7480 octave across four valid partials. Validation whole-tail
RMSE is 3.4566 dB. Validation partial mean and maximum errors are 0.4469 and
0.9447 octave across four valid partials.

The selectors demonstrate fit/validation improvement, not final generality.
C2 has 14 eligible points and D3 has six; the fit-only rule selected one point
from each predeclared set without using validation magnitude to optimize it.

## Receipts

| Artifact | C2 SHA-256 | D3 SHA-256 |
|---|---|---|
| Bundle receipt | `3cdc4094404f6dd47e2319879169192abb2155cde314ad48357ed5e6836a1147` | `c1bff364f2367869d987a61df276e8c3c638e4d8ced7dc338fe89051d2a9b13f` |
| Frozen renderer | `4e8cef7e80b743d029d7c72b915500885941bf062fbc8644ffd29e8c93508574` | `4f586d8f6ea16015e7b190c0d7863ea5cb576583bd2fd324bbdc7fad853fbea0` |
| Experiment manifest | `4563a14060dc844748e5c2fcbf560f811b946d015074b36f5fcffbcf9014ad29` | `7df31d896cd93e84c7623a022cd62fd042f0e8bb15a1e3108b78c596e9d1eb02` |
| Fit manifest | `c771960dfa742ebeb0351eb7bacba95218e85f14c73638e3cbd2edfc8fd8734e` | `239384a7e5e287fb9a9a6b71c7915f0511b42cd7f7897b5bbf3c3ba0581a925d` |
| Experiment result | `e2852f4b85e9326f7f8f8ae79dc8dc5b86210b47ab3c426db2c1484aab8f6c90` | `62b29b43233d5a2c330cfda1bffcfad86b0bcbf8345f183a59d0e18515b59067` |
| Selection | `b820cedaa0958fedd7fd305f3e93994eed23c0ea594d25a50678f1aa6d5f4cbd` | `ce544c36224c3d5bd337b70cafcebfca8aa43cf644bb56db8b39d5fa03be316b` |
| Staged profile | `fe943682aca72b2c0798a8db4c83fb333985824823bad63beb4a4e3ba7b9cae9` | `7f0a0afe9d09ce64f9a1c60866f6686509b45a8adc131ff622ffeac0d2f248e6` |
| Profile-write receipt | `7cc2c5614bf4ccc9918bd861c4bedb5b3cd7634a01c1f4fd6fca7b5fc76de27e` | `301b36421fb3e19dc0c1373754b99c04fe4f7b45a5df1c32bb14326f86dd3e2f` |

The C2 experiment has 96 model artifacts and 48 unique hashes. D3 has 48
artifacts and 24 unique hashes. Fit and validation jobs at each point have the
same model hash. Every model is non-silent, unclipped stereo PCM24 at 44.1 kHz
with 529,200 frames.

The frozen code and model hashes are:

| Item | SHA-256 |
|---|---|
| Cello adapter | `71276aa51d68d998ba86a154fdc05db608ee1b0a86f4dc0ff973b2100d4edc73` |
| Fit selector | `7ebfd54d3899e1a980a34c63db8f4064335ec479a2afe964434f556c66eb1cc6` |
| Analyzer binary | `5e08ffae39e8537320bc9ef2b38a9b5db5feaf8db0d7c3afe361e3889f7201c3` |
| Fixed cello profile | `af2fcd8a7bafa2dd40ef5883f5f5ca85c4fa9f096d213ee902ef515eded763ac` |
| Cello C source | `72ea0fb069ea4bf88c330d2ac9cde7f26e4823b48046e3cff618a19aa2da2e14` |
| External result summary | `28113a9867173c66928f8172a5a776da7369aa7674db4e059f54811c1f02df33` |

## Next gate

Keep both staged profiles outside Git. A future four-string candidate may
combine these values only with separately passing G2 and A3 evidence. It then
needs another untouched four-string audit, explicit review, and authorization
before writing any fixed data. The consumed OrchideaSOL and RWC variation 1
audit sets cannot serve that role.

## Superseded for generic-model selection

A later source-balanced run uses 28 admitted takes from four independent
cellos/players/sessions. Its [generic-corpus report](cello-generic-passive-corpus-v1.md)
supersedes these one-recording selections for parameter choice. Generic D3
passes at 0.75 s loss, 8 kHz nut cutoff, and the original 7086.471 Hz bridge
cutoff. Generic C2 fails even after an expanded existing-control grid, so this
report's C2 point 29 must not enter a new four-string candidate.
