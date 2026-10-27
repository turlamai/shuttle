#include "pch.h"
#include "TransferItem.h"
#if __has_include("TransferItem.g.cpp")
#include "TransferItem.g.cpp"
#endif

#include <cctype>
#include <sstream>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace winrt::RivetHost::implementation {

namespace {

std::wstring format_bytes(std::int64_t bytes) {
  long double const value = static_cast<long double>(bytes);
  std::wstringstream stream;
  if (bytes >= 1024LL * 1024 * 1024) {
    stream.precision(1);
    stream << std::fixed << (value / (1024.0L * 1024 * 1024)) << L" GB";
  } else if (bytes >= 1024LL * 1024) {
    stream.precision(1);
    stream << std::fixed << (value / (1024.0L * 1024)) << L" MB";
  } else if (bytes >= 1024) {
    stream.precision(1);
    stream << std::fixed << (value / 1024.0L) << L" KB";
  } else {
    stream << bytes << L" B";
  }
  return stream.str();
}

// "10-27 18:30" out of "2026-10-27 18:30:12"
std::wstring short_stamp(std::wstring const& stamp) {
  if (stamp.size() >= 16 && stamp[4] == L'-') {
    return stamp.substr(5);
  }
  return stamp;
}

std::wstring to_wide(std::string const& utf8) {
  return winrt::to_hstring(utf8).c_str();
}

}  // namespace

winrt::event_token TransferItem::PropertyChanged(
    Data::PropertyChangedEventHandler const& handler) {
  return property_changed_.add(handler);
}

void TransferItem::PropertyChanged(winrt::event_token const& token) noexcept {
  property_changed_.remove(token);
}

void TransferItem::Raise(std::wstring_view property) {
  property_changed_(*this, Data::PropertyChangedEventArgs{property});
}

void TransferItem::Apply(std::int32_t direction, hstring const& filename,
                         std::int64_t size, std::int64_t progress,
                         std::int32_t status, hstring const& message,
                         hstring const& stamp) {
  bool const changed_direction = !applied_ || direction_ != direction;
  bool const changed_filename = !applied_ || filename_ != filename;
  bool const changed_size = !applied_ || size_ != size;
  bool const changed_progress = !applied_ || progress_bytes_ != progress;
  bool const changed_status = !applied_ || status_ != status;
  bool const changed_message = !applied_ || message_text_ != message;
  bool const changed_stamp = !applied_ || size_stamp_text_ != stamp;

  direction_ = direction;
  filename_ = filename;
  size_ = size;
  progress_bytes_ = progress;
  status_ = status;
  message_text_ = message;

  if (changed_direction) {
    direction_glyph_ = (direction == 0) ? hstring(L"\u2191") : hstring(L"\u2193");
  }

  if (changed_status || changed_direction) {
    wchar_t const* key = L"status.queued";
    switch (status) {
      case 1:
        key = (direction == 0) ? L"status.active-sending"
                               : L"status.active-receiving";
        break;
      case 2: key = L"status.sent"; break;
      case 3: key = L"status.received"; break;
      case 4: key = L"status.failed"; break;
      default: break;
    }
    status_text_ = hstring(rivet_app::i18n::tr("zh", key));
  }

  if (changed_progress || changed_size) {
    progress_ = (size > 0) ? (100.0 * static_cast<double>(progress) /
                               static_cast<double>(size))
                           : 0.0;
  }

  if (changed_progress || changed_size || changed_status) {
    bool const active = (status == 1);
    progress_visibility_ = active ? Visibility::Visible : Visibility::Collapsed;
  }

  std::wstring const stamp_text =
      format_bytes(size) + L" \u00b7 " + short_stamp(std::wstring(stamp));
  size_stamp_text_ = hstring(stamp_text);

  if (!applied_) {
    // First paint: everything is new.
    applied_ = true;
    Raise(L"Id");
    Raise(L"Filename");
    Raise(L"DirectionGlyph");
    Raise(L"MessageText");
    Raise(L"StatusText");
    Raise(L"SizeStampText");
    Raise(L"Progress");
    Raise(L"ProgressVisibility");
    return;
  }

  if (changed_direction) Raise(L"DirectionGlyph");
  if (changed_filename) Raise(L"Filename");
  if (changed_message) Raise(L"MessageText");
  if (changed_status || changed_direction) Raise(L"StatusText");
  if (changed_stamp || changed_size) Raise(L"SizeStampText");
  if (changed_progress || changed_size) {
    Raise(L"Progress");
    Raise(L"ProgressVisibility");
  }
}

}  // namespace winrt::RivetHost::implementation
