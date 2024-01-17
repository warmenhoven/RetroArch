/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2023 Jesse Talavera-Greenberg
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

#include "verbosity.h"
#include "retro_assert.h"
#include "retro_math.h"
#include "audio/microphone_driver.h"
#include <rthreads/rthreads.h>
#include <queues/fifo_queue.h>

#ifndef TARGET_OS_IPHONE
#include <CoreAudio/CoreAudio.h>
#endif

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>

typedef struct
{
   bool nonblock;
} ca_mic_drv_t;

typedef struct
{
   AudioQueueRef audioQueue;
   AudioQueueBufferRef buffers[3];
   fifo_buffer_t *buffer;
   slock_t *lock;
   scond_t *cond;
   bool running;
} ca_mic_t;

static void *coreaudio_microphone_init(void)
{
   ca_mic_drv_t *drv = (ca_mic_drv_t *)calloc(1, sizeof(*drv));
   return drv;
}

static void coreaudio_microphone_free(void *drv)
{
   if (drv)
      free(drv);
}

static int coreaudio_microphone_read(void *drv, void *m, void *buf, size_t sz)
{
   ca_mic_drv_t *cadrv = (ca_mic_drv_t *)drv;
   ca_mic_t *mic = (ca_mic_t *)m;
   size_t read = 0;
   slock_lock(mic->lock);
   if (cadrv->nonblock)
   {
      size_t avail, read_amt;
      avail = FIFO_READ_AVAIL(mic->buffer);
      read_amt = avail > sz ? sz : avail;
      if (read_amt > 0)
         fifo_read(mic->buffer, buf, read_amt);
      read = read_amt;
   }
   else
   {
      while (read < sz)
      {
         size_t avail = FIFO_READ_AVAIL(mic->buffer);
         if (avail)
         {
            size_t read_amt = MIN(sz - read, avail);
            fifo_read(mic->buffer, buf + read, read_amt);
            read += read_amt;
         }
//         if (read != sz)
//            scond_wait(mic->cond, mic->lock);
      }
   }
   scond_signal(mic->cond);
   slock_lock(mic->lock);
   return (int)read;
}

static void coreaudio_microphone_set_nonblock_state(void *drv, bool nonblock)
{
   ca_mic_drv_t *cadrv = (ca_mic_drv_t *)drv;
   if (cadrv)
      cadrv->nonblock = nonblock;
}

static void AudioInputCallback(void *inUserData, AudioQueueRef inAQ,
                               AudioQueueBufferRef inBuffer, const AudioTimeStamp *inStartTime,
                               UInt32 inNumPackets, const AudioStreamPacketDescription *inPacketDesc)
{
   ca_mic_t *mic = (ca_mic_t *)inUserData;

   if (inNumPackets > 0) {
      size_t read = 0;
      slock_lock(mic->lock);
      while (read < inBuffer->mAudioDataByteSize)
      {
         if (FIFO_WRITE_AVAIL(mic->buffer) >= inBuffer->mAudioDataByteSize)
         {
            fifo_write(mic->buffer, inBuffer->mAudioData, inBuffer->mAudioDataByteSize);
            read += inBuffer->mAudioDataByteSize;
         }
      }
      scond_signal(mic->cond);
      slock_unlock(mic->lock);
   }

   OSStatus status = AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
    printf("%d\n", status);
}

static void *coreaudio_microphone_open_mic(void *drv, const char *device, unsigned rate, unsigned latency, unsigned *new_rate)
{
   ca_mic_t *mic = (ca_mic_t *)calloc(1, sizeof(*mic));
   if (!mic)
      return NULL;

   int frames = next_pow2(rate * latency / 1000);
   AudioStreamBasicDescription audioFormat;
   memset(&audioFormat, 0, sizeof(audioFormat));
   audioFormat.mSampleRate = rate;
   audioFormat.mFormatID = kAudioFormatLinearPCM;
   audioFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
   audioFormat.mFramesPerPacket = 1;
   audioFormat.mChannelsPerFrame = 1;
   audioFormat.mBitsPerChannel = 16;
   audioFormat.mBytesPerPacket = 2;
   audioFormat.mBytesPerFrame = 2;

   AudioQueueRef audioQueue;
   AudioQueueNewInput(&audioFormat, AudioInputCallback, mic, NULL, kCFRunLoopCommonModes, 0, &audioQueue);

   const int bufferByteSize = frames * 2;

   for (int i = 0; i < 3; ++i)
      AudioQueueAllocateBuffer(audioQueue, bufferByteSize, &mic->buffers[i]);

   mic->audioQueue = audioQueue;
   mic->buffer = fifo_new(frames << 3);
   mic->lock = slock_new();
   mic->cond = scond_new();
   mic->running = false;
   return mic;
}

static void coreaudio_microphone_close_mic(void *drv, void *m)
{
   ca_mic_t *mic = (ca_mic_t *)m;
   AudioQueueDispose(mic->audioQueue, true);
   fifo_free(mic->buffer);
   slock_free(mic->lock);
   scond_free(mic->cond);
   free(mic);
}

static bool coreaudio_microphone_mic_alive(const void *drv, const void *m)
{
   ca_mic_t *mic = (ca_mic_t *)m;
   return mic && mic->running;
}

static bool coreaudio_microphone_start_mic(void *drv, void *m)
{
   ca_mic_t *mic = (ca_mic_t *)m;
   if (!mic->running)
   {
       for (int i = 0; i < 3; i++)
           AudioQueueEnqueueBuffer(mic->audioQueue, mic->buffers[i], 0, NULL);
      OSStatus status = AudioQueueStart(mic->audioQueue, NULL);
      mic->running = (status == 0);
   }
   return mic->running;
}

static bool coreaudio_microphone_stop_mic(void *drv, void *m)
{
   ca_mic_t *mic = (ca_mic_t *)m;
   AudioQueueStop(mic->audioQueue, true);
   mic->running = false;
   return true;
}

static bool coreaudio_microphone_mic_use_float(const void *drv, const void *mic)
{
   return false;
}

microphone_driver_t microphone_coreaudio = {
      coreaudio_microphone_init,
      coreaudio_microphone_free,
      coreaudio_microphone_read,
      coreaudio_microphone_set_nonblock_state,
      "coreaudio",
      NULL,
      NULL,
      coreaudio_microphone_open_mic,
      coreaudio_microphone_close_mic,
      coreaudio_microphone_mic_alive,
      coreaudio_microphone_start_mic,
      coreaudio_microphone_stop_mic,
      coreaudio_microphone_mic_use_float,
};
