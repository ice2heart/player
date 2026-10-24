#include "audio_output.h"

#include <AudioToolbox/AudioToolbox.h>

#include <cstdio>

namespace oap {

namespace {

class CoreAudioOutput : public AudioOutput {
public:
    ~CoreAudioOutput() override { stop(); }

    bool start(Mixer& mixer) override {
        mixer_ = &mixer;
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_DefaultOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (!component || AudioComponentInstanceNew(component, &unit_) != noErr) {
            std::fprintf(stderr, "audio: no default output device\n");
            return false;
        }

        AudioStreamBasicDescription format{};
        format.mSampleRate = Mixer::kSampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mFramesPerPacket = 1;
        format.mChannelsPerFrame = AudioStream::kChannels;
        format.mBitsPerChannel = 32;
        format.mBytesPerFrame = sizeof(float) * AudioStream::kChannels;
        format.mBytesPerPacket = format.mBytesPerFrame;

        AURenderCallbackStruct callback{&CoreAudioOutput::render, this};
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                 &format, sizeof format) != noErr ||
            AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                 &callback, sizeof callback) != noErr ||
            AudioUnitInitialize(unit_) != noErr || AudioOutputUnitStart(unit_) != noErr) {
            std::fprintf(stderr, "audio: failed to start output unit\n");
            return false;
        }
        return true;
    }

    void stop() override {
        if (!unit_) return;
        AudioOutputUnitStop(unit_);
        AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
        unit_ = nullptr;
    }

private:
    static OSStatus render(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                           UInt32 frames, AudioBufferList* data) {
        auto* self = static_cast<CoreAudioOutput*>(ref);
        self->mixer_->render(static_cast<float*>(data->mBuffers[0].mData), frames);
        return noErr;
    }

    AudioUnit unit_ = nullptr;
    Mixer* mixer_ = nullptr;
};

} // namespace

std::unique_ptr<AudioOutput> makeCoreAudioOutput() { return std::make_unique<CoreAudioOutput>(); }

} // namespace oap
