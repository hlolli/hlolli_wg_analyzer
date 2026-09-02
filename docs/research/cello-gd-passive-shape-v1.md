# Cello G2 and D3 passive-shape fit

Checked on 2026-08-31. This report covers the next passive-string fit after
the one-value loss sweep. It does not change the fixed cello model.

## Goal and inputs

The sweep varies three values on one string at a time:

- passive loss time;
- nut low-pass cutoff; and
- bridge low-pass cutoff.

It uses RWC cello variation 2 mezzo pizzicato for fit and variation 3 mezzo
pizzicato for validation. The source and exact sample spans are in
[the RWC source receipt](cello-alternative-passive-audit-sources.md).

| String | Fit WAVE SHA-256 | Validation WAVE SHA-256 |
|---|---|---|
| G2 | `e7cc4028cf861cf1a1cd6dd78d885ab441beb3b84cfd0d146c673ebaf6997ac8` | `90081ed3a28788e2c7d0e004e2654b895ce2cc01a3fb84120a718005fdbab3c0` |
| D3 | `38841a28803e1d38283686678d9467cffc49ddbaffd0dde54907d203abc6d43a` | `78665e1ac6f4f18155f4e04717940c07322fe42a78670c0c3c7762df24d400bd` |

The G2 grid has 63 points and 126 renders. The D3 grid has 54 points and 108
renders. Each render uses a new temporary profile and Release plug-in. All
output stays outside Git.

The D3 adapter had still rendered the old Iowa pitch, 146.7681298 Hz. This
run fixes it at the RWC fit pitch, 144.7113 Hz, with A4 set to
433.6439298 Hz. Every D3 point was rendered again; no old D3 job was reused.

## Harmonic decay check

`harmonic-decay-v1` measures eight partials after the attack. It uses an
80 ms window every 80 ms at each expected harmonic. For each partial it fits
the checked falling span and reports T60. A partial needs at most 5 dB line
residual, and a comparison needs at least three valid partials.

The fixed limits are:

- whole-tail level error no more than 6 dB;
- mean absolute partial T60 error no more than 0.75 octave;
- worst partial T60 error no more than 1.5 octaves;
- model/reference whole-tail T60 ratio from 0.5 through 2.0; and
- model/reference support ratio at least 0.5.

The score gives equal weight to whole-tail level error and partial T60 error.
The validation loss may rise by at most 0.15 above the baseline. The selector
does not use the failed RWC variation 1 audit to rank these points.

## G2 result

No G2 point passes. The lowest score uses bridge cutoff 18 kHz, loss time
1.0 s, and nut cutoff 4.2 kHz. It fails the fit whole-tail limit and both
validation partial limits.

| Split | Level RMSE | Mean partial error | Worst partial error | Valid partials |
|---|---:|---:|---:|---:|
| fit | 7.119 dB | 0.449 octave | 0.710 octave | 3 |
| validation | 2.558 dB | 0.935 octave | 1.895 octaves | 3 |

The result status is `fail` and has no chosen fields. Raising the limits to
accept this point would change the rule after seeing the result, so this run
does not do that.

The G2 audio grid predates the harmonic objective, but its render inputs and
three parameter values did not change. The current selector scores those
saved WAVE files directly. The saved experiment SHA-256 is
`9c759897b7a1b352972fcc0cdb3d23677e59f8c7e5a44a6fa5c23feb3be17fed`.
The failed selection SHA-256 is
`56a584d8679bd5853ee6401251f1779d903ff6a14f11bba07ed4ce487508300b`.

## D3 result

D3 passes. Point 43 keeps the prior loss value and bridge cutoff, and lowers
the nut cutoff from 12 kHz to 8 kHz.

| Value | Baseline | Chosen |
|---|---:|---:|
| bridge cutoff | 7086.471046 Hz | 7086.471046 Hz |
| loss time | 0.25 s | 0.75 s |
| nut cutoff | 12 kHz | 8 kHz |

| Split | Level RMSE | Mean partial error | Worst partial error | Valid partials |
|---|---:|---:|---:|---:|
| fit | 5.878 dB | 0.442 octave | 1.002 octaves | 5 |
| validation | 5.022 dB | 0.292 octave | 0.839 octave | 7 |

The corrected D3 bundle receipt SHA-256 is
`4eb8c1ba48e3706ec6d73fe0cde3c6d26eee6ca63d8ba23d9148f24d92296d1e`.
The experiment SHA-256 is
`fd2aa3f66494d903dffc4d8cc0086403f4529b6c3a70b46d7d8e6e13e27997a1`.
The passing selection SHA-256 is
`f8b996aa2fc0ec73014d3cb52bbd22b00d97f37f19940ec9f14a29db5d930874`.

## Code and model state

The run binds these files:

| Item | SHA-256 |
|---|---|
| fit selector | `4ee20a44b1b3d73dcac13b1cdd65120356a251e0a9f7004959c8d7688c80c26d` |
| cello adapter | `c18e3ac8f4610fb7f939fb0f14f3524a5e33f2fc3db0db06d26febed0e79ba59` |
| G2 fit manifest | `02a49b5bc2985fe46393dc54da8649888691670a65f51bd0f28c3fc9e9fbc581` |
| D3 fit manifest | `79597c1f90a80864a1c8a4c33ce8691ea4e24b8d2103a756dcb989d5cf1092c2` |
| analyzer binary | `4fe29bb62beaff4e9d8a11e342069349c3882a66e40a69eba93f9683fe92015f` |
| fixed cello model | `14f81b089c9958e14a1c36b3826e076d0677ca550c33c13becf1411ec6f82835` |
| cello C source | `d42c9c8cc77a211a3a64b840bb450c587ac1b19b3214389ab3947981cda4da42` |

The fixed model still has 0.25 s loss on all four strings and still says
`violin-derived`. G2 has no passing shape point, and there is no unused third
four-string recording set for a final audit. The next step is to widen the G2
model itself or find new controlled G2 data. Do not merge the D3 value alone.
