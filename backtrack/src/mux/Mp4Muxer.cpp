#include "mux/Mp4Muxer.h"

#include "core/Logger.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

namespace backtrack {

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kMuxAudioSampleRate = 48000;
constexpr uint16_t kMuxAudioChannels = 2;
constexpr uint16_t kMuxAudioBitsPerSample = 16;
constexpr uint32_t kMuxAudioAverageBytesPerSecond = 24000;
constexpr uint32_t kMaxWaveFormatBytes = 4096;
constexpr wchar_t kRecoveryManifestName[] = L"recovery.tsv";
constexpr wchar_t kRecoveryReadmeName[] = L"RECOVERY.txt";

struct RecoveryManifest {
    MuxedInputs inputs;
    std::filesystem::path tempDirectory;
};

std::string codecText(VideoCodec codec) {
    return codec == VideoCodec::Hevc ? "hevc" : "h264";
}

VideoCodec codecFromText(const std::string& text) {
    return text == "hevc" ? VideoCodec::Hevc : VideoCodec::H264;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            parts.push_back(line.substr(start));
            return parts;
        }
        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

std::string narrowFileName(const std::filesystem::path& path) {
    return path.filename().string();
}

std::filesystem::path recoveryRoot(const std::filesystem::path& clipDirectory) {
    return clipDirectory / L".backtrack_tmp";
}

std::filesystem::path manifestPathFor(const std::filesystem::path& tempDirectory) {
    return tempDirectory / kRecoveryManifestName;
}

std::filesystem::path uniqueRecoveredOutputPath(const std::filesystem::path& clipDirectory, const std::wstring& stem) {
    std::error_code error;
    const std::filesystem::path preferred = clipDirectory / (stem + L".mp4");
    if (!std::filesystem::exists(preferred, error)) {
        return preferred;
    }

    const std::filesystem::path recovered = clipDirectory / (stem + L"_recovered.mp4");
    error = {};
    if (!std::filesystem::exists(recovered, error)) {
        return recovered;
    }

    for (uint32_t suffix = 2; suffix < 1000; ++suffix) {
        const auto candidate = clipDirectory / (stem + L"_recovered_" + std::to_wstring(suffix) + L".mp4");
        error = {};
        if (!std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
    return clipDirectory / (stem + L"_recovered_" + std::to_wstring(GetTickCount64()) + L".mp4");
}

bool writeRecoveryManifest(const MuxedInputs& inputs, const std::filesystem::path& tempDirectory) {
    std::ofstream stream(manifestPathFor(tempDirectory), std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        Logger::instance().warning(L"Could not write recording recovery manifest: " + manifestPathFor(tempDirectory).wstring());
        return false;
    }

    stream << "backtrack-recovery-v1\n";
    stream << "codec\t" << codecText(inputs.codec) << "\n";
    stream << "fps\t" << inputs.fps << "\n";
    stream << "width\t" << inputs.width << "\n";
    stream << "height\t" << inputs.height << "\n";
    stream << "bitrateKbps\t" << inputs.bitrateKbps << "\n";
    stream << "video\t" << narrowFileName(inputs.videoPath) << "\n";
    if (!inputs.systemAudioPath.empty()) {
        stream << "systemAudio\t" << narrowFileName(inputs.systemAudioPath) << "\n";
    }
    if (!inputs.microphoneAudioPath.empty()) {
        stream << "microphoneAudio\t" << narrowFileName(inputs.microphoneAudioPath) << "\n";
    }
    stream << "samples\t" << inputs.videoSamples.size() << "\n";
    for (const auto& sample : inputs.videoSamples) {
        stream << "sample\t"
               << sample.offset << "\t"
               << sample.size << "\t"
               << sample.pts100ns << "\t"
               << sample.duration100ns << "\t"
               << (sample.keyFrame ? 1 : 0) << "\n";
    }
    return stream.good();
}

void writeRecoveryReadme(const MuxedInputs& inputs, const std::filesystem::path& tempDirectory) {
    std::ofstream stream(tempDirectory / kRecoveryReadmeName, std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        return;
    }

    stream << "Backtrack preserved this recording because native MP4 muxing failed.\n\n";
    stream << "Use the Recover Failed action in Backtrack to retry muxing this folder.\n";
    stream << "The elementary streams are also available for manual recovery:\n";
    stream << "- Video: " << narrowFileName(inputs.videoPath) << "\n";
    if (!inputs.systemAudioPath.empty()) {
        stream << "- System audio: " << narrowFileName(inputs.systemAudioPath) << "\n";
    }
    if (!inputs.microphoneAudioPath.empty()) {
        stream << "- Microphone audio: " << narrowFileName(inputs.microphoneAudioPath) << "\n";
    }
    stream << "\nThe recovery.tsv file contains packet timing used by Backtrack's native muxer.\n";
}

bool readRecoveryManifest(const std::filesystem::path& tempDirectory, RecoveryManifest& manifest) {
    std::ifstream stream(manifestPathFor(tempDirectory));
    if (!stream.is_open()) {
        return false;
    }

    std::string line;
    if (!std::getline(stream, line) || line != "backtrack-recovery-v1") {
        return false;
    }

    manifest = {};
    manifest.tempDirectory = tempDirectory;
    size_t expectedSamples = 0;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }

        const auto parts = splitTabs(line);
        if (parts.empty()) {
            continue;
        }

        try {
            if (parts[0] == "codec" && parts.size() >= 2) {
                manifest.inputs.codec = codecFromText(parts[1]);
            } else if (parts[0] == "fps" && parts.size() >= 2) {
                manifest.inputs.fps = static_cast<uint32_t>(std::stoul(parts[1]));
            } else if (parts[0] == "width" && parts.size() >= 2) {
                manifest.inputs.width = static_cast<uint32_t>(std::stoul(parts[1]));
            } else if (parts[0] == "height" && parts.size() >= 2) {
                manifest.inputs.height = static_cast<uint32_t>(std::stoul(parts[1]));
            } else if (parts[0] == "bitrateKbps" && parts.size() >= 2) {
                manifest.inputs.bitrateKbps = static_cast<uint32_t>(std::stoul(parts[1]));
            } else if (parts[0] == "video" && parts.size() >= 2) {
                manifest.inputs.videoPath = tempDirectory / std::filesystem::path(parts[1]);
            } else if (parts[0] == "systemAudio" && parts.size() >= 2) {
                manifest.inputs.systemAudioPath = tempDirectory / std::filesystem::path(parts[1]);
            } else if (parts[0] == "microphoneAudio" && parts.size() >= 2) {
                manifest.inputs.microphoneAudioPath = tempDirectory / std::filesystem::path(parts[1]);
            } else if (parts[0] == "samples" && parts.size() >= 2) {
                expectedSamples = static_cast<size_t>(std::stoull(parts[1]));
                manifest.inputs.videoSamples.reserve(expectedSamples);
            } else if (parts[0] == "sample" && parts.size() >= 6) {
                MuxedInputs::VideoSample sample;
                sample.offset = std::stoull(parts[1]);
                sample.size = static_cast<uint32_t>(std::stoul(parts[2]));
                sample.pts100ns = std::stoll(parts[3]);
                sample.duration100ns = std::stoll(parts[4]);
                sample.keyFrame = std::stoul(parts[5]) != 0;
                manifest.inputs.videoSamples.push_back(sample);
            }
        } catch (...) {
            return false;
        }
    }

    if (manifest.inputs.videoPath.empty()) {
        manifest.inputs.videoPath = tempDirectory / (manifest.inputs.codec == VideoCodec::H264 ? L"video.h264" : L"video.hevc");
    }
    if (!manifest.inputs.systemAudioPath.empty() && !std::filesystem::exists(manifest.inputs.systemAudioPath)) {
        manifest.inputs.systemAudioPath.clear();
    }
    if (!manifest.inputs.microphoneAudioPath.empty() && !std::filesystem::exists(manifest.inputs.microphoneAudioPath)) {
        manifest.inputs.microphoneAudioPath.clear();
    }

    return std::filesystem::exists(manifest.inputs.videoPath) &&
           !manifest.inputs.videoSamples.empty() &&
           (expectedSamples == 0 || manifest.inputs.videoSamples.size() == expectedSamples);
}

std::wstring preservedRecordingDetail(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& tempDirectory,
    const MuxedInputs& inputs) {
    return L"Native MP4 mux failed after retry for " + outputPath.filename().wstring() +
           L". Preserved " + std::to_wstring(inputs.videoSamples.size()) +
           L" video packets and elementary streams in " + tempDirectory.wstring() +
           L". Use Recover Failed to retry, or recover manually from the preserved video/audio files.";
}

bool muxToMp4WithRetry(const MuxedInputs& inputs, const std::filesystem::path& outputPath) {
    if (Mp4Muxer::muxToMp4(inputs, outputPath)) {
        return true;
    }

    Logger::instance().warning(L"Native MP4 mux failed; retrying once after a short delay: " + outputPath.wstring());
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    return Mp4Muxer::muxToMp4(inputs, outputPath);
}

struct ScopedComInitialization {
    ScopedComInitialization()
        : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
          initialized(SUCCEEDED(result)) {
    }

