#pragma once

#include "pch.h"
#include "TransferItem.g.h"
#include "i18n.hpp"

namespace winrt::RivetHost::implementation {

// One row in the transfers list. Events arrive as immutable snapshots;
// Apply() diffs against the current fields and raises PropertyChanged
// only for what actually changed, so a fast progress stream does not
// spam the whole row. Apply is an implementation-side helper (the UI
// binds the read-only properties below), not part of the projected API.
struct TransferItem : TransferItemT<TransferItem> {
  TransferItem() = default;

  winrt::event_token PropertyChanged(
      winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
  void PropertyChanged(winrt::event_token const& token) noexcept;

  void Apply(std::int32_t direction, winrt::hstring const& filename,
             std::int64_t size, std::int64_t progress, std::int32_t status,
             winrt::hstring const& message, winrt::hstring const& stamp);

  hstring Id() const { return id_; }
  hstring Filename() const { return filename_; }
  hstring DirectionGlyph() const { return direction_glyph_; }
  hstring MessageText() const { return message_text_; }
  hstring StatusText() const { return status_text_; }
  hstring SizeStampText() const { return size_stamp_text_; }
  double Progress() const { return progress_; }
  winrt::Microsoft::UI::Xaml::Visibility ProgressVisibility() const {
    return progress_visibility_;
  }

  void Id(hstring const& value) { id_ = value; }

 private:
  void Raise(std::wstring_view property);

  winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler>
      property_changed_;

  hstring id_{};
  hstring filename_{};
  hstring direction_glyph_{};
  hstring message_text_{};
  hstring status_text_{};
  hstring size_stamp_text_{};
  double progress_ = 0.0;
  winrt::Microsoft::UI::Xaml::Visibility progress_visibility_ =
      winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
  std::int32_t direction_ = 0;   // 0 send, 1 receive
  std::int32_t status_ = 0;      // TransferStatus ordinal
  std::int64_t size_ = 0;
  std::int64_t progress_bytes_ = 0;
  bool applied_ = false;
};

}  // namespace winrt::RivetHost::implementation

namespace winrt::RivetHost::factory_implementation {

struct TransferItem : TransferItemT<TransferItem, implementation::TransferItem> {};

}  // namespace winrt::RivetHost::factory_implementation
