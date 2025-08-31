/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2025 - RetroArch team
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

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <gfx/scaler/scaler.h>
#include <gfx/video_frame.h>
#include <string/stdstring.h>

#include "record_avfoundation.h"
#include "../../retroarch.h"
#include "../../verbosity.h"

typedef struct record_avfoundation
{
   AVAssetWriter *assetWriter;
   AVAssetWriterInput *videoInput;
   AVAssetWriterInput *audioInput;
   AVAssetWriterInputPixelBufferAdaptor *pixelBufferAdaptor;

   /* Timing */
   CMTime startTime;
   CMTime lastVideoTime;
   CMTime lastAudioTime;
   double fps;
   double sampleRate;
   bool hasStarted;

   /* Video properties */
   unsigned width;
   unsigned height;
   unsigned fb_width;
   unsigned fb_height;
   enum ffemu_pix_format pix_fmt;
   CVPixelBufferPoolRef pixelBufferPool;
   struct scaler_ctx scaler;
   bool use_scaler;

   /* Audio properties */
   unsigned channels;
   unsigned samples_per_frame;

   /* Dispatch queue for encoding */
   dispatch_queue_t encodingQueue;
} record_avfoundation_t;

static void *avfoundation_record_init(const struct record_params *params)
{
   record_avfoundation_t *handle = NULL;
   NSError *error = nil;
   NSURL *outputURL = nil;
   NSDictionary *videoSettings = nil;
   NSDictionary *audioSettings = nil;
   NSDictionary *pixelBufferAttributes = nil;

   if (!params || !params->filename)
   {
      RARCH_ERR("[AVFoundation] Invalid parameters\n");
      return NULL;
   }

   handle = (record_avfoundation_t*)calloc(1, sizeof(record_avfoundation_t));
   if (!handle)
   {
      RARCH_ERR("[AVFoundation] Failed to allocate handle\n");
      return NULL;
   }

   /* Store parameters */
   handle->width = params->out_width;
   handle->height = params->out_height;
   handle->fb_width = params->fb_width;
   handle->fb_height = params->fb_height;
   handle->fps = params->fps;
   handle->sampleRate = params->samplerate;
   handle->channels = params->channels;
   handle->pix_fmt = params->pix_fmt;
   handle->hasStarted = false;

   /* Validate and clamp sample rate */
   if (handle->sampleRate < 8000.0 || handle->sampleRate > 192000.0)
   {
      RARCH_WARN("[AVFoundation] Sample rate %.2f Hz out of range, clamping to 48000 Hz\n", handle->sampleRate);
      handle->sampleRate = 48000.0;
   }

   /* Create output URL */
   outputURL = [NSURL fileURLWithPath:@(params->filename)];

   /* Determine file type from extension */
   NSString *pathExtension = [[outputURL pathExtension] lowercaseString];
   AVFileType fileType = AVFileTypeQuickTimeMovie; /* Default to .mov */

   if ([pathExtension isEqualToString:@"mp4"] || [pathExtension isEqualToString:@"m4v"])
      fileType = AVFileTypeMPEG4;
   else if ([pathExtension isEqualToString:@"mov"])
      fileType = AVFileTypeQuickTimeMovie;

   /* Create asset writer */
   handle->assetWriter = [[AVAssetWriter alloc] initWithURL:outputURL
                                                    fileType:fileType
                                                       error:&error];
   if (error)
   {
      RARCH_ERR("[AVFoundation] Failed to create asset writer: %s\n",
                [[error localizedDescription] UTF8String]);
      free(handle);
      return NULL;
   }

   /* Configure video settings */
   NSString *videoCodec = AVVideoCodecTypeH264;
#if defined(MAC_OS_X_VERSION_10_13) || defined(__IPHONE_11_0)
   if (@available(macOS 10.13, iOS 11.0, tvOS 11.0, *))
   {
      /* Use HEVC on newer systems if available */
      if (params->video_record_scale_factor > 2) /* Use as quality indicator */
         videoCodec = AVVideoCodecTypeHEVC;
   }
#endif

   videoSettings = @{
      AVVideoCodecKey: videoCodec,
      AVVideoWidthKey: @(handle->width),
      AVVideoHeightKey: @(handle->height),
      AVVideoCompressionPropertiesKey: @{
         AVVideoAverageBitRateKey: @(params->video_record_scale_factor > 0 ?
                                    params->video_record_scale_factor * 1000000 : 5000000),
         AVVideoExpectedSourceFrameRateKey: @(handle->fps),
         AVVideoMaxKeyFrameIntervalKey: @(60),
         AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel
      }
   };

   /* Create video input */
   handle->videoInput = [[AVAssetWriterInput alloc]
                        initWithMediaType:AVMediaTypeVideo
                        outputSettings:videoSettings];
   handle->videoInput.expectsMediaDataInRealTime = YES;

   /* Configure pixel buffer attributes */
   pixelBufferAttributes = @{
      (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
      (NSString*)kCVPixelBufferWidthKey: @(handle->width),
      (NSString*)kCVPixelBufferHeightKey: @(handle->height),
      (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
   };

   /* Create pixel buffer adaptor */
   handle->pixelBufferAdaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
                                initWithAssetWriterInput:handle->videoInput
                                sourcePixelBufferAttributes:pixelBufferAttributes];

   /* Get pixel buffer pool */
   handle->pixelBufferPool = handle->pixelBufferAdaptor.pixelBufferPool;
   if (!handle->pixelBufferPool)
   {
      RARCH_WARN("[AVFoundation] Failed to get pixel buffer pool\n");
   }

   /* Add video input to writer */
   if ([handle->assetWriter canAddInput:handle->videoInput])
   {
      [handle->assetWriter addInput:handle->videoInput];
   }
   else
   {
      RARCH_ERR("[AVFoundation] Cannot add video input\n");
      free(handle);
      return NULL;
   }

   /* Configure audio settings */
   if (handle->channels > 0 && handle->sampleRate > 0)
   {
      AudioChannelLayout channelLayout = {
         .mChannelLayoutTag = handle->channels == 1 ?
                             kAudioChannelLayoutTag_Mono :
                             kAudioChannelLayoutTag_Stereo,
         .mChannelBitmap = 0,
         .mNumberChannelDescriptions = 0
      };

      NSData *channelLayoutData = [NSData dataWithBytes:&channelLayout
                                                  length:sizeof(channelLayout)];

      audioSettings = @{
         AVFormatIDKey: @(kAudioFormatMPEG4AAC),
         AVSampleRateKey: @(handle->sampleRate),
         AVNumberOfChannelsKey: @(handle->channels),
         AVChannelLayoutKey: channelLayoutData,
         AVEncoderBitRateKey: @(128000)
      };

      /* Create audio input */
      handle->audioInput = [[AVAssetWriterInput alloc]
                           initWithMediaType:AVMediaTypeAudio
                           outputSettings:audioSettings];
      handle->audioInput.expectsMediaDataInRealTime = YES;

      /* Add audio input to writer */
      if ([handle->assetWriter canAddInput:handle->audioInput])
      {
         [handle->assetWriter addInput:handle->audioInput];
      }
      else
      {
         RARCH_WARN("[AVFoundation] Cannot add audio input\n");
         handle->audioInput = nil;
      }
   }

   /* Initialize scaler if needed */
   handle->use_scaler = (handle->width != handle->fb_width ||
                        handle->height != handle->fb_height);

   if (handle->use_scaler)
   {
      struct scaler_ctx *scaler = &handle->scaler;
      scaler->in_width = handle->fb_width;
      scaler->in_height = handle->fb_height;
      scaler->out_width = handle->width;
      scaler->out_height = handle->height;
      scaler->in_stride = handle->fb_width * 4; /* Assuming BGRA */
      scaler->out_stride = handle->width * 4;

      /* Set pixel format */
      switch (handle->pix_fmt)
      {
         case FFEMU_PIX_RGB565:
            scaler->in_fmt = SCALER_FMT_RGB565;
            break;
         case FFEMU_PIX_BGR24:
            scaler->in_fmt = SCALER_FMT_BGR24;
            break;
         case FFEMU_PIX_ARGB8888:
         default:
            scaler->in_fmt = SCALER_FMT_ARGB8888;
            break;
      }
      scaler->out_fmt = SCALER_FMT_ARGB8888;
      scaler->scaler_type = SCALER_TYPE_BILINEAR;

      if (!scaler_ctx_gen_filter(scaler))
      {
         RARCH_ERR("[AVFoundation] Failed to initialize scaler\n");
         free(handle);
         return NULL;
      }
   }

   /* Create encoding queue */
   handle->encodingQueue = dispatch_queue_create("com.retroarch.avfoundation.encoding",
                                                 DISPATCH_QUEUE_SERIAL);

   /* Start writing */
   if (![handle->assetWriter startWriting])
   {
      RARCH_ERR("[AVFoundation] Failed to start writing\n");
      free(handle);
      return NULL;
   }

   RARCH_LOG("[AVFoundation] Initialized recording to %s (%ux%u @ %.2f fps)\n",
             params->filename, handle->width, handle->height, handle->fps);

   return handle;
}

static void avfoundation_record_free(void *data)
{
   record_avfoundation_t *handle = (record_avfoundation_t*)data;

   if (!handle)
      return;

   /* Clean up scaler */
   if (handle->use_scaler)
      scaler_ctx_gen_reset(&handle->scaler);

   /* Release dispatch queue */
   if (handle->encodingQueue)
   {
      dispatch_sync(handle->encodingQueue, ^{});
      handle->encodingQueue = nil;
   }

   /* Release AVFoundation objects */
   handle->pixelBufferAdaptor = nil;
   handle->videoInput = nil;
   handle->audioInput = nil;
   handle->assetWriter = nil;

   free(handle);
}

static bool avfoundation_record_push_video(void *data,
                                        const struct record_video_data *video_data)
{
   record_avfoundation_t *handle = (record_avfoundation_t*)data;
   CVPixelBufferRef pixelBuffer = NULL;
   CVReturn result;

   if (!handle || !video_data)
      return false;

   /* Handle duplicate frames */
   if (video_data->is_dupe)
      return true;

   if (!video_data->data)
      return false;

   /* Start session on first frame */
   if (!handle->hasStarted)
   {
      handle->startTime = CMTimeMake(0, (int32_t)handle->fps);
      [handle->assetWriter startSessionAtSourceTime:handle->startTime];
      handle->hasStarted = true;
      handle->lastVideoTime = handle->startTime;
   }

   /* Calculate presentation time */
   CMTime presentationTime = CMTimeAdd(handle->lastVideoTime,
                                      CMTimeMake(1, (int32_t)handle->fps));

   /* Create pixel buffer from pool */
   if (handle->pixelBufferPool)
   {
      result = CVPixelBufferPoolCreatePixelBuffer(NULL,
                                                 handle->pixelBufferPool,
                                                 &pixelBuffer);
      if (result != kCVReturnSuccess)
      {
         RARCH_WARN("[AVFoundation] Failed to create pixel buffer from pool\n");
         return false;
      }
   }
   else
   {
      /* Create pixel buffer manually if pool is not available */
      result = CVPixelBufferCreate(NULL,
                                  handle->width,
                                  handle->height,
                                  kCVPixelFormatType_32BGRA,
                                  NULL,
                                  &pixelBuffer);
      if (result != kCVReturnSuccess)
      {
         RARCH_WARN("[AVFoundation] Failed to create pixel buffer\n");
         return false;
      }
   }

   /* Lock pixel buffer for writing */
   CVPixelBufferLockBaseAddress(pixelBuffer, 0);

   void *baseAddress = CVPixelBufferGetBaseAddress(pixelBuffer);
   size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);

   /* Convert and copy video data */
   if (handle->use_scaler)
   {
      /* Use scaler to resize */
      struct scaler_ctx *scaler = &handle->scaler;
      scaler->in_stride = abs(video_data->pitch);
      scaler->out_stride = (int)bytesPerRow;

      scaler_ctx_scale(scaler, baseAddress, video_data->data);
   }
   else
   {
      /* Direct copy or simple conversion */
      const uint8_t *src = (const uint8_t*)video_data->data;
      uint8_t *dst = (uint8_t*)baseAddress;
      int src_pitch = video_data->pitch;
      bool flip = src_pitch < 0;

      if (flip)
      {
         src_pitch = -src_pitch;
         src += src_pitch * (video_data->height - 1);
      }

      for (unsigned y = 0; y < video_data->height; y++)
      {
         const uint8_t *src_line = flip ? (src - y * src_pitch) : (src + y * src_pitch);
         uint8_t *dst_line = dst + y * bytesPerRow;

         switch (handle->pix_fmt)
         {
            case FFEMU_PIX_BGR24:
               /* Convert BGR24 to BGRA */
               for (unsigned x = 0; x < video_data->width; x++)
               {
                  dst_line[x*4 + 0] = src_line[x*3 + 0]; /* B */
                  dst_line[x*4 + 1] = src_line[x*3 + 1]; /* G */
                  dst_line[x*4 + 2] = src_line[x*3 + 2]; /* R */
                  dst_line[x*4 + 3] = 255;               /* A */
               }
               break;

            case FFEMU_PIX_RGB565:
               /* Convert RGB565 to BGRA */
               for (unsigned x = 0; x < video_data->width; x++)
               {
                  uint16_t pixel = ((uint16_t*)src_line)[x];
                  uint8_t r = ((pixel >> 11) & 0x1F) << 3;
                  uint8_t g = ((pixel >> 5) & 0x3F) << 2;
                  uint8_t b = (pixel & 0x1F) << 3;
                  dst_line[x*4 + 0] = b;
                  dst_line[x*4 + 1] = g;
                  dst_line[x*4 + 2] = r;
                  dst_line[x*4 + 3] = 255;
               }
               break;

            case FFEMU_PIX_ARGB8888:
            default:
               /* ARGB to BGRA conversion */
               for (unsigned x = 0; x < video_data->width; x++)
               {
                  uint32_t pixel = ((uint32_t*)src_line)[x];
                  dst_line[x*4 + 0] = (pixel >> 16) & 0xFF; /* B */
                  dst_line[x*4 + 1] = (pixel >> 8) & 0xFF;  /* G */
                  dst_line[x*4 + 2] = pixel & 0xFF;         /* R */
                  dst_line[x*4 + 3] = (pixel >> 24) & 0xFF; /* A */
               }
               break;
         }
      }
   }

   CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

   /* Append to video input */
   dispatch_sync(handle->encodingQueue, ^{
      if (handle->videoInput.readyForMoreMediaData)
      {
         [handle->pixelBufferAdaptor appendPixelBuffer:pixelBuffer
                               withPresentationTime:presentationTime];
      }
   });

   /* Update last video time */
   handle->lastVideoTime = presentationTime;

   /* Release pixel buffer */
   CVPixelBufferRelease(pixelBuffer);

   return true;
}

