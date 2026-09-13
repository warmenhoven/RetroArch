#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../../audio/audio_stretch.h"

static unsigned failures, heap_calls;
static int guarded, fail_init;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void*, size_t);
void __real_free(void*);
void *__wrap_malloc(size_t n) { if (guarded) heap_calls++; return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t z)
{
   if (guarded) heap_calls++;
   return fail_init ? NULL : __real_calloc(n, z);
}
void *__wrap_realloc(void *p, size_t n) { if (guarded) heap_calls++; return __real_realloc(p, n); }
void __wrap_free(void *p) { if (guarded) heap_calls++; __real_free(p); }
#define CHECK(x) do { if (!(x)) { if (failures < 20) printf("FAIL %u: %s\n", (unsigned)__LINE__, #x); failures++; } } while (0)
#define FRAMES 8192
#define OUT_FRAMES (FRAMES * 4)
static float input_f[FRAMES * 8], output_f[2][OUT_FRAMES * 8];
static int16_t input_i[FRAMES * 8], output_i[2][OUT_FRAMES * 8];

static void fill(unsigned channels)
{
   unsigned f, c;
   for (f = 0; f < FRAMES; f++)
      for (c = 0; c < channels; c++)
      {
         int16_t v = (int16_t)(12000.0 * sin(6.283185307179586 * 440.0 * f / 48000));
         if (c == 1) v = (int16_t)-v;
         if (c == 3) v = (int16_t)((int)((f * 7919u) % 60000) - 30000);
         input_i[f * channels + c] = v;
         input_f[f * channels + c] = v / 32768.0f;
      }
}

static size_t run(audio_stretch_t *s, unsigned channels, int floating,
      double tempo, unsigned fragmented, unsigned slot)
{
   size_t used = 0, made = 0;
   unsigned iteration = 0;
   size_t sample = floating ? sizeof(float) : sizeof(int16_t);
   const void *input = floating ? (const void*)input_f : (const void*)input_i;
   void *output = floating ? (void*)output_f[slot] : (void*)output_i[slot];
   memset(output, 0x5a, OUT_FRAMES * channels * sample);
   guarded = 1;
   for (;;)
   {
      struct audio_stretch_io io;
      size_t offered = FRAMES - used;
      size_t capacity = OUT_FRAMES - made;
      if (fragmented)
      {
         size_t limit = 1 + (iteration * 17u) % 113;
         if (offered > limit) offered = limit;
         limit = 1 + (iteration * 31u) % 97;
         if (capacity > limit) capacity = limit;
      }
      io.input = (const char*)input + used * channels * sample;
      io.output = (char*)output + made * channels * sample;
      io.input_frames = offered; io.output_capacity = capacity;
      if (fragmented)
      {
         struct audio_stretch_io probe = io;
         CHECK(!audio_stretch_process(s, &probe, 0.0));
         CHECK(!probe.input_used && !probe.output_frames);
         probe = io; probe.output_capacity = 0;
         CHECK(audio_stretch_process(s, &probe, tempo));
         CHECK(!probe.input_used && !probe.output_frames);
      }
      CHECK(audio_stretch_process(s, &io, tempo));
      CHECK(io.input_used <= offered && io.output_frames <= capacity);
      used += io.input_used; made += io.output_frames;
      if (!io.input_used && !io.output_frames) break;
      if (++iteration > 100000) { CHECK(0); break; }
   }
   CHECK(used == FRAMES);
   CHECK(made < OUT_FRAMES);
   {
      size_t n;
      for (n = made * channels * sample; n < OUT_FRAMES * channels * sample; n++)
         CHECK(((const unsigned char*)output)[n] == 0x5a);
   }
   guarded = 0;
   return made;
}

