/* Copyright (C) 2026 - The RetroArch team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AUDIO_PIPELINE_LAYOUT_H
#define AUDIO_PIPELINE_LAYOUT_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_atomic.h>
#include <retro_inline.h>

#define AUDIO_PIPELINE_LAYOUT_CAPACITY 64

/* Metadata only. One producer publishes a boundary BEFORE the corresponding
 * audio head. One consumer observes the audio head BEFORE calling limit.
 * Audio positions use the byte ring's wrapping size_t counters. */
typedef struct audio_pipeline_layout
{
   retro_atomic_size_t head;
   unsigned published_layout;
   uint8_t producer_pad[64];
   retro_atomic_size_t tail;
   unsigned current_layout;
   uint8_t consumer_pad[64];
   struct { size_t position; unsigned layout; } events[AUDIO_PIPELINE_LAYOUT_CAPACITY];
} audio_pipeline_layout_t;

/* Initialize/reset only with both owners stopped, alongside the audio ring. */
static INLINE void audio_pipeline_layout_init(audio_pipeline_layout_t *q,
      unsigned layout)
{
   retro_atomic_size_init(&q->head, 0);
   retro_atomic_size_init(&q->tail, 0);
   q->published_layout = q->current_layout = layout;
}

/* Producer only. False means metadata is full: do not publish new-layout
 * audio until this succeeds. Unchanged layouts touch no shared cursor. */
static INLINE bool audio_pipeline_layout_publish(audio_pipeline_layout_t *q,
      size_t position, unsigned layout)
{
   size_t head, tail, slot;
   if (layout == q->published_layout) return true;
   head = retro_atomic_load_relaxed_size(&q->head);
   tail = retro_atomic_load_acquire_size(&q->tail);
   if (head - tail == AUDIO_PIPELINE_LAYOUT_CAPACITY) return false;
   slot = head & (AUDIO_PIPELINE_LAYOUT_CAPACITY - 1);
   q->events[slot].position = position;
   q->events[slot].layout   = layout;
   q->published_layout     = layout;
   retro_atomic_store_release_size(&q->head, head + 1);
   return true;
}

/* Consumer only. Bound a read to one layout and retire boundaries passed by
 * an explicit consumer discard. capacity is the audio ring's capacity (at
 * most SIZE_MAX/2); bytes was obtained from its acquired head snapshot. */
static INLINE size_t audio_pipeline_layout_limit(audio_pipeline_layout_t *q,
      size_t position, size_t bytes, size_t capacity)
{
   size_t tail = retro_atomic_load_relaxed_size(&q->tail);
   size_t head = retro_atomic_load_acquire_size(&q->head);
   size_t first = tail;
   while (tail != head)
   {
      size_t slot = tail & (AUDIO_PIPELINE_LAYOUT_CAPACITY - 1);
      size_t distance = q->events[slot].position - position;
      if (distance && distance <= capacity)
      {
         if (bytes > distance) bytes = distance;
         break;
      }
      q->current_layout = q->events[slot].layout;
      tail++;
   }
   if (tail != first) retro_atomic_store_release_size(&q->tail, tail);
   return bytes;
}
#endif
