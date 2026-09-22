#include "modules/quiet.hpp"

#include <glibmm/main.h>
#include <spdlog/spdlog.h>

namespace waybar::modules {

Quiet::Quiet(const std::string& id, const Json::Value& config)
    : ALabel(config, "quiet", id, "{count}", 0, false, true),
      preview_timeout_seconds_(config["preview-timeout"].isUInt()
                                   ? config["preview-timeout"].asUInt()
                                   : 5) {
  event_box_.set_no_show_all(true);
  event_box_.hide();
  label_.show();
  GError* error = nullptr;
  connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!connection_) {
    spdlog::error("Quiet module: Failed to connect to session bus: {}", error ? error->message : "unknown");
    if (error) g_error_free(error);
    return;
  }

  subscription_id_ = g_dbus_connection_signal_subscribe(
      connection_,
      "org.freedesktop.Notifications",
      "org.freedesktop.Notifications",
      "NotificationsUpdated",
      "/org/freedesktop/Notifications",
      nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE,
      onSignalReceived,
      this,
      nullptr);

  // Request initial count from notification daemon
  g_dbus_connection_call(
      connection_,
      "org.freedesktop.Notifications",
      "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications",
      "SignalNotificationCount",
      nullptr,
      nullptr,
      G_DBUS_CALL_FLAGS_NO_AUTO_START,
      500,
      nullptr,
      nullptr,
      nullptr);

  dp.emit();
}

Quiet::~Quiet() {
  if (timer_connection_.connected()) {
    timer_connection_.disconnect();
  }
  if (connection_ && subscription_id_) {
    g_dbus_connection_signal_unsubscribe(connection_, subscription_id_);
  }
  if (connection_) {
    g_object_unref(connection_);
  }
}

void Quiet::onSignalReceived(GDBusConnection* connection,
                             const gchar* sender_name,
                             const gchar* object_path,
                             const gchar* interface_name,
                             const gchar* signal_name,
                             GVariant* parameters,
                             gpointer user_data) {
  auto* self = static_cast<Quiet*>(user_data);
  if (self) {
    self->handleNotificationUpdate(parameters);
  }
}

void Quiet::handleNotificationUpdate(GVariant* parameters) {
  if (!parameters) return;

  uint32_t mode = 0;
  uint32_t num = 0;
  uint32_t urgency = 0;
  const gchar* msg = nullptr;

  // Expected signature: (uuus) or (iiis)
  GVariantIter iter;
  g_variant_iter_init(&iter, parameters);
  GVariant* v_mode = g_variant_iter_next_value(&iter);
  GVariant* v_num = g_variant_iter_next_value(&iter);
  GVariant* v_urgency = g_variant_iter_next_value(&iter);
  GVariant* v_msg = g_variant_iter_next_value(&iter);

  if (v_mode && v_num && v_urgency && v_msg) {
    mode = g_variant_is_of_type(v_mode, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(v_mode) : g_variant_get_int32(v_mode);
    num = g_variant_is_of_type(v_num, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(v_num) : g_variant_get_int32(v_num);
    urgency = g_variant_is_of_type(v_urgency, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(v_urgency) : g_variant_get_int32(v_urgency);
    msg = g_variant_get_string(v_msg, nullptr);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    count_ = num;
    urgency_ = urgency;

    if (mode == 0 && msg && *msg && count_ > 0 && preview_timeout_seconds_ > 0) {
      message_preview_ = msg;
      arm_timer_ = true;
    } else {
      message_preview_.clear();
      arm_timer_ = false;
    }
  }

  if (v_mode) g_variant_unref(v_mode);
  if (v_num) g_variant_unref(v_num);
  if (v_urgency) g_variant_unref(v_urgency);
  if (v_msg) g_variant_unref(v_msg);

  dp.emit();
}

bool Quiet::onTimeout() {
  clearPreview();
  return false;
}

void Quiet::clearPreview() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    message_preview_.clear();
    arm_timer_ = false;
  }
  dp.emit();
}

bool Quiet::handleToggle(GdkEventButton* const& e) {
  if (e->button == 1 && connection_) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      message_preview_.clear();
      arm_timer_ = false;
    }
    g_dbus_connection_call(
        connection_,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "ShowNotifications",
        nullptr,
        nullptr,
        G_DBUS_CALL_FLAGS_NO_AUTO_START,
        500,
        nullptr,
        nullptr,
        nullptr);
    dp.emit();
    return true;
  }
  return ALabel::handleToggle(e);
}

auto Quiet::update() -> void {
  std::string display_text;
  bool is_urgent = false;
  bool should_arm_timer = false;
  bool has_preview = false;
  uint32_t current_count = 0;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    current_count = count_;
    is_urgent = (urgency_ == 2);
    should_arm_timer = arm_timer_;
    arm_timer_ = false;
    has_preview = !message_preview_.empty();
    display_text = has_preview ? message_preview_ : std::to_string(count_);
  }

  if (should_arm_timer) {
    if (timer_connection_.connected()) {
      timer_connection_.disconnect();
    }
    timer_connection_ = Glib::signal_timeout().connect_seconds(
        sigc::mem_fun(*this, &Quiet::onTimeout), preview_timeout_seconds_);
  } else if (!has_preview && timer_connection_.connected()) {
    timer_connection_.disconnect();
  }

  if (is_urgent) {
    label_.get_style_context()->add_class("urgent");
  } else {
    label_.get_style_context()->remove_class("urgent");
  }

  if (current_count > 0 || has_preview) {
    label_.get_style_context()->add_class("has-notifications");
  } else {
    label_.get_style_context()->remove_class("has-notifications");
  }

  if (current_count == 0) {
    event_box_.hide();
    label_.set_text("");
  } else {
    event_box_.show();
    label_.set_text(display_text);
  }

  if (tooltipEnabled()) {
    label_.set_tooltip_text(fmt::format("{} unread notification(s)", current_count));
  }

  ALabel::update();
}

}  // namespace waybar::modules
