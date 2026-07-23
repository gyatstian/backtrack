#include "audio/AudioMix.h"

#include "audio/WavReader.h"
#include "core/Logger.h"
#include "mux/WavWriter.h"

#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace backtrack {
namespace {

float readSourceSample(const uint8_t* sample, const WavInfo& info) {
    if (info.encoding == SampleEncoding::Float && info.bitsPerSample == 32) {
        float value = 0.0f;
        std::memcpy(&value, sample, sizeof(value));
        return std::clamp(value, -1.0f, 1.0f);
    }

    if (info.encoding != SampleEncoding::Pcm) {
        return 0.0f;
    }

    switch (info.bitsPerSample) {
    case 8:
        return (static_cast<int32_t>(*sample) - 128) / 128.0f;
    case 16: {
        int16_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return value / 32768.0f;
    }
    case 24: {
        int32_t value = sample[0] | (sample[1] << 8) | (sample[2] << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<int32_t>(0xff000000);
        }
        return value / 8388608.0f;
    }
    case 32: {
        int32_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return value / 2147483648.0f;
    }
    default:
        return 0.0f;
    }
}

struct AudioSource {
    WavInfo info;
    std::ifstream stream;
    std::vector<uint8_t> chunk;
    uint64_t frames = 0;
    uint64_t outputFrames = 0;
    uint64_t loadedStartFrame = 0;
    uint64_t loadedFrames = 0;
};

uint64_t audioSourceChunkFrames(const AudioSource& source) {
    constexpr uint64_t kAudioSourceChunkBytes = 256 * 1024;
    constexpr uint64_t kMaxAudioSourceChunkFrames = 65536;
    const uint64_t blockAlign = std::max<uint16_t>(source.info.blockAlign, 1);
    return std::max<uint64_t>(1, std::min<uint64_t>(kMaxAudioSourceChunkFrames, kAudioSourceChunkBytes / blockAlign));
}

bool openAudioSource(const std::filesystem::path& path, uint32_t targetSamplesPerSec, AudioSource& source) {
    if (!parseWav(path, source.info)) {
        return false;
    }
    source.frames = source.info.dataBytes / source.info.blockAlign;
    if (source.frames == 0) {
        return false;
    }
    source.outputFrames =
        (source.frames * static_cast<uint64_t>(targetSamplesPerSec) + source.info.samplesPerSec - 1) /
        source.info.samplesPerSec;

    source.stream.open(path, std::ios::binary);
    if (!source.stream.is_open()) {
        return false;
    }
    source.stream.seekg(static_cast<std::streamoff>(source.info.dataOffset), std::ios::beg);
    return source.stream.good();
}

bool ensureSourceFrameLoaded(AudioSource& source, uint64_t sourceFrame) {
    if (sourceFrame >= source.frames || source.info.blockAlign == 0) {
        return false;
    }
    if (source.loadedFrames > 0 &&
        sourceFrame >= source.loadedStartFrame &&
        sourceFrame < source.loadedStartFrame + source.loadedFrames) {
        return true;
    }

    const uint64_t capacityFrames = audioSourceChunkFrames(source);
    const uint64_t startFrame = (sourceFrame / capacityFrames) * capacityFrames;
    const uint64_t framesToRead = std::min<uint64_t>(capacityFrames, source.frames - startFrame);
    const uint64_t bytesToRead = framesToRead * source.info.blockAlign;
    const uint64_t byteOffset = source.info.dataOffset + startFrame * source.info.blockAlign;
    if (bytesToRead > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
        byteOffset > static_cast<uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
        return false;
    }

    source.chunk.resize(static_cast<size_t>(bytesToRead));
    source.stream.clear();
    source.stream.seekg(static_cast<std::streamoff>(byteOffset), std::ios::beg);
    source.stream.read(
        reinterpret_cast<char*>(source.chunk.data()),
        static_cast<std::streamsize>(source.chunk.size()));

    const uint64_t bytesRead = static_cast<uint64_t>(std::max<std::streamsize>(0, source.stream.gcount()));
    source.loadedStartFrame = startFrame;
    source.loadedFrames = bytesRead / source.info.blockAlign;
    source.chunk.resize(static_cast<size_t>(source.loadedFrames * source.info.blockAlign));
    return source.loadedFrames > 0 &&
           sourceFrame >= source.loadedStartFrame &&
           sourceFrame < source.loadedStartFrame + source.loadedFrames;
}

float sourceChannelAt(AudioSource& source, uint64_t outputFrame, uint16_t outputChannel, uint32_t targetSamplesPerSec) {
    if (outputFrame >= source.outputFrames || source.frames == 0 || targetSamplesPerSec == 0) {
        return 0.0f;
    }

    const double sourcePosition = std::min<double>(
        static_cast<double>(source.frames - 1),
        (static_cast<double>(outputFrame) * source.info.samplesPerSec) / targetSamplesPerSec);
    const uint64_t sourceFrame0 = static_cast<uint64_t>(sourcePosition);
    const uint64_t sourceFrame1 = std::min<uint64_t>(source.frames - 1, sourceFrame0 + 1);
    if (!ensureSourceFrameLoaded(source, sourceFrame0)) {
        return 0.0f;
    }
    const uint16_t sourceChannel = source.info.channels == 1
        ? 0
        : std::min<uint16_t>(outputChannel, static_cast<uint16_t>(source.info.channels - 1));
    const uint16_t bytesPerSample = source.info.bitsPerSample / 8;
    const size_t frameOffset =
        static_cast<size_t>(sourceFrame0 - source.loadedStartFrame) * source.info.blockAlign +
        static_cast<size_t>(sourceChannel) * bytesPerSample;
    if (bytesPerSample == 0 || frameOffset + bytesPerSample > source.chunk.size()) {
        return 0.0f;
    }
    const float sample0 = readSourceSample(source.chunk.data() + frameOffset, source.info);
    if (sourceFrame1 == sourceFrame0) {
        return sample0;
    }

    if (!ensureSourceFrameLoaded(source, sourceFrame1)) {
        return sample0;
    }
    const size_t nextFrameOffset =
        static_cast<size_t>(sourceFrame1 - source.loadedStartFrame) * source.info.blockAlign +
        static_cast<size_t>(sourceChannel) * bytesPerSample;
    if (nextFrameOffset + bytesPerSample > source.chunk.size()) {
        return sample0;
    }
    const float sample1 = readSourceSample(source.chunk.data() + nextFrameOffset, source.info);
    const float fraction = static_cast<float>(sourcePosition - static_cast<double>(sourceFrame0));
    return sample0 + (sample1 - sample0) * fraction;
}

WaveFormatBlob pcm16StereoFormat(uint32_t samplesPerSec) {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kMuxAudioChannels;
    format.nSamplesPerSec = samplesPerSec;
    format.wBitsPerSample = kMuxAudioBitsPerSample;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    WaveFormatBlob blob;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&format);
    blob.bytes.assign(bytes, bytes + sizeof(format));
    return blob;
}

} // namespace

