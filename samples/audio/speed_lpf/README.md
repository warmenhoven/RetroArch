# Native low-pass core

Run `make check` (C89); `make check SANITIZER=address,undefined` enables
sanitizers on supported toolchains. The 60 cases cover both native formats,
all 1..11 channel widths at 8/44.1/48/96/192 kHz, chunk invariance with changing
targets and reset, dry bit identity, invalid arguments, full-scale DC,
silence decay, channel isolation, native-lane agreement and measured response.
Allocator wrappers assert that initialization, controls, processing and reset
make no heap calls.

This is an independent caller-owned component, not an enabled playback mode.
Future integration must own it on the audio processor, after transport and
before SRC, supply core-rate cutoff Hz, and define the speed-to-cutoff policy.
No playback settings or default pipeline calls are added here.

Two cascaded real poles give a combined -3 dB cutoff. Each pole's coefficient
stays between zero and one throughout interpolation. Float uses float history;
int16 uses Q16 history and Q30 coefficients with signed 64-bit products.
Integer convex updates cannot exceed native input extrema. Rounding can leave
sub-sample internal residue at silence, but tests require zero native output.
The float lane flushes magnitudes below 1e-20 while wet to avoid denormals.

Engagement primes history from the first actual sample and fades over 50 ms.
Cutoff changes interpolate at a 64-source-frame cadence, reaching the exact
endpoint after 50 ms; callback splitting does not change this clock. Repeated
targets do not restart ramps. Disengagement stops touching samples/history
when dry. Reset clears history and restarts engagement for an enabled target.
Control and process calls belong to one owner; no internal locks are provided.
