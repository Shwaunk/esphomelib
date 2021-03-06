//
// Created by Otto Winter on 25.11.17.
//

#include <algorithm>
#include "esphomelib/component.h"

#include "esphomelib/esphal.h"
#include "esphomelib/log.h"
#include "esphomelib/helpers.h"

ESPHOMELIB_NAMESPACE_BEGIN

static const char *TAG = "component";

namespace setup_priority {

const float PRE_HARDWARE = 110.0f;
const float HARDWARE = 100.0f;
const float POST_HARDWARE = 90.0f;
const float WIFI = 10.0f;
const float MQTT_CLIENT = 7.5f;
const float HARDWARE_LATE = 0.0f;
const float MQTT_COMPONENT = -5.0f;
const float LATE = -10.0f;

} // namespace setup_priority

const uint32_t COMPONENT_STATE_MASK = 0xFF;
const uint32_t COMPONENT_STATE_CONSTRUCTION = 0x00;
const uint32_t COMPONENT_STATE_SETUP = 0x01;
const uint32_t COMPONENT_STATE_LOOP = 0x02;
const uint32_t COMPONENT_STATE_FAILED = 0x03;
const uint32_t STATUS_LED_MASK = 0xFF00;
const uint32_t STATUS_LED_OK = 0x0000;
const uint32_t STATUS_LED_WARNING = 0x0100;
const uint32_t STATUS_LED_ERROR = 0x0200;

uint32_t global_state = 0;

float Component::get_loop_priority() const {
  return 0.0f;
}

float Component::get_setup_priority() const {
  return 0.0f;
}

void Component::setup() {

}

void Component::loop() {

}

void Component::set_interval(const std::string &name, uint32_t interval, std::function<void()> &&f) {
  // only put offset in lower half
  uint32_t offset = 0;
  if (interval != 0)
    offset = (random_uint32() % interval) / 2;
  ESP_LOGV(TAG, "set_interval(name='%s', interval=%u, offset=%u)", name.c_str(), interval, offset);

  this->cancel_interval(name);
  struct TimeFunction function = {
      .name = name,
      .type = TimeFunction::INTERVAL,
      .interval = interval,
      .last_execution = millis() - interval - offset,
      .f = std::move(f),
      .remove = false,
  };
  this->time_functions_.push_back(function);
}

bool Component::cancel_interval(const std::string &name) {
  return this->cancel_time_function(name, TimeFunction::INTERVAL);
}

void Component::set_timeout(const std::string &name, uint32_t timeout, std::function<void()> &&f) {
  ESP_LOGV(TAG, "set_timeout(name='%s', timeout=%u)", name.c_str(), timeout);

  this->cancel_timeout(name);
  struct TimeFunction function = {
      .name = name,
      .type = TimeFunction::TIMEOUT,
      .interval = timeout,
      .last_execution = millis(),
      .f = std::move(f),
      .remove = false,
  };
  this->time_functions_.push_back(function);
}

bool Component::cancel_timeout(const std::string &name) {
  return this->cancel_time_function(name, TimeFunction::TIMEOUT);
}

void Component::loop_() {
  this->loop_internal();
  this->loop();
}

