#include "modules/namaz.hpp"

#include <curl/curl.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace waybar::modules {

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  auto* s = static_cast<std::string*>(userp);
  s->append(static_cast<char*>(contents), size * nmemb);
  return size * nmemb;
}

static std::string HttpGet(const std::string& url) {
  CURL* curl = curl_easy_init();
  std::string response;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      spdlog::warn("Namaz HTTP GET failed for {}: {}", url, curl_easy_strerror(res));
      response.clear();
    }
    curl_easy_cleanup(curl);
  }
  return response;
}

static std::string CleanTimeStr(const std::string& raw) {
  std::string s = raw;
  auto t_pos = s.find('T');
  if (t_pos != std::string::npos && t_pos + 1 < s.size()) {
    s = s.substr(t_pos + 1);
  }
  auto space_pos = s.find(' ');
  if (space_pos != std::string::npos) {
    s = s.substr(0, space_pos);
  }
  if (s.size() >= 5) {
    return s.substr(0, 5); // HH:MM
  }
  return s;
}

Namaz::Namaz(const std::string& id, const Json::Value& config)
    : ALabel(config, "namaz", id, "{text}", 60, false, true) {
  const char* home = getenv("HOME");
  if (home) {
    cache_path_ = std::string(home) + "/.dotfiles/tmp/namaz_cache.json";
  } else {
    cache_path_ = "/tmp/namaz_cache.json";
  }

  loadFromCache();

  thread_ = [this] {
    // Check if we need to fetch new data
    {
      std::lock_guard<std::mutex> lock(mutex_);
      time_t now = time(nullptr);
      struct tm tm_now;
      localtime_r(&now, &tm_now);
      char today_buf[32];
      strftime(today_buf, sizeof(today_buf), "%d-%m-%Y", &tm_now);
      if (all_month_timings_.find(today_buf) == all_month_timings_.end()) {
        std::thread([this]() { fetchData(false); }).detach();
      }
    }
    dp.emit();
    thread_.sleep_for(interval_);
  };
}

Namaz::~Namaz() {
  // thread_ cleans up automatically
}

bool Namaz::loadFromCache() {
  std::ifstream f(cache_path_);
  if (!f.is_open()) return false;

  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errs;
  if (!Json::parseFromStream(builder, f, &root, &errs)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (root.isMember("location")) {
    city_ = root["location"].get("city", "").asString();
    country_ = root["location"].get("country", "").asString();
  }

  all_month_timings_.clear();
  todays_timings_.clear();

  time_t now = time(nullptr);
  struct tm tm_now;
  localtime_r(&now, &tm_now);
  char today_buf[32];
  strftime(today_buf, sizeof(today_buf), "%d-%m-%Y", &tm_now);
  std::string today_str(today_buf);

  const auto& data = root["data"];
  if (data.isArray()) {
    for (const auto& item : data) {
      std::string d_str = item["date"]["gregorian"]["date"].asString();
      const auto& timings = item["timings"];
      std::map<std::string, std::string> t_map;
      for (const auto& key : timings.getMemberNames()) {
        t_map[key] = CleanTimeStr(timings[key].asString());
      }
      all_month_timings_[d_str] = t_map;
      if (d_str == today_str) {
        todays_timings_ = t_map;
      }
    }
  }

  return !todays_timings_.empty();
}

void Namaz::saveToCache(const std::string& data_json_str, const std::string& city, const std::string& country) {
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errs;
  std::stringstream ss(data_json_str);
  if (!Json::parseFromStream(builder, ss, &root, &errs)) return;

  Json::Value cache_root;
  cache_root["data"] = root["data"];
  cache_root["location"]["city"] = city;
  cache_root["location"]["country"] = country;

  std::filesystem::create_directories(std::filesystem::path(cache_path_).parent_path());
  std::ofstream out(cache_path_);
  if (out.is_open()) {
    Json::StreamWriterBuilder wbuilder;
    out << Json::writeString(wbuilder, cache_root);
  }
}

void Namaz::fetchData(bool force) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fetching_) return;
    fetching_ = true;
  }

  // 1. IP Geolocation
  std::string ip_json = HttpGet("https://ipapi.co/json/");
  std::string city = "Jersey City";
  std::string country = "United States";

  if (!ip_json.empty()) {
    Json::Value ip_root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::stringstream ss(ip_json);
    if (Json::parseFromStream(builder, ss, &ip_root, &errs)) {
      if (ip_root.isMember("city")) city = ip_root["city"].asString();
      if (ip_root.isMember("country_name")) country = ip_root["country_name"].asString();
    }
  }

  // 2. Aladhan API
  CURL* curl = curl_easy_init();
  char* escaped_city = curl ? curl_easy_escape(curl, city.c_str(), city.length()) : nullptr;
  char* escaped_country = curl ? curl_easy_escape(curl, country.c_str(), country.length()) : nullptr;

  std::string url = fmt::format(
      "https://api.aladhan.com/v1/calendarByCity?city={}&country={}&method=15&iso8601=true",
      escaped_city ? escaped_city : city,
      escaped_country ? escaped_country : country);

  if (escaped_city) curl_free(escaped_city);
  if (escaped_country) curl_free(escaped_country);
  if (curl) curl_easy_cleanup(curl);

  std::string aladhan_json = HttpGet(url);
  if (!aladhan_json.empty()) {
    saveToCache(aladhan_json, city, country);
    loadFromCache();
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    fetching_ = false;
  }
  dp.emit();
}

