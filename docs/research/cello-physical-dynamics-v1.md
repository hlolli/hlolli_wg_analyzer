# Physical dynamic law for the sample-free cello

Date: 2026-09-03

## Result

A written `p`, `mf`, or `f` is not a physical unit and has no universal force,
displacement, sound-pressure, or dB ratio. The useful mathematical relation is
instead the relation from the player's gesture to the string state. For an
ideal finger-plucked string it is exact in the small-displacement linear model:

\[
X = \frac{F a(L-a)}{T L},
\]

where `F` is transverse plucking force immediately before release, `a` is the
pluck distance from one end, `L` is speaking length, `T` is string tension, and
`X` is displacement at the pluck point. The displacement amplitude of mode
`n` is

\[
A_n = \frac{2 L F}{n^2 \pi^2 T}
      \sin\!\left(\frac{n\pi a}{L}\right).
\]

Consequently:

- string and radiated pressure amplitudes scale linearly with `F` while the
  string/body model remains linear;
- stored string energy scales as
  \(E=FX/2=F^2a(L-a)/(2TL)\);
- a force ratio maps to level by
  \(\Delta L_\mathrm{dB}=20\log_{10}(F_2/F_1)\);
- pluck position determines the spectral notches; a finite finger width rounds
  the corner and low-pass filters the ideal spectrum.

This is enough to implement a continuous physical dynamic control. It is not
enough to derive one universal numerical `p`-to-`f` ratio, because notation
does not specify either endpoint. A score or host may choose an editorial dB
span `D`; the corresponding force ratio is then
\(F_f/F_p=10^{D/20}\). The DSP must not infer that ratio from microphone dBFS.

## What this changes in the current model

The ordinary pizzicato path in `wg_cello_start_exciter` and
`wg_cello_exciter_tick` does not implement the initial condition above. It:

1. computes `strength = trigger * sqrt(force) * abs(speed)`;
2. emits a short raised-cosine junction pulse;
3. changes pulse width with `speed`.

That is a struck/impulsive velocity excitation, not a string pulled aside and
released from rest. It makes the excitation spectrum and level depend on an
unsupported mixture of three controls.

Mansour, Woodhouse, and Scavone describe the directly relevant cello
waveguide construction in Section 2.4. An ideal pluck has zero initial velocity
and nonzero displacement. They give two valid implementations:

- initialise the two travelling waves so their sum gives the required initial
  string state; or
- for an isolated zero-state simulation, start both velocity waves at zero and
  apply a constant point-force step from the release time onward. By linear
  superposition this differs from releasing the preloaded string only by a
  static displacement offset.

The current finite pulse is neither construction. The C2 symptom is consistent
with that mismatch: the synthesized fundamental is effectively absent while
upper partials remain measurable. More passive damping cannot repair an
incorrect initial spectrum.

The production implementation added on 2026-09-04 initialises the held,
rounded triangular **displacement** directly in the two velocity-wave histories.
A short half-cosine release removes its holding force and band-limits the corner.
Each trigger adds an independent state, so no constant force persists between
or across repeated real-time notes. The isolated constant-force step remains a
diagnostic only.

## Sample-free diagnostic

A predeclared private-source probe compared the unchanged renderer with the
paper's isolated-note force-step construction. It used no recording and changed
no repository or fixed-model file.

- The C2 fundamental rose by **21.296 dB**, from -97.023 to -75.726 dBFS in
  the fixed analysis window.
- With the old pulse, harmonics 2–8 were 4.3–9.8 dB **above** the
  fundamental. With the step, they fell monotonically from -0.5 to -13.4 dB
  below it.
- The same replacement raised the G2, D3, and A3 fundamentals by 17.8, 14.3,
  and 10.8 dB. The frequency trend is the expected consequence of replacing a
  short impulse-like input with a displacement-producing step.
- A second no-recording check rendered C2 at force controls 0.25, 0.5, and
  1.0. Every force doubling changed all first eight harmonics by the predicted
  6.0206 dB. Maximum level error was 0.00015 dB and maximum normalized-spectrum
  change was 0.00018 dB, both at PCM24 quantization scale.

The diagnostic passed its frozen gates. Result receipt SHA-256 values are
`c7ec292ab23ebcb0263eff9cc91ceb8b18dfdc8f909fbe434a983e6e00dabfe9`
for the force-step test and
`fbd9f83ffaea8cdb7294a19a00770305772f09cfaf1315bbe08bf294d48524c0`
for force scaling. These results identified the initial-condition class; the
production code implements the displacement state rather than shipping the
isolated step hack. Its focused native test measures a -77.170 dBFS C2
fundamental at force 0.5, a descending first-eight-harmonic balance, maximum
force-doubling error of 0.00066 dB, and maximum normalized-spectrum drift of
0.00105 dB. It also proves that trigger magnitude does not set loudness and
that a retrigger leaves no persistent point force.

## Control law

For ordinary pizzicato:

| Public control | Physical role |
|---|---|
| `kTrigger` | gate/retrigger state, not loudness multiplication |
| `kForce` | normalized pre-release transverse plucking force; map linearly to `F` |
| `kSpeed` | contact/release detail or corner rounding only; do not multiply the ideal displacement amplitude by it |
| `kPosition` | `a/L`, setting displacement geometry and spectral notches |

If a host wants a perceptually even dynamic knob, it may map its own normalized
value `u` exponentially between chosen nonzero endpoints:

\[
F(u)=F_\min(F_\max/F_\min)^u.
\]

The cello opcode's control is already named force, so the engine itself should
keep force linear rather than silently applying `sqrt(force)`. A square root
would only be justified if the public input were explicitly defined as
normalized **energy**, which it is not.

