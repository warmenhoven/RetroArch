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

static size_t drain_run(audio_stretch_t *s, unsigned channels, int floating,
      unsigned fragmented, unsigned slot, size_t *gap)
{
   size_t made = 0, sample = floating ? sizeof(float) : sizeof(int16_t);
   char *output = floating ? (char*)output_f[slot] : (char*)output_i[slot];
   unsigned iteration = 0;
   struct audio_stretch_drain_io io;
   *gap = (size_t)-1;
   memset(output, 0x5a, OUT_FRAMES * channels * sample);
   guarded = 1;
   for (;;)
   {
      size_t capacity = fragmented ? 1 + (iteration * 7u) % 67 : OUT_FRAMES;
      io.output = NULL; io.output_capacity = 1;
      CHECK(!audio_stretch_drain(s, &io));
      CHECK(!io.output_frames && io.gap_offset == (size_t)-1 && !io.complete);
      io.output = output; io.output_capacity = (size_t)-1;
      CHECK(!audio_stretch_drain(s, &io));
      io.output_capacity = 0;
      CHECK(audio_stretch_drain(s, &io));
      CHECK(!io.output_frames && io.gap_offset == (size_t)-1);
      io.output = output + made * channels * sample;
      io.output_capacity = capacity;
      CHECK(audio_stretch_drain(s, &io));
      CHECK(io.output_frames <= capacity);
      if (io.gap_offset != (size_t)-1)
      {
         CHECK(*gap == (size_t)-1 && io.gap_offset <= io.output_frames);
         *gap = made + io.gap_offset;
      }
      made += io.output_frames;
      if (io.complete) break;
      CHECK(io.output_frames != 0);
      if (++iteration > 100000) { CHECK(0); break; }
   }
   io.output_capacity = 0; io.output = NULL;
   CHECK(audio_stretch_drain(s, &io) && io.complete && !io.output_frames);
   io.output = output + made * channels * sample; io.output_capacity = 1;
   CHECK(audio_stretch_drain(s, &io) && io.complete && !io.output_frames);
   CHECK(io.gap_offset == (size_t)-1);
   {
      struct audio_stretch_io probe;
      size_t n;
      memset(&probe, 0, sizeof(probe));
      CHECK(!audio_stretch_process(s, &probe, 1));
      CHECK(!probe.input_used && !probe.output_frames);
      for (n = made * channels * sample; n < OUT_FRAMES * channels * sample; n++)
         CHECK((unsigned char)output[n] == 0x5a);
   }
   guarded = 0;
   return made;
}

static size_t prepare_drain(audio_stretch_t *s, unsigned channels, int floating,
      unsigned scenario, size_t *prefix)
{
   static float scratch_f[6 * 512 * 8];
   static int16_t scratch_i[6 * 512 * 8];
   unsigned hop = audio_stretch_hop(s);
   size_t frame = channels * (floating ? sizeof(float) : sizeof(int16_t));
   size_t used;
   double tempo = scenario == 4 ? 4 : scenario == 5 ? 32 : scenario == 6 ? 1.37 : 2;
   const char *input = floating ? (const char*)input_f : (const char*)input_i;
   struct audio_stretch_io io;
   struct audio_stretch_drain_io query;
   memset(&query, 0, sizeof(query));
   guarded = 1;
   CHECK(audio_stretch_drain(s, &query) && query.complete);
   io.input = input; io.input_frames = scenario == 0 ? hop - 1 : 2 * hop;
   io.output = floating ? (void*)scratch_f : (void*)scratch_i;
   io.output_capacity = scenario == 1 ? 1 : scenario == 7 ? hop / 2 : hop;
   if (scenario == 6) { io.input_frames = FRAMES; io.output_capacity = 6 * hop; }
   if (scenario == 8) io.input_frames = 0;
   CHECK(audio_stretch_process(s, &io, tempo));
   used = io.input_used; *prefix = io.output_frames;
   if (scenario == 3 || scenario == 4 || scenario == 5 || scenario == 7)
   {
      io.input = input + used * frame;
      io.input_frames = scenario == 4 ? 2 * hop - hop / 2 + hop / 4 : hop / 4;
      io.output_capacity = hop;
      if (scenario == 7) { io.input_frames = 0; io.output_capacity = hop / 4; }
      CHECK(audio_stretch_process(s, &io, tempo));
      if (scenario != 7) CHECK(!io.output_frames);
      used += io.input_used; *prefix += io.output_frames;
   }
   guarded = 0;
   return used;
}

