#include "modules/battery.hpp"

waybar::modules::Battery::Battery(const std::string& id, const Json::Value& config)
    : ALabel(config, "{capacity}%", 60) {
  label_.set_name("battery");
  if (!id.empty()) {
    label_.get_style_context()->add_class(id);
  }
  getBatteries();
  fd_ = inotify_init1(IN_CLOEXEC);
  if (fd_ == -1) {
    throw std::runtime_error("Unable to listen batteries.");
  }
  for (auto const& bat : batteries_) {
    auto wd = inotify_add_watch(fd_, (bat / "uevent").c_str(), IN_ACCESS);
    if (wd != -1) {
      wds_.push_back(wd);
    }
  }
  worker();
}

waybar::modules::Battery::~Battery() {
  for (auto wd : wds_) {
    inotify_rm_watch(fd_, wd);
  }
  close(fd_);
}

void waybar::modules::Battery::worker() {
  thread_timer_ = [this] {
    dp.emit();
    thread_timer_.sleep_for(interval_);
  };
  thread_ = [this] {
    struct inotify_event event = {0};
    int                  nbytes = read(fd_, &event, sizeof(event));
    if (nbytes != sizeof(event) || event.mask & IN_IGNORED) {
      thread_.stop();
      return;
    }
    // TODO: don't stop timer for now since there is some bugs :?
    // thread_timer_.stop();
    dp.emit();
  };
}

void waybar::modules::Battery::getBatteries() {
  try {
    for (auto& node : fs::directory_iterator(data_dir_)) {
      if (!fs::is_directory(node)) {
        continue;
      }
      auto dir_name = node.path().filename();
      auto bat_defined = config_["bat"].isString();
      if (((bat_defined && dir_name == config_["bat"].asString()) || !bat_defined) &&
          fs::exists(node.path() / "capacity") && fs::exists(node.path() / "uevent") &&
          fs::exists(node.path() / "status")) {
        batteries_.push_back(node.path());
      }
      auto adap_defined = config_["adapter"].isString();
      if (((adap_defined && dir_name == config_["adapter"].asString()) || !adap_defined) &&
          fs::exists(node.path() / "online")) {
        adapter_ = node.path();
      }
    }
  } catch (fs::filesystem_error& e) {
    throw std::runtime_error(e.what());
  }
  if (batteries_.empty()) {
    if (config_["bat"].isString()) {
      throw std::runtime_error("No battery named " + config_["bat"].asString());
    }
    throw std::runtime_error("No batteries.");
  }
}

const std::tuple<uint8_t, uint32_t, std::string> waybar::modules::Battery::getInfos() const {
  try {
    uint16_t    total = 0;
    uint32_t    total_current = 0;
    std::string status = "Unknown";
    for (auto const& bat : batteries_) {
      uint16_t    capacity;
      uint32_t    current_now;
      std::string _status;
      std::ifstream(bat / "capacity") >> capacity;
      std::ifstream(bat / "status") >> _status;
      std::ifstream(bat / "current_now") >> current_now;
      if (_status != "Unknown") {
        status = _status;
      }
      total += capacity;
      total_current += current_now;
    }
    uint16_t capacity = total / batteries_.size();
    if (status == "Charging" && total_current != 0) {
      status = "Plugged";
    }
    return {capacity, total_current, status};
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return {0, 0, "Unknown"};
  }
}

const std::string waybar::modules::Battery::getAdapterStatus(uint8_t  capacity,
                                                             uint32_t current_now) const {
  if (!adapter_.empty()) {
    bool online;
    std::ifstream(adapter_ / "online") >> online;
    if (capacity == 100) {
      return "Full";
    }
    if (online) {
      return current_now == 0 ? "Charging" : "Plugged";
    }
    return "Discharging";
  }
  return "Unknown";
}

auto waybar::modules::Battery::update() -> void {
  auto [capacity, current_now, status] = getInfos();
  if (status == "Unknown") {
    status = getAdapterStatus(capacity, current_now);
  }
  if (tooltipEnabled()) {
    label_.set_tooltip_text(status);
  }
  std::transform(status.begin(), status.end(), status.begin(), ::tolower);
  auto format = format_;
  auto state = getState(capacity, true);
  if (!old_status_.empty()) {
    label_.get_style_context()->remove_class(old_status_);
  }
  label_.get_style_context()->add_class(status);
  old_status_ = status;
  if (!state.empty() && config_["format-" + status + "-" + state].isString()) {
    format = config_["format-" + status + "-" + state].asString();
  } else if (config_["format-" + status].isString()) {
    format = config_["format-" + status].asString();
  } else if (!state.empty() && config_["format-" + state].isString()) {
    format = config_["format-" + state].asString();
  }
  if (format.empty()) {
    event_box_.hide();
  } else {
    event_box_.show();
    label_.set_markup(
        fmt::format(format, fmt::arg("capacity", capacity), fmt::arg("icon", getIcon(capacity))));
  }
}
