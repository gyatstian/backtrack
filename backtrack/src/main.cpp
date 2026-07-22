#include "app/Application.h"

#include <Windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    backtrack::Application app;
    return app.run(instance, commandLine, showCommand);
}
