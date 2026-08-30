#pragma once

#include "MainWindow.xaml.g.h"
#include "RecoveryStatusClient.h"
#include <memory>

namespace winrt::QuantumCacheGui::implementation {

// STATUS: real, hand-written source; unbuilt/unverified in the sandbox
// that produced it (no WinUI 3 toolchain available there — see
// docs/ENVIRONMENT.md). Stage 1 scope only: read-only recovery-status
// display via RecoveryStatusClient. No cache control/configuration UI.
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();

    void OnRefreshClicked(
        winrt::Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& args);

private:
    void RefreshStatus();

    std::unique_ptr<::QuantumCacheGui::RecoveryStatusClient> statusClient_;
};

} // namespace winrt::QuantumCacheGui::implementation

namespace winrt::QuantumCacheGui::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
