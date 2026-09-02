# Cello G2 bridge-termination fit

Checked on 2026-08-31. This report records the G2 model change and fit that
followed the failed three-value loss sweep. It does not apply the selected
values to the fixed cello model.

## Inputs and rule

The fit uses RWC cello variation 2 mezzo pizzicato. Validation uses variation
3 mezzo pizzicato. The source, rights, exact sample spans, and lossless work
WAVE hashes are in
[the RWC source receipt](cello-alternative-passive-audit-sources.md).

| Split | Work WAVE SHA-256 |
|---|---|
| fit | `e7cc4024989597cb8bd1a3127d20fa6481c96403352cf75570b11efbac5853b3c` |
| validation | `90081ed3a28788e2c7d0e004e2654b895ce2cc01a3fb84120a718005fdbab3c0` |

The selector uses `passive-decay-v4` and `harmonic-decay-v1`. The limits did
not change during these runs:

- each whole-tail or partial-decay loss must not exceed 2.0;
- mean partial T60 error must not exceed 0.75 octave;
- worst partial T60 error must not exceed 1.5 octaves;
- model/reference whole-tail T60 must stay from 0.5 through 2.0;
- model/reference support must stay at or above 0.5; and
- validation loss may rise by at most 0.15 from baseline.

The fixed profile stays `violin-derived`. All candidate profiles, plug-ins,
renders, and selection files stay outside Git.

## Why the old controls were not enough

The old nut-loss split cannot change linear round-trip decay. It applies one
part of the same loop gain at the nut and the rest at the bridge. Their product
is the same for every split. It can change a short within-loop transient, but
not the passive decay rate used by this fit.

Characteristic impedance scales the excitation and coupling terms. The dry
passive probe removes body coupling, and the decay score removes initial gain.
The impedance value therefore cannot identify the missing normalized decay
shape in this test.

The validation take gives the more useful clue. Its third harmonic has a T60
of 1.467 s, while the prior model holds it for 9.250 s. Its fifth and sixth
harmonics decay in 2.559 s and 3.334 s. This calls for a narrow loss near the
third harmonic, not a change in total gain.

## Rejected low-shelf model

The first model added a low-frequency bridge-loss shelf. A zero loss fraction
made the new path signal-neutral. The model included the shelf phase in the
waveguide delay solve.

The first grid covered 80 points and 160 renders. Its shallowest shelf loss
was 0.03. The best shelf point shortened the validation tail and fixed its
partial balance, but its fit whole-tail loss was 3.510, above the fixed 2.0
limit.

The second grid covered 120 points and 240 renders. It tried shelf losses from
0.01 through 0.025, corners from 150 through 500 Hz, and loss times through
5.0 s. No point passed. The best score was 2.871. Its fit whole-tail loss was
2.537, and its validation partial errors were 0.827 octave mean and 1.756
octaves worst. Both partial limits failed.

| Shelf run | Bundle receipt | Experiment result | Failed selection |
|---|---|---|---|
| first | `292fd06867b87813868cba6c3cc62a2f154dcf3da6acfc9934f8e10c31b6e652` | `539c6aa8be678c760efac51bc9e0adf8405c4e2544ebb3b1dd8d07de529456c7` | `ca88a4025b02b1adbc2482e98e945e6e1ab55133759f4737681dc588f7f30fb1` |
| refined | `3ce3f1b116ffa7fee09682baec49f88c68b23314428e90d503f157d6730a55f2` | `775972a514c3e696bc28573c32f177f9ebd3be825d92e0400984830d5a46954c` | `66b9eff1cffa4a48ca52b73c6292eb461a3f24c81c91259a5e1a11a076746fa5` |

The shelf result is a closed negative test. The later fit manifest does not
offer those shelf values.

## Narrow bridge-loss peak

The next model adds a second-order bridge-loss peak. Its fixed center is
293 Hz, close to the third harmonic of the measured G2 notes. The profile
stores center, bandwidth, and loss fraction. Zero loss makes the new path
signal-neutral. The delay solve includes the exact phase of the peak filter.

The first peak grid fixed bandwidth at 200 Hz. It covered 60 points and 120
renders. It cut validation partial error, but its best fit whole-tail loss was
2.954. The width was still too broad. The run failed and did not write a
profile.