static bool avfoundation_record_push_audio(void *data,
                                        const struct record_audio_data *audio_data)
{
   record_avfoundation_t *handle = (record_avfoundation_t*)data;
   CMSampleBufferRef sampleBuffer = NULL;
   CMBlockBufferRef blockBuffer = NULL;
   CMFormatDescriptionRef formatDescription = NULL;
   OSStatus status;

   if (!handle || !handle->audioInput || !audio_data || !audio_data->data)
      return false;

   if (!handle->hasStarted)
      return false; /* Wait for video to start */

   /* Calculate audio timing */
   CMTime duration = CMTimeMake((int64_t)audio_data->frames, (int32_t)handle->sampleRate);
   CMTime presentationTime = handle->lastAudioTime.value == 0 ?
                            handle->startTime : handle->lastAudioTime;

   /* Create format description */
   AudioStreamBasicDescription audioFormat = {0};
   audioFormat.mSampleRate = handle->sampleRate;
   audioFormat.mFormatID = kAudioFormatLinearPCM;
   audioFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
   audioFormat.mBytesPerPacket = sizeof(int16_t) * handle->channels;
   audioFormat.mFramesPerPacket = 1;
   audioFormat.mBytesPerFrame = sizeof(int16_t) * handle->channels;
   audioFormat.mChannelsPerFrame = handle->channels;
   audioFormat.mBitsPerChannel = 16;

   status = CMAudioFormatDescriptionCreate(NULL,
                                          &audioFormat,
                                          0, NULL,
                                          0, NULL,
                                          NULL,
                                          &formatDescription);
   if (status != noErr)
   {
      RARCH_WARN("[AVFoundation] Failed to create audio format description\n");
      return false;
   }

   /* Create block buffer */
   size_t dataSize = audio_data->frames * sizeof(int16_t) * handle->channels;
   status = CMBlockBufferCreateWithMemoryBlock(NULL,
                                              NULL,
                                              dataSize,
                                              NULL,
                                              NULL,
                                              0,
                                              dataSize,
                                              0,
                                              &blockBuffer);
   if (status != noErr)
   {
      CFRelease(formatDescription);
      RARCH_WARN("[AVFoundation] Failed to create block buffer\n");
      return false;
   }

   /* Copy audio data to block buffer */
   status = CMBlockBufferReplaceDataBytes(audio_data->data,
                                         blockBuffer,
                                         0,
                                         dataSize);
   if (status != noErr)
   {
      CFRelease(blockBuffer);
      CFRelease(formatDescription);
      RARCH_WARN("[AVFoundation] Failed to copy audio data\n");
      return false;
   }

   /* Create sample buffer */
   status = CMAudioSampleBufferCreateWithPacketDescriptions(NULL,
                                                           blockBuffer,
                                                           true,
                                                           NULL,
                                                           NULL,
                                                           formatDescription,
                                                           (CMItemCount)audio_data->frames,
                                                           presentationTime,
                                                           NULL,
                                                           &sampleBuffer);

   CFRelease(blockBuffer);
   CFRelease(formatDescription);

   if (status != noErr)
   {
      RARCH_WARN("[AVFoundation] Failed to create audio sample buffer\n");
      return false;
   }

   /* Append to audio input */
   dispatch_sync(handle->encodingQueue, ^{
      if (handle->audioInput.readyForMoreMediaData)
      {
         [handle->audioInput appendSampleBuffer:sampleBuffer];
      }
   });

   /* Update last audio time */
   handle->lastAudioTime = CMTimeAdd(presentationTime, duration);

   CFRelease(sampleBuffer);
   return true;
}

