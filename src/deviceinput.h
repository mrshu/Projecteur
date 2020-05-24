// This file is part of Projecteur - https://github.com/jahnf/projecteur - See LICENSE.md and README.md
#pragma once

#include <memory>

#include <QObject>

class VirtualDevice;

// -------------------------------------------------------------------------------------------------
/// This is basically the input_event struct from linux/input.h without the time member
struct DeviceInputEvent
{
  DeviceInputEvent() = default;
  DeviceInputEvent(uint16_t type, uint16_t code, uint32_t value) : type(type), code(code), value(value) {}
  DeviceInputEvent(const struct input_event& ie);
  DeviceInputEvent(const DeviceInputEvent&) = default;
  DeviceInputEvent(DeviceInputEvent&&) = default;

  DeviceInputEvent& operator=(const DeviceInputEvent&) = default;
  DeviceInputEvent& operator=(DeviceInputEvent&&) = default;

  uint16_t type;
  uint16_t code;
  uint32_t value;

  bool operator==(const DeviceInputEvent& o) const;
  bool operator==(const struct input_event& o) const;
  bool operator<(const DeviceInputEvent& o) const;
  bool operator<(const struct input_event& o) const;
};

// -------------------------------------------------------------------------------------------------
/// KeyEvent is a sequence of DeviceInputEvent.
using KeyEvent = std::vector<DeviceInputEvent>;

/// KeyEventSequence is a sequence of KeyEvents.
using KeyEventSequence = std::vector<KeyEvent>;

// -------------------------------------------------------------------------------------------------
class InputMapper : public QObject
{
  Q_OBJECT

public:
  InputMapper(std::shared_ptr<VirtualDevice> virtualDevice, QObject* parent = nullptr);
  ~InputMapper();

  void resetState(); // Reset any stored sequence state.

  // input_events = complete sequence including SYN event
  void addEvents(const struct input_event input_events[], size_t num);

  bool recordingMode() const;
  void setRecordingMode(bool recording);

  std::shared_ptr<VirtualDevice> virtualDevice() const;

signals:
  void recordingModeChanged(bool recording);
  void keyEventRecorded(const KeyEvent&);
  void recordingStarted(); // right befor first key event recorded
  void recordingFinished(); // after key sequence interval timer timout or max sequence length reached

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};