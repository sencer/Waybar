#include "modules/google_calendar.hpp"

#include <curl/curl.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include "util/command.hpp"

namespace waybar::modules {

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  auto* s = static_cast<std::string*>(userp);
  s->append(static_cast<char*>(contents), size * nmemb);
  return size * nmemb;
}

static std::string HttpPost(const std::string& url, const std::string& post_data) {
  CURL* curl = curl_easy_init();
  std::string response;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      spdlog::warn("Google Calendar POST failed: {}", curl_easy_strerror(res));
      response.clear();
    }
    curl_easy_cleanup(curl);
  }
  return response;
}

static std::string HttpGetWithAuth(const std::string& url, const std::string& token) {
  CURL* curl = curl_easy_init();
  std::string response;
  if (curl) {
    struct curl_slist* headers = nullptr;
    std::string auth_header = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, auth_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      spdlog::warn("Google Calendar GET failed: {}", curl_easy_strerror(res));
      response.clear();
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  }
  return response;
}

static std::string FormatIsoTime(const std::string& iso_str, int& out_hour, int& out_min) {
  // Expected format: YYYY-MM-DDTHH:MM:SS...
  auto t_pos = iso_str.find('T');
  if (t_pos != std::string::npos && t_pos + 6 <= iso_str.size()) {
    try {
      int h = std::stoi(iso_str.substr(t_pos + 1, 2));
      int m = std::stoi(iso_str.substr(t_pos + 4, 2));
      out_hour = h;
      out_min = m;

      std::string ampm = (h >= 12) ? "PM" : "AM";
      int h12 = h % 12;
      if (h12 == 0) h12 = 12;
      return fmt::format("{}:{:02d} {}", h12, m, ampm);
    } catch (...) {}
  }
  return "";
}

GoogleCalendar::GoogleCalendar(const std::string& id, const Json::Value& config)
    : ALabel(config, "google_calendar", id, "{text}", 120, false, true) {
  const char* home = getenv("HOME");
  std::string base_path = home ? std::string(home) + "/.dotfiles/tmp/" : "/tmp/";

  token_path_ = config.get("auth_token", base_path + "google_calendar.auth.token").asString();
  client_secret_path_ = config.get("client_secret", base_path + "google_calendar.credentials.json").asString();

  // Load credentials from token file
  std::ifstream f(token_path_);
  if (f.is_open()) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    if (Json::parseFromStream(builder, f, &root, &errs)) {
      access_token_ = root.get("access_token", "").asString();
      client_id_ = root.get("client_id", "").asString();
      client_secret_ = root.get("client_secret", "").asString();
      refresh_token_ = root.get("refresh_token", "").asString();
    }
  }

  thread_ = [this] {
    fetchEvents();
    dp.emit();
    thread_.sleep_for(interval_);
  };
}

GoogleCalendar::~GoogleCalendar() {
  // SleeperThread handles join
}

bool GoogleCalendar::refreshAccessToken() {
  if (refresh_token_.empty() || client_id_.empty() || client_secret_.empty()) {
    return false;
  }

  std::string post_data = fmt::format(
      "client_id={}&client_secret={}&refresh_token={}&grant_type=refresh_token",
      client_id_, client_secret_, refresh_token_);

  std::string resp = HttpPost("https://oauth2.googleapis.com/token", post_data);
  if (resp.empty()) return false;

  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errs;
  std::stringstream ss(resp);
  if (!Json::parseFromStream(builder, ss, &root, &errs)) return false;

  if (root.isMember("access_token")) {
    access_token_ = root["access_token"].asString();
    int expires_in = root.get("expires_in", 3600).asInt();
    token_expiry_ = time(nullptr) + expires_in - 60;

    // Update token file
    std::ifstream in(token_path_);
    Json::Value token_file_root;
    if (in.is_open()) {
      Json::parseFromStream(builder, in, &token_file_root, &errs);
      in.close();
    }
    token_file_root["access_token"] = access_token_;
    std::ofstream out(token_path_);
    if (out.is_open()) {
      Json::StreamWriterBuilder wbuilder;
      out << Json::writeString(wbuilder, token_file_root);
    }
    return true;
  }

  return false;
}

std::string GoogleCalendar::getAccessToken() {
  time_t now = time(nullptr);
  if (access_token_.empty() || now >= token_expiry_) {
    refreshAccessToken();
  }
  return access_token_;
}