static bool avfoundation_record_finalize(void *data)
{
   record_avfoundation_t *handle = (record_avfoundation_t*)data;
   __block BOOL success = NO;
   dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

   if (!handle || !handle->assetWriter)
      return false;

   RARCH_LOG("[AVFoundation] Finalizing recording...\n");

   /* Mark inputs as finished */
   dispatch_sync(handle->encodingQueue, ^{
      [handle->videoInput markAsFinished];
      if (handle->audioInput)
         [handle->audioInput markAsFinished];
   });

   /* Finish writing */
   [handle->assetWriter finishWritingWithCompletionHandler:^{
      if (handle->assetWriter.status == AVAssetWriterStatusCompleted)
      {
         RARCH_LOG("[AVFoundation] Recording completed successfully\n");
         success = YES;
      }
      else if (handle->assetWriter.status == AVAssetWriterStatusFailed)
      {
         RARCH_ERR("[AVFoundation] Recording failed: %s\n",
                  [[handle->assetWriter.error localizedDescription] UTF8String]);
      }
      else
      {
         RARCH_WARN("[AVFoundation] Recording ended with status: %ld\n",
                   (long)handle->assetWriter.status);
      }
      dispatch_semaphore_signal(semaphore);
   }];

   /* Wait for completion (with timeout) */
   dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC);
   if (dispatch_semaphore_wait(semaphore, timeout) != 0)
   {
      RARCH_WARN("[AVFoundation] Timeout waiting for finalization\n");
      return false;
   }

   return success;
}

const record_driver_t record_avfoundation = {
   avfoundation_record_init,
   avfoundation_record_free,
   avfoundation_record_push_video,
   avfoundation_record_push_audio,
   avfoundation_record_finalize,
   "avfoundation"
};
