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

Startup needs two hops of input; later search also needs lookahead. Zero-input
process calls can emit a pending synthesized hop. To exit or end a finite input,
`audio_stretch_drain` returns pending synthesis, the last overlap, and source
lookahead not already represented by that overlap. Nothing is padded, synthesized
or converted by drain. Repeated partial calls preserve order and report complete
only after all available tail data has been emitted. A positive-capacity drain
call latches drain mode; process then rejects requests until reset. Zero-capacity
drain calls are non-mutating queries. Reset discards retained data without freeing
storage, including a partially drained tail.

High-tempo processing may already have skipped source beyond the last overlap.
Drain reports that gap once via `gap_offset`, relative to the current output
buffer. The marker may equal `output_frames`, even on the final call: the gap
then precedes future caller input. Otherwise `(size_t)-1` means no new marker.
The owner must retain enough transition context to reconcile that discontinuity
(e.g. crossfade); the drain API does not invent skipped audio or guarantee a
click-free boundary. Entry/exit crossfades, stream epochs and device short writes
still require frontend integration before exposing a transport mode.

`make check` builds C89 tests and guards heap calls during processing/reset.
`make -B check CC="gcc -DAUDIO_STRETCH_SCALAR"` tests scalar selection.
`SANITIZER=address,undefined` is supported by the Linux CI target. Tests cover
fragmentation, canaries, fractional consumption, invalid requests, backpressure,
reset, partial native tail drains, gap markers, full-scale integer samples,
anti-phase/coherent channels, LFE exclusion
from the reference, duration bounds and a coarse 440 Hz pitch check. They do not
constitute listening or device-latency acceptance.

The tests print allocated bytes (including state) for 2/6/8 channels at several
rates. On x86-64, 48 kHz uses 4,488/9,608/12,168 bytes for int16 and
7,304/17,544/22,664 bytes for float. The largest supported eight-channel float
instance uses 90,248 bytes at 192 kHz. ABI padding can change these figures.

## Native transition spans

`audio_stretch_crossfade` blends caller-owned outgoing/incoming spans into caller
output without retaining state or allocating memory. Use one total frame count
and advance the offset across fragmented calls. The same Q16 linear weight is
shared by all channels; endpoints select the source exactly. Int16 uses convex
int64 accumulation with nearest rounding, ties away from zero. Float remains
float. The weight recurrence avoids per-frame division. Output can alias either
input exactly; partial overlap is unsupported. Length is bounded to 1..65536
frames, and a one-frame transition selects incoming.

The helper does not acquire history, schedule transitions or consume drain gap
markers. The runtime owner must retain suitable outgoing audio and select spans
before calling it. There is no added engine storage or normal-processing work.
Tests cover all 1..8 channels, both lanes, endpoints, full-range integer inputs,
fragmentation, exact aliasing, invalid requests and allocation guards. Playback
integration and listening/device acceptance remain pending.

## Transition owner

`audio_stretch_transition_new` allocates a single native tail ring and metadata.
It is optional: inactive playback must bypass it. The owner retains up to the
chosen tail length before emitting continuous audio. For example, 128 frames
adds 2.67 ms of holdback at 48 kHz. Large blocks copy their middle directly to
caller output; only the bounded tail passes through the ring. Small fragmented
calls can require two ring copies. Process, boundary, flush and reset allocate
nothing and never convert sample formats.

At a drain gap, submit all pre-gap frames to the transition owner, retrying any
unconsumed input. Then call `audio_stretch_transition_boundary` exactly once
before submitting post-gap frames. A gap at the end of a buffer applies before
future caller input. The retained outgoing tail overlaps incoming frames with
the shared native crossfade, shortening combined duration by the overlap length.
No search/alignment is performed at this boundary. If a boundary arrives before
the current overlap finishes, it is rejected without mutation; runtime policy
must handle rapid transitions explicitly. Do not silently drop that boundary.

Flush emits an ordinary retained tail and latches EOF until reset. If EOF occurs
after partial blending, unused outgoing frames are discarded. If no incoming
frames arrived, the original tail is preserved. Zero-capacity calls do not
mutate state. Reset discards retained data for an explicit stream discontinuity.
This owner does not own device writes, stream epochs or the engine itself.

Tests compare bulk and fragmented operation against a two-segment reference,
including short/empty streams, wrapped rings, partial EOF, reset and allocation
failure. Actual high-tempo engine drains are split at their gap markers and
rejoined with future source input in both native lanes. Runtime frontend wiring,
short-write ownership, entry scheduling and listening acceptance remain pending.

## Stream adapter

`audio_stretch_stream` owns the engine, transition owner and one hop of native
staging. Construction performs three allocations; processing and reset perform
none. All objects are single-consumer. The runtime must publish control changes
to that consumer and call reset at a stream discontinuity; the adapter does not
provide atomics, epoch publication or device I/O.

Process accepts a desired active flag and tempo with consumed/produced counts.
Raw state copies caller input directly. Activation starts the engine with empty
history. Exit drains pending synthesis/lookahead through the transition owner,
applies the reported gap boundary, joins future raw input if needed, flushes
retained transition audio and returns to raw. A subsequent activation waits for
this exit to finish. EOF drains without future input and latches until reset.
Zero-capacity calls do not mutate state, including zero-capacity EOF queries.

The adapter stages at most one hop between engine and transition owner, retaining
partial progress and gap markers across output backpressure. This adds bounded
native copies in active mode. The transition owner adds one hop of holdback in
addition to the engine's lookahead. The runtime must retain produced audio until
SRC/device consumers accept it; resetting or reprocessing a partially written
output buffer is incorrect. Default inactive frontend paths must bypass the
adapter entirely. No frontend setting is enabled by this patch.

Tests cover repeated raw/stretch transitions, rapid requests, EOF, allocation
failures at all three construction stages, reset during processing, output
canaries and bulk/fragmented equality. Uninterrupted active output is compared
against direct engine processing plus drain at slow, unity and fractional tempos.
Full runtime ownership, SRC capacity integration and hardware acceptance remain.
