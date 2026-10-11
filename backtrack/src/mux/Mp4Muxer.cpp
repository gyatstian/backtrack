#include "mux/Mp4Muxer.h"

#include "audio/AudioConvert.h"
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
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace backtrack {

namespace {

using Microsoft::WRL::ComPtr;

// The MP4 is written here first and moved into place only after Finalize plus a
// forced disk flush, so a crash mid-save can never leave a partial file at the
// final clip path. Same volume as the output -> MoveFileEx stays atomic.
constexpr uint64_t kMuxFreeSpaceSlackBytes = 256ull * 1024 * 1024;

std::wstring formatHresult(HRESULT hr) {
    std::wstringstream stream;
    stream << L"0x" << std::uppercase << std::hex
           << std::setw(8) << std::setfill(L'0') << static_cast<uint32_t>(hr);
    return stream.str();
}

// Sharing violations surface either as a raw Win32 code or wrapped in a
// Win32-facility HRESULT depending on which layer failed (Media Foundation
// vs. a direct CreateFile/MoveFile call).
DWORD win32CodeOfHresult(HRESULT hr) {
    return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_SUCCESS;
}

double bytesToGiB(uint64_t bytes) {
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    return static_cast<double>(bytes) / kGiB;
}

std::wstring formatGiB(uint64_t bytes) {
    std::wstringstream stream;
    stream << std::fixed << std::setprecision(1) << bytesToGiB(bytes) << L" GB";
    return stream.str();
}

// Removes the staged file unless the publish already disarmed the guard.
struct StagedOutputGuard {
    std::filesystem::path path;
    bool armed = true;

    ~StagedOutputGuard() {
        if (!armed || path.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

uint64_t fileSizeOrZero(const std::filesystem::path& path) {
    if (path.empty()) {
        return 0;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

// True when path is one of our own clip-name reservations: the transient
// placeholder created when a clip name was claimed (fail-if-exists) and not
// yet published over. Reservations are always created empty AND hidden
// (FILE_ATTRIBUTE_HIDDEN, see reserveClipOutputName and
// uniqueRecoveredOutputPath), so requiring both keeps a user's own zero-byte
// .mp4 placeholder at the target from being silently removed on a failed
// save.
bool isEmptyOutputReservation(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_HIDDEN) == 0) {
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    const auto size = std::filesystem::file_size(path, error);
    return !error && size == 0;
}

// Preflight so an obviously full disk fails fast with a readable reason instead
// of an opaque Media Foundation HRESULT deep inside WriteSample/Finalize.
// Estimate covers the elementary streams, the mixed PCM copy, and the MP4.
bool ensureFreeDiskSpaceForMux(
    const MuxedInputs& inputs,
    const std::filesystem::path& outputPath,
    std::wstring& detail) {
    const uint64_t needed =
        fileSizeOrZero(inputs.videoPath) +
        2 * (fileSizeOrZero(inputs.systemAudioPath) + fileSizeOrZero(inputs.microphoneAudioPath)) +
        kMuxFreeSpaceSlackBytes;

    ULARGE_INTEGER freeToCaller{};
    ULARGE_INTEGER totalBytes{};
    ULARGE_INTEGER totalFree{};
    const std::filesystem::path volumeRoot = outputPath.parent_path().empty()
        ? std::filesystem::path(L".")
        : outputPath.parent_path();
    if (!GetDiskFreeSpaceExW(volumeRoot.c_str(), &freeToCaller, &totalBytes, &totalFree)) {
        return true; // Volume query failed; let Media Foundation surface any real problem.
    }
    if (freeToCaller.QuadPart >= needed) {
        return true;
    }

    detail = L"not enough free disk space (need about " + formatGiB(needed) +
             L", have " + formatGiB(freeToCaller.QuadPart) + L")";
    return false;
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
        Logger::instance().error(L"mux", L"Native MP4 mux could not add video stream: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
        return false;
    }
    hr = writer->SetInputMediaType(streamIndex, mediaType.Get(), nullptr);
    if (FAILED(hr)) {
        Logger::instance().error(L"mux", L"Native MP4 mux could not set video input type: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
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
        Logger::instance().error(L"mux", L"Native MP4 mux could not add audio stream: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
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
        Logger::instance().error(L"mux", L"Native MP4 mux could not create audio input type: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
        return false;
    }

    hr = writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr);
    if (FAILED(hr)) {
        Logger::instance().error(L"mux", L"Native MP4 mux could not set audio input type: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
        return false;
    }
    return true;
}

// Presentation delay of the first video sample caused by B-frame reordering:
// the stream starts decoding this long before its first frame is displayed.
// Audio must be delayed equally so tracks stay in sync. Returns 0 for streams
// without reordering.
int64_t videoCompositionDelay100ns(const MuxedInputs& inputs) {
    if (inputs.videoSamples.empty()) {
        return 0;
    }
    const MuxedInputs::VideoSample& first = inputs.videoSamples.front();
    if (!first.hasDts || first.dts100ns >= first.pts100ns) {
        return 0;
    }
    constexpr int64_t kMaxPlausibleDelay = 2 * kHundredNanosecondsPerSecond;
    return std::min(first.pts100ns - first.dts100ns, kMaxPlausibleDelay);
}

bool writeVideoSamples(IMFSinkWriter* writer, DWORD streamIndex, const MuxedInputs& inputs) {
    std::ifstream stream(inputs.videoPath, std::ios::binary);
    if (!stream.is_open()) {
        Logger::instance().error(L"mux", L"Native MP4 mux could not read video stream: " + inputs.videoPath.wstring());
        return false;
    }
    if (inputs.videoSamples.empty()) {
        Logger::instance().error(L"mux", L"Native MP4 mux has no video sample timing data");
        return false;
    }

    const int64_t firstPts = inputs.videoSamples.front().pts100ns;
    // Samples are stored in decode order, so presentation times regress within
    // every GOP when B-frames are enabled. The MPEG-4 sink requires a decode
    // timestamp on each sample in that case: stts is built from DTS deltas and
    // PTS-DTS becomes the ctts composition offset. Without it the sink drops
    // every regressing frame and the file is structurally broken.
    const bool hasDecodeTimestamps = std::any_of(
        inputs.videoSamples.begin(),
        inputs.videoSamples.end(),
        [](const MuxedInputs::VideoSample& sample) {
            return sample.hasDts && sample.dts100ns != sample.pts100ns;
        });
    // Both timelines must share one origin so composition offsets stay exact.
    const int64_t timeBase =
        hasDecodeTimestamps && inputs.videoSamples.front().hasDts
        ? inputs.videoSamples.front().dts100ns
        : firstPts;

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
            Logger::instance().error(L"mux", L"Native MP4 mux could not read a complete video sample");
            return false;
        }
        buffer->SetCurrentLength(videoSample.size);

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            return false;
        }
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(videoSample.pts100ns - timeBase);
        if (hasDecodeTimestamps) {
            const int64_t dts = videoSample.hasDts ? videoSample.dts100ns : videoSample.pts100ns;
            const int64_t dtsOffset = dts - timeBase;
            // Decode order keeps every offset non-negative, so this is a no-op on
            // valid input; it only stops a corrupt DTS wrapping to a huge delta.
            if (dtsOffset < 0) {
                Logger::instance().warning(L"mux",
                    L"Video sample decode timestamp precedes the mux time base; clamped to the base");
            }
            sample->SetUINT64(
                MFSampleExtension_DecodeTimestamp,
                static_cast<UINT64>(std::max<int64_t>(dtsOffset, 0)));
        }
        sample->SetSampleDuration(videoSample.duration100ns > 0
            ? videoSample.duration100ns
            : static_cast<int64_t>(kHundredNanosecondsPerSecond / std::max<uint32_t>(inputs.fps, 1)));
        sample->SetUINT32(MFSampleExtension_CleanPoint, videoSample.keyFrame ? TRUE : FALSE);

        hr = writer->WriteSample(streamIndex, sample.Get());
        if (FAILED(hr)) {
            Logger::instance().error(L"mux", L"Native MP4 mux could not write video sample: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
            return false;
        }
    }
    return true;
}

bool writeAudioSamples(
    IMFSinkWriter* writer,
    DWORD streamIndex,
    const WavInfo& wav,
    int64_t startOffset100ns) {
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
        sample->SetSampleTime(startOffset100ns +
            static_cast<int64_t>((framesWritten * kHundredNanosecondsPerSecond) / wav.samplesPerSec));
        sample->SetSampleDuration(static_cast<int64_t>((framesThisChunk * kHundredNanosecondsPerSecond) / wav.samplesPerSec));

        hr = writer->WriteSample(streamIndex, sample.Get());
        if (FAILED(hr)) {
            Logger::instance().error(L"mux", L"Native MP4 mux could not write audio sample: 0x" + std::to_wstring(static_cast<uint32_t>(hr)));
            return false;
        }
        framesWritten += framesThisChunk;
    }
    return true;
}

} // namespace

bool removeEmptyOutputReservation(const std::filesystem::path& path) {
    if (!isEmptyOutputReservation(path)) {
        return false;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    return !error;
}

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
    closeManifestStreamLocked();
    tempDirectory_ = outputPath.parent_path() / L".backtrack_tmp" / outputPath.stem();
    std::error_code directoryError;
    std::filesystem::create_directories(tempDirectory_, directoryError);
    if (directoryError) {
        Logger::instance().error(L"mux", L"Could not create recording temporary directory: " + tempDirectory_.wstring());
        return false;
    }

    videoPath_ = tempDirectory_ / (settings.codec == VideoCodec::H264 ? L"video.h264" : L"video.hevc");
    videoStream_.open(videoPath_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!videoStream_.is_open()) {
        Logger::instance().error(L"mux", L"Could not open temporary video stream: " + videoPath_.wstring());
        std::error_code cleanupError;
        std::filesystem::remove_all(tempDirectory_, cleanupError);
        return false;
    }

    // Crash recovery: create the manifest up front and keep it in sync with
    // every written packet, so a BSOD/OOM mid-recording still leaves muxable
    // timing data behind instead of orphan elementary streams.
    MuxedInputs partialInputs;
    partialInputs.videoPath = videoPath_;
    partialInputs.codec = settings.codec;
    partialInputs.fps = settings.fps;
    partialInputs.width = settings.width;
    partialInputs.height = settings.height;
    partialInputs.bitrateKbps = settings.bitrateKbps;
    partialInputs.resolutionMode = settings.resolutionMode;
    manifestStream_.open(tempDirectory_ / RecoveryStore::kRecoveryManifestName, std::ios::out | std::ios::trunc);
    if (manifestStream_.is_open()) {
        RecoveryStore::writeManifestHeader(manifestStream_, partialInputs);
        manifestStream_.flush();
    } else {
        Logger::instance().warning(L"mux",
            L"Could not open incremental recovery manifest; a crash during this recording will not be recoverable: " +
            tempDirectory_.wstring());
    }
    unflushedManifestSamples_ = 0;
    manifestWriteFailed_ = false;

    videoPackets_ = 0;
    videoBytes_ = 0;
    writeFailed_ = false;
    lastError_.clear();
    videoSamples_.clear();
    pendingAudio_.clear();
    lastWrittenVideoPts100ns_ = 0;
    warnedRegressingPts_ = false;
    active_ = true;
    Logger::instance().info(L"mux", L"Recording muxer started: " + outputPath_.wstring());
    return true;
}

void Mp4Muxer::writeVideoPacket(const EncodedPacket& packet) {
    std::scoped_lock lock(mutex_);
    if (!active_ || writeFailed_ || !videoStream_.is_open() || packet.bytes.empty()) {
        return;
    }
    if (packet.bytes.size() > static_cast<size_t>((std::numeric_limits<std::streamsize>::max)())) {
        writeFailed_ = true;
        Logger::instance().error(L"mux", L"Encoded video packet is too large for the temporary recording stream: " + videoPath_.wstring());
        return;
    }

    const uint64_t offset = videoBytes_;
    videoStream_.write(reinterpret_cast<const char*>(packet.bytes.data()), static_cast<std::streamsize>(packet.bytes.size()));
    if (!videoStream_) {
        writeFailed_ = true;
        Logger::instance().error(L"mux", L"Could not write temporary video stream: " + videoPath_.wstring());
        return;
    }
    if (!firstVideoPts100ns_) {
        firstVideoPts100ns_ = packet.pts100ns;
        systemAudio_.nextPts100ns = packet.pts100ns;
        microphoneAudio_.nextPts100ns = packet.pts100ns;
        flushPendingAudioLocked();
    }
    // Packets arrive in decode order. A decode timestamp below the PTS marks
    // a reordered (B-frame) stream; without one, regressing PTS cannot be
    // represented in MP4 and would corrupt the stts timeline.
    const bool hasDts = packet.hasDts && packet.dts100ns <= packet.pts100ns;
    const int64_t dts100ns = hasDts ? packet.dts100ns : packet.pts100ns;
    if (!warnedRegressingPts_ &&
        firstVideoPts100ns_ &&
        packet.pts100ns < lastWrittenVideoPts100ns_ &&
        !hasDts) {
        warnedRegressingPts_ = true;
        Logger::instance().warning(L"mux",
            L"Encoder produced video packets with regressing presentation times but no "
            L"decode timestamps; resulting MP4 timing may be invalid");
    }
    lastWrittenVideoPts100ns_ = packet.pts100ns;
    videoSamples_.push_back(MuxedInputs::VideoSample{
        offset,
        static_cast<uint32_t>(std::min<size_t>(packet.bytes.size(), std::numeric_limits<uint32_t>::max())),
        packet.pts100ns,
        dts100ns,
        hasDts,
        packet.duration100ns,
        packet.keyFrame});
    appendManifestSampleLocked(videoSamples_.back(), packet.keyFrame);
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
    if (!active_ || writeFailed_ || packet.bytes.empty()) {
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
        // The full manifest below is rewritten with exact counts/durations;
        // close the incremental handle first so both never interleave.
        closeManifestStreamLocked();
        active_ = false;

        inputs.videoPath = videoPath_;
        inputs.systemAudioPath = systemAudio_.writer.dataBytes() > 0 ? systemAudio_.writer.path() : std::filesystem::path();
        inputs.microphoneAudioPath = microphoneAudio_.writer.dataBytes() > 0 ? microphoneAudio_.writer.path() : std::filesystem::path();
        // Transfer ownership instead of copying: the sample table scales with
        // recording length and is only needed for this final mux pass. The
        // muxer is inactive from here on, so nothing else reads videoSamples_.
        inputs.videoSamples = std::move(videoSamples_);
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
        // No MP4 is staged over the clip-name reservation, so drop it here
        // too (same rule as the mux-failure path below).
        removeEmptyOutputReservation(outputPath);
        const std::wstring detail = L"Recording stream had write errors (disk full?); MP4 was not created";
        {
            std::scoped_lock lock(mutex_);
            lastError_ = detail;
        }
        Logger::instance().warning(L"mux", detail);
        return {};
    }

    if (videoBytes == 0) {
        std::error_code cleanupError;
        std::filesystem::remove_all(tempDirectory, cleanupError);
        removeEmptyOutputReservation(outputPath);
        const std::wstring detail = L"Recording produced no encoded video packets; MP4 was not created";
        {
            std::scoped_lock lock(mutex_);
            lastError_ = detail;
        }
        Logger::instance().warning(L"mux", detail);
        return {};
    }

    RecoveryStore::writeManifest(inputs, tempDirectory);
    RecoveryStore::writeReadme(inputs, tempDirectory);

    std::wstring muxErrorDetail;
    if (!RecoveryStore::muxToMp4WithRetry(inputs, outputPath, &muxErrorDetail)) {
        // The muxer never writes to outputPath directly (staged publish), so
        // anything there can only be our own empty name reservation; a foreign
        // file must survive a failed save untouched.
        removeEmptyOutputReservation(outputPath);
        const std::wstring detail =
            RecoveryStore::preservedRecordingDetail(outputPath, tempDirectory, inputs, muxErrorDetail);
        {
            std::scoped_lock lock(mutex_);
            lastError_ = detail;
        }
        Logger::instance().warning(L"mux", detail);
        return {};
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(tempDirectory, cleanupError);
    if (cleanupError) {
        Logger::instance().warning(L"mux", L"Could not remove recording temporary directory: " + tempDirectory.wstring());
    }

    Logger::instance().info(L"mux", L"MP4 finalized: " + outputPath.wstring());
    return outputPath;
}

void Mp4Muxer::abort() {
    std::filesystem::path tempDirectory;
    std::filesystem::path outputPath;
    {
        std::scoped_lock lock(mutex_);
        tempDirectory = tempDirectory_;
        outputPath = outputPath_;
        active_ = false;
        writeFailed_ = false;
        if (videoStream_.is_open()) {
            videoStream_.close();
        }
        closeManifestStreamLocked();
        systemAudio_.writer.close();
        microphoneAudio_.writer.close();
        pendingAudio_.clear();
        // Release the per-recording sample table here too so an aborted
        // recording does not pin its memory until the next startRecording().
        videoSamples_.clear();
        videoSamples_.shrink_to_fit();
    }
    if (!tempDirectory.empty()) {
        std::error_code cleanupError;
        std::filesystem::remove_all(tempDirectory, cleanupError);
    }
    // Nothing is ever published over the clip-name reservation on abort, so
    // an empty hidden file at the target can only be our own placeholder;
    // a foreign file survives untouched (see removeEmptyOutputReservation).
    removeEmptyOutputReservation(outputPath);
}

bool Mp4Muxer::active() const {
    std::scoped_lock lock(mutex_);
    return active_;
}

uint64_t Mp4Muxer::videoPacketCount() const {
    std::scoped_lock lock(mutex_);
    return videoPackets_;
}

std::wstring Mp4Muxer::lastError() const {
    std::scoped_lock lock(mutex_);
    return lastError_;
}

void Mp4Muxer::appendManifestSampleLocked(const MuxedInputs::VideoSample& sample, bool keyFrame) {
    if (!manifestStream_.is_open() || manifestWriteFailed_) {
        return;
    }
    // Flush at GOP boundaries: a crash then loses only the incomplete current
    // GOP instead of every packet still sitting in the stream buffer.
    if (keyFrame && unflushedManifestSamples_ > 0) {
        manifestStream_.flush();
        unflushedManifestSamples_ = 0;
    }
    manifestStream_ << RecoveryStore::manifestSampleLine(sample);
    ++unflushedManifestSamples_;
    if (!manifestStream_) {
        manifestWriteFailed_ = true;
        Logger::instance().warning(L"mux",
            L"Could not update the incremental recovery manifest; a crash during this recording will not be recoverable");
    }
}

void Mp4Muxer::appendManifestAudioTrackLocked(const char* trackKey, const wchar_t* fileName) {
    if (!manifestStream_.is_open() || manifestWriteFailed_) {
        return;
    }
    manifestStream_ << trackKey << '\t' << wideToUtf8(fileName) << '\n';
    manifestStream_.flush();
}

void Mp4Muxer::closeManifestStreamLocked() {
    if (manifestStream_.is_open()) {
        manifestStream_.flush();
        manifestStream_.close();
    }
    manifestStream_.clear();
    unflushedManifestSamples_ = 0;
}

void Mp4Muxer::writeTimelineAudioPacket(AudioTimelineState& state, const AudioPacket& packet, const wchar_t* fileName) {
    // Hard limit on PTS gap-fill: a long capture outage (device failure, sleep,
    // clock jump) otherwise becomes hours of written silence, ballooning the
    // temporary WAV and stalling the save. Gaps beyond this are bridged with
    // the capped fill and the audio timeline then resumes immediately; audio
    // after a longer outage plays that much earlier relative to video instead
    // of the recording growing unbounded.
    constexpr int64_t kMaxSilenceGapFill100ns = 10 * kHundredNanosecondsPerSecond;
    if (!firstVideoPts100ns_ || packet.frameCount == 0 || writeFailed_) {
        return;
    }
    // Packets are canonicalized at ingest, so every packet must match the
    // format this WAV was opened with. Should a producer ever violate that,
    // copying raw bytes would append wrong-rate samples (pitch-shifted audio
    // plus duration skew); dropping the packet keeps the timeline intact and
    // the PTS gap-fill below papers over the hole with silence.
    if (packet.format) {
        const AudioFormatInfo packetFormat = parseAudioFormat(*packet.format);
        const AudioFormatInfo streamFormat = parseAudioFormat(state.writer.format());
        const bool writerOpen = state.writer.isOpen();
        if (writerOpen && packetFormat != streamFormat) {
            if (!state.warnedFormatMismatch) {
                state.warnedFormatMismatch = true;
                Logger::instance().warning(L"mux",
                    std::wstring(L"Dropping audio packet whose format no longer matches the recording stream: ") +
                    fileName);
            }
            return;
        }
    }
    if (!state.writer.isOpen()) {
        if (!packet.format || !state.writer.open(tempDirectory_ / fileName, *packet.format)) {
            Logger::instance().warning(L"mux", std::wstring(L"Could not open temporary audio stream: ") + fileName);
            return;
        }
        state.nextPts100ns = *firstVideoPts100ns_;
        // Record the audio track in the incremental manifest as soon as its
        // stream exists, so crash recovery knows to include it.
        appendManifestAudioTrackLocked(
            packet.track == AudioTrack::System ? "systemAudio" : "microphoneAudio",
            fileName);
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
        const int64_t gap100ns = packet.pts100ns - state.nextPts100ns;
        const int64_t fill100ns = std::min<int64_t>(gap100ns, kMaxSilenceGapFill100ns);
        const auto gapFrames = static_cast<uint64_t>(
            (fill100ns * static_cast<int64_t>(samplesPerSec)) / kHundredNanosecondsPerSecond);
        const bool silenceWritten = state.writer.writeSilenceFrames(gapFrames);
        state.nextPts100ns += static_cast<int64_t>((gapFrames * kHundredNanosecondsPerSecond) / samplesPerSec);
        if (!silenceWritten) {
            noteAudioWriteFailedLocked(fileName);
            return;
        }
        if (fill100ns < gap100ns) {
            // Cap hit: bridge with the capped silence, then resynchronize the
            // pacing pointer to the incoming packet. Without the resync every
            // later packet would still lag the residual gap and trigger
            // another capped fill, re-inflating the stream. Post-outage audio
            // then plays (gap - limit) earlier against video than captured,
            // which beats an unbounded silent WAV.
            state.nextPts100ns = packet.pts100ns;
            Logger::instance().warning(L"mux",
                std::wstring(L"Audio gap of ") + std::to_wstring((gap100ns + kHundredNanosecondsPerSecond - 1) / kHundredNanosecondsPerSecond) +
                L"s exceeded the silence-fill limit; audio timeline compressed after " + fileName);
        }
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
    if (!state.writer.write(packet.bytes.data() + byteOffset, bytesToWrite)) {
        noteAudioWriteFailedLocked(fileName);
        return;
    }
    const uint64_t framesWritten = bytesToWrite / blockAlign;
    state.nextPts100ns += static_cast<int64_t>((framesWritten * kHundredNanosecondsPerSecond) / samplesPerSec);
}

// A failed temporary-WAV write (typically a full disk) must fail the save
// instead of silently truncating audio while video keeps going: stop
// consuming packets and let finalize() report the reason.
void Mp4Muxer::noteAudioWriteFailedLocked(const wchar_t* fileName) {
    if (writeFailed_) {
        return;
    }
    writeFailed_ = true;
    Logger::instance().error(L"mux",
        std::wstring(L"Recording audio stream could not be written (disk full?): ") + fileName);
}

bool Mp4Muxer::muxToMp4(
    const MuxedInputs& inputs,
    const std::filesystem::path& outputPath,
    std::wstring* errorDetail,
    unsigned stagingVariant,
    DWORD* win32Error) {
    if (win32Error) {
        *win32Error = ERROR_SUCCESS;
    }
    const auto muxStart = std::chrono::steady_clock::now();
    Logger::instance().info(L"mux",
        L"Native MP4 mux begin: samples=" + std::to_wstring(inputs.videoSamples.size()) +
        L", output=" + outputPath.wstring());

    // Every failure records a specific reason so the UI/Diagnostics page can
    // show why a save failed instead of a generic message.
    auto fail = [errorDetail](const std::wstring& reason) {
        if (errorDetail) {
            *errorDetail = reason;
        }
        Logger::instance().error(L"mux", reason);
        return false;
    };

    // error_code overload: a throwing std::filesystem::exists here would
    // escape save paths that run on non-main threads and terminate the app.
    std::error_code videoExistsError;
    if (inputs.videoPath.empty() || !std::filesystem::exists(inputs.videoPath, videoExistsError)) {
        return fail(L"No video elementary stream to mux");
    }
    if (!outputPath.parent_path().empty()) {
        std::error_code directoryError;
        std::filesystem::create_directories(outputPath.parent_path(), directoryError);
        if (directoryError) {
            return fail(L"Could not create MP4 output folder " + outputPath.parent_path().wstring() +
                        L" (" + utf8ToWide(directoryError.message()) + L")");
        }
    }

    std::wstring spaceDetail;
    if (!ensureFreeDiskSpaceForMux(inputs, outputPath, spaceDetail)) {
        return fail(L"Disk full: " + spaceDetail);
    }

    ScopedComInitialization com;
    if (!com.usable()) {
        return fail(L"COM initialization failed for native MP4 mux");
    }
    ScopedMfStartup mf;
    if (!mf.started) {
        return fail(L"Media Foundation startup failed for native MP4 mux: " + formatHresult(mf.result));
    }

    std::filesystem::path mixedAudioPath;
    if (!buildMixedPcmAudio(inputs, outputPath, mixedAudioPath)) {
        return fail(L"Mixing recording audio tracks failed before MP4 writing");
    }
    Logger::instance().debug(L"mux", L"Native MP4 mux audio mix complete");
    WavInfo mixedAudio;
    bool hasAudio = !mixedAudioPath.empty() && parseWav(mixedAudioPath, mixedAudio);

    // Atomic publish: the MP4 is written to a staging file next to the output
    // (same volume, hidden from the clip library scan), then moved into place
    // only after Finalize and a forced flush. A crash mid-save can therefore
    // never leave a partial MP4 at the final clip path. Retry attempts rotate
    // to an alternate staging name so an external handle (antivirus/indexer)
    // pinning the previous staging file does not block the next try.
    const std::filesystem::path stagingDirectory = outputPath.parent_path().empty()
        ? std::filesystem::path(L".backtrack_tmp")
        : outputPath.parent_path() / L".backtrack_tmp";
    std::error_code stagingDirectoryError;
    std::filesystem::create_directories(stagingDirectory, stagingDirectoryError);
    if (stagingDirectoryError) {
        return fail(L"Could not create MP4 staging folder " + stagingDirectory.wstring() +
                    L" (" + utf8ToWide(stagingDirectoryError.message()) + L")");
    }
    std::wstring stagingName = outputPath.stem().wstring() + L".publish.mp4";
    if (stagingVariant > 0) {
        stagingName = outputPath.stem().wstring() +
                      L".publish." + std::to_wstring(stagingVariant) + L".mp4";
    }
    const std::filesystem::path stagingPath = stagingDirectory / stagingName;
    StagedOutputGuard stagedGuard{stagingPath, true};
    std::error_code staleStagingError;
    std::filesystem::remove(stagingPath, staleStagingError);

    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 3);
    if (FAILED(hr)) {
        return fail(L"MFCreateAttributes failed: " + formatHresult(hr));
    }
    attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    // Offline remux of already-compressed elementary streams. Hardware MFT
    // enablement has stalled WriteSample/Finalize on some driver stacks.
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    attributes->SetUINT32(MF_MPEG4SINK_SPSPPS_PASSTHROUGH, TRUE);

    ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(stagingPath.wstring().c_str(), nullptr, attributes.Get(), &writer);
    if (FAILED(hr)) {
        if (win32Error) {
            *win32Error = win32CodeOfHresult(hr);
        }
        return fail(L"MFCreateSinkWriterFromURL failed: " + formatHresult(hr));
    }

    DWORD videoStreamIndex = 0;
    if (!configureVideoStream(writer.Get(), inputs, videoStreamIndex)) {
        return fail(L"MP4 writer could not configure the video stream");
    }

    DWORD audioStreamIndex = 0;
    if (hasAudio && !configureAudioStream(writer.Get(), mixedAudio, audioStreamIndex)) {
        Logger::instance().warning(L"mux", L"Audio stream could not be configured for MP4 mux; saving video without audio");
        hasAudio = false;
        writer.Reset();
        staleStagingError = {};
        std::filesystem::remove(stagingPath, staleStagingError);

        hr = MFCreateSinkWriterFromURL(stagingPath.wstring().c_str(), nullptr, attributes.Get(), &writer);
        if (FAILED(hr)) {
            if (win32Error) {
                *win32Error = win32CodeOfHresult(hr);
            }
            return fail(L"MFCreateSinkWriterFromURL failed for video-only fallback: " + formatHresult(hr));
        }

        videoStreamIndex = 0;
        if (!configureVideoStream(writer.Get(), inputs, videoStreamIndex)) {
            return fail(L"MP4 writer could not configure the video stream");
        }
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        return fail(L"MP4 writer could not begin writing: " + formatHresult(hr));
    }

    if (!writeVideoSamples(writer.Get(), videoStreamIndex, inputs)) {
        return fail(L"MP4 writer rejected a video sample");
    }
    Logger::instance().debug(L"mux", L"Native MP4 mux video samples written");
    if (hasAudio && !writeAudioSamples(writer.Get(), audioStreamIndex, mixedAudio, videoCompositionDelay100ns(inputs))) {
        return fail(L"MP4 writer rejected an audio sample");
    }
    if (hasAudio) {
        Logger::instance().debug(L"mux", L"Native MP4 mux audio samples written");
    }

    hr = writer->Finalize();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - muxStart)
                               .count();
    if (FAILED(hr)) {
        return fail(L"MF Finalize failed " + formatHresult(hr) + L" after " + std::to_wstring(elapsedMs) + L"ms");
    }
    // Release the sink's handle before flushing and moving the staged file.
    writer.Reset();

    DWORD flushError = ERROR_SUCCESS;
    if (!RecoveryStore::flushFileToDisk(stagingPath, flushError)) {
        if (win32Error) {
            *win32Error = flushError;
        }
        return fail(L"Could not flush finished MP4 to disk (Win32 error " + std::to_wstring(flushError) + L")");
    }

    // Fail-if-exists publish. The final name was claimed as an empty file when
    // the clip path was picked, so the only thing allowed to sit at the target
    // is our own reservation; a foreign file created in the meantime must never
    // be overwritten. After releasing the reservation, the plain move below
    // still fails instead of replacing should anything reappear at the name.
    std::error_code targetError;
    const bool targetExists = std::filesystem::exists(outputPath, targetError);
    if (!targetError && targetExists) {
        if (!isEmptyOutputReservation(outputPath)) {
            return fail(L"MP4 output already exists and was not overwritten: " + outputPath.wstring());
        }
        removeEmptyOutputReservation(outputPath);
    }
    if (!MoveFileExW(
            stagingPath.wstring().c_str(),
            outputPath.wstring().c_str(),
            MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        if (win32Error) {
            *win32Error = moveError;
        }
        return fail(L"Finished MP4 could not be moved into place at " + outputPath.wstring() +
                    L" (Win32 error " + std::to_wstring(moveError) + L")");
    }
    stagedGuard.armed = false;

    Logger::instance().info(L"mux",
        L"Native MP4 mux complete: " + outputPath.wstring() + L" in " + std::to_wstring(elapsedMs) + L"ms");
    return true;
}

std::filesystem::path Mp4Muxer::recoverLatestFailedRecording(
    const std::filesystem::path& clipDirectory,
    std::wstring& detail) {
    return RecoveryStore::recoverLatestFailedRecording(clipDirectory, detail);
}

} // namespace backtrack