bool buildMixedPcmAudio(const MuxedInputs& inputs, const std::filesystem::path& /*outputPath*/, std::filesystem::path& mixedPath) {
    std::vector<std::filesystem::path> audioPaths;
    if (!inputs.systemAudioPath.empty() && std::filesystem::exists(inputs.systemAudioPath)) {
        audioPaths.push_back(inputs.systemAudioPath);
    }
    if (!inputs.microphoneAudioPath.empty() && std::filesystem::exists(inputs.microphoneAudioPath)) {
        audioPaths.push_back(inputs.microphoneAudioPath);
    }
    if (audioPaths.empty()) {
        mixedPath.clear();
        return true;
    }

    std::vector<AudioSource> sources;
    for (const auto& path : audioPaths) {
        AudioSource source;
        if (openAudioSource(path, kMuxAudioSampleRate, source)) {
            sources.push_back(std::move(source));
        } else {
            Logger::instance().warning(L"Skipping unsupported audio stream for native MP4 mux: " + path.wstring());
        }
    }
    if (sources.empty()) {
        mixedPath.clear();
        return true;
    }

    uint64_t outputFrames = 0;
    for (const auto& source : sources) {
        outputFrames = std::max(outputFrames, source.outputFrames);
    }
    if (outputFrames == 0) {
        mixedPath.clear();
        return true;
    }

    const auto parent = !inputs.systemAudioPath.empty()
        ? inputs.systemAudioPath.parent_path()
        : inputs.microphoneAudioPath.parent_path();
    mixedPath = parent / L"mixed_audio.wav";

    WavWriter writer;
    if (!writer.open(mixedPath, pcm16StereoFormat(kMuxAudioSampleRate))) {
        Logger::instance().warning(L"Could not create mixed audio stream for MP4 mux: " + mixedPath.wstring());
        mixedPath.clear();
        return true;
    }

    constexpr uint64_t kChunkFrames = 4096;
    std::vector<int16_t> chunk;
    chunk.reserve(static_cast<size_t>(kChunkFrames * 2));
    for (uint64_t frame = 0; frame < outputFrames; frame += kChunkFrames) {
        const uint64_t framesThisChunk = std::min<uint64_t>(kChunkFrames, outputFrames - frame);
        chunk.clear();
        for (uint64_t index = 0; index < framesThisChunk; ++index) {
            for (uint16_t channel = 0; channel < 2; ++channel) {
                float mixed = 0.0f;
                uint32_t activeSources = 0;
                const uint64_t outputFrame = frame + index;
                for (auto& source : sources) {
                    if (outputFrame < source.outputFrames) {
                        mixed += sourceChannelAt(source, outputFrame, channel, kMuxAudioSampleRate);
                        ++activeSources;
                    }
                }
                if (activeSources > 1) {
                    mixed /= static_cast<float>(activeSources);
                }
                mixed = std::clamp(mixed, -1.0f, 1.0f);
                chunk.push_back(static_cast<int16_t>(std::lrintf(mixed * 32767.0f)));
            }
        }
        writer.write(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size() * sizeof(int16_t));
    }
    writer.close();
    return true;
}

} // namespace backtrack
