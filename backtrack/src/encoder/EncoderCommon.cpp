#include "encoder/EncoderCommon.h"

#include "encoder/BitstreamUtils.h"

#include <algorithm>

namespace backtrack {

EncoderEffectiveSettings effectiveSettingsFor(const VideoSettings& settings) {
    EncoderEffectiveSettings effective;
    effective.profile = settings.encoderProfile;

    switch (settings.encoderProfile) {
    case EncoderProfile::LowestGpu:
        effective.preset = EncoderPreset::P1;
        effective.mode = EncoderMode::UltraLowLatency;
        effective.zeroReorderDelay = true;
        effective.referenceFrames = 1;
        break;
    case EncoderProfile::Balanced:
        effective.preset = EncoderPreset::P3;
        effective.mode = EncoderMode::LowLatency;
        effective.zeroReorderDelay = true;
        effective.referenceFrames = 2;
        break;
    case EncoderProfile::Custom:
        effective.preset = settings.encoderPreset;
        effective.mode = settings.encoderMode;
        effective.lookahead = settings.encoderLookahead;
        effective.lookaheadDepth = std::min<uint32_t>(31, settings.encoderLookaheadDepth);
        effective.spatialAQ = settings.encoderSpatialAQ;
        effective.aqStrength = std::clamp<uint32_t>(settings.encoderAQStrength, 1, 15);
        effective.temporalAQ = settings.encoderTemporalAQ;
        effective.multipass = settings.encoderMultipass;
        effective.bFrames = settings.encoderBFrames;
        effective.adaptiveBFrames = settings.encoderAdaptiveBFrames;
        effective.adaptiveIFrames = settings.encoderAdaptiveIFrames;
        effective.zeroReorderDelay = settings.encoderZeroReorderDelay;
        effective.referenceFrames = std::clamp<uint32_t>(settings.encoderReferenceFrames, 1, 4);
        break;
    }

    if (!effective.lookahead) {
        effective.lookaheadDepth = 0;
        effective.adaptiveBFrames = false;
        effective.adaptiveIFrames = false;
    }
    if (!effective.spatialAQ) {
        effective.aqStrength = 0;
    }
    if (!effective.bFrames) {
        effective.adaptiveBFrames = false;
    }
    return effective;
}

void ensureKeyFrameCarriesHeaders(
    VideoCodec codec,
    EncodedPacket& packet,
    const std::vector<uint8_t>& cachedParameterSets) {
    if (!packet.keyFrame || bitstreamContainsParameterSets(codec, packet.bytes)) {
        return;
    }
    if (cachedParameterSets.empty()) {
        return;
    }
    std::vector<uint8_t> withHeaders;
    withHeaders.reserve(cachedParameterSets.size() + packet.bytes.size());
    withHeaders.insert(withHeaders.end(), cachedParameterSets.begin(), cachedParameterSets.end());
    withHeaders.insert(withHeaders.end(), packet.bytes.begin(), packet.bytes.end());
    packet.bytes = std::move(withHeaders);
}

bool finalizePacket(
    VideoCodec codec,
    const uint8_t* data,
    size_t size,
    int64_t driverPts100ns,
    bool hasDriverPts,
    int64_t driverDuration100ns,
    int64_t fallbackPts100ns,
    int64_t fallbackDuration100ns,
    bool metadataIdr,
    DecodeOrderTimestamps& timestamps,
    std::vector<uint8_t>& cachedParameterSets,
    EncoderStats& stats,
    std::chrono::steady_clock::time_point submittedAt,
    EncodedPacket& out) {
    EncodedPacket packet;
    if (data != nullptr && size > 0) {
        packet.bytes.assign(data, data + size);
    }
    // Driver timestamps echo the submitted PTS; validity arrives as an
    // explicit flag because timestamps can legitimately be zero or negative
    // and no sentinel value works.
    packet.pts100ns = hasDriverPts ? driverPts100ns : fallbackPts100ns;
    packet.duration100ns =
        driverDuration100ns != 0 ? driverDuration100ns : fallbackDuration100ns;
    packet.dts100ns = timestamps.onOutput(packet.pts100ns, packet.duration100ns);
    packet.hasDts = true;
    packet.codec = codec;
    // Only real IDRs are cut points: plain I or open-GOP CRA frames leave the
    // decoder referencing pre-cut pictures.
    packet.keyFrame = metadataIdr ||
        (bitstreamContainsKeyFrame(codec, packet.bytes) &&
         !bitstreamContainsCraFrame(codec, packet.bytes));
    // Empty first: prepending headers to a zero-byte payload would fabricate a
    // packet out of headers alone.
    if (packet.bytes.empty()) {
        ++stats.droppedFrames;
        return false;
    }
    ensureKeyFrameCarriesHeaders(codec, packet, cachedParameterSets);
    if (cachedParameterSets.empty()) {
        std::vector<uint8_t> parameterSets;
        if (extractParameterSets(codec, packet.bytes, parameterSets)) {
            cachedParameterSets = std::move(parameterSets);
        }
    }

    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - submittedAt)
                             .count();
    stats.averageEncodeMs = stats.encodedFrames == 0
        ? elapsed
        : (stats.averageEncodeMs * 0.95 + elapsed * 0.05);
    ++stats.encodedFrames;
    stats.encodedBytes += packet.bytes.size();
    if (packet.keyFrame) {
        ++stats.keyFrames;
    }
    out = std::move(packet);
    return true;
}

} // namespace backtrack
