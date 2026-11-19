#pragma once

#include "core/Types.h"
#include "integrations/LeagueOfLegendsIntegration.h"
#include "voice/VoiceCommandService.h"

#include <array>

namespace backtrack {

class RecorderController;
// Runtime and recent-log columns. The encoder column is separate because it only
// changes when an action re-applies settings, so the window caches it.
std::array<std::wstring, 2> diagnosticText(RecorderController& controller, const RecordingStats& stats,
    const VoiceCommandStats& voiceStats, const LeagueIntegrationStats& leagueStats);
std::wstring encoderDiagnosticText(RecorderController& controller);
std::wstring audioLivenessWarning(const RecordingStats& stats);
std::wstring exportDiagnostics(HWND window, RecorderController& controller);

} // namespace backtrack
