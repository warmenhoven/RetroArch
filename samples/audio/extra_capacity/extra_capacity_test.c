#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned heap_calls;
static struct { void *ptr; size_t size; } allocations[128];
static void *tracked_malloc(size_t size)
{
   unsigned i;
   void *p = malloc(size);
   heap_calls++;
   if (!p) return NULL;
   for (i = 0; i < 128; i++)
      if (!allocations[i].ptr || allocations[i].ptr == p)
      {
         allocations[i].ptr = p;
         allocations[i].size = size;
         return p;
      }
   abort();
   return NULL;
}
static size_t allocation_size(void *p)
{
   unsigned i;
   for (i = 0; i < 128; i++)
      if (allocations[i].ptr == p) return allocations[i].size;
   return 0;
}
#define malloc tracked_malloc
#include "../../../audio/audio_driver.c"
#undef malloc

/* Only the resampler factory is stubbed; preparation and processing are real. */
bool retro_resampler_realloc(void **re, const retro_resampler_t **backend,
      const char *ident, enum resampler_quality quality, double ratio)
{
   (void)ident;
   if (*re && *backend) (*backend)->free(*re);
   *backend = &sinc_resampler;
   *re = sinc_resampler.init(NULL, ratio, quality, 0);
   return *re != NULL;
}

static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

static void check_lane(int integer)
{
   static audio_driver_state_t st;
   unsigned step, i;
   void *front;
   static float output_f[16384 * 2];
   static int16_t output_i[16384 * 2];
   const size_t counts[] = {32, 512, 1024, 16, 1024};
   memset(&st, 0, sizeof(st));
   st.resampler = &sinc_resampler;
   st.resampler_quality = RESAMPLER_QUALITY_NORMAL;
   st.src_ratio_orig = 8.0;
   st.output_samples_buf_length = 16384 * 2 * sizeof(float);
   st.output_samples_int16_length = 16384 * 2 * sizeof(int16_t);
   st.resampler_int16_process = sinc_resampler_int16_process;
   st.resampler_int16_free = sinc_resampler_int16_free;
   front = integer ? sinc_resampler_int16_init(8.0, SINC_INT16_QUALITY_NORMAL)
      : sinc_resampler.init(NULL, 8.0, RESAMPLER_QUALITY_NORMAL, 0);
   if (!front) exit(2);
   for (step = 0; step < 5; step++)
   {
      unsigned before = heap_calls;
      size_t produced;
      CHECK(audio_driver_extra_prepare(&st, 2, 0, counts[step], !integer, integer));
      CHECK(st.extra.cap_out >= 16384);
      if (step >= 3) CHECK(heap_calls == before);
      CHECK(allocation_size(st.extra.pair_in) >= counts[step] * 2 * sizeof(float));
      CHECK(allocation_size(st.extra.pair_in_i) >= counts[step] * 2 * sizeof(int16_t));
      if (allocation_size(st.extra.pair_in) < counts[step] * 2 * sizeof(float)
            || allocation_size(st.extra.pair_in_i) < counts[step] * 2 * sizeof(int16_t)) break;
      /* Refuse to exercise unsafe allocations when run against the old code. */
      if (st.extra.cap_out < 16384) break;
      for (i = 0; i < counts[step] * 2; i++)
      {
         if (integer) st.extra.in_i[i] = (int16_t)(i * 53);
         else st.extra.in_f[i] = (float)(i % 256) / 512.0f;
      }
      if (integer)
      {
         struct resampler_data_int16 d;
         d.data_in = st.extra.in_i; d.data_out = output_i;
         d.input_frames = counts[step]; d.ratio = 8.0;
         sinc_resampler_int16_process(front, &d); produced = d.output_frames;
      }
      else
      {
         struct resampler_data d;
         d.data_in = st.extra.in_f; d.data_out = output_f;
         d.input_frames = counts[step]; d.ratio = 8.0;
         sinc_resampler.process(front, &d); produced = d.output_frames;
      }
      before = heap_calls;
      st.extra.pending = true;
      audio_driver_extra_resample(&st, 8.0, counts[step], false, integer);
      CHECK(st.extra.out_frames >= counts[step] * 8);
      CHECK(st.extra.out_frames <= st.extra.cap_out);
      CHECK(heap_calls == before);
      CHECK(st.extra.out_frames == produced);
      if (integer) CHECK(memcmp(st.extra.out_i, output_i, produced * 2 * sizeof(int16_t)) == 0);
      else CHECK(memcmp(st.extra.out_f, output_f, produced * 2 * sizeof(float)) == 0);
   }
   audio_driver_extra_free(&st);
   if (integer) sinc_resampler_int16_free(front);
   else sinc_resampler.free(front);
}

static unsigned reset_calls;
static void count_reset(void *state)
{
   reset_calls++;
   sinc_resampler.reset(state);
}

static void check_bypass(void)
{
   static audio_driver_state_t st;
   static float front_input[256 * 2], front_output[1024 * 2];
   retro_resampler_t backend = sinc_resampler;
   void *front;
   unsigned pass, i, before;
   backend.reset = count_reset;
   memset(&st, 0, sizeof(st));
   st.resampler = &backend;
   st.resampler_quality = RESAMPLER_QUALITY_NORMAL;
   st.src_ratio_orig = 1.0;
   st.output_samples_buf_length = sizeof(front_output);
   /* Four extras exercise two independently owned histories. */
   CHECK(audio_driver_extra_prepare(&st, 4, 0, 256, true, false));
   front = sinc_resampler.init(NULL, 1.0, RESAMPLER_QUALITY_NORMAL, 0);
   if (!front) exit(2);
   for (i = 0; i < 256; i++)
   {
      unsigned ch;
      for (ch = 0; ch < 4; ch++)
         st.extra.in_f[4 * i + ch] = (float)((i + ch) % 23) / 32.0f;
   }
   st.extra.pending = true;
   audio_driver_extra_resample(&st, 1.5, 256, false, false);
   reset_calls = 0;
   before = heap_calls;
   for (pass = 0; pass < 3; pass++)
   {
      st.extra.pending = true;
      audio_driver_extra_resample(&st, 1.0, 256, true, false);
      CHECK(st.extra.out_frames == 256);
      CHECK(memcmp(st.extra.out_f, st.extra.in_f, 256 * 4 * sizeof(float)) == 0);
      CHECK(reset_calls == 2);
   }
   memset(st.extra.in_f, 0, 256 * 4 * sizeof(float));
   memset(front_input, 0, sizeof(front_input));
   for (pass = 0; pass < 2; pass++)
   {
      struct resampler_data d;
      d.data_in = front_input; d.data_out = front_output;
      d.input_frames = 256; d.ratio = 1.5;
      sinc_resampler.process(front, &d);
      st.extra.pending = true;
      audio_driver_extra_resample(&st, 1.5, 256, false, false);
      CHECK(st.extra.out_frames == d.output_frames);
      for (i = 0; i < d.output_frames * 4; i++)
         CHECK(st.extra.out_f[i] == front_output[(i / 4) * 2 + (i % 2)]);
      CHECK(reset_calls == 2);
   }
   CHECK(heap_calls == before);
   audio_driver_extra_free(&st);
   sinc_resampler.free(front);
}

int main(void)
{
   check_lane(0);
   check_lane(1);
   check_bypass();
   printf("extra capacity: %u failures\n", failures);
   return failures != 0;
}
