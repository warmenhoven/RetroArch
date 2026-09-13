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

#include <stdlib.h>
#include <string.h>
#include <audio/wsola_search.h>
#include "audio_stretch.h"

struct audio_stretch
{
   unsigned channels, hop, radius, capacity, head, count, next;
   unsigned pending, read;
   uint32_t fraction, search_channels;
   bool is_float, started;
   size_t bytes, frame_bytes;
   void *ring, *overlap, *output, *reference, *search, *window;
   wsola_corr_func_t correlation;
};

static unsigned astretch_index(const audio_stretch_t *s, unsigned offset)
{
   return (s->head + offset) % s->capacity;
}

static void astretch_copy(const audio_stretch_t *s, void *dst,
      unsigned offset, unsigned frames)
{
   unsigned index = astretch_index(s, offset);
   unsigned first = s->capacity - index;
   if (first > frames) first = frames;
   memcpy(dst, (const char*)s->ring + index * s->frame_bytes,
         first * s->frame_bytes);
   if (frames > first)
      memcpy((char*)dst + first * s->frame_bytes, s->ring,
            (frames - first) * s->frame_bytes);
}

static unsigned astretch_search(audio_stretch_t *s)
{
   unsigned c, f, selected = 0, begin, end, candidate, best, distance;
   unsigned index;
   double energy_f = 0.0, score_f = 0.0;
   uint64_t energy_i = 0;
   int64_t score_i = 0;
   begin = s->next > s->radius ? s->next - s->radius : 0;
   end = s->next + s->radius;
   best = s->next;
   distance = 0;
   for (c = 0; c < s->channels; c++)
   {
      if (!(s->search_channels & (1u << c))) continue;
      if (s->is_float)
      {
         double energy = 0.0;
         const float *p = (const float*)s->overlap + c;
         for (f = 0; f < s->hop; f++)
         {
            double v = p[f * s->channels];
            energy += v * v;
         }
         if (energy > energy_f) { energy_f = energy; selected = c; }
      }
      else
      {
         uint64_t energy = 0;
         const int16_t *p = (const int16_t*)s->overlap + c;
         for (f = 0; f < s->hop; f++)
         {
            int64_t v = p[f * s->channels];
            energy += (uint64_t)(v * v);
         }
         if (energy > energy_i) { energy_i = energy; selected = c; }
      }
   }
   if (s->is_float ? energy_f == 0.0 : energy_i == 0)
      return best;
   index = astretch_index(s, begin);
   for (f = 0; f < end - begin + s->hop; f++)
   {
      if (s->is_float)
         ((float*)s->search)[f] = ((const float*)s->ring)[index * s->channels + selected];
      else
         ((int32_t*)s->search)[f] = ((const int16_t*)s->ring)[index * s->channels + selected];
      if (++index == s->capacity) index = 0;
   }
   for (f = 0; f < s->hop; f++)
   {
      if (s->is_float)
         ((float*)s->reference)[f] = ((const float*)s->overlap)[f * s->channels + selected];
      else
         ((int32_t*)s->reference)[f] = ((const int16_t*)s->overlap)[f * s->channels + selected];
   }
   /* Seed at the nominal position so silence/equal scores cannot drift. */
   if (s->is_float)
      score_f = s->correlation((const float*)s->reference,
            (const float*)s->search + best - begin, s->hop, energy_f);
   else
      score_i = wsola_corr_i((const int32_t*)s->reference,
            (const int32_t*)s->search + best - begin, s->hop);
   for (candidate = begin; candidate <= end; candidate++)
   {
      unsigned d = candidate > s->next ? candidate - s->next : s->next - candidate;
      bool better;
      if (s->is_float)
      {
         double score = s->correlation((const float*)s->reference,
               (const float*)s->search + candidate - begin, s->hop, energy_f);
         better = score > score_f || (score == score_f && d < distance);
         if (better) score_f = score;
      }
      else
      {
         int64_t score = wsola_corr_i((const int32_t*)s->reference,
               (const int32_t*)s->search + candidate - begin, s->hop);
         better = score > score_i || (score == score_i && d < distance);
         if (better) score_i = score;
      }
      if (better) { best = candidate; distance = d; }
   }
   return best;
}

static void astretch_synthesize(audio_stretch_t *s, void *dst, unsigned start)
{
   unsigned f, c;
   unsigned a = astretch_index(s, start);
   unsigned b = astretch_index(s, start + s->hop);
   for (f = 0; f < s->hop; f++)
   {
      if (s->is_float)
      {
         float w = ((const float*)s->window)[f];
         for (c = 0; c < s->channels; c++)
         {
            unsigned o = f * s->channels + c;
            float previous = ((float*)s->overlap)[o];
            float current = ((const float*)s->ring)[a * s->channels + c];
            ((float*)dst)[o] = previous * (1.0f - w) + current * w;
            ((float*)s->overlap)[o] = ((const float*)s->ring)[b * s->channels + c];
         }
      }
      else
      {
         unsigned w = ((const uint16_t*)s->window)[f];
         for (c = 0; c < s->channels; c++)
         {
            unsigned o = f * s->channels + c;
            int64_t sum = (int64_t)((int16_t*)s->overlap)[o] * (32768 - w)
               + (int64_t)((const int16_t*)s->ring)[a * s->channels + c] * w;
            /* Convex Q15 mix; nearest, half away from zero, no clipping. */
            ((int16_t*)dst)[o] = (int16_t)(sum < 0
                  ? -((-sum + 16384) / 32768) : (sum + 16384) / 32768);
            ((int16_t*)s->overlap)[o] = ((const int16_t*)s->ring)[b * s->channels + c];
         }
      }
      if (++a == s->capacity) a = 0;
      if (++b == s->capacity) b = 0;
   }
}

