#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <retro_spsc.h>
#include <rthreads/rthreads.h>
#include "../../../audio/audio_pipeline_layout.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int boundaries(void)
{
   audio_pipeline_layout_t q;
   unsigned i;
   size_t origin = SIZE_MAX - 127;
   audio_pipeline_layout_init(&q, 3);
   REQUIRE(audio_pipeline_layout_publish(&q, origin, 63));
   REQUIRE(audio_pipeline_layout_publish(&q, origin + 44, 255));
   REQUIRE(audio_pipeline_layout_publish(&q, origin + 132, 3));
   REQUIRE(audio_pipeline_layout_limit(&q, origin, 176, 256) == 44);
   REQUIRE(q.current_layout == 63);
   REQUIRE(audio_pipeline_layout_limit(&q, origin + 44, 132, 256) == 88);
   REQUIRE(q.current_layout == 255);
   REQUIRE(audio_pipeline_layout_limit(&q, origin + 132, 44, 256) == 44);
   REQUIRE(q.current_layout == 3);
   /* Consumer drops past two boundaries before taking its next span. */
   audio_pipeline_layout_init(&q, 3);
   REQUIRE(audio_pipeline_layout_publish(&q, origin, 63));
   REQUIRE(audio_pipeline_layout_publish(&q, origin + 44, 255));
   REQUIRE(audio_pipeline_layout_publish(&q, origin + 132, 3));
   REQUIRE(audio_pipeline_layout_limit(&q, origin + 88, 88, 256) == 44);
   REQUIRE(q.current_layout == 255);
   /* Full metadata, repeated controls, no audio between edges, index wrap. */
   audio_pipeline_layout_init(&q, 3);
   retro_atomic_size_init(&q.head, SIZE_MAX - 31);
   retro_atomic_size_init(&q.tail, SIZE_MAX - 31);
   for (i = 0; i < AUDIO_PIPELINE_LAYOUT_CAPACITY; i++)
      REQUIRE(audio_pipeline_layout_publish(&q, origin, i + 4));
   REQUIRE(!audio_pipeline_layout_publish(&q, origin, 999));
   REQUIRE(q.published_layout == AUDIO_PIPELINE_LAYOUT_CAPACITY + 3);
   REQUIRE(audio_pipeline_layout_publish(&q, origin, q.published_layout));
   REQUIRE(audio_pipeline_layout_limit(&q, origin, 0, 256) == 0);
   REQUIRE(q.current_layout == AUDIO_PIPELINE_LAYOUT_CAPACITY + 3);
   REQUIRE(audio_pipeline_layout_publish(&q, origin, 999));
   REQUIRE(audio_pipeline_layout_limit(&q, origin, 22, 256) == 22);
   REQUIRE(q.current_layout == 999);
   audio_pipeline_layout_init(&q, 3);
   REQUIRE(audio_pipeline_layout_limit(&q, 0, 22, 256) == 22);
   REQUIRE(q.current_layout == 3);
   return 0;
}

typedef struct
{
   retro_spsc_t audio;
   audio_pipeline_layout_t layout;
   size_t width;
   unsigned failures;
} stress_t;

static unsigned token_layout(unsigned token)
{
   static const unsigned layouts[] = { 3, 63, 255 };
   return layouts[(token / 3) % 3];
}

static void consume(void *arg)
{
   stress_t *s = (stress_t*)arg;
   unsigned token = 0;
   uint8_t data[256];
   while (token < 100000)
   {
      size_t bytes = retro_spsc_read_avail(&s->audio);
      size_t f, i;
      if (!bytes) continue;
      bytes = audio_pipeline_layout_limit(&s->layout,
            retro_atomic_load_relaxed_size(&s->audio.tail), bytes, s->audio.capacity);
      if (!bytes || bytes % s->width) { s->failures++; abort(); }
      if (retro_spsc_read(&s->audio, data, bytes) != bytes) abort();
      for (f = 0; f < bytes / s->width; f++, token++)
      {
         if (s->layout.current_layout != token_layout(token)) s->failures++;
         for (i = 0; i < s->width; i++)
            if (data[f * s->width + i] != (uint8_t)(token * 13 + i)) s->failures++;
      }
   }
}

static int stress(size_t width)
{
   stress_t s;
   sthread_t *thread;
   unsigned token;
   size_t i, origin = SIZE_MAX - 127;
   uint8_t frame[44];
   REQUIRE(retro_spsc_init(&s.audio, 256));
   s.width = width;
   s.failures = 0;
   audio_pipeline_layout_init(&s.layout, 3);
   retro_atomic_size_init(&s.audio.head, origin);
   retro_atomic_size_init(&s.audio.tail, origin);
   s.audio.cached_head = s.audio.cached_tail = origin;
   thread = sthread_create(consume, &s);
   REQUIRE(thread != NULL);
   for (token = 0; token < 100000; token++)
   {
      for (i = 0; i < width; i++) frame[i] = (uint8_t)(token * 13 + i);
      while (!audio_pipeline_layout_publish(&s.layout,
               retro_atomic_load_relaxed_size(&s.audio.head), token_layout(token))) ;
      while (!retro_spsc_write_frames(&s.audio, frame, 1, width)) ;
   }
   sthread_join(thread);
   REQUIRE(!s.failures);
   REQUIRE(!retro_spsc_read_avail(&s.audio));
   retro_spsc_free(&s.audio);
   return 0;
}

int main(void)
{
   if (boundaries() || stress(22) || stress(44)) return 1;
   printf("layout epochs: boundaries, saturation, reset, cursor wrap; 200000 concurrent frames pass (%u bytes metadata)\n",
         (unsigned)sizeof(audio_pipeline_layout_t));
   return 0;
}
