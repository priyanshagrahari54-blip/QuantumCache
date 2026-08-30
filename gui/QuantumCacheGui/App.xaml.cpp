#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::QuantumCacheGui::implementation {

App::App() { InitializeComponent(); }

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    window_ = make<MainWindow>();
    window_.Activate();
}

} // namespace winrt::QuantumCacheGui::implementation