audio_stretch_t *audio_stretch_new(unsigned rate, unsigned channels,
      bool is_float, uint32_t search_channels)
{
   audio_stretch_t *s;
   unsigned hop, radius, capacity, f;
   size_t sample, native_bytes, bytes;
   char *p;
   if (rate < 8000 || rate > 192000 || !channels || channels > 8
         || !search_channels || (search_channels >> channels)) return NULL;
   hop = (rate + 187) / 375;
   radius = hop / 2;
   capacity = 2 * hop + 2 * radius;
   sample = is_float ? sizeof(float) : sizeof(int16_t);
   native_bytes = (capacity + 2 * hop) * channels * sample;
   /* Round the native region up for float/int32 search alignment. */
   native_bytes = (native_bytes + 3) & ~(size_t)3;
   bytes = sizeof(*s) + native_bytes + (2 * hop + 2 * radius) * sizeof(int32_t)
      + hop * sample;
   s = (audio_stretch_t*)calloc(1, bytes);
   if (!s) return NULL;
   s->channels = channels; s->hop = hop; s->radius = radius;
   s->capacity = capacity; s->is_float = is_float;
   s->search_channels = search_channels;
   s->bytes = bytes; s->frame_bytes = channels * sample;
   p = (char*)(s + 1);
   s->ring = p;
   s->overlap = p + capacity * s->frame_bytes;
   s->output = p + (capacity + hop) * s->frame_bytes;
   s->reference = p + native_bytes;
   s->search = (char*)s->reference + hop * sizeof(int32_t);
   s->window = (char*)s->search + (hop + 2 * radius) * sizeof(int32_t);
   s->correlation = wsola_corr_get(WSOLA_SIMD_SCALAR);
#if !defined(AUDIO_STRETCH_SCALAR)
#if WSOLA_HAVE_SSE2
   s->correlation = wsola_corr_get(WSOLA_SIMD_SSE2);
#elif WSOLA_HAVE_NEON
   s->correlation = wsola_corr_get(WSOLA_SIMD_NEON);
#endif
#endif
   for (f = 0; f < hop; f++)
   {
      if (is_float) ((float*)s->window)[f] = (float)f / hop;
      else ((uint16_t*)s->window)[f] = (uint16_t)((f * 32768u) / hop);
   }
   return s;
}

void audio_stretch_free(audio_stretch_t *s) { free(s); }
size_t audio_stretch_storage(const audio_stretch_t *s) { return s ? s->bytes : 0; }
unsigned audio_stretch_hop(const audio_stretch_t *s) { return s ? s->hop : 0; }
void audio_stretch_reset(audio_stretch_t *s)
{
   if (!s) return;
   s->head = s->count = s->next = s->pending = s->read = 0;
   s->fraction = 0;
   s->started = false;
}

bool audio_stretch_process(audio_stretch_t *s, struct audio_stretch_io *io,
      double tempo)
{
   uint64_t bits, step;
   if (!io) return false;
   io->input_used = io->output_frames = 0;
   memcpy(&bits, &tempo, sizeof(bits));
   if (!s || bits >= UINT64_C(0x7ff0000000000000) || tempo < 0.25 || tempo > 32.0
         || (!io->input && io->input_frames) || (!io->output && io->output_capacity)
         || io->input_frames > (size_t)-1 / s->frame_bytes
         || io->output_capacity > (size_t)-1 / s->frame_bytes) return false;
   step = (uint64_t)(tempo * s->hop * 4294967296.0 + 0.5);
   while (io->output_frames < io->output_capacity)
   {
      size_t remaining = io->output_capacity - io->output_frames;
      void *dst = (char*)io->output + io->output_frames * s->frame_bytes;
      unsigned needed, start;
      if (s->pending)
      {
         size_t n = s->pending < remaining ? s->pending : remaining;
         memcpy(dst, (const char*)s->output + s->read * s->frame_bytes, n * s->frame_bytes);
         s->pending -= (unsigned)n; s->read += (unsigned)n;
         io->output_frames += n;
         continue;
      }
      if (s->started && s->next > s->radius)
      {
         unsigned drop = s->next - s->radius;
         size_t skip;
         if (drop > s->count) drop = s->count;
         s->head = astretch_index(s, drop);
         s->count -= drop; s->next -= drop;
         skip = s->next - s->radius;
         if (skip > io->input_frames - io->input_used) skip = io->input_frames - io->input_used;
         s->next -= (unsigned)skip; io->input_used += skip;
         if (s->next > s->radius) break;
      }
      needed = (s->started ? s->next + s->radius : 0) + 2 * s->hop;
      while (s->count < needed && io->input_used < io->input_frames)
      {
         unsigned index = astretch_index(s, s->count);
         size_t n = needed - s->count;
         if (n > s->capacity - index) n = s->capacity - index;
         if (n > io->input_frames - io->input_used) n = io->input_frames - io->input_used;
         memcpy((char*)s->ring + index * s->frame_bytes,
               (const char*)io->input + io->input_used * s->frame_bytes, n * s->frame_bytes);
         s->count += (unsigned)n; io->input_used += n;
      }
      if (s->count < needed) break;
      start = s->started ? astretch_search(s) : 0;
      if (remaining < s->hop) dst = s->output;
      if (s->started) astretch_synthesize(s, dst, start);
      else
      {
         astretch_copy(s, dst, 0, s->hop);
         astretch_copy(s, s->overlap, s->hop, s->hop);
         s->started = true;
      }
      if (remaining < s->hop) { s->pending = s->hop; s->read = 0; }
      else io->output_frames += s->hop;
      {
         uint64_t advance = step + s->fraction;
         s->next += (unsigned)(advance >> 32);
         s->fraction = (uint32_t)advance;
      }
   }
   return true;
}
