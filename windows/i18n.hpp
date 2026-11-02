#pragma once

// Single-source string lookup (issue #242). GeneratedI18n.hpp carries the
// embedded table regenerated from shared/i18n/{zh,en}.json by
// `raco rivet i18n`; tr() consults the active language, then the other
// language, then the key, mirroring the Swift loader byte for byte.
#include <string_view>

#include "GeneratedI18n.hpp"

namespace rivet_app::i18n {

inline std::string_view tr(std::string_view language, std::string_view key) {
  for (auto const& entry : kI18nTable) {
    if (entry.key != key) {
      continue;
    }
    auto const& active = (language == "zh") ? entry.zh : entry.en;
    if (!active.empty()) {
      return active;
    }
    auto const& other = (language == "zh") ? entry.en : entry.zh;
    if (!other.empty()) {
      return other;
    }
    break;
  }
  return key;
}

}  // namespace rivet_app::i18n
