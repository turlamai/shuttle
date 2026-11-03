#pragma once

// UI language selection shared by every translation unit that renders
// text. Empty override = follow the system UI language; otherwise
// "zh" or "en" as chosen in Settings. Written only from the UI thread
// (startup and the settings dialog); read from the UI thread when
// rendering rows and static texts.
#include <string>
#include <Windows.h>

namespace rivet_app::i18n {

inline std::string g_override_language;

inline std::string const& ActiveLanguage() {
  static std::string const system_language = [] {
    return PRIMARYLANGID(::GetUserDefaultUILanguage()) == 0x0004
               ? std::string{"zh"}
               : std::string{"en"};
  }();
  if (!g_override_language.empty()) {
    return g_override_language;
  }
  return system_language;
}

inline void SetOverrideLanguage(std::string const& language) {
  g_override_language = language; // "auto" clears the override
}

}  // namespace rivet_app::i18n
