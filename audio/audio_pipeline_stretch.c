/* Copyright (C) 2026 - The RetroArch team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>
#include "audio_pipeline_stretch.h"
#include "audio_stretch.h"

struct audio_pipeline_stretch
{
   audio_stretch_stream_t *stream;
   retro_spsc_t *ring;
   audio_pipeline_layout_t *metadata;
   const uint8_t *direct;
   size_t direct_frames, offered, frame_bytes, sample_bytes;
   unsigned layout;
   uint32_t seen_reset, reset_serial;
   bool draining_layout;
   union { float f[AUDIO_STRETCH_MAX_CHANNELS]; int16_t i[AUDIO_STRETCH_MAX_CHANNELS]; } wrap;
};

audio_pipeline_stretch_t *audio_pipeline_stretch_new(unsigned rate,
      unsigned channels, bool is_float, uint32_t search_channels,
      retro_spsc_t *ring, audio_pipeline_layout_t *metadata,
      void *output, size_t output_frames)
{
   audio_pipeline_stretch_t *s;
   size_t sample = is_float ? sizeof(float) : sizeof(int16_t);
   if (!ring || !metadata || !output || !output_frames
         || !channels || channels > AUDIO_STRETCH_MAX_CHANNELS
         || (uintptr_t)output % sample || output_frames > SIZE_MAX / (channels * sample)
         || ring->capacity < channels * sample) return NULL;
   s = (audio_pipeline_stretch_t*)calloc(1, sizeof(*s));
   if (!s) return NULL;
   s->stream = audio_stretch_stream_new(rate, channels, is_float, search_channels);
   if (!s->stream || !audio_stretch_stream_bind(s->stream, output, output_frames))
   {
      audio_pipeline_stretch_free(s);
      return NULL;
   }
   s->ring = ring; s->metadata = metadata;
   s->frame_bytes = channels * sample; s->sample_bytes = sample;
   s->layout = metadata->current_layout; s->seen_reset = metadata->reset_serial;
   return s;
}

void audio_pipeline_stretch_free(audio_pipeline_stretch_t *s)
{
   if (!s) return;
   audio_stretch_stream_free(s->stream);
   free(s);
}

static void apstretch_offer(audio_pipeline_stretch_t *s,
      struct audio_pipeline_stretch_block *block, const void *data,
      size_t frames, size_t budget)
{
   if (frames > budget) frames = budget;
   block->data = frames ? data : NULL;
   block->frames = s->offered = frames;
   block->passthrough = frames && s->direct_frames;
   block->layout = s->layout;
   block->reset_serial = s->reset_serial;
}

bool audio_pipeline_stretch_next(audio_pipeline_stretch_t *s,
      size_t input_budget, size_t output_budget,
      struct audio_pipeline_stretch_block *block)
{
   const void *data;
   size_t frames, bytes, span, used = 0;
   uint32_t control;
   bool active;
   if (!block) return false;
   block->data = NULL; block->frames = block->input_used = 0;
   block->passthrough = false;
   block->layout = s ? s->layout : 0;
   block->reset_serial = s ? s->reset_serial : 0;
   if (!s) return false;
   if (!output_budget) return true;
   if (s->direct_frames)
   {
      apstretch_offer(s, block, s->direct, s->direct_frames, output_budget);
      return true;
   }
   data = audio_stretch_stream_peek(s->stream, &frames);
   if (frames)
   {
      apstretch_offer(s, block, data, frames, output_budget);
      return true;
   }
   if (s->draining_layout)
   {
      bool complete;
      if (!audio_stretch_stream_finish_limit(s->stream, &complete, output_budget))
         return false;
      if (!complete)
      {
         data = audio_stretch_stream_peek(s->stream, &frames);
         apstretch_offer(s, block, data, frames, output_budget);
         return true;
      }
      audio_stretch_stream_reset(s->stream);
      s->layout = s->metadata->current_layout;
      s->seen_reset = s->metadata->reset_serial;
      s->reset_serial++;
      s->draining_layout = false;
   }
   bytes = retro_spsc_read_avail(s->ring);
   if (bytes % s->frame_bytes) return false;
   frames = bytes / s->frame_bytes;
   if (frames > input_budget) frames = input_budget;
   bytes = audio_pipeline_layout_limit_transport(s->metadata,
         retro_atomic_load_relaxed_size(&s->ring->tail),
         frames * s->frame_bytes, s->ring->capacity);
   if (bytes % s->frame_bytes) return false;
   if (s->layout != s->metadata->current_layout
         && s->seen_reset == s->metadata->reset_serial
         && !audio_stretch_stream_quiescent(s->stream))
   {
      bool complete;
      if (!audio_stretch_stream_finish_limit(s->stream, &complete, output_budget))
         return false;
      if (!complete)
      {
         s->draining_layout = true;
         data = audio_stretch_stream_peek(s->stream, &frames);
         apstretch_offer(s, block, data, frames, output_budget);
         return true;
      }
   }
   if (s->layout != s->metadata->current_layout
         || s->seen_reset != s->metadata->reset_serial)
   {
      audio_stretch_stream_reset(s->stream);
      s->layout = s->metadata->current_layout;
      s->seen_reset = s->metadata->reset_serial;
      s->reset_serial++;
   }
   frames = bytes / s->frame_bytes;
   data = NULL;
   if (frames)
   {
      span = retro_spsc_read_begin(s->ring, &data) / s->frame_bytes;
      /* An incomplete or unaligned physical frame needs just one-frame
       * staging. The source remains owned by the ring until acknowledged. */
      if (!span || (uintptr_t)data % s->sample_bytes)
      {
         retro_spsc_read_end(s->ring, 0);
         if (retro_spsc_peek(s->ring, &s->wrap, s->frame_bytes) != s->frame_bytes)
            return false;
         data = &s->wrap; frames = 1;
      }
      else
      {
         if (frames > span) frames = span;
         retro_spsc_read_end(s->ring, 0);
      }
   }
   control = s->metadata->current_control;
   active = (control & AUDIO_PIPELINE_STRETCH) != 0;
   if (!active && audio_stretch_stream_quiescent(s->stream))
   {
      if (frames > output_budget) frames = output_budget;
      s->direct = (const uint8_t*)data;
      s->direct_frames = frames;
      apstretch_offer(s, block, data, frames, output_budget);
      return true;
   }
   if (!audio_stretch_stream_push_limit(s->stream, data, frames, &used,
            (double)(control & AUDIO_PIPELINE_TEMPO_MASK) / 65536.0,
            active, output_budget)) return false;
   if (used && retro_spsc_skip(s->ring, used * s->frame_bytes) != used * s->frame_bytes)
      return false;
   block->input_used = used;
   data = audio_stretch_stream_peek(s->stream, &frames);
   apstretch_offer(s, block, data, frames, output_budget);
   return true;
}

