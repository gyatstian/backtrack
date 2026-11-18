#pragma once

#include "core/Types.h"
#include "integrations/LeagueOfLegendsIntegration.h"
#include "voice/VoiceCommandService.h"

#include <array>

namespace backtrack {

class RecorderController;
std::array<std::wstring, 3> diagnosticText(RecorderController& controller, const RecordingStats& stats,
    const VoiceCommandStats& voiceStats, const LeagueIntegrationStats& leagueStats);
std::wstring audioLivenessWarning(const RecordingStats& stats);
std::wstring exportDiagnostics(HWND window, RecorderController& controller);

} // namespace backtrack
