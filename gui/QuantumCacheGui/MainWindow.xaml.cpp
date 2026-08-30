#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::QuantumCacheGui::implementation {

namespace {

// Must match the default in
// src/Configuration/include/QuantumCache/Configuration/AppConfig.h
// (ipcPipeName). Stage 1 hardcodes this; reading it from the shared
// config file the Service also reads is a natural follow-up once an
// on-disk config location convention is finalized, not a Stage-1 gap in
// the power-resilience design itself.
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\QuantumCacheControl";

winrt::hstring DescribeRecoveryState(std::uint32_t state) {
    // Mirrors QuantumCache::PowerResilience::RecoveryState numeric values.
    switch (state) {
        case 0: return L"Unknown";
        case 1: return L"Clean shutdown (no recovery needed)";
        case 2: return L"Unclean shutdown detected (possible power loss)";
        case 3: return L"Recovery in progress...";
        case 4: return L"Recovery complete";
        case 5: return L"Recovery FAILED — store may be inconsistent";
        default: return L"Invalid state value";
    }
}

} // namespace

MainWindow::MainWindow()
    : statusClient_(std::make_unique<::QuantumCacheGui::RecoveryStatusClient>(kPipeName)) {
    InitializeComponent();
}

void MainWindow::OnRefreshClicked(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    RefreshStatus();
}

void MainWindow::RefreshStatus() {
    auto status = statusClient_->FetchStatus();
    if (!status.has_value()) {
        RecoveryStateText().Text(
            L"Service unreachable (not running, or IPC pipe not yet available).");
        SessionGenerationText().Text(L"Session generation: —");
        return;
    }

    RecoveryStateText().Text(DescribeRecoveryState(status->recoveryState));
    SessionGenerationText().Text(
        L"Session generation: " + winrt::to_hstring(status->sessionGeneration));
}

} // namespace winrt::QuantumCacheGui::implementation
