/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 - The RetroArch team
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AUDIO_STRETCH_H
#define AUDIO_STRETCH_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct audio_stretch audio_stretch_t;

struct audio_stretch_io
{
   const void *input;
   void *output;
   size_t input_frames;
   size_t output_capacity;
   size_t input_used;
   size_t output_frames;
};

struct audio_stretch_drain_io
{
   void *output;
   size_t output_capacity;
   size_t output_frames;
   /* (size_t)-1 unless a source gap boundary occurs in this call.
    * May equal output_frames: the gap then precedes future output/input. */
   size_t gap_offset;
   bool complete;
};

/* Single-owner engine, interleaved native float or int16 throughout.
 * Rate: 8000..192000 Hz; channels: 1..8. search_channels is a nonzero
 * mask of channel indices, with LFE excluded by the caller. */
audio_stretch_t *audio_stretch_new(unsigned rate, unsigned channels,
      bool is_float, uint32_t search_channels);
void audio_stretch_free(audio_stretch_t *state);
void audio_stretch_reset(audio_stretch_t *state);
size_t audio_stretch_storage(const audio_stretch_t *state);
unsigned audio_stretch_hop(const audio_stretch_t *state);

/* Tempo: source frames per output frame, 0.25..32. Applied at hop
 * boundaries; fractional advance is retained across calls. Input and output
 * must not overlap. Counts describe actual consumption/production; retry
 * unconsumed input. Zero output capacity consumes nothing. No allocation,
 * device I/O or locking. Invalid requests return false without changing state.
 * A partial hop may be processed with zero input. After a positive-capacity
 * drain call, processing requires reset. Reset discards retained data.
 * Inactive transport must bypass this engine entirely. */
bool audio_stretch_process(audio_stretch_t *state, struct audio_stretch_io *io,
      double tempo);

/* Exit/EOF: drain pending synthesis, then the last overlap, then remaining
 * source lookahead, without padding or new input. No allocations/conversions.
 * A positive-capacity call latches drain mode until reset. Zero capacity is
 * non-mutating; complete reports whether any tail remains. Invalid arguments
 * return false without changing state. A gap is reported once, including at
 * the end of the final buffer if previously skipped source lies before future
 * caller input. The owner must handle that discontinuity (e.g. crossfade);
 * this function preserves available samples and does not invent missing ones. */
bool audio_stretch_drain(audio_stretch_t *state, struct audio_stretch_drain_io *io);

/* Blend matching native spans for entry/exit transitions. The caller owns
 * history and advances offset by frames across fragmented calls. total is
 * 1..65536 frames; a one-frame transition selects incoming. Output may equal
 * either input exactly; otherwise spans must not overlap. No allocation.
 * Invalid arguments return false without writing output. */
bool audio_stretch_crossfade(void *output, const void *outgoing,
      const void *incoming, size_t frames, unsigned channels, bool is_float,
      unsigned offset, unsigned total);

RETRO_END_DECLS
#endif