static void drain_cases(void)
{
   const unsigned rates[] = {8000, 48000, 192000};
   const unsigned widths[] = {1, 6, 8};
   unsigned r, w, floating, scenario, f, c;
   for (r = 0; r < 3; r++)
      for (w = 0; w < 3; w++)
         for (floating = 0; floating < 2; floating++)
         {
            unsigned channels = widths[w];
            size_t frame = channels * (floating ? sizeof(float) : sizeof(int16_t));
            const char *input = floating ? (const char*)input_f : (const char*)input_i;
            const char *a = floating ? (const char*)output_f[0] : (const char*)output_i[0];
            const char *b = floating ? (const char*)output_f[1] : (const char*)output_i[1];
            for (f = 0; f < FRAMES; f++)
               for (c = 0; c < channels; c++)
               {
                  input_i[f * channels + c] = (int16_t)(f * 3 + c);
                  input_f[f * channels + c] = (float)(f * 16 + c);
               }
            for (scenario = 0; scenario < 9; scenario++)
            {
               audio_stretch_t *s = audio_stretch_new(rates[r], channels, floating, 1);
               unsigned hop = audio_stretch_hop(s);
               size_t used, prefix, n, m, gap_a, gap_b, start;
               struct audio_stretch_io io;
               used = prepare_drain(s, channels, floating, scenario, &prefix);
               n = drain_run(s, channels, floating, 0, 0, &gap_a);
               audio_stretch_reset(s);
               CHECK(prepare_drain(s, channels, floating, scenario, &start) == used);
               CHECK(start == prefix);
               m = drain_run(s, channels, floating, 1, 1, &gap_b);
               CHECK(n == m && gap_a == gap_b);
               CHECK(memcmp(a, b, n * frame) == 0);
               if (scenario == 6)
               {
                  start = floating ? (size_t)output_f[1][0] / 16 : (size_t)output_i[1][0] / 3;
                  CHECK(n == used - start && gap_b == (size_t)-1);
                  CHECK(memcmp(b, input + start * frame, n * frame) == 0);
               }
               else if (scenario == 4 || scenario == 5)
               {
                  CHECK(gap_b == hop);
                  CHECK(memcmp(b, input + hop * frame, hop * frame) == 0);
                  if (scenario == 4)
                  {
                     start = 4 * hop - hop / 2;
                     CHECK(n == hop + used - start);
                     CHECK(memcmp(b + hop * frame, input + start * frame, (used - start) * frame) == 0);
                  }
                  else CHECK(n == hop);
               }
               else
               {
                  CHECK(n == used - prefix && gap_b == (size_t)-1);
                  CHECK(memcmp(b, input + prefix * frame, n * frame) == 0);
               }
               guarded = 1; audio_stretch_reset(s); guarded = 0;
               io.input = input; io.input_frames = 2 * hop;
               io.output = (void*)a; io.output_capacity = hop;
               CHECK(audio_stretch_process(s, &io, 1));
               CHECK(io.output_frames == hop && memcmp(a, input, hop * frame) == 0);
               audio_stretch_free(s);
            }
         }
}