void GoogleCalendar::fetchEvents() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fetching_) return;
    fetching_ = true;
  }

  std::string token = getAccessToken();
  if (token.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    fetching_ = false;
    return;
  }

  time_t now = time(nullptr);
  char min_buf[64], max_buf[64];
  struct tm tm_utc;
  gmtime_r(&now, &tm_utc);
  strftime(min_buf, sizeof(min_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

  time_t now_plus_12h = now + (12 * 3600);
  gmtime_r(&now_plus_12h, &tm_utc);
  strftime(max_buf, sizeof(max_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

  std::string url = fmt::format(
      "https://www.googleapis.com/calendar/v3/calendars/primary/events?timeMin={}&timeMax={}&singleEvents=true&orderBy=startTime",
      min_buf, max_buf);

  std::string resp = HttpGetWithAuth(url, token);
  std::vector<CalendarEvent> new_events;

  if (!resp.empty()) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::stringstream ss(resp);
    if (Json::parseFromStream(builder, ss, &root, &errs)) {
      const auto& items = root["items"];
      if (items.isArray()) {
        static const std::vector<std::string> event_colors = {
            "#d0e6ff", "#bbdaff", "#99c7ff", "#86bcff", "#62a9ff", "#8c8cff", "#7979ff"};

        size_t color_idx = 0;
        for (const auto& item : items) {
          std::string summary = item.get("summary", "").asString();
          if (summary.find("CANCELLED") != std::string::npos || summary.find("[TBC]") != std::string::npos) {
            continue;
          }
          if (item["start"].isMember("date")) {
            // All-day event; skip per py3status config
            continue;
          }

          std::string start_iso = item["start"].get("dateTime", "").asString();
          std::string end_iso = item["end"].get("dateTime", "").asString();
          if (start_iso.empty()) continue;

          int start_h = 0, start_m = 0, end_h = 0, end_m = 0;
          std::string start_time = FormatIsoTime(start_iso, start_h, start_m);
          std::string end_time = FormatIsoTime(end_iso, end_h, end_m);

          struct tm tm_local;
          localtime_r(&now, &tm_local);
          int cur_minutes = tm_local.tm_hour * 60 + tm_local.tm_min;
          int event_minutes = start_h * 60 + start_m;
          int minutes_until = event_minutes - cur_minutes;

          std::string location = item.get("location", "").asString();
          // Clean up location prefix if present
          if (location.rfind("US-NYC-9TH-", 0) == 0) {
            location = location.substr(11);
          } else if (location.rfind("NYC-", 0) == 0) {
            location = location.substr(4);
          }

          CalendarEvent ev;
          ev.summary = summary;
          ev.location = location;
          ev.start_time = start_time;
          ev.end_time = end_time;
          ev.minutes_until_start = minutes_until;
          ev.html_link = item.get("htmlLink", "").asString();
          ev.color = event_colors[color_idx % event_colors.size()];
          color_idx++;

          new_events.push_back(ev);
        }
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    events_ = new_events;
    fetching_ = false;
  }
}

bool GoogleCalendar::handleToggle(GdkEventButton* const& e) {
  if (e->type == GDK_2BUTTON_PRESS || e->button == 3) { // Double click or Right click
    util::command::forkExec("google-chrome --app=http://calendar.google.com/");
    return true;
  } else if (e->button == 1) { // Single Left click: toggle detail expansion
    {
      std::lock_guard<std::mutex> lock(mutex_);
      show_details_ = !show_details_;
    }
    dp.emit();
    return true;
  } else if (e->button == 2) { // Middle click: force refresh
    std::thread([this]() {
      fetchEvents();
      dp.emit();
    }).detach();
    return true;
  }
  return ALabel::handleToggle(e);
}

auto GoogleCalendar::update() -> void {
  std::string text;
  std::string tooltip;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) {
      text = "";
    } else {
      const auto& ev = events_.front();
      std::string colored_summary = fmt::format("<span color='{}'>{}</span>", ev.color, ev.summary);

      std::string extra;
      if (!ev.location.empty()) {
        extra += fmt::format(" ({})", ev.location);
      }
      if (ev.minutes_until_start > 0 && ev.minutes_until_start <= 180) {
        extra += fmt::format(" <span color='#ff7f7f'>({}m)</span>", ev.minutes_until_start);
      }

      if (!show_details_) {
        text = fmt::format("{} ({} - {})", colored_summary, ev.start_time, ev.end_time);
      } else {
        text = fmt::format("{}{}", colored_summary, extra);
      }

      // Build tooltip with all upcoming events
      for (const auto& item : events_) {
        tooltip += fmt::format("• {} ({} - {})\n", item.summary, item.start_time, item.end_time);
        if (!item.location.empty()) {
          tooltip += fmt::format("  Location: {}\n", item.location);
        }
      }
      if (!tooltip.empty() && tooltip.back() == '\n') {
        tooltip.pop_back();
      }
    }
  }

  if (text.empty()) {
    event_box_.hide();
  } else {
    event_box_.show();
    label_.set_markup(text);
    if (tooltipEnabled() && !tooltip.empty()) {
      label_.set_tooltip_text(tooltip);
    }
  }

  ALabel::update();
}

}  // namespace waybar::modules