| Wide-peak receipt | SHA-256 |
|---|---|
| bundle receipt | `73cb14a1fc4554ca76a73f87a36e4789693fa87a22fba3c878dc0692d03023e9` |
| experiment result | `31b63272e669800855b4ac5b59b9da709ca24a8456b15ecf9523cf5aa473f0cd` |
| failed selection | `dafce9dad4b83ab0d41b39db093539fb31b8e5387e24dbd0fc93a6c0318f4800` |

The final grid adds 50, 100, and 200 Hz bandwidths. It covers 96 points and
192 fit/validation renders. Point 71 passes every fixed gate.

| G2 value | Baseline | Selected |
|---|---:|---:|
| bridge cutoff | 4964.244281 Hz | 18000 Hz |
| bridge-loss peak bandwidth | 200 Hz | 100 Hz |
| bridge-loss peak fraction | 0 | 0.02 |
| passive loss time | 0.25 s | 2.5 s |

| Split | Whole-tail loss | Partial loss | Mean partial error | Worst partial error | Valid partials |
|---|---:|---:|---:|---:|---:|
| fit | 1.941 | 0.890 | 0.445 octave | 0.604 octave | 3 |
| validation | 1.401 | 1.418 | 0.709 octave | 1.143 octaves | 4 |

The total score falls from 7.665 at baseline to 2.825. A bounded pitch check
on the selected fit render gives 97.0238 Hz against 96.9577 Hz expected, or
+1.179 cents. The peak phase correction therefore keeps this render within
the existing 2-cent pitch bound.

| Passing receipt | SHA-256 |
|---|---|
| bundle receipt | `10b148715a71450bfb8d8bb91ee64f92228b54f13ec9b4d681f9c71e1b873c56` |
| experiment result | `a991924091c5dc8b0f0a5a5aa24a474037aeeac13fdcd11261388caa9a0f13a1` |
| selection | `0c7e6d6c46a59cad5ec7bdb1395625461cb034103303a02fbaaa4017e8c0e31b` |

## Source and test state

The passing bundle binds these bytes:

| Item | SHA-256 |
|---|---|
| fixed cello model | `af2fcd8a7bafa2dd40ef5883f5f5ca85c4fa9f096d213ee902ef515eded763ac` |
| cello C source | `72ea0fb069ea4bf88c330d2ac9cde7f26e4823b48046e3cff618a19aa2da2e14` |
| cello model schema | `bf6e421eda78cfad51d08ef96d4ccf32518adf696881c4825eb01701cfb5de3a` |
| cello model generator | `0076d617c759b1d3e55be737cbe129c8e1e050b52c7fa955fce0af17943a6835` |
| G2 fit manifest | `d0299bf999299c68ce82513adc70c7961c53a87b89b5b8aca8563708b35ce66f` |
| fit selector | `f8952be56bb854f5ccbc61a2c90abab11fbecf6b01e5724fdea31df942acbf91` |
| analyzer binary | `0b9f7b2164d1262d5d1229a1cfb96f07f26ab955e6654f650e4de2539321d280` |

The fixed profile has zero shelf loss and zero peak loss on all four strings,
so this work does not change the current demo sound. A strict cello build and
all ten CTests pass, including native/WASM parity and geometry. Fresh C2, D3,
and A3 scalar runs against the neutral schema also pass and select 1.0 s,
0.75 s, and 1.0 s again.

| Current selection | SHA-256 |
|---|---|
| C2, 1.0 s | `44bd328120b33b668135ea5802f613ebda6d38a79bc6b0fc9b4de96e8345a5ae` |
| G2, bridge peak and 2.5 s | `0c7e6d6c46a59cad5ec7bdb1395625461cb034103303a02fbaaa4017e8c0e31b` |
| D3, 0.75 s | `754cf3ac44d962dcca31638901c1d96500780cdbe9dfc389f0daca02df4eec89` |
| A3, 1.0 s | `8e473ddedf46f61f2c3ca7ae038878983980991ef16fb63b0823224be98f87a8` |

The selected G2 values remain staged. The next gate needs an unused
four-open-string pizzicato set and one joint render containing all seven
candidate changes across C2, G2, D3, and A3. The
[source search](cello-alternative-passive-audit-sources.md) found no unused,
accessible set that meets the fixed input checks. Do not write the fixed model
before that gate passes.