    ~ScopedComInitialization() {
        if (initialized) {
            CoUninitialize();
        }
    }

    bool usable() const {
        return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }

    HRESULT result = E_FAIL;
    bool initialized = false;
};

struct ScopedMfStartup {
    ScopedMfStartup()
        : result(MFStartup(MF_VERSION)),
          started(SUCCEEDED(result)) {
    }

    ~ScopedMfStartup() {
        if (started) {
            MFShutdown();
        }
    }

    HRESULT result = E_FAIL;
    bool started = false;
};

enum class SampleEncoding {
    Pcm,
    Float,
    Unsupported,
};

struct WavInfo {
    std::filesystem::path path;
    WaveFormatBlob format;
    uint64_t dataOffset = 0;
    uint64_t dataBytes = 0;
    uint16_t channels = 0;
    uint32_t samplesPerSec = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;
    SampleEncoding encoding = SampleEncoding::Unsupported;
};

bool isWaveExtensibleSubFormat(const GUID& guid, uint16_t tag) {
    static constexpr std::array<uint8_t, 8> kWaveSubFormatTail = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    return guid.Data1 == tag &&
           guid.Data2 == 0 &&
           guid.Data3 == 0x0010 &&
           std::equal(std::begin(guid.Data4), std::end(guid.Data4), kWaveSubFormatTail.begin());
}

SampleEncoding encodingFromFormat(const std::vector<uint8_t>& formatBytes) {
    if (formatBytes.size() < sizeof(WAVEFORMATEX)) {
        return SampleEncoding::Unsupported;
    }

    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(formatBytes.data());
    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        return SampleEncoding::Pcm;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return SampleEncoding::Float;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && formatBytes.size() >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(formatBytes.data());
        if (isWaveExtensibleSubFormat(extensible->SubFormat, WAVE_FORMAT_PCM)) {
            return SampleEncoding::Pcm;
        }
        if (isWaveExtensibleSubFormat(extensible->SubFormat, WAVE_FORMAT_IEEE_FLOAT)) {
            return SampleEncoding::Float;
        }
    }
    return SampleEncoding::Unsupported;
}

