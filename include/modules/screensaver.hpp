#pragma once

#include "ALabel.hpp"
#include "util/sleeper_thread.hpp"

namespace waybar::modules {

class Screensaver : public ALabel {
 public:
  Screensaver(const std::string&, const Json::Value&);
  virtual ~Screensaver() = default;
  auto update() -> void override;

 private:
  bool handleToggle(GdkEventButton* const& e) override;
  bool isServiceActive();
  void toggleService();

  util::SleeperThread thread_;
  bool is_active_{false};
};

}  // namespace waybar::modules