For bowed notes there is no corresponding one-dimensional `p`/`f` law. Bow
speed primarily sets the Helmholtz-motion amplitude. Bow normal force must
remain between position- and speed-dependent Schelleng limits, and bow
position changes both tone colour and the playable force range. The existing
three independent bow controls are therefore the right interface shape; a
single dynamic macro, if added above it, must coordinate all three rather than
replace them.

## Published cello constants

The same 2016 paper reports measured transverse properties for D'Addario
Kaplan Solutions cello strings at a 690 mm effective length:

| String | Tension (N) | Mass/length (g/m) | Characteristic impedance (kg/s) |
|---|---:|---:|---:|
| A3 | 171.0 | 1.85 | 0.56 |
| D3 | 135.9 | 3.31 | 0.67 |
| G2 | 135.5 | 7.40 | 1.00 |
| C2 | 131.5 | 16.14 | 1.46 |

The compiled typical-property sheet hosted by Knut's Acoustics gives
multi-string ranges that contain those values. In A3-to-C2 order its
characteristic-impedance ranges are approximately 0.456–0.584,
0.597–0.725, 0.863–1.023, and 1.293–1.532 kg/s. The 2026-09-04 fixed profile now uses these measured values in C2-to-A3 order:
1.46, 1.00, 0.67, and 0.56 kg/s. Its evidence status remains
`violin-derived` because the remaining string, bridge, body, and gesture data
have not passed the later cello stages.

These constants describe a representative real string set, not a universal
cello. They are appropriate fixed physical inputs for one algorithmic model
and can be checked against published ranges without importing runtime samples.

## Revised evidence policy

Recordings remain useful, but their role is smaller:

- use equations and published physical constants to define excitation,
  geometry, impedance, and control scaling;
- use a few recordings to falsify the model's relative spectrum, decay, attack,
  and gesture behaviour;
- normalize nuisance recording gain, or compare within one calibrated chain;
- never treat `p`, `mf`, and `f` filenames from unrelated microphone chains as
  absolute dBFS calibration points;
- keep the untouched player/cello/session only as the final audit.

The 28-take development corpus can remain a robustness check. It should no
longer be the mechanism that determines the dynamic law.

## Development-corpus falsification

A predeclared 2026-09-04 rerun kept the 28 admitted recordings, four source
identities, folds, weights, passive-control grids, and gates unchanged. It did
not use absolute microphone dBFS to alter force scales or impedance.

D3 passes 12 grid points and selects a 7086.471 Hz bridge cutoff, 0.75-second
loss time, and 4 kHz nut cutoff. C2, G2, and A3 have no eligible point. Each
best point satisfies its aggregate source and error gates but overlaps only two
valid harmonics in one already-admitted RWC reference, below the frozen minimum
of three. The displacement initializer therefore fixes the wrong initial
condition and sharply improves C2, but does not yet provide enough robust
harmonic support for a four-string candidate.

The rerun verified all 960 stereo PCM24 renders, 66 focused analyzer tests, and
all 11 native/prepared-source/browser-WASM cello tests. Its predeclaration and
result SHA-256 values are respectively
`3f2e39cf3384aa76a86ad09f65c94a8bdff7695e0ae4553ae3bd5ba5287725de`
and
`36fa24974d5913f91e78717ab4a5112a10265ae6565b14249131258875eed496`.
A post-run live-selector change was repaired by preserving the exact
predeclared bytes and repeating the full artifact verification; repair receipt
SHA-256 is
`3700216c8e520539ed9dbab8322846ab03f6cf6f3adf60970b3cfbc435863fb8`.
No scientific output changed. No profile or candidate was written and the
reserved audit stayed sealed.
A follow-up that could select release time, pluck position, termination, or a
new loss range requires owner review and a new predeclaration.

## Sources

1. H. Mansour, J. Woodhouse, and G. P. Scavone, “Enhanced Wave-Based
   Modelling of Musical Strings. Part 1: Plucked Strings,” *Acta Acustica united
   with Acustica* 102 (2016), 1082–1093, especially Table 1 and Section 2.4.
   [Repository record and accepted manuscript](https://www.repository.cam.ac.uk/items/c55237bc-4a48-4972-a074-73fc6f97283b),
   [DOI](https://doi.org/10.3813/AAA.919021).
2. J. Woodhouse, “Synthesising plucked string sounds,” especially
   [the modal derivation](https://euphonics.org/5-4-1-motion-of-a-plucked-string-as-a-modal-sum/)
   and [overview](https://euphonics.org/5-3-synthesising-plucked-strings/).
3. J. O. Smith III, “The Ideal Plucked String” and “Pluck Modeling,”
   *Physical Audio Signal Processing*.
   [Initial conditions](https://www.dsprelated.com/freebooks/pasp/Ideal_Plucked_String.html),
   [plectrum model](https://www.dsprelated.com/freebooks/pasp/Pluck_Modeling.html).
4. M. Karjalainen, V. Välimäki, and T. Tolonen, “Plucked-String Models: From
   the Karplus-Strong Algorithm to Digital Waveguides and Beyond,” *Computer
   Music Journal* 22(3), 1998, 17–32.
   [Author page and full-text link](http://users.spa.aalto.fi/vpv/publications/cmj98.htm),
   [DOI](https://doi.org/10.2307/3681155).
5. S. Carral, “Plucking the String: The Excitation Mechanism of the Guitar,”
   ASA meeting paper summary, 2010. It reviews the effects of initial
   displacement, force, position, contact width, and angle.
   [ASA text](https://acoustics.org/pressroom/httpdocs/160th/carral.html).
6. “Some typical properties of bowed strings,” compiled physical-property
   sheet.
   [PDF](http://knutsacoustics.com/files/Typical-string-properties.pdf).
