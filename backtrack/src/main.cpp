#include "app/Application.h"
#include "audio/WasapiCapture.h"
#include "core/Logger.h"

#include <Windows.h>

#include <exception>
#include <sstream>

namespace {

// sound-separation mutes are global system state; attempt restoration before
// constructing or logging a message, either of which can fail during a crash.
LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exceptionPointers) {
    backtrack::WasapiCapture::emergencyRestoreSessionMutes();
    std::wstringstream message;
    message << L"Unhandled SEH exception";
    if (exceptionPointers && exceptionPointers->ExceptionRecord) {
        const auto* record = exceptionPointers->ExceptionRecord;
        message << L": code=0x" << std::hex << std::uppercase << record->ExceptionCode
                << L", address=" << record->ExceptionAddress;
    }
    backtrack::Logger::instance().error(L"fatal", message.str());
    backtrack::Logger::instance().flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

void terminateHandler() {
    backtrack::WasapiCapture::emergencyRestoreSessionMutes();
    std::wstring message = L"std::terminate called";
    if (const auto exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            message += L": ";
            message += backtrack::utf8ToWide(error.what());
        } catch (...) {
            message += L": non-std exception";
        }
    }
    backtrack::Logger::instance().error(L"fatal", message);
    backtrack::Logger::instance().flush();
    std::abort();
}

BOOL WINAPI consoleCtrlHandler(DWORD controlType) {
    // GUI app: this mostly covers CTRL_SHUTDOWN/CTRL_LOGOFF style events.
    switch (controlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    case CTRL_LOGOFF_EVENT:
        backtrack::WasapiCapture::emergencyRestoreSessionMutes();
        // Console control events terminate through the default handler, which
        // does not run Logger's destructor. Flush so buffered lines survive.
        backtrack::Logger::instance().flush();
        break;
    default:
        break;
    }
    return FALSE; // never swallow default termination handling
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
    std::set_terminate(terminateHandler);
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    backtrack::Application app;
    return app.run(instance, commandLine, showCommand);
}
