#include "mux/Mp4Muxer.h"

#include "audio/AudioMix.h"
#include "audio/WavReader.h"
#include "core/Logger.h"
#include "mux/RecoveryStore.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <vector>

namespace backtrack {

namespace {

using Microsoft::WRL::ComPtr;

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
    pendingAudio_.clear();
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
        flushPendingAudioLocked();
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
        // Hold audio until the first video sample anchors the timeline so short
        // recordings and pre-keyframe audio are not silently dropped.
        constexpr size_t kMaxPendingAudioPackets = 512;
        if (pendingAudio_.size() >= kMaxPendingAudioPackets) {
            pendingAudio_.erase(pendingAudio_.begin());
        }
        pendingAudio_.push_back(packet);
        return;
    }

    auto& state = packet.track == AudioTrack::System ? systemAudio_ : microphoneAudio_;
    writeTimelineAudioPacket(state, packet, packet.track == AudioTrack::System ? L"system.wav" : L"microphone.wav");
}

void Mp4Muxer::flushPendingAudioLocked() {
    if (!firstVideoPts100ns_ || pendingAudio_.empty()) {
        pendingAudio_.clear();
        return;
    }

    for (const auto& packet : pendingAudio_) {
        auto& state = packet.track == AudioTrack::System ? systemAudio_ : microphoneAudio_;
        writeTimelineAudioPacket(
            state,
            packet,
            packet.track == AudioTrack::System ? L"system.wav" : L"microphone.wav");
    }
    pendingAudio_.clear();
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
        flushPendingAudioLocked();
        systemAudio_.writer.close();
        microphoneAudio_.writer.close();
        pendingAudio_.clear();
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

    RecoveryStore::writeManifest(inputs, tempDirectory);
    RecoveryStore::writeReadme(inputs, tempDirectory);

    if (!RecoveryStore::muxToMp4WithRetry(inputs, outputPath)) {
        std::error_code cleanupError;
        std::filesystem::remove(outputPath, cleanupError);
        const std::wstring detail = RecoveryStore::preservedRecordingDetail(outputPath, tempDirectory, inputs);
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
        pendingAudio_.clear();
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
    return RecoveryStore::recoverLatestFailedRecording(clipDirectory, detail);
}

} // namespace backtrack