static void stream_cases(void)
{
   const unsigned widths[] = {2, 6, 8};
   const double tempos[] = {0.25, 0.5, 1, 1.37, 2, 32};
   unsigned c, t, floating, f;
   for (c = 0; c < 3; c++)
      for (floating = 0; floating < 2; floating++)
      {
         unsigned channels = widths[c];
         uint32_t mask = ((1u << channels) - 1) & ~8u;
         audio_stretch_t *s = audio_stretch_new(48000, channels, floating, mask);
         CHECK(s != NULL);
         if (!s) exit(2);
         fill(channels);
         for (t = 0; t < 6; t++)
         {
            size_t a, b, sample = floating ? sizeof(float) : sizeof(int16_t);
            double error;
            guarded = 1; audio_stretch_reset(s); guarded = 0;
            a = run(s, channels, floating, tempos[t], 0, 0);
            guarded = 1; audio_stretch_reset(s); guarded = 0;
            b = run(s, channels, floating, tempos[t], 1, 1);
            CHECK(a == b);
            CHECK(memcmp(floating ? (void*)output_f[0] : (void*)output_i[0],
                     floating ? (void*)output_f[1] : (void*)output_i[1], a * channels * sample) == 0);
            error = fabs((double)a - FRAMES / tempos[t]);
            CHECK(error <= 3 * audio_stretch_hop(s) / tempos[t] + audio_stretch_hop(s));
            for (f = 0; f < a; f++)
            {
               if (floating)
               {
                  CHECK(output_f[0][f * channels] == -output_f[0][f * channels + 1]);
                  if (channels > 2) CHECK(output_f[0][f * channels] == output_f[0][f * channels + 2]);
               }
               else
               {
                  CHECK(output_i[0][f * channels] == -output_i[0][f * channels + 1]);
                  if (channels > 2) CHECK(output_i[0][f * channels] == output_i[0][f * channels + 2]);
               }
            }
            if (tempos[t] == 0.5 || tempos[t] == 2)
            {
               unsigned crossings = 0, begin = 512;
               for (f = begin + 1; f < a; f++)
               {
                  double x = floating ? output_f[0][(f - 1) * channels] : output_i[0][(f - 1) * channels];
                  double y = floating ? output_f[0][f * channels] : output_i[0][f * channels];
                  if (x <= 0 && y > 0) crossings++;
               }
               CHECK(fabs(crossings * 48000.0 / (a - begin) - 440.0) < 30.0);
            }
         }
         audio_stretch_free(s);
      }
}

static void scheduling(void)
{
   const double tempos[] = {0.25, 1.001, 1.37, 2.75, 32};
   unsigned t, k;
   memset(input_i, 0, sizeof(input_i));
   for (t = 0; t < 5; t++)
   {
      audio_stretch_t *s = audio_stretch_new(48000, 2, false, 3);
      size_t used = 0;
      unsigned hop = audio_stretch_hop(s);
      for (k = 0; k < 40; k++)
      {
         struct audio_stretch_io io;
         size_t expected = k ? (size_t)floor(k * hop * tempos[t] + 0.000001) + hop / 2 + 2 * hop : 2 * hop;
         if (expected > FRAMES) break;
         io.input = input_i + used * 2; io.input_frames = FRAMES - used;
         io.output = output_i[0]; io.output_capacity = hop;
         CHECK(audio_stretch_process(s, &io, tempos[t]));
         used += io.input_used;
         CHECK(io.output_frames == hop && used == expected);
      }
      audio_stretch_free(s);
   }
}

static void contracts(void)
{
   const unsigned rates[] = {8000, 44100, 48000, 96000, 192000};
   unsigned r, c, floating;
   struct audio_stretch_io io;
   uint64_t nan_bits = UINT64_C(0x7ff8000000000000);
   double nan;
   audio_stretch_t *s;
   memcpy(&nan, &nan_bits, sizeof(nan));
   CHECK(!audio_stretch_new(7999, 2, false, 3));
   CHECK(!audio_stretch_new(192001, 2, false, 3));
   CHECK(!audio_stretch_new(48000, 9, false, 3));
   CHECK(!audio_stretch_new(48000, 2, false, 0));
   CHECK(!audio_stretch_new(48000, 2, false, 4));
   fail_init = 1; CHECK(!audio_stretch_new(48000, 2, false, 3)); fail_init = 0;
   for (r = 0; r < 5; r++)
      for (c = 2; c <= 8; c += 2)
         for (floating = 0; floating < 2; floating++)
         {
            s = audio_stretch_new(rates[r], c, floating, 1);
            CHECK(s != NULL);
            if (!s) exit(2);
            CHECK(audio_stretch_hop(s) >= 21 && audio_stretch_hop(s) <= 512);
            CHECK(audio_stretch_storage(s) < 100000);
            if (c != 4) printf("storage rate=%u ch=%u float=%u bytes=%lu\n", rates[r], c, floating, (unsigned long)audio_stretch_storage(s));
            audio_stretch_free(s);
         }
   s = audio_stretch_new(48000, 2, false, 3);
   memset(input_i, 0x55, sizeof(input_i));
   memset(&io, 0, sizeof(io));
   io.input = input_i; io.input_frames = FRAMES;
   guarded = 1;
   CHECK(audio_stretch_process(s, &io, 2));
   CHECK(io.input_used == 0 && io.output_frames == 0);
   CHECK(!audio_stretch_process(s, &io, nan));
   CHECK(!audio_stretch_process(s, &io, 0));
   CHECK(!audio_stretch_process(s, &io, 32.01));
   io.output_capacity = 1;
   CHECK(!audio_stretch_process(s, &io, 1));
   io.output = output_i[0]; io.input_frames = (size_t)-1;
   CHECK(!audio_stretch_process(s, &io, 1));
   io.input_frames = FRAMES;
   CHECK(audio_stretch_process(s, &io, 1));
   CHECK(io.output_frames == 1);
   io.input = NULL; io.input_frames = 0;
   CHECK(audio_stretch_process(s, &io, 1));
   CHECK(io.output_frames == 1 && io.input_used == 0);
   audio_stretch_reset(s);
   guarded = 0;
   memset(input_i, 0, sizeof(input_i));
   CHECK(run(s, 2, 0, 1, 1, 0) > 0);
   CHECK(output_i[0][0] == 0);
   audio_stretch_free(s);
}