bool audio_pipeline_stretch_consume(audio_pipeline_stretch_t *s, size_t frames)
{
   if (!s || frames > s->offered) return false;
   if (!frames) return true;
   if (s->direct_frames)
   {
      size_t bytes = frames * s->frame_bytes;
      if (retro_spsc_skip(s->ring, bytes) != bytes) return false;
      s->direct += bytes; s->direct_frames -= frames;
   }
   else if (!audio_stretch_stream_consume(s->stream, frames)) return false;
   s->offered -= frames;
   return true;
}

bool audio_pipeline_stretch_finish(audio_pipeline_stretch_t *s,
      size_t output_budget, struct audio_pipeline_stretch_block *block,
      bool *complete)
{
   const void *data;
   size_t frames;
   if (complete) *complete = false;
   if (!block || !complete) return false;
   block->data = NULL; block->frames = block->input_used = 0;
   block->passthrough = false;
   block->layout = s ? s->layout : 0;
   block->reset_serial = s ? s->reset_serial : 0;
   if (!s || retro_spsc_read_avail(s->ring)) return false;
   if (!audio_stretch_stream_finish_limit(s->stream, complete, output_budget))
      return false;
   if (!output_budget) return true;
   data = audio_stretch_stream_peek(s->stream, &frames);
   apstretch_offer(s, block, data, frames, output_budget);
   return true;
}

bool audio_pipeline_stretch_discard(audio_pipeline_stretch_t *s, size_t frames)
{
   if (!s || frames > retro_spsc_read_avail(s->ring) / s->frame_bytes) return false;
   if (frames && retro_spsc_skip(s->ring, frames * s->frame_bytes) != frames * s->frame_bytes)
      return false;
   audio_stretch_stream_reset(s->stream);
   s->direct = NULL; s->direct_frames = s->offered = 0;
   s->draining_layout = false;
   s->reset_serial++;
   return true;
}
