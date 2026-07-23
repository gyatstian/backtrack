#include "app/Application.h"
#include "core/Logger.h"

#include <Windows.h>

#include <exception>
#include <sstream>

namespace {

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exceptionPointers) {
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

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
    std::set_terminate(terminateHandler);
    backtrack::Application app;
    return app.run(instance, commandLine, showCommand);
}
