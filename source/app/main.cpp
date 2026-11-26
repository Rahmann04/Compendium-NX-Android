#include <borealis.hpp>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <exception>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#endif

#include "app_state.hpp"
#include "debug_script.hpp"
#include "main_activity.hpp"
#include "platform.hpp"

namespace {

std::mutex logMutex;

void writeRuntimeLog(const char* level, const char* message) {
#ifdef __ANDROID__
    JNIEnv* env = reinterpret_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return;

    jclass logger = env->FindClass("com/borealis/demo/CompendiumLogger");
    if (!logger) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    jmethodID method = env->GetStaticMethodID(
        logger, "logNative",
        "(Ljava/lang/String;Ljava/lang/String;)V"
    );

    if (method) {
        jstring javaLevel = env->NewStringUTF(level ? level : "INFO");
        jstring javaMessage = env->NewStringUTF(message ? message : "");

        if (javaLevel && javaMessage) {
            env->CallStaticVoidMethod(logger, method, javaLevel, javaMessage);
        }

        if (env->ExceptionCheck()) env->ExceptionClear();
        if (javaLevel) env->DeleteLocalRef(javaLevel);
        if (javaMessage) env->DeleteLocalRef(javaMessage);
    } else if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    env->DeleteLocalRef(logger);
#endif
}

#ifdef __ANDROID__
void SDLCALL sdlLogCallback(void*, int category,
                            SDL_LogPriority priority, const char* message) {
    const char* level = "INFO";
    if (priority >= SDL_LOG_PRIORITY_ERROR) level = "ERROR";
    else if (priority >= SDL_LOG_PRIORITY_WARN) level = "WARN";
    else if (priority <= SDL_LOG_PRIORITY_DEBUG) level = "DEBUG";

    char entry[2048];
    std::snprintf(entry, sizeof(entry), "category=%d %s",
                  category, message ? message : "");
    writeRuntimeLog(level, entry);
}
#endif

}

int main(int argc, char* argv[]) {
    writeRuntimeLog("INFO", "MAIN_ENTERED");

#ifdef __ANDROID__
    SDL_LogSetOutputFunction(sdlLogCallback, nullptr);
    writeRuntimeLog("INFO", "SDL_LOG_CAPTURE_ENABLED");
#endif

    try {
#ifdef __ANDROID__
        const char* internalStorage = SDL_AndroidGetInternalStoragePath();
        if (!internalStorage || !*internalStorage || chdir(internalStorage) != 0)
            throw std::runtime_error("Cannot enter app internal storage.");

        struct stat resourceStatus {};
        if (stat("resources", &resourceStatus) != 0 ||
            !S_ISDIR(resourceStatus.st_mode))
            throw std::runtime_error("App resources directory is missing.");

        writeRuntimeLog("INFO", "ANDROID_RESOURCE_CWD_READY");
#endif

        for (int i = 1; i < argc; i++) {
            if (std::strcmp(argv[i], "-d") == 0)
                brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
            if (std::strcmp(argv[i], "-v") == 0)
                brls::Application::enableDebuggingView(true);
        }

        writeRuntimeLog("INFO", "BOREALIS_INIT_START");
        if (!brls::Application::init()) {
            writeRuntimeLog("FATAL", "BOREALIS_INIT_FAILED");
            brls::Logger::error("Unable to init Borealis application");
            return EXIT_FAILURE;
        }
        writeRuntimeLog("INFO", "BOREALIS_INIT_SUCCESS");

        brls::Application::createWindow(APP_TITLE);
        writeRuntimeLog("INFO", "WINDOW_CREATED");

        brls::Application::getPlatform()->setThemeVariant(
            brls::ThemeVariant::DARK);
        brls::Application::setGlobalQuit(true);

        writeRuntimeLog("INFO", "PLATFORM_INIT_START");
        app::platform::init();
        writeRuntimeLog("INFO", "PLATFORM_INIT_SUCCESS");

        writeRuntimeLog("INFO", "APP_STATE_INIT_START");
        app::AppState::get().init();
        writeRuntimeLog("INFO", "APP_STATE_INIT_SUCCESS");

        brls::Application::pushActivity(new app::MainActivity());
        writeRuntimeLog("INFO", "MAIN_ACTIVITY_PUSHED");

        app::debug::startScriptFromEnv();
        writeRuntimeLog("INFO", "MAIN_LOOP_START");

        while (brls::Application::mainLoop())
            app::debug::afterFrame();

        writeRuntimeLog("INFO", "MAIN_LOOP_EXIT");

        app::AppState::get().shutdown();
        writeRuntimeLog("INFO", "APP_STATE_SHUTDOWN");

        app::platform::exit();
        writeRuntimeLog("INFO", "PLATFORM_EXIT");

        writeRuntimeLog("INFO", "MAIN_SUCCESS");
        return EXIT_SUCCESS;

    } catch (const std::exception& error) {
        writeRuntimeLog("FATAL", error.what());
        return EXIT_FAILURE;
    } catch (...) {
        writeRuntimeLog("FATAL", "UNKNOWN_NATIVE_EXCEPTION");
        return EXIT_FAILURE;
    }
}
