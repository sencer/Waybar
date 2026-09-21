#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include "ALabel.hpp"
#include "util/sleeper_thread.hpp"

namespace waybar::modules {

class Namaz : public ALabel {
 public:
  Namaz(const std::string&, const Json::Value&);
  virtual ~Namaz();
  auto update() -> void override;

 private:
  bool handleToggle(GdkEventButton* const& e) override;
  void fetchData(bool force = false);
  bool loadFromCache();
  void saveToCache(const std::string& data, const std::string& city, const std::string& country);
  std::string calculateRemaining();
  std::string getFullDayString();

  util::SleeperThread thread_;
  std::mutex mutex_;

  bool show_full_day_{false};
  bool fetching_{false};
  std::string city_{""};
  std::string country_{""};
  std::map<std::string, std::string> todays_timings_;
  std::map<std::string, std::map<std::string, std::string>> all_month_timings_;
  std::string cache_path_;
};

}  // namespace waybar::modules