std::string Namaz::calculateRemaining() {
  time_t now = time(nullptr);
  struct tm tm_now;
  localtime_r(&now, &tm_now);
  int cur_minutes = tm_now.tm_hour * 60 + tm_now.tm_min;

  static const std::vector<std::string> prayers = {"Fajr", "Sunrise", "Dhuhr", "Asr", "Maghrib", "Isha"};

  int next_prayer_minutes = -1;
  for (const auto& p : prayers) {
    auto it = todays_timings_.find(p);
    if (it != todays_timings_.end() && it->second.size() >= 5) {
      try {
        int h = std::stoi(it->second.substr(0, 2));
        int m = std::stoi(it->second.substr(3, 2));
        int p_minutes = h * 60 + m;
        if (p_minutes > cur_minutes) {
          next_prayer_minutes = p_minutes;
          break;
        }
      } catch (...) {}
    }
  }

  if (next_prayer_minutes != -1) {
    int diff = next_prayer_minutes - cur_minutes;
    int saat = diff / 60;
    int dk = diff % 60;
    return fmt::format("{}sa{}dk", saat, dk);
  }

  // Next prayer is tomorrow's Fajr
  time_t tomorrow = now + 86400;
  struct tm tm_tom;
  localtime_r(&tomorrow, &tm_tom);
  char tom_buf[32];
  strftime(tom_buf, sizeof(tom_buf), "%d-%m-%Y", &tm_tom);
  std::string tom_str(tom_buf);

  auto it_tom = all_month_timings_.find(tom_str);
  if (it_tom != all_month_timings_.end()) {
    auto it_fajr = it_tom->second.find("Fajr");
    if (it_fajr != it_tom->second.end() && it_fajr->second.size() >= 5) {
      try {
        int h = std::stoi(it_fajr->second.substr(0, 2));
        int m = std::stoi(it_fajr->second.substr(3, 2));
        int tom_fajr = h * 60 + m;
        int diff = (1440 - cur_minutes) + tom_fajr;
        int saat = diff / 60;
        int dk = diff % 60;
        return fmt::format("{}sa{}dk", saat, dk);
      } catch (...) {}
    }
  }

  return "Fetching...";
}

std::string Namaz::getFullDayString() {
  static const std::vector<std::string> prayers = {"Fajr", "Sunrise", "Dhuhr", "Asr", "Maghrib", "Isha"};
  std::string schedule;
  for (size_t i = 0; i < prayers.size(); ++i) {
    auto it = todays_timings_.find(prayers[i]);
    if (it != todays_timings_.end()) {
      schedule += it->second;
    } else {
      schedule += "--:--";
    }
    if (i + 1 < prayers.size()) {
      schedule += " | ";
    }
  }
  if (!city_.empty()) {
    return fmt::format("{}: [{}]", city_, schedule);
  }
  return fmt::format("[{}]", schedule);
}

bool Namaz::handleToggle(GdkEventButton* const& e) {
  if (e->button == 1) { // Left click: toggle full day schedule
    {
      std::lock_guard<std::mutex> lock(mutex_);
      show_full_day_ = !show_full_day_;
    }
    dp.emit();
    return true;
  } else if (e->button == 3) { // Right click: force background refresh
    std::thread([this]() { fetchData(true); }).detach();
    return true;
  }
  return ALabel::handleToggle(e);
}

auto Namaz::update() -> void {
  std::string text;
  std::string full_day;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (todays_timings_.empty()) {
      text = "Namaz: Fetching...";
    } else if (show_full_day_) {
      text = getFullDayString();
    } else {
      text = calculateRemaining();
    }
    full_day = getFullDayString();
  }

  label_.set_markup(text);

  if (tooltipEnabled()) {
    label_.set_tooltip_text(full_day);
  }

  ALabel::update();
}

}  // namespace waybar::modules
