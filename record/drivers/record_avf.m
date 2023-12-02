/*  RetroArch - A frontend for libretro.
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

#import <AVFoundation/AVFoundation.h>

#import "record_avf.h"

@interface AVFRecorder : NSObject

// Declare properties
@property (nonatomic, strong) AVAssetWriter *assetWriter;
@property (nonatomic, strong) AVAssetWriterInput *videoInput;
@property (nonatomic, strong) AVAssetWriterInput *audioInput;
@property (nonatomic, strong) AVAssetWriterInputPixelBufferAdaptor *videoPixelBufferAdaptor;
@property (nonatomic, strong) NSDictionary *audioSettings;

@end

@implementation AVFRecorder

- (void)setupAssetWriter {
    NSError *error = nil;
    // Create a URL for the output file
    NSURL *outputFileURL = [NSURL fileURLWithPath:@"output.mov"];

    // Create an AVAssetWriter with the desired output URL and file type
    self.assetWriter = [AVAssetWriter assetWriterWithURL:outputFileURL fileType:AVFileTypeQuickTimeMovie error:&error];

    // Configure video settings
    NSDictionary *videoSettings = @{
        AVVideoCodecKey: AVVideoCodecTypeH264,
        AVVideoWidthKey: @(256),
        AVVideoHeightKey: @(256)
    };
    self.videoInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:videoSettings];

    // Create a pixel buffer adaptor for video input
    NSDictionary *sourcePixelBufferAttributes = @{
        (NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32ARGB)
    };
    self.videoPixelBufferAdaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:self.videoInput sourcePixelBufferAttributes:sourcePixelBufferAttributes];

    if ([self.assetWriter canAddInput:self.videoInput]) {
        [self.assetWriter addInput:self.videoInput];
    }

    // Configure audio settings
    self.audioSettings = @{
        AVFormatIDKey: @(kAudioFormatLinearPCM),
        AVSampleRateKey: @(2),
        AVNumberOfChannelsKey: @(2),
        AVLinearPCMBitDepthKey: @(16), // Example: 16-bit audio
        AVLinearPCMIsBigEndianKey: @(NO),
        AVLinearPCMIsFloatKey: @(NO),
    };
    self.audioInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:self.audioSettings];

    if ([self.assetWriter canAddInput:self.audioInput]) {
        [self.assetWriter addInput:self.audioInput];
    }
}

- (void)writePixelBuffer:(CVPixelBufferRef)pixelBuffer presentationTime:(CMTime)presentationTime {
    if (!self.videoPixelBufferAdaptor.assetWriterInput.readyForMoreMediaData) {
        return;
    }

    if (self.assetWriter.status == AVAssetWriterStatusUnknown) {
        [self.assetWriter startWriting];
        [self.assetWriter startSessionAtSourceTime:presentationTime];
    }

    [self.videoPixelBufferAdaptor appendPixelBuffer:pixelBuffer withPresentationTime:presentationTime];
}

- (void)writePCMData:(AudioBuffer)audioBuffer presentationTimeStamp:(CMTime)presentationTimeStamp {
    if (!self.audioInput.isReadyForMoreMediaData) {
        return;
    }

    if (self.assetWriter.status == AVAssetWriterStatusUnknown) {
        [self.assetWriter startWriting];
        [self.assetWriter startSessionAtSourceTime:presentationTimeStamp];
    }

//    CMItemCount frameCount = audioBuffer.mDataByteSize / [self.audioSettings[AVLinearPCMBitDepthKey] intValue];
    CMSampleBufferRef audioBufferCopy = NULL;
    CMSampleTimingInfo timing = {kCMTimeInvalid, presentationTimeStamp, kCMTimeInvalid};

    OSStatus status = CMSampleBufferCreate(kCFAllocatorDefault,
                                           NULL,
                                           false,
                                           NULL,
                                           NULL,
                                           NULL,
                                           1,
                                           1,
                                           &timing,
                                           0,
                                           NULL,
                                           &audioBufferCopy);

    if (status == noErr) {
        AudioBufferList audioBufferList;
        audioBufferList.mNumberBuffers = 1;
        audioBufferList.mBuffers[0] = audioBuffer;

        status = CMSampleBufferSetDataBufferFromAudioBufferList(audioBufferCopy,
                                                                kCFAllocatorDefault,
                                                                kCFAllocatorDefault,
                                                                0,
                                                                &audioBufferList);
    }

    if (status == noErr) {
        [self.audioInput appendSampleBuffer:audioBufferCopy];
        CFRelease(audioBufferCopy);
    }
}

- (void)endRecordingWithCompletion:(void (^)(void))completion {
    [self.videoInput markAsFinished];
    [self.audioInput markAsFinished];

    [self.assetWriter finishWritingWithCompletionHandler:completion];}


@end

static void *avf_record_new(const struct record_params *params)
{
    AVFRecorder *recorder = [[AVFRecorder alloc] init];
    [recorder setupAssetWriter];
    return (__bridge_retained void *)recorder;
}

static void avf_record_free(void *data)
{
    __block AVFRecorder *recorder = (__bridge_transfer AVFRecorder *)data;
    if (recorder == nil)
        return;

    [recorder endRecordingWithCompletion:^{
        recorder = nil;
    }];
}

static bool avf_record_push_video(void *data, const struct record_video_data *video_data)
{
    AVFRecorder *recorder = (__bridge AVFRecorder *)data;
    if (recorder == nil)
        return false;

    return true;
}

static bool avf_record_push_audio(void *data, const struct record_audio_data *audio_data)
{
    AVFRecorder *recorder = (__bridge AVFRecorder *)data;
    if (recorder == nil)
        return false;

    return true;
}

static bool avf_record_finalize(void *data)
{
    AVFRecorder *recorder = (__bridge AVFRecorder *)data;
    if (recorder == nil)
        return false;

    return true;
}

const record_driver_t record_avf = {
   avf_record_new,
   avf_record_free,
   avf_record_push_video,
   avf_record_push_audio,
   avf_record_finalize,
   "avfoundation",
};