static void edge_rates(void)
{
   const unsigned rates[] = {8000, 44100, 192000};
   unsigned r, floating, c;
   for (r = 0; r < 3; r++)
      for (floating = 0; floating < 2; floating++)
         for (c = 1; c <= 8; c += 7)
         {
            audio_stretch_t *s = audio_stretch_new(rates[r], c, floating, 1);
            size_t a, b;
            fill(c);
            a = run(s, c, floating, 1.37, 0, 0);
            guarded = 1; audio_stretch_reset(s); guarded = 0;
            b = run(s, c, floating, 1.37, 1, 1);
            CHECK(a == b);
            CHECK(memcmp(floating ? (void*)output_f[0] : (void*)output_i[0],
                     floating ? (void*)output_f[1] : (void*)output_i[1],
                     a * c * (floating ? sizeof(float) : sizeof(int16_t))) == 0);
            audio_stretch_free(s);
         }
   for (r = 0; r < 2; r++)
   {
      audio_stretch_t *s = audio_stretch_new(192000, 8, false, 1);
      size_t n, i;
      int16_t value = r ? 32767 : -32768;
      for (i = 0; i < FRAMES * 8; i++) input_i[i] = value;
      n = run(s, 8, 0, 1.37, 1, 0);
      for (i = 0; i < n * 8; i++) CHECK(output_i[0][i] == value);
      audio_stretch_free(s);
   }
}

static void excluded_channel(void)
{
   unsigned floating, f, c;
   for (floating = 0; floating < 2; floating++)
   {
      audio_stretch_t *s = audio_stretch_new(48000, 6, floating, 0x37);
      size_t a, b;
      fill(6);
      a = run(s, 6, floating, 1.37, 0, 0);
      for (f = 0; f < FRAMES; f++)
      {
         input_i[f * 6 + 3] = 32767;
         input_f[f * 6 + 3] = 1.0f;
      }
      audio_stretch_reset(s);
      b = run(s, 6, floating, 1.37, 1, 1);
      CHECK(a == b);
      for (f = 0; f < a; f++)
         for (c = 0; c < 6; c++)
            if (c != 3)
            {
               if (floating) CHECK(output_f[0][f * 6 + c] == output_f[1][f * 6 + c]);
               else CHECK(output_i[0][f * 6 + c] == output_i[1][f * 6 + c]);
            }
      audio_stretch_free(s);
   }
}

static void tempo_changes(void)
{
   const double tempos[] = {1, 0.5, 2, 4, 0.25, 1.37, 1, 2};
   unsigned floating, h;
   fill(6);
   for (floating = 0; floating < 2; floating++)
   {
      audio_stretch_t *a = audio_stretch_new(48000, 6, floating, 0x37);
      audio_stretch_t *b = audio_stretch_new(48000, 6, floating, 0x37);
      size_t used_a = 0, used_b = 0;
      size_t frame = 6 * (floating ? sizeof(float) : sizeof(int16_t));
      const char *input = floating ? (const char*)input_f : (const char*)input_i;
      char *out_a = floating ? (char*)output_f[0] : (char*)output_i[0];
      char *out_b = floating ? (char*)output_f[1] : (char*)output_i[1];
      unsigned hop = audio_stretch_hop(a);
      guarded = 1;
      for (h = 0; h < 24; h++)
      {
         struct audio_stretch_io io;
         unsigned made = 0, iteration = 0;
         io.input = input + used_a * frame; io.input_frames = FRAMES - used_a;
         io.output = out_a; io.output_capacity = hop;
         CHECK(audio_stretch_process(a, &io, tempos[h % 8]));
         CHECK(io.output_frames == hop);
         used_a += io.input_used;
         while (made < hop)
         {
            size_t cap = 1 + (iteration * 13u) % hop;
            if (cap > hop - made) cap = hop - made;
            io.input = input + used_b * frame;
            io.input_frames = FRAMES - used_b;
            if (io.input_frames > 37) io.input_frames = 37;
            io.output = out_b + made * frame; io.output_capacity = cap;
            CHECK(audio_stretch_process(b, &io, tempos[h % 8]));
            used_b += io.input_used; made += (unsigned)io.output_frames;
            if (++iteration > 10000) { CHECK(0); break; }
         }
         CHECK(used_a == used_b);
         CHECK(memcmp(out_a, out_b, hop * frame) == 0);
      }
      guarded = 0;
      audio_stretch_free(a); audio_stretch_free(b);
   }
}

int main(void)
{
   contracts();
   scheduling();
   stream_cases();
   edge_rates();
   excluded_channel();
   tempo_changes();
   CHECK(heap_calls == 0);
   printf("stretch: %u failures, %u processing/reset heap calls\n", failures, heap_calls);
   return failures != 0;
}
