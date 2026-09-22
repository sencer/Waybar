#pragma once

#include <gio/gio.h>
#include <mutex>
#include <string>
#include "ALabel.hpp"

namespace waybar::modules {

class Quiet : public ALabel {
 public:
  Quiet(const std::string&, const Json::Value&);
  virtual ~Quiet();
  auto update() -> void override;

 private:
  bool handleToggle(GdkEventButton* const& e) override;
  static void onSignalReceived(GDBusConnection* connection,
                               const gchar* sender_name,
                               const gchar* object_path,
                               const gchar* interface_name,
                               const gchar* signal_name,
                               GVariant* parameters,
                               gpointer user_data);
  void handleNotificationUpdate(GVariant* parameters);
  void clearPreview();
  bool onTimeout();

  GDBusConnection* connection_{nullptr};
  guint subscription_id_{0};
  sigc::connection timer_connection_;

  uint32_t count_{0};
  uint32_t urgency_{0};
  uint32_t preview_timeout_seconds_{5};
  bool arm_timer_{false};
  std::string message_preview_;
  std::mutex mutex_;
};

}  // namespace waybar::modules