bool Component::cancel_time_function(const std::string &name, TimeFunction::Type type) {
  if (name.empty())
    return false;
  for (auto iter = this->time_functions_.begin(); iter != this->time_functions_.end(); iter++) {
    if (!iter->remove && iter->name == name && iter->type == type) {
      ESP_LOGV(TAG, "Removing old time function %s.", iter->name.c_str());
      iter->remove = true;
      return true;
    }
  }
  return false;
}
void Component::setup_() {
  this->setup_internal();
  this->setup();
}
uint32_t Component::get_component_state() const {
  return this->component_state_;
}
void Component::loop_internal() {
  assert((this->component_state_ & COMPONENT_STATE_MASK) == COMPONENT_STATE_SETUP ||
         (this->component_state_ & COMPONENT_STATE_MASK) == COMPONENT_STATE_LOOP);
  this->component_state_ &= ~COMPONENT_STATE_MASK;
  this->component_state_ |= COMPONENT_STATE_LOOP;

  for (unsigned int i = 0; i < this->time_functions_.size(); i++) { // NOLINT
    const uint32_t now = millis();
    TimeFunction *tf = &this->time_functions_[i];
    if (tf->should_run(now)) {
#ifdef ESPHOMELIB_LOG_HAS_VERY_VERBOSE
      const char *type = tf->type == TimeFunction::INTERVAL ? "interval" : (tf->type == TimeFunction::TIMEOUT ? "timeout" : "defer");
      ESP_LOGVV(TAG, "Running %s '%s':%u with interval=%u last_execution=%u (now=%u)",
                type, tf->name.c_str(), i, tf->interval, tf->last_execution, now);
#endif

      tf->f();
      // The vector might have reallocated due to new items
      tf = &this->time_functions_[i];

      if (tf->type == TimeFunction::INTERVAL && tf->interval != 0) {
        const uint32_t amount = (now - tf->last_execution) / tf->interval;
        tf->last_execution += (amount * tf->interval);
      } else if (tf->type == TimeFunction::DEFER || tf->type == TimeFunction::TIMEOUT) {
        tf->remove = true;
      }
    }
  }

  this->time_functions_.erase(
      std::remove_if(this->time_functions_.begin(), this->time_functions_.end(),
                     [](const TimeFunction &tf) -> bool {
                       return tf.remove;
                     }),
      this->time_functions_.end()
  );
}
void Component::setup_internal() {
  assert((this->component_state_ & COMPONENT_STATE_MASK) == COMPONENT_STATE_CONSTRUCTION);
  this->component_state_ &= ~COMPONENT_STATE_MASK;
  this->component_state_ |= COMPONENT_STATE_SETUP;
}
void Component::mark_failed() {
  ESP_LOGE(TAG, "Component was marked as failed.");
  this->component_state_ &= ~COMPONENT_STATE_MASK;
  this->component_state_ |= COMPONENT_STATE_FAILED;
  this->status_set_error();
}
void Component::defer(std::function<void()> &&f) {
  this->defer("", std::move(f));
}
bool Component::cancel_defer(const std::string &name) {
  return this->cancel_time_function(name, TimeFunction::DEFER);
}
void Component::defer(const std::string &name, std::function<void()> &&f) {
  this->cancel_defer(name);
  struct TimeFunction function = {
      .name = name,
      .type = TimeFunction::DEFER,
      .interval = 0,
      .last_execution = 0,
      .f = std::move(f),
      .remove = false,
  };
  this->time_functions_.push_back(function);
}
void Component::set_timeout(uint32_t timeout, std::function<void()> &&f) {
  this->set_timeout("", timeout, std::move(f));
}
void Component::set_interval(uint32_t interval, std::function<void()> &&f) {
  this->set_interval("", interval, std::move(f));
}
bool Component::is_failed() {
  return (this->component_state_ & COMPONENT_STATE_MASK) == COMPONENT_STATE_FAILED;
}
bool Component::can_proceed() {
  return true;
}
bool Component::status_has_warning() {
  return this->component_state_ &  STATUS_LED_WARNING;
}
bool Component::status_has_error() {
  return this->component_state_ & STATUS_LED_ERROR;
}
void Component::status_set_warning() {
  this->component_state_ |= STATUS_LED_WARNING;
}
void Component::status_set_error() {
  this->component_state_ |= STATUS_LED_ERROR;
}
void Component::status_clear_warning() {
  this->component_state_ &= ~STATUS_LED_WARNING;
}
void Component::status_clear_error() {
  this->component_state_ &= ~STATUS_LED_ERROR;
}
void Component::status_momentary_warning(const std::string &name, uint32_t length) {
  this->status_set_warning();
  this->set_timeout(name, length, [this]() {
    this->status_clear_warning();
  });
}
void Component::status_momentary_error(const std::string &name, uint32_t length) {
  this->status_set_error();
  this->set_timeout(name, length, [this]() {
    this->status_clear_error();
  });
}

PollingComponent::PollingComponent(uint32_t update_interval)
    : Component(), update_interval_(update_interval) {}

void PollingComponent::setup_() {
  // Call component internal setup.
  this->setup_internal();

  // Let the polling component subclass setup their HW.
  this->setup();

  // Register interval.
  ESP_LOGCONFIG(TAG, "    Update interval: %ums", this->get_update_interval());
  this->set_interval("update", this->get_update_interval(), [this]() { this->update(); });
}

uint32_t PollingComponent::get_update_interval() const {
  return this->update_interval_;
}
void PollingComponent::set_update_interval(uint32_t update_interval) {
  this->update_interval_ = update_interval;
}

const std::string &Nameable::get_name() const {
  return this->name_;
}
void Nameable::set_name(const std::string &name) {
  this->name_ = name;
}
Nameable::Nameable(const std::string &name)
    : name_(name) {}

std::string Nameable::get_name_id() {
  return sanitize_string_whitelist(to_lowercase_underscore(this->name_), HOSTNAME_CHARACTER_WHITELIST);
}
bool Nameable::is_internal() const {
  return this->internal_;
}
void Nameable::set_internal(bool internal) {
  this->internal_ = internal;
}

bool Component::TimeFunction::should_run(uint32_t now) const {
  if (this->remove)
    return false;
  if (this->type == DEFER)
    return true;
  return this->interval != 4294967295UL && now - this->last_execution > this->interval;
}

ESPHOMELIB_NAMESPACE_END
