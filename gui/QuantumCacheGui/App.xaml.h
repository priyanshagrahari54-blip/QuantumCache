#pragma once

#include "App.xaml.g.h"

namespace winrt::QuantumCacheGui::implementation {

// Application entry point. Stage 1 responsibility: create the single
// MainWindow and nothing else — no cache control logic lives here.
struct App : AppT<App> {
    App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

private:
    winrt::Microsoft::UI::Xaml::Window window_{nullptr};
};

} // namespace winrt::QuantumCacheGui::implementation

namespace winrt::QuantumCacheGui::factory_implementation {
struct App : AppT<App, implementation::App> {};
}