bool parseWav(const std::filesystem::path& path, WavInfo& info) {
    std::error_code fileError;
    const uintmax_t fileBytesRaw = std::filesystem::file_size(path, fileError);
    if (fileError ||
        fileBytesRaw < 12 ||
        fileBytesRaw > static_cast<uintmax_t>((std::numeric_limits<std::streamoff>::max)())) {
        return false;
    }
    const uint64_t fileBytes = static_cast<uint64_t>(fileBytesRaw);

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }

    char riff[4]{};
    uint32_t riffSize = 0;
    char wave[4]{};
    stream.read(riff, sizeof(riff));
    stream.read(reinterpret_cast<char*>(&riffSize), sizeof(riffSize));
    stream.read(wave, sizeof(wave));
    if (!stream ||
        std::string_view(riff, 4) != "RIFF" ||
        std::string_view(wave, 4) != "WAVE" ||
        riffSize < 4 ||
        static_cast<uint64_t>(riffSize) > fileBytes - 8) {
        return false;
    }

    const uint64_t riffEnd = 8ull + static_cast<uint64_t>(riffSize);
    bool foundFormat = false;
    bool foundData = false;
    while (stream && (!foundFormat || !foundData)) {
        const std::streamoff chunkHeaderOffset = static_cast<std::streamoff>(stream.tellg());
        if (chunkHeaderOffset < 0 || static_cast<uint64_t>(chunkHeaderOffset) + 8 > riffEnd) {
            break;
        }

        char id[4]{};
        uint32_t size = 0;
        stream.read(id, sizeof(id));
        stream.read(reinterpret_cast<char*>(&size), sizeof(size));
        if (!stream) {
            break;
        }

        const std::streamoff chunkDataOffset = static_cast<std::streamoff>(stream.tellg());
        if (chunkDataOffset < 0) {
            return false;
        }
        const uint64_t chunkData = static_cast<uint64_t>(chunkDataOffset);
        if (static_cast<uint64_t>(size) > riffEnd - chunkData) {
            return false;
        }
        const uint64_t nextChunk = chunkData + static_cast<uint64_t>(size) + (size & 1u);
        if (nextChunk > riffEnd + 1 || nextChunk > fileBytes) {
            return false;
        }

        if (std::string_view(id, 4) == "fmt ") {
            if (size < sizeof(WAVEFORMATEX) || size > kMaxWaveFormatBytes) {
                return false;
            }
            info.format.bytes.resize(size);
            stream.read(reinterpret_cast<char*>(info.format.bytes.data()), size);
            if (!stream) {
                return false;
            }
            const auto* format = reinterpret_cast<const WAVEFORMATEX*>(info.format.bytes.data());
            info.channels = format->nChannels;
            info.samplesPerSec = format->nSamplesPerSec;
            info.bitsPerSample = format->wBitsPerSample;
            info.blockAlign = format->nBlockAlign;
            info.encoding = encodingFromFormat(info.format.bytes);
            foundFormat = true;
        } else if (std::string_view(id, 4) == "data") {
            info.dataOffset = chunkData;
            info.dataBytes = size;
            foundData = true;
        }

        stream.seekg(static_cast<std::streamoff>(nextChunk), std::ios::beg);
    }

    const uint16_t bytesPerSample = info.bitsPerSample / 8;
    const uint32_t minimumBlockAlign = static_cast<uint32_t>(info.channels) * bytesPerSample;
    info.path = path;
    return foundFormat &&
           foundData &&
           info.channels > 0 &&
           info.samplesPerSec > 0 &&
           info.bitsPerSample > 0 &&
           info.bitsPerSample % 8 == 0 &&
           bytesPerSample > 0 &&
           info.blockAlign >= minimumBlockAlign &&
           info.encoding != SampleEncoding::Unsupported;
}

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