static void crossfade_cases(void)
{
   static float a[65536 * 8], b[65536 * 8], out[65536 * 8 + 1], part[65536 * 8 + 1];
   static int16_t ai[65536 * 8], bi[65536 * 8], oi[65536 * 8 + 1], pi[65536 * 8 + 1];
   static const unsigned lengths[] = {1, 2, 3, 21, 128, 512, 65536};
   unsigned l, channels, native;
   size_t k;
   for (k = 0; k < 65536 * 8; k++)
   {
      ai[k] = (int16_t)((int)(k % 65536) - 32768);
      bi[k] = (int16_t)(32767 - (int)(k % 65536));
      a[k] = ai[k] / 32768.0f; b[k] = bi[k] / 32768.0f;
   }
   for (l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++)
      for (channels = 1; channels <= 8; channels++)
         for (native = 0; native < 2; native++)
         {
            unsigned total = lengths[l], offset = 0;
            size_t frame = channels * (native ? sizeof(float) : sizeof(int16_t));
            const char *left = (const char*)(native ? (void*)a : (void*)ai);
            const char *right = (const char*)(native ? (void*)b : (void*)bi);
            char *whole = (char*)(native ? (void*)out : (void*)oi);
            char *fragment = (char*)(native ? (void*)part : (void*)pi);
            out[total * channels] = part[total * channels] = 1234.0f;
            oi[total * channels] = pi[total * channels] = 1234;
            guarded = 1;
            CHECK(audio_stretch_crossfade(whole, left, right, total, channels, native, 0, total));
            while (offset < total)
            {
               unsigned n = offset % 17 + 1;
               if (n > total - offset) n = total - offset;
               CHECK(audio_stretch_crossfade(fragment + offset * frame,
                        left + offset * frame, right + offset * frame,
                        n, channels, native, offset, total));
               offset += n;
            }
            guarded = 0;
            CHECK(memcmp(whole, fragment, total * frame) == 0);
            for (k = 0; k < total * channels; k++)
            {
               unsigned w = total == 1 ? 65536 : (unsigned)
                  (((uint64_t)(k / channels) * 65536) / (total - 1));
               if (native)
               {
                  float expected = a[k] * (1.0f - w / 65536.0f) + b[k] * (w / 65536.0f);
                  CHECK(out[k] == expected);
               }
               else
               {
                  int64_t v = (int64_t)ai[k] * (65536 - w) + (int64_t)bi[k] * w;
                  int expected = (int)(v < 0 ? -((-v + 32768) / 65536) : (v + 32768) / 65536);
                  CHECK(oi[k] == expected);
               }
            }
            CHECK(out[total * channels] == 1234.0f && part[total * channels] == 1234.0f);
            CHECK(oi[total * channels] == 1234 && pi[total * channels] == 1234);
            memcpy(fragment, left, total * frame);
            CHECK(audio_stretch_crossfade(fragment, fragment, right, total, channels, native, 0, total));
            CHECK(memcmp(whole, fragment, total * frame) == 0);
            memcpy(fragment, right, total * frame);
            CHECK(audio_stretch_crossfade(fragment, left, fragment, total, channels, native, 0, total));
            CHECK(memcmp(whole, fragment, total * frame) == 0);
         }
   for (k = 0; k < 3; k++) ai[k] = bi[k] = -32768;
   CHECK(audio_stretch_crossfade(oi, ai, bi, 3, 1, false, 0, 3));
   CHECK(oi[0] == -32768 && oi[1] == -32768 && oi[2] == -32768);
   for (k = 0; k < 3; k++) ai[k] = bi[k] = 32767;
   CHECK(audio_stretch_crossfade(oi, ai, bi, 3, 1, false, 0, 3));
   CHECK(oi[0] == 32767 && oi[1] == 32767 && oi[2] == 32767);
   oi[0] = 1234;
   CHECK(!audio_stretch_crossfade(oi, ai, bi, 1, 0, false, 0, 1));
   CHECK(!audio_stretch_crossfade(oi, ai, bi, 1, 9, false, 0, 1));
   CHECK(!audio_stretch_crossfade(oi, ai, bi, 1, 1, false, 0, 0));
   CHECK(!audio_stretch_crossfade(oi, ai, bi, 1, 1, false, 0, 65537));
   CHECK(!audio_stretch_crossfade(oi, ai, bi, 1, 1, false, 2, 1));
   CHECK(!audio_stretch_crossfade(oi, ai, bi, (size_t)-1, 1, false, 0, 1));
   CHECK(!audio_stretch_crossfade(oi, NULL, bi, 1, 1, false, 0, 1));
   CHECK(oi[0] == 1234);
   CHECK(audio_stretch_crossfade(NULL, NULL, NULL, 0, 1, false, 65536, 65536));
}

