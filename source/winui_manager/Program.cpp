#include "pch.h"
#include "App.xaml.h"
#include "../program_mode.h"
#include "../program_log.h"
#include "../macro_recording_log.h"
#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>
#include <winrt/Microsoft.Windows.Globalization.h>

int TrayProgramMain(HINSTANCE,HINSTANCE,PWSTR,int);

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show) {
    Localization::ConfigureNativeLanguage();
    // No WinRT apartment or Windows App SDK initialization occurs on the tray path.
    if(ProgramMode::HasArgument(L"--tray"))return TrayProgramMain(instance,previous,command,show);
    HMODULE bootstrap=nullptr;
    using Initialize=HRESULT(WINAPI*)(UINT32,PCWSTR,PACKAGE_VERSION,MddBootstrapInitializeOptions);
    using Shutdown=void(WINAPI*)();
    Shutdown shutdown=nullptr;
    bool initialized=false;
    int result=0;
    try {
        wchar_t module[32768]{};
        DWORD length=GetModuleFileNameW(nullptr,module,ARRAYSIZE(module));
        winrt::check_bool(length && length<ARRAYSIZE(module));
        std::wstring path(module,length);
        {
            const auto root=path.substr(0,path.find_last_of(L"\\/"));
            ProgramLog::Configure(root,root+L"\\settings-0.0.7-alpha.ini","manager");ProgramLog::Write("program_started");
        }
        path.resize(path.find_last_of(L"\\/")+1);path+=L"Microsoft.WindowsAppRuntime.Bootstrap.dll";
        bootstrap=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!bootstrap)winrt::throw_last_error();
        auto initialize=reinterpret_cast<Initialize>(GetProcAddress(bootstrap,"MddBootstrapInitialize2"));
        shutdown=reinterpret_cast<Shutdown>(GetProcAddress(bootstrap,"MddBootstrapShutdown"));
        if(!initialize||!shutdown)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
        PACKAGE_VERSION version{WINDOWSAPPSDK_RUNTIME_VERSION_UINT64};
        winrt::check_hresult(initialize(WINDOWSAPPSDK_RELEASE_MAJORMINOR,WINDOWSAPPSDK_RELEASE_VERSION_TAG_W,
                                       version,MddBootstrapInitializeOptions_None));
        initialized=true;
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        struct ApartmentScope {
            ~ApartmentScope(){winrt::clear_factory_cache();winrt::uninit_apartment();}
        } apartment;
        winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(Localization::LanguageTag());
        winrt::Microsoft::UI::Xaml::Application::Start([](auto&&){
            winrt::make<winrt::G3Companion::implementation::App>();
        });
    } catch(winrt::hresult_error const& error) {
        result=1;ProgramLog::Write("startup_failed",{},static_cast<DWORD>(error.code().value));
        {
            std::wstring message=Localization::Format(Localization::T294,{{L"details",error.message().c_str()}});
            MessageBoxW(nullptr,message.c_str(),L"G3 Companion",MB_OK|MB_ICONERROR);
        }
    }
    MacroRecordingLog::Drain();
    if(initialized)shutdown();
    if(bootstrap)FreeLibrary(bootstrap);
    if(ProgramLog::Enabled())ProgramLog::Write("program_stopped","exit_code="+std::to_string(result));
    return result;
}