float sourceFrameChannelAt(AudioSource& source, uint64_t sourceFrame, uint16_t outputChannel) {
    const uint16_t sourceChannel = source.info.channels == 1
        ? 0
        : std::min<uint16_t>(outputChannel, static_cast<uint16_t>(source.info.channels - 1));
    const uint16_t bytesPerSample = source.info.bitsPerSample / 8;
    if (bytesPerSample == 0) {
        return 0.0f;
    }
    if (!ensureSourceFrameLoaded(source, sourceFrame)) {
        return 0.0f;
    }

    const size_t offset =
        static_cast<size_t>(sourceFrame - source.loadedStartFrame) * source.info.blockAlign +
        static_cast<size_t>(sourceChannel) * bytesPerSample;
    if (offset + bytesPerSample > source.chunk.size()) {
        return 0.0f;
    }
    return readSourceSample(source.chunk.data() + offset, source.info);
}

float sourceChannelAt(AudioSource& source, uint64_t outputFrame, uint16_t outputChannel, uint32_t targetSamplesPerSec) {
    if (outputFrame >= source.outputFrames || source.frames == 0 || targetSamplesPerSec == 0) {
        return 0.0f;
    }

    const long double sourcePosition = std::min<long double>(
        static_cast<long double>(source.frames - 1),
        (static_cast<long double>(outputFrame) * source.info.samplesPerSec) / targetSamplesPerSec);
    const uint64_t sourceFrame0 = static_cast<uint64_t>(sourcePosition);
    const uint64_t sourceFrame1 = std::min<uint64_t>(source.frames - 1, sourceFrame0 + 1);
    const float sample0 = sourceFrameChannelAt(source, sourceFrame0, outputChannel);
    if (sourceFrame1 == sourceFrame0) {
        return sample0;
    }

    const float sample1 = sourceFrameChannelAt(source, sourceFrame1, outputChannel);
    const float fraction = static_cast<float>(sourcePosition - static_cast<long double>(sourceFrame0));
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

bool buildMixedPcmAudio(const MuxedInputs& inputs, const std::filesystem::path& outputPath, std::filesystem::path& mixedPath) {
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

bool configureVideoStream(IMFSinkWriter* writer, const MuxedInputs& inputs, DWORD& streamIndex) {
    ComPtr<IMFMediaType> mediaType;
    HRESULT hr = MFCreateMediaType(&mediaType);
    if (FAILED(hr)) {
        return false;
    }
    mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mediaType->SetGUID(MF_MT_SUBTYPE, inputs.codec == VideoCodec::H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC);
    mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mediaType->SetUINT32(MF_MT_AVG_BITRATE, inputs.bitrateKbps * 1000);
    mediaType->SetUINT32(MF_MT_IN_BAND_PARAMETER_SET, TRUE);
    MFSetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, inputs.width, inputs.height);
    MFSetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, inputs.fps, 1);
    MFSetAttributeRatio(mediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = writer->AddStream(mediaType.Get(), &streamIndex);
    if (FAILED(hr)) {
        Logger::instance().error(L"Native MP4 mux could not add video stream: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
        return false;
    }
    hr = writer->SetInputMediaType(streamIndex, mediaType.Get(), nullptr);
    if (FAILED(hr)) {
        Logger::instance().error(L"Native MP4 mux could not set video input type: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
        return false;
    }
    return true;
}

bool configureAudioStream(IMFSinkWriter* writer, const WavInfo& wav, DWORD& streamIndex) {
    ComPtr<IMFMediaType> outputType;
    HRESULT hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) {
        return false;
    }
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kMuxAudioBitsPerSample);
    outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kMuxAudioSampleRate);
    outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kMuxAudioChannels);
    outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kMuxAudioAverageBytesPerSecond);
    outputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);

    hr = writer->AddStream(outputType.Get(), &streamIndex);
    if (FAILED(hr)) {
        Logger::instance().error(L"Native MP4 mux could not add audio stream: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
        return false;
    }

    ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) {
        return false;
    }

    hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) {
        hr = inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kMuxAudioSampleRate);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kMuxAudioChannels);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kMuxAudioBitsPerSample);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kMuxAudioChannels * kMuxAudioBitsPerSample / 8);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kMuxAudioSampleRate * kMuxAudioChannels * kMuxAudioBitsPerSample / 8);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }

    if (FAILED(hr)) {
        Logger::instance().error(L"Native MP4 mux could not create audio input type: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
        return false;
    }

    hr = writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr);
    if (FAILED(hr)) {
        Logger::instance().error(L"Native MP4 mux could not set audio input type: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
        return false;
    }
    return true;
}

