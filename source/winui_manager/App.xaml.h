#pragma once
#include "App.xaml.g.h"
#include "DeviceController.h"
#include "../manager_window_size.h"

namespace winrt::G3Companion::implementation
{
    struct App : AppT<App>
    {
        App();
        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
    private:
        ManagerWindowSize::State windowSize;
        Microsoft::UI::Xaml::Window window{nullptr};
        std::unique_ptr<DeviceController> deviceController;
    };
}
