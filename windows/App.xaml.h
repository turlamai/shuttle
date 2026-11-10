#pragma once

#include "pch.h"
#include "App.xaml.g.h"

namespace winrt::RivetHost::implementation {

struct App : AppT<App> {
  App();
  void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

 private:
  Microsoft::UI::Xaml::Window window_{nullptr};
  // Single-instance ownership: held for the whole process lifetime; the
  // OS releases it when the process exits (including the update
  // handoff's exit-before-relaunch flow). Null in a duplicate launch.
  winrt::handle instance_mutex_;
};

}  // namespace winrt::RivetHost::implementation