bool writeVideoSamples(IMFSinkWriter* writer, DWORD streamIndex, const MuxedInputs& inputs) {
    std::ifstream stream(inputs.videoPath, std::ios::binary);
    if (!stream.is_open()) {
        Logger::instance().error(L"Native MP4 mux could not read video stream: " + inputs.videoPath.wstring());
        return false;
    }
    if (inputs.videoSamples.empty()) {
        Logger::instance().error(L"Native MP4 mux has no video sample timing data");
        return false;
    }

    const int64_t firstPts = inputs.videoSamples.front().pts100ns;
    for (const auto& videoSample : inputs.videoSamples) {
        if (videoSample.size == 0) {
            continue;
        }

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(videoSample.size, &buffer);
        if (FAILED(hr)) {
            return false;
        }

        BYTE* destination = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        hr = buffer->Lock(&destination, &maxLength, &currentLength);
        if (FAILED(hr)) {
            return false;
        }
        stream.seekg(static_cast<std::streamoff>(videoSample.offset), std::ios::beg);
        stream.read(reinterpret_cast<char*>(destination), videoSample.size);
        const bool readOk = stream.gcount() == videoSample.size;
        buffer->Unlock();
        if (!readOk) {
            Logger::instance().error(L"Native MP4 mux could not read a complete video sample");
            return false;
        }
        buffer->SetCurrentLength(videoSample.size);

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            return false;
        }
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(videoSample.pts100ns - firstPts);
        sample->SetSampleDuration(videoSample.duration100ns > 0
            ? videoSample.duration100ns
            : static_cast<int64_t>(kHundredNanosecondsPerSecond / std::max<uint32_t>(inputs.fps, 1)));
        sample->SetUINT32(MFSampleExtension_CleanPoint, videoSample.keyFrame ? TRUE : FALSE);

        hr = writer->WriteSample(streamIndex, sample.Get());
        if (FAILED(hr)) {
            Logger::instance().error(L"Native MP4 mux could not write video sample: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
            return false;
        }
    }
    return true;
}

bool writeAudioSamples(IMFSinkWriter* writer, DWORD streamIndex, const WavInfo& wav) {
    std::ifstream stream(wav.path, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(wav.dataOffset), std::ios::beg);

    constexpr uint64_t kChunkFrames = 4096;
    const uint64_t totalFrames = wav.dataBytes / wav.blockAlign;
    uint64_t framesWritten = 0;
    while (framesWritten < totalFrames) {
        const uint64_t framesThisChunk = std::min<uint64_t>(kChunkFrames, totalFrames - framesWritten);
        const DWORD bytesThisChunk = static_cast<DWORD>(framesThisChunk * wav.blockAlign);

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(bytesThisChunk, &buffer);
        if (FAILED(hr)) {
            return false;
        }

        BYTE* destination = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        hr = buffer->Lock(&destination, &maxLength, &currentLength);
        if (FAILED(hr)) {
            return false;
        }
        stream.read(reinterpret_cast<char*>(destination), bytesThisChunk);
        const bool readOk = stream.gcount() == bytesThisChunk;
        buffer->Unlock();
        if (!readOk) {
            return false;
        }
        buffer->SetCurrentLength(bytesThisChunk);

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            return false;
        }
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(static_cast<int64_t>((framesWritten * kHundredNanosecondsPerSecond) / wav.samplesPerSec));
        sample->SetSampleDuration(static_cast<int64_t>((framesThisChunk * kHundredNanosecondsPerSecond) / wav.samplesPerSec));

        hr = writer->WriteSample(streamIndex, sample.Get());
        if (FAILED(hr)) {
            Logger::instance().error(L"Native MP4 mux could not write audio sample: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
            return false;
        }
        framesWritten += framesThisChunk;
    }
    return true;
}

} // namespace

bool Mp4Muxer::startRecording(const std::filesystem::path& outputPath, const VideoSettings& settings) {
    std::scoped_lock lock(mutex_);
    if (active_) {
        return true;
    }

    outputPath_ = outputPath;
    settings_ = settings;
    systemAudio_.writer.reset();
    microphoneAudio_.writer.reset();
    systemAudio_.nextPts100ns = 0;
    microphoneAudio_.nextPts100ns = 0;
    firstVideoPts100ns_.reset();
    tempDirectory_ = outputPath.parent_path() / L".backtrack_tmp" / outputPath.stem();
    std::error_code directoryError;
    std::filesystem::create_directories(tempDirectory_, directoryError);
    if (directoryError) {
        Logger::instance().error(L"Could not create recording temporary directory: " + tempDirectory_.wstring());
        return false;
    }

    videoPath_ = tempDirectory_ / (settings.codec == VideoCodec::H264 ? L"video.h264" : L"video.hevc");
    videoStream_.open(videoPath_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!videoStream_.is_open()) {
        Logger::instance().error(L"Could not open temporary video stream: " + videoPath_.wstring());
        std::error_code cleanupError;
        std::filesystem::remove_all(tempDirectory_, cleanupError);
        return false;
    }

    videoPackets_ = 0;
    videoBytes_ = 0;
    writeFailed_ = false;
    lastError_.clear();
    videoSamples_.clear();
    active_ = true;
    Logger::instance().info(L"Recording muxer started: " + outputPath_.wstring());
    return true;
}