static size_t transition_run(unsigned channels, unsigned native, unsigned tail,
      unsigned first, unsigned second, unsigned fragmented, unsigned slot)
{
   audio_stretch_transition_t *s = audio_stretch_transition_new(channels, native, tail);
   char *dst = (char*)(native ? (void*)output_f[slot] : (void*)output_i[slot]);
   const char *src = (const char*)(native ? (void*)input_f : (void*)input_i);
   size_t frame = channels * (native ? sizeof(float) : sizeof(int16_t));
   size_t made = 0, check;
   unsigned segment;
   struct audio_stretch_io io;
   struct audio_stretch_drain_io drain;
   CHECK(s != NULL);
   if (!s) return 0;
   memset(dst, 0x5a, OUT_FRAMES * frame);
   guarded = 1;
   for (segment = 0; segment < 2; segment++)
   {
      unsigned used = 0, length = segment ? second : first;
      if (segment) CHECK(audio_stretch_transition_boundary(s));
      while (used < length)
      {
         size_t n = fragmented ? used % 19 + 1 : length - used;
         if (n > length - used) n = length - used;
         io.input = src + ((segment ? 4096 : 0) + used) * frame;
         io.input_frames = n; io.output = dst + made * frame;
         io.output_capacity = 0;
         CHECK(audio_stretch_transition_process(s, &io));
         CHECK(io.input_used == 0 && io.output_frames == 0);
         io.output_capacity = fragmented ? used % 13 + 1 : FRAMES;
         CHECK(audio_stretch_transition_process(s, &io));
         CHECK(io.input_used <= n && io.output_frames <= io.output_capacity);
         CHECK(io.input_used || io.output_frames);
         if (!io.input_used && !io.output_frames) break;
         used += (unsigned)io.input_used; made += io.output_frames;
      }
   }
   do
   {
      drain.output = dst + made * frame;
      drain.output_capacity = fragmented ? 7 : FRAMES;
      CHECK(audio_stretch_transition_flush(s, &drain));
      CHECK(drain.output_frames <= drain.output_capacity && drain.gap_offset == (size_t)-1);
      made += drain.output_frames;
   } while (!drain.complete);
   for (check = made * frame; check < OUT_FRAMES * frame; check++)
      CHECK((unsigned char)dst[check] == 0x5a);
   io.input = src; io.input_frames = 1; io.output = dst; io.output_capacity = 1;
   CHECK(!audio_stretch_transition_process(s, &io));
   CHECK(!audio_stretch_transition_boundary(s));
   audio_stretch_transition_reset(s);
   CHECK(audio_stretch_transition_process(s, &io));
   CHECK(io.input_used == 1 && io.output_frames == 0);
   guarded = 0;
   audio_stretch_transition_free(s);
   return made;
}

