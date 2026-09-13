# Bounded native transport stretcher

This is an engine foundation, not an enabled frontend transport mode. The existing
WSOLA pitch DSP is unchanged. Normal playback does not call or feed this engine.

`audio_stretch_new` chooses one native sample format and allocates one contiguous
working set. The caller owns its lifetime and must serialize processing/reset.
The search mask uses **channel indices**, not speaker-position bits: the future
frontend adapter must map its layout and exclude LFE. The highest-energy eligible
overlap channel supplies the search reference; all channels use the same selected
offset. This avoids cancellation on anti-phase material. Ties prefer the nominal
position, then the closest candidate (lower index for equal distance).

At 48 kHz, synthesis hop is 128 frames, the overlap window spans 256 source
frames, and search radius is 64. Hop scales as round(rate / 375), with rates
restricted to 8..192 kHz and 1..8 channels. Radius is floor(hop / 2). The ring
holds only 2*hop + 2*radius frames, irrespective of tempo (0.25..32). High tempos
consume skipped source directly. A shared linear search neighborhood is staged
once per hop; sample storage stays native. Integer correlation uses int32
references and int64 accumulation, with native int16 overlap synthesis using
Q15 weights and nearest rounding, halfway away from zero. Float uses the shared
scalar/SSE2/NEON correlation selected once at creation.

Fractional source advance is Q32 with rounding error at most half a Q32 unit
per hop. Scheduling follows the nominal position, not the correlation offset,
so the search cannot accumulate tempo drift. Each synthesized hop schedules its
next analysis advance using that call's tempo. Large tempo changes should be
slewed by the future transport owner, not by an independent queue controller.

Processing reports actual input consumed and output produced. It emits directly
into caller storage whenever a complete hop fits. Otherwise it retains at most
one hop for partial-output calls. Zero output capacity consumes nothing. Invalid
parameters return false and leave state unchanged (result counts are zeroed).
Input/output storage must not overlap, and its sample format must match creation.

Startup needs two hops of input; later search also needs lookahead. There is no
EOF zero-padding or tail-flush operation. Zero-input calls can drain a pending
output hop, and reset discards all pending/history state without freeing storage.
The frontend integration must handle entry/exit crossfades and source tails,
stream epochs, and short device writes explicitly before exposing a mode.

`make check` builds C89 tests and guards heap calls during processing/reset.
`make -B check CC="gcc -DAUDIO_STRETCH_SCALAR"` tests scalar selection.
`SANITIZER=address,undefined` is supported by the Linux CI target. Tests cover
fragmentation, canaries, fractional consumption, invalid requests, backpressure,
reset, full-scale integer samples, anti-phase/coherent channels, LFE exclusion
from the reference, duration bounds and a coarse 440 Hz pitch check. They do not
constitute listening or device-latency acceptance.

The tests print allocated bytes (including state) for 2/6/8 channels at several
rates. On x86-64, 48 kHz uses 4,472/9,592/12,152 bytes for int16 and
7,288/17,528/22,648 bytes for float. The largest supported eight-channel float
instance uses 90,232 bytes at 192 kHz. ABI padding can change these figures.