void Mp4Muxer::writeVideoPacket(const EncodedPacket& packet) {
    std::scoped_lock lock(mutex_);
    if (!active_ || writeFailed_ || !videoStream_.is_open() || packet.bytes.empty()) {
        return;
    }
    if (packet.bytes.size() > static_cast<size_t>((std::numeric_limits<std::streamsize>::max)())) {
        writeFailed_ = true;
        Logger::instance().error(L"Encoded video packet is too large for the temporary recording stream: " + videoPath_.wstring());
        return;
    }

    const uint64_t offset = videoBytes_;
    videoStream_.write(reinterpret_cast<const char*>(packet.bytes.data()), static_cast<std::streamsize>(packet.bytes.size()));
    if (!videoStream_) {
        writeFailed_ = true;
        Logger::instance().error(L"Could not write temporary video stream: " + videoPath_.wstring());
        return;
    }
    if (!firstVideoPts100ns_) {
        firstVideoPts100ns_ = packet.pts100ns;
        systemAudio_.nextPts100ns = packet.pts100ns;
        microphoneAudio_.nextPts100ns = packet.pts100ns;
    }
    videoSamples_.push_back(MuxedInputs::VideoSample{
        offset,
        static_cast<uint32_t>(std::min<size_t>(packet.bytes.size(), std::numeric_limits<uint32_t>::max())),
        packet.pts100ns,
        packet.duration100ns,
        packet.keyFrame});
    ++videoPackets_;
    videoBytes_ += packet.bytes.size();
}

bool Mp4Muxer::extendLastVideoDuration(int64_t expectedPts100ns, int64_t duration100ns) {
    std::scoped_lock lock(mutex_);
    if (!active_ || writeFailed_ || videoSamples_.empty() || duration100ns <= 0) {
        return false;
    }

    auto& sample = videoSamples_.back();
    if (sample.pts100ns != expectedPts100ns) {
        return false;
    }
    sample.duration100ns = std::max(sample.duration100ns, duration100ns);
    return true;
}

void Mp4Muxer::writeAudioPacket(const AudioPacket& packet) {
    std::scoped_lock lock(mutex_);
    if (!active_ || packet.bytes.empty()) {
        return;
    }

    if (!firstVideoPts100ns_) {
        return;
    }

    auto& state = packet.track == AudioTrack::System ? systemAudio_ : microphoneAudio_;
    writeTimelineAudioPacket(state, packet, packet.track == AudioTrack::System ? L"system.wav" : L"microphone.wav");
}