static void transition_cases(void)
{
   static float expected_f[FRAMES * 8];
   static int16_t expected_i[FRAMES * 8];
   static const unsigned tails[] = {1, 3, 21, 128};
   unsigned channels, native, t, scenario;
   audio_stretch_transition_t *s;
   struct audio_stretch_io io;
   struct audio_stretch_drain_io drain;
   for (channels = 1; channels <= 8; channels++)
   {
      fill(channels);
      for (native = 0; native < 2; native++)
         for (t = 0; t < sizeof(tails) / sizeof(tails[0]); t++)
            for (scenario = 0; scenario < 7; scenario++)
            {
               unsigned tail = tails[t], first = 511, second = 523, held, blend;
               size_t a, b, expected, frame = channels * (native ? sizeof(float) : sizeof(int16_t));
               char *ref = (char*)(native ? (void*)expected_f : (void*)expected_i);
               const char *input = (const char*)(native ? (void*)input_f : (void*)input_i);
               const char *whole = (const char*)(native ? (void*)output_f[0] : (void*)output_i[0]);
               const char *parts = (const char*)(native ? (void*)output_f[1] : (void*)output_i[1]);
               if (scenario == 1) first = tail - 1;
               if (scenario == 2) first = tail;
               if (scenario == 3) second = tail - 1;
               if (scenario == 4) second = 0;
               if (scenario == 5) first = 0;
               if (scenario == 6) { first = 0; second = 0; }
               a = transition_run(channels, native, tail, first, second, 0, 0);
               b = transition_run(channels, native, tail, first, second, 1, 1);
               held = first < tail ? first : tail;
               if (!second) held = 0;
               blend = second < held ? second : held;
               expected = first - held;
               memcpy(ref, input, expected * frame);
               if (held)
                  CHECK(audio_stretch_crossfade(ref + expected * frame,
                           input + expected * frame, input + 4096 * frame,
                           blend, channels, native, 0, held));
               expected += blend;
               if (second > blend)
               {
                  memcpy(ref + expected * frame, input + (4096 + blend) * frame,
                        (second - blend) * frame);
                  expected += second - blend;
               }
               CHECK(a == b && a == expected);
               CHECK(memcmp(whole, parts, a * frame) == 0);
               CHECK(memcmp(whole, ref, a * frame) == 0);
            }
   }
   CHECK(!audio_stretch_transition_new(0, false, 1));
   CHECK(!audio_stretch_transition_new(9, false, 1));
   CHECK(!audio_stretch_transition_new(2, false, 0));
   CHECK(!audio_stretch_transition_new(2, false, 65537));
   fail_init = 1; CHECK(!audio_stretch_transition_new(2, false, 3)); fail_init = 0;
   s = audio_stretch_transition_new(2, false, 3);
   io.input = input_i; io.input_frames = 3; io.output = output_i[0]; io.output_capacity = 3;
   guarded = 1;
   CHECK(audio_stretch_transition_process(s, &io));
   CHECK(audio_stretch_transition_boundary(s));
   CHECK(!audio_stretch_transition_boundary(s));
   io.input_frames = (size_t)-1;
   CHECK(!audio_stretch_transition_process(s, &io));
   CHECK(io.input_used == 0 && io.output_frames == 0);
   drain.output = NULL; drain.output_capacity = 0;
   CHECK(audio_stretch_transition_flush(s, &drain) && !drain.complete);
   io.input_frames = 3;
   CHECK(audio_stretch_transition_process(s, &io));
   CHECK(io.input_used == 3 && io.output_frames == 3);
   CHECK(audio_stretch_transition_boundary(s));
   audio_stretch_transition_reset(s);
   guarded = 0;
   audio_stretch_transition_free(s);
}

static void transition_drain_chain(void)
{
   static float expected_f[FRAMES * 8];
   static int16_t expected_i[FRAMES * 8];
   unsigned channels, native, scenario;
   for (channels = 1; channels <= 8; channels++)
      for (native = 0; native < 2; native++)
         for (scenario = 4; scenario <= 5; scenario++)
         {
            audio_stretch_t *s = audio_stretch_new(48000, channels, native, 1);
            size_t used, prefix, n, gap, a, b, expected;
            size_t frame = channels * (native ? sizeof(float) : sizeof(int16_t));
            char *input = (char*)(native ? (void*)input_f : (void*)input_i);
            char *ref = (char*)(native ? (void*)expected_f : (void*)expected_i);
            const char *drained = (const char*)(native ? (void*)output_f[0] : (void*)output_i[0]);
            const char *parts = (const char*)(native ? (void*)output_f[1] : (void*)output_i[1]);
            fill(channels);
            used = prepare_drain(s, channels, native, scenario, &prefix);
            n = drain_run(s, channels, native, 1, 0, &gap);
            CHECK(gap == 128 && gap <= n);
            /* Future caller input follows retained post-gap lookahead. */
            memmove(input + (4096 + n - gap) * frame, input + used * frame, 128 * frame);
            memcpy(input + 4096 * frame, drained + gap * frame, (n - gap) * frame);
            memcpy(input, drained, gap * frame);
            expected = n;
            CHECK(audio_stretch_crossfade(ref, input, input + 4096 * frame,
                     128, channels, native, 0, 128));
            memcpy(ref + 128 * frame, input + (4096 + 128) * frame,
                  (expected - 128) * frame);
            a = transition_run(channels, native, 128, (unsigned)gap,
                  (unsigned)(n - gap + 128), 0, 0);
            b = transition_run(channels, native, 128, (unsigned)gap,
                  (unsigned)(n - gap + 128), 1, 1);
            CHECK(a == expected && b == a);
            CHECK(memcmp(drained, parts, a * frame) == 0);
            CHECK(memcmp(drained, ref, a * frame) == 0);
            audio_stretch_free(s);
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
   drain_cases();
   crossfade_cases();
   transition_cases();
   transition_drain_chain();
   CHECK(heap_calls == 0);
   printf("stretch: %u failures, %u processing/reset heap calls\n", failures, heap_calls);
   return failures != 0;
}
