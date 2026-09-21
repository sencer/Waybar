#include "modules/screensaver.hpp"

#include <cstdlib>
#include <spdlog/spdlog.h>

namespace waybar::modules {

Screensaver::Screensaver(const std::string& id, const Json::Value& config)
    : ALabel(config, "screensaver", id, "{icon}", 2, false, true) {
  is_active_ = isServiceActive();
  thread_ = [this] {
    bool active = isServiceActive();
    if (active != is_active_) {
      is_active_ = active;
      dp.emit();
    }
    thread_.sleep_for(interval_);
  };
  dp.emit();
}

bool Screensaver::isServiceActive() {
  int ret = system("systemctl --user is-active --quiet swayidle.service");
  return (ret == 0);
}

void Screensaver::toggleService() {
  if (is_active_) {
    int ret = system("systemctl --user stop --quiet swayidle.service >/dev/null 2>&1");
    (void)ret;
    is_active_ = false;
  } else {
    int ret = system("systemctl --user start --quiet swayidle.service >/dev/null 2>&1");
    (void)ret;
    is_active_ = true;
  }
}

bool Screensaver::handleToggle(GdkEventButton* const& e) {
  if (e->button == 1) {
    toggleService();
    dp.emit();
    return true;
  }
  return ALabel::handleToggle(e);
}

auto Screensaver::update() -> void {
  is_active_ = isServiceActive();

  if (is_active_) {
    label_.get_style_context()->remove_class("active");
    label_.get_style_context()->add_class("sleeping");
    label_.set_markup("💤");
    if (tooltipEnabled()) {
      label_.set_tooltip_text("Screensaver Active (Display will sleep)");
    }
  } else {
    label_.get_style_context()->remove_class("sleeping");
    label_.get_style_context()->add_class("active");
    label_.set_markup("👁");
    if (tooltipEnabled()) {
      label_.set_tooltip_text("Screensaver Inhibited (Display stays on)");
    }
  }

  label_.show();
  event_box_.show();
  ALabel::update();
}

}  // namespace waybar::modules
