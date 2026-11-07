#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>

namespace backtrack {

std::error_code saveDiagnosticsLog(const std::filesystem::path& destination, std::string_view tail);

} // namespace backtrack
