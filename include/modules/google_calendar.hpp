#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>
#include "ALabel.hpp"
#include "util/sleeper_thread.hpp"

namespace waybar::modules {

struct CalendarEvent {
  std::string summary;
  std::string location;
  std::string start_time;
  std::string end_time;
  int minutes_until_start{0};
  std::string html_link;
  std::string color;
};

class GoogleCalendar : public ALabel {
 public:
  GoogleCalendar(const std::string&, const Json::Value&);
  virtual ~GoogleCalendar();
  auto update() -> void override;

 private:
  bool handleToggle(GdkEventButton* const& e) override;
  void fetchEvents();
  bool refreshAccessToken();
  std::string getAccessToken();

  util::SleeperThread thread_;
  std::mutex mutex_;

  bool show_details_{false};
  bool fetching_{false};
  std::string token_path_;
  std::string client_secret_path_;
  std::string access_token_;
  std::string client_id_;
  std::string client_secret_;
  std::string refresh_token_;
  time_t token_expiry_{0};

  std::vector<CalendarEvent> events_;
};

}  // namespace waybar::modules