std::filesystem::path Mp4Muxer::finalize() {
    MuxedInputs inputs;
    std::filesystem::path outputPath;
    std::filesystem::path tempDirectory;
    uint64_t videoBytes = 0;
    bool writeFailed = false;

    {
        std::scoped_lock lock(mutex_);
        if (!active_) {
            return {};
        }

        videoStream_.close();
        systemAudio_.writer.close();
        microphoneAudio_.writer.close();
        active_ = false;

        inputs.videoPath = videoPath_;
        inputs.systemAudioPath = systemAudio_.writer.dataBytes() > 0 ? systemAudio_.writer.path() : std::filesystem::path();
        inputs.microphoneAudioPath = microphoneAudio_.writer.dataBytes() > 0 ? microphoneAudio_.writer.path() : std::filesystem::path();
        inputs.videoSamples = videoSamples_;
        inputs.codec = settings_.codec;
        inputs.fps = settings_.fps;
        inputs.width = settings_.width;
        inputs.height = settings_.height;
        inputs.bitrateKbps = settings_.bitrateKbps;
        inputs.resolutionMode = settings_.resolutionMode;
        outputPath = outputPath_;
        tempDirectory = tempDirectory_;
        videoBytes = videoBytes_;
        writeFailed = writeFailed_;
    }

    if (writeFailed) {
        std::error_code cleanupError;
        std::filesystem::remove_all(tempDirectory, cleanupError);
        const std::wstring detail = L"Recording video stream had write errors; MP4 was not created";
        {
            std::scoped_lock lock(mutex_);
            lastError_ = detail;
        }
        Logger::instance().warning(detail);
        return {};
    }

    if (videoBytes == 0) {
        std::error_code cleanupError;
        std::filesystem::remove_all(tempDirectory, cleanupError);
        const std::wstring detail = L"Recording produced no encoded video packets; MP4 was not created";
        {
            std::scoped_lock lock(mutex_);
            lastError_ = detail;
        }
        Logger::instance().warning(detail);
        return {};
    }

    writeRecoveryManifest(inputs, tempDirectory);
    writeRecoveryReadme(inputs, tempDirectory);

    if (!muxToMp4WithRetry(inputs, outputPath)) {
        std::error_code cleanupError;
        std::filesystem::remove(outputPath, cleanupError);
        const std::wstring detail = preservedRecordingDetail(outputPath, tempDirectory, inputs);
        {
            std::scoped_lock lock(mutex_);
            lastError_ = detail;
        }
        Logger::instance().warning(detail);
        return {};
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(tempDirectory, cleanupError);
    if (cleanupError) {
        Logger::instance().warning(L"Could not remove recording temporary directory: " + tempDirectory.wstring());
    }

    Logger::instance().info(L"MP4 finalized: " + outputPath.wstring());
    return outputPath;
}

void Mp4Muxer::abort() {
    std::filesystem::path tempDirectory;
    {
        std::scoped_lock lock(mutex_);
        tempDirectory = tempDirectory_;
        active_ = false;
        writeFailed_ = false;
        if (videoStream_.is_open()) {
            videoStream_.close();
        }
        systemAudio_.writer.close();
        microphoneAudio_.writer.close();
    }
    if (!tempDirectory.empty()) {
        std::error_code cleanupError;
        std::filesystem::remove_all(tempDirectory, cleanupError);
    }
}

bool Mp4Muxer::active() const {
    std::scoped_lock lock(mutex_);
    return active_;
}

uint64_t Mp4Muxer::videoPacketCount() const {
    std::scoped_lock lock(mutex_);
    return videoPackets_;
}

uint64_t Mp4Muxer::videoByteCount() const {
    std::scoped_lock lock(mutex_);
    return videoBytes_;
}

std::wstring Mp4Muxer::lastError() const {
    std::scoped_lock lock(mutex_);
    return lastError_;
}

void Mp4Muxer::writeTimelineAudioPacket(AudioTimelineState& state, const AudioPacket& packet, const wchar_t* fileName) {
    if (!firstVideoPts100ns_ || packet.frameCount == 0) {
        return;
    }
    if (!state.writer.isOpen()) {
        if (!state.writer.open(tempDirectory_ / fileName, packet.format)) {
            Logger::instance().warning(std::wstring(L"Could not open temporary audio stream: ") + fileName);
            return;
        }
        state.nextPts100ns = *firstVideoPts100ns_;
    }
    const uint32_t samplesPerSec = state.writer.samplesPerSec();
    const uint16_t blockAlign = state.writer.blockAlign();
    if (samplesPerSec == 0 || blockAlign == 0) {
        return;
    }

    const int64_t packetDuration100ns =
        static_cast<int64_t>((static_cast<int64_t>(packet.frameCount) * kHundredNanosecondsPerSecond) / samplesPerSec);
    const int64_t packetEnd100ns = packet.pts100ns + packetDuration100ns;
    if (packetEnd100ns <= state.nextPts100ns) {
        return;
    }

    if (packet.pts100ns > state.nextPts100ns) {
        const auto gapFrames = static_cast<uint64_t>(
            ((packet.pts100ns - state.nextPts100ns) * static_cast<int64_t>(samplesPerSec)) / kHundredNanosecondsPerSecond);
        state.writer.writeSilenceFrames(gapFrames);
        state.nextPts100ns += static_cast<int64_t>((gapFrames * kHundredNanosecondsPerSecond) / samplesPerSec);
    }

    uint32_t startFrame = 0;
    if (packet.pts100ns < state.nextPts100ns) {
        startFrame = static_cast<uint32_t>(std::min<uint64_t>(
            packet.frameCount,
            (((state.nextPts100ns - packet.pts100ns) * static_cast<int64_t>(samplesPerSec)) + kHundredNanosecondsPerSecond - 1) /
                kHundredNanosecondsPerSecond));
    }
    if (startFrame >= packet.frameCount) {
        return;
    }

    const size_t byteOffset = static_cast<size_t>(startFrame) * blockAlign;
    if (byteOffset >= packet.bytes.size()) {
        return;
    }
    const size_t bytesToWrite = std::min(
        packet.bytes.size() - byteOffset,
        static_cast<size_t>(packet.frameCount - startFrame) * blockAlign);
    state.writer.write(packet.bytes.data() + byteOffset, bytesToWrite);
    const uint64_t framesWritten = bytesToWrite / blockAlign;
    state.nextPts100ns += static_cast<int64_t>((framesWritten * kHundredNanosecondsPerSecond) / samplesPerSec);
}

bool Mp4Muxer::muxToMp4(const MuxedInputs& inputs, const std::filesystem::path& outputPath) {
    if (inputs.videoPath.empty() || !std::filesystem::exists(inputs.videoPath)) {
        Logger::instance().warning(L"No video elementary stream to mux");
        return false;
    }
    if (!outputPath.parent_path().empty()) {
        std::error_code directoryError;
        std::filesystem::create_directories(outputPath.parent_path(), directoryError);
        if (directoryError) {
            Logger::instance().error(L"Could not create MP4 output directory: " + outputPath.parent_path().wstring());
            return false;
        }
    }

    ScopedComInitialization com;
    if (!com.usable()) {
        Logger::instance().error(L"COM initialization failed for native MP4 mux");
        return false;
    }
    ScopedMfStartup mf;
    if (!mf.started) {
        Logger::instance().error(L"Media Foundation startup failed for native MP4 mux: 0x" + std::to_wstring(static_cast<uint32_t>(mf.result)));
        return false;
    }

    std::filesystem::path mixedAudioPath;
    if (!buildMixedPcmAudio(inputs, outputPath, mixedAudioPath)) {
        return false;
    }
    WavInfo mixedAudio;
    bool hasAudio = !mixedAudioPath.empty() && parseWav(mixedAudioPath, mixedAudio);

    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);

    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 2);
    if (FAILED(hr)) {
        return false;
    }
    attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attributes->SetUINT32(MF_MPEG4SINK_SPSPPS_PASSTHROUGH, TRUE);

    ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(outputPath.wstring().c_str(), nullptr, attributes.Get(), &writer);
    if (FAILED(hr)) {
        Logger::instance().error(L"Could not create native MP4 writer: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
        return false;
    }

    DWORD videoStreamIndex = 0;
    if (!configureVideoStream(writer.Get(), inputs, videoStreamIndex)) {
        return false;
    }

    DWORD audioStreamIndex = 0;
    if (hasAudio && !configureAudioStream(writer.Get(), mixedAudio, audioStreamIndex)) {
        Logger::instance().warning(L"Audio stream could not be configured for MP4 mux; saving video without audio");
        hasAudio = false;
        writer.Reset();
        removeError = {};
        std::filesystem::remove(outputPath, removeError);

        hr = MFCreateSinkWriterFromURL(outputPath.wstring().c_str(), nullptr, attributes.Get(), &writer);
        if (FAILED(hr)) {
            Logger::instance().error(L"Could not create native MP4 writer for video-only fallback: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
            return false;
        }

        videoStreamIndex = 0;
        if (!configureVideoStream(writer.Get(), inputs, videoStreamIndex)) {
            return false;
        }
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        Logger::instance().error(L"Native MP4 writer could not begin writing: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
        return false;
    }

    if (!writeVideoSamples(writer.Get(), videoStreamIndex, inputs)) {
        return false;
    }
    if (hasAudio && !writeAudioSamples(writer.Get(), audioStreamIndex, mixedAudio)) {
        return false;
    }

    hr = writer->Finalize();
    if (FAILED(hr)) {
        Logger::instance().error(L"Native MP4 writer finalize failed: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
        return false;
    }
    return true;
}

std::filesystem::path Mp4Muxer::recoverLatestFailedRecording(
    const std::filesystem::path& clipDirectory,
    std::wstring& detail) {
    detail.clear();
    const std::filesystem::path root = recoveryRoot(clipDirectory);
    std::error_code error;
    if (clipDirectory.empty() || !std::filesystem::exists(root, error)) {
        detail = L"No failed recording recovery data was found";
        return {};
    }

    std::optional<RecoveryManifest> selected;
    std::filesystem::file_time_type selectedTime{};
    std::filesystem::directory_iterator iterator(root, error);
    const std::filesystem::directory_iterator end;
    if (error) {
        detail = L"Could not enumerate failed recording recovery data in " + root.wstring();
        return {};
    }

    for (; iterator != end; iterator.increment(error)) {
        const auto& entry = *iterator;
        if (error) {
            error = {};
            continue;
        }
        if (!entry.is_directory(error) || error) {
            error = {};
            continue;
        }

        RecoveryManifest manifest;
        if (!readRecoveryManifest(entry.path(), manifest)) {
            continue;
        }

        const auto manifestPath = manifestPathFor(entry.path());
        const auto modified = std::filesystem::last_write_time(manifestPath, error);
        if (error) {
            error = {};
        }
        if (!selected || modified > selectedTime) {
            selected = std::move(manifest);
            selectedTime = modified;
        }
    }

    if (!selected) {
        detail = L"No recoverable failed recording manifest was found in " + root.wstring();
        return {};
    }

    const std::wstring stem = selected->tempDirectory.filename().wstring();
    const std::filesystem::path outputPath = uniqueRecoveredOutputPath(clipDirectory, stem);
    if (!muxToMp4WithRetry(selected->inputs, outputPath)) {
        std::error_code removeError;
        std::filesystem::remove(outputPath, removeError);
        detail = preservedRecordingDetail(outputPath, selected->tempDirectory, selected->inputs);
        return {};
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(selected->tempDirectory, cleanupError);
    if (cleanupError) {
        Logger::instance().warning(L"Recovered recording but could not remove recovery directory: " + selected->tempDirectory.wstring());
    }

    detail = L"Recovered failed recording to " + outputPath.filename().wstring();
    Logger::instance().info(L"Recovered failed recording: " + outputPath.wstring());
    return outputPath;
}

} // namespace backtrack
