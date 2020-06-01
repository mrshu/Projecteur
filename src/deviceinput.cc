// This file is part of Projecteur - https://github.com/jahnf/projecteur - See LICENSE.md and README.md
#include "deviceinput.h"

#include "logging.h"
#include "settings.h"
#include "virtualdevice.h"

#include <map>
#include <set>

#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QTimer>

#include <linux/input.h>

LOGGING_CATEGORY(input, "input")

namespace  {
  // -----------------------------------------------------------------------------------------------
}

// -------------------------------------------------------------------------------------------------
DeviceInputEvent::DeviceInputEvent(const struct input_event& ie)
  : type(ie.type), code(ie.code), value(ie.value) {}

bool DeviceInputEvent::operator==(const DeviceInputEvent& o) const {
  return std::tie(type,code,value) == std::tie(o.type,o.code,o.value);
}

bool DeviceInputEvent::operator==(const input_event& o) const {
  return std::tie(type,code,value) == std::tie(o.type,o.code,o.value);
}

bool DeviceInputEvent::operator<(const DeviceInputEvent& o) const {
  return std::tie(type,code,value) < std::tie(o.type,o.code,o.value);
}

bool DeviceInputEvent::operator<(const input_event& o) const {
  return std::tie(type,code,value) < std::tie(o.type,o.code,o.value);
}

// -------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug debug, const DeviceInputEvent &d)
{
  QDebugStateSaver saver(debug);
  debug.nospace() << '{' << d.type << ", " << d.code << ", " << d.value << '}';
  return debug;
}

// -------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug debug, const KeyEvent &ke)
{
  QDebugStateSaver saver(debug);
  debug.nospace() << "[";
  for (const auto& e : ke)
    debug.nospace() << e << ',';
  debug.nospace() << "]";
  return debug;
}

// -------------------------------------------------------------------------------------------------
QString& operator<<(QString& s, const KeyEventSequence& kes)
{
  QJsonArray seqArr;
  for (const auto& ke : kes)
  {
    QJsonArray keArr;
    for (const auto& die : ke)
    {
      QJsonArray dieArr;
      dieArr.append(die.type);
      dieArr.append(die.code);
      dieArr.append(double(die.value));
      keArr.append(dieArr);
    }
    seqArr.append(keArr);
  }

  return s.append(QJsonDocument(seqArr).toJson(QJsonDocument::JsonFormat::Compact));
}

// -------------------------------------------------------------------------------------------------
const QString& operator<<(QString&& s, const KeyEventSequence& kes)
{
  return s << kes;
}

// -------------------------------------------------------------------------------------------------
const QString& operator>>(const QString& s,  KeyEventSequence& kes)
{
  const auto doc = QJsonDocument::fromJson(s.toLocal8Bit());
  const auto seqArr = doc.array();

  kes.clear();
  for (const auto& keObj : seqArr)
  {
    KeyEvent ke;
    const auto keArr = keObj.toArray();
    for (const auto& dieObj : keArr)
    {
      const auto dieArr = dieObj.toArray();
      if (dieArr.size() != 3) continue;
      ke.emplace_back(DeviceInputEvent{
                        uint16_t(dieArr[0].toInt()),
                        uint16_t(dieArr[1].toInt()),
                        uint32_t(dieArr[2].toDouble())
                      });
    }
    if (!ke.empty()) kes.emplace_back(std::move(ke));
  }
  return s;
}

// -------------------------------------------------------------------------------------------------
namespace  {
  struct Next;
  // Map of Key event and the next possible key events.
  using SynKeyEventMap = std::map<const KeyEvent, std::unique_ptr<Next>>;
  using RefPair = SynKeyEventMap::value_type;
  // Set of references to the next possible key event.
  using RefSet = std::set<const RefPair*>;

  struct Next {
    NativeKeySequence mappedKeys; // TODO replace with generic action
    RefSet next_events;
  };

  // Helper function
  size_t maxSequenceLength(const InputMapConfig& config) {
    size_t maxLen = 0;
    for (const auto& item: config)
      if (item.first.size() > maxLen) maxLen = item.first.size();
    return maxLen;
  }

  // Internal data structure for keeping track of key events and checking if a configured
  // key event sequence was pressed. Needs to be completely reconstructed/reconfigured
  // if the configuration changes.
  struct DeviceKeyMap
  {
    DeviceKeyMap(const InputMapConfig& config = {}) { reconfigure(config); }

    enum Result : uint8_t {
      Miss, Valid, Hit, AmbigiouslyHit
    };

    Result feed(const struct input_event input_events[], size_t num);

    auto state() const { return m_pos; }
    void resetState();
    void reconfigure(const InputMapConfig& config = {});
    bool hasConfig() const { return m_keymaps.size(); }

  private:
    const RefPair* m_pos = nullptr;
    std::vector<SynKeyEventMap> m_keymaps;
  };
}

// -------------------------------------------------------------------------------------------------
DeviceKeyMap::Result DeviceKeyMap::feed(const struct input_event input_events[], size_t num)
{
  if (!hasConfig()) return Result::Miss;

  if (m_pos == nullptr)
  {
    const auto find_it = m_keymaps[0].find(KeyEvent(input_events, input_events + num));
    if (find_it == m_keymaps[0].cend()) return Result::Miss;
    m_pos = &(*find_it);
  }
  else
  {
    if (!m_pos->second) return Result::Miss;

    const auto ke = KeyEvent(KeyEvent(input_events, input_events + num));
    const auto& set = m_pos->second->next_events;
    const auto find_it = std::find_if(set.cbegin(), set.cend(), [&ke](RefPair const* next_ptr) {
      return ke == next_ptr->first;
    });

    if (find_it == set.cend()) return Result::Miss;

    m_pos = (*find_it);
  }

  // Last KeyEvent in possible sequence...
  if (!m_pos->second || m_pos->second->next_events.empty()) {
    return Result::Hit;
  }

  // KeyEvent in Sequence has action attached, but there are other possible sequences...
  if (m_pos->second->mappedKeys.count() != 0) {
    return Result::AmbigiouslyHit;
  }

  return Result::Valid;
}

// -------------------------------------------------------------------------------------------------
void DeviceKeyMap::resetState()
{
  m_pos = nullptr;
}

// -------------------------------------------------------------------------------------------------
void DeviceKeyMap::reconfigure(const InputMapConfig& config)
{
  m_keymaps.resize(maxSequenceLength(config));

  // -- clear maps + position
  m_pos = nullptr;
  for (auto& synKeyEventMap : m_keymaps) { synKeyEventMap.clear(); }

  // -- fill maps
  for (const auto& item: config)
  {
    if (item.second.sequence.count() == 0) continue;

    const auto& kes = item.first;
    for (size_t i = 0; i < kes.size(); ++i) {
      m_keymaps[i].emplace(kes[i], nullptr);
    }
  }

  // -- fill references
  for (const auto& item: config)
  {
    if (item.second.sequence.count() == 0) continue;

    const auto& kes = item.first;
    for (size_t i = 0; i < kes.size(); ++i)
    {
      const auto r = m_keymaps[i].equal_range(kes[i]);
      if (r.first == r.second) continue;
      auto& refobj = r.first->second;
      if (!refobj) {
        refobj = std::make_unique<Next>();
      }

      if (i == kes.size() - 1) // last keyevent in seq
      {
        // Set (placeholder/fake) action for now in this prototype.
        refobj->mappedKeys = item.second.sequence; // TODO generic action
      }
      else if (i+1 < m_keymaps.size()) // if not last keyevent in seq
      {
        const auto r = m_keymaps[i+1].equal_range(kes[i+1]);
        if (r.first == r.second) continue;
        refobj->next_events.emplace(&(*r.first));
      }
    }
  }
}

// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------
NativeKeySequence::NativeKeySequence() = default;

  // -------------------------------------------------------------------------------------------------
NativeKeySequence::NativeKeySequence(QKeySequence&& ks, KeyEventSequence&& kes)
  : m_keySequence(std::move(ks))
    , m_nativeSequence(std::move(kes))
{}

// -------------------------------------------------------------------------------------------------
bool NativeKeySequence::operator==(const NativeKeySequence &other) const
{
  return m_keySequence == other.m_keySequence && m_nativeSequence == other.m_nativeSequence;
}

// -------------------------------------------------------------------------------------------------
bool NativeKeySequence::operator!=(const NativeKeySequence &other) const
{
  return m_keySequence != other.m_keySequence || m_nativeSequence != other.m_nativeSequence;
}

// -------------------------------------------------------------------------------------------------
void NativeKeySequence::clear()
{
  m_keySequence = QKeySequence{};
  m_nativeSequence.clear();
}

void NativeKeySequence::swap(NativeKeySequence& other)
{
  m_keySequence.swap(other.m_keySequence);
  m_nativeSequence.swap(other.m_nativeSequence);
}

// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------
struct InputMapper::Impl
{
  Impl(InputMapper* parent, std::shared_ptr<VirtualDevice> vdev);

  void sequenceTimeout();
  void resetState();
  void record(const struct input_event input_events[], size_t num);
  void emitQKeySequence(const NativeKeySequence& ks);

  InputMapper* m_parent = nullptr;
  std::shared_ptr<VirtualDevice> m_vdev; // can be a nullptr if application is started without uinput
  QTimer* m_seqTimer = nullptr;
  DeviceKeyMap m_keymap;

  std::pair<DeviceKeyMap::Result, const RefPair*> m_lastState;
  std::vector<input_event> m_events;
  InputMapConfig m_config;
  bool m_recordingMode = false;
};

// -------------------------------------------------------------------------------------------------
InputMapper::Impl::Impl(InputMapper* parent, std::shared_ptr<VirtualDevice> vdev)
  : m_parent(parent)
  , m_vdev(std::move(vdev))
  , m_seqTimer(new QTimer(parent))
{
  m_seqTimer->setSingleShot(true);
  m_seqTimer->setInterval(250);
  connect(m_seqTimer, &QTimer::timeout, parent, [this](){ sequenceTimeout(); });
}

// -------------------------------------------------------------------------------------------------
void InputMapper::Impl::sequenceTimeout()
{
  if(m_recordingMode)
  {
    emit m_parent->recordingFinished(false);
    return;
  }

  if (m_lastState.first == DeviceKeyMap::Result::Valid) {
    // Last input event was part of a valid key sequence, but timeout hit
    // So we emit our stored event so far to the virtual device
    if (m_vdev && m_events.size())
    {
      m_vdev->emitEvents(m_events);
    }
    resetState();
  }
  else if (m_lastState.first == DeviceKeyMap::Result::AmbigiouslyHit) {
    // Last input could have triggered an action, but we needed to wait for the timeout, since
    // other sequences could have been possible.
    if (m_lastState.second->second)
    {
      logDebug(input) << "AbigiouslyHit: Emitting Key Sequence:"
                      << m_lastState.second->second->mappedKeys.keySequence();
      emitQKeySequence(m_lastState.second->second->mappedKeys);
      // TODO trigger actions(s) / inject mapped key(s)
    }
    else if (m_vdev && m_events.size())
    {
      m_vdev->emitEvents(m_events);
      m_events.resize(0);
    }
    resetState();
  }
}

// -------------------------------------------------------------------------------------------------
void InputMapper::Impl::resetState()
{
  m_keymap.resetState();
  m_events.resize(0);
}

// -------------------------------------------------------------------------------------------------
void InputMapper::Impl::emitQKeySequence(const NativeKeySequence& ks)
{
  if (!m_vdev) return;

  std::vector<input_event> events;
  events.reserve(5); // 3 modifier keys + 1 key + 1 syn event
  for (const auto& ke : ks.nativeSequence())
  {
    for (const auto& ie : ke)
      events.emplace_back(input_event{{}, ie.type, ie.code, ie.value});

    m_vdev->emitEvents(events);
    events.resize(0);
  }
}

// -------------------------------------------------------------------------------------------------
void InputMapper::Impl::record(const struct input_event input_events[], size_t num)
{
  const auto ev = KeyEvent(input_events, input_events + num);

  if (!m_seqTimer->isActive()) {
    emit m_parent->recordingStarted();
  }
  m_seqTimer->start();
  emit m_parent->keyEventRecorded(ev);
}

// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------
InputMapper::InputMapper(std::shared_ptr<VirtualDevice> virtualDevice, QObject* parent)
  : QObject(parent)
  , impl(std::make_unique<Impl>(this, std::move(virtualDevice)))
{
}

// -------------------------------------------------------------------------------------------------
InputMapper::~InputMapper()
{
}

// -------------------------------------------------------------------------------------------------
std::shared_ptr<VirtualDevice> InputMapper::virtualDevice() const
{
  return impl->m_vdev;
}

// -------------------------------------------------------------------------------------------------
bool InputMapper::recordingMode() const
{
  return impl->m_recordingMode;
}

// -------------------------------------------------------------------------------------------------
void InputMapper::setRecordingMode(bool recording)
{
  if (impl->m_recordingMode == recording)
    return;

  const auto wasRecording = (impl->m_recordingMode && impl->m_seqTimer->isActive());
  impl->m_recordingMode = recording;

  if (wasRecording) emit recordingFinished(true);
  impl->m_seqTimer->stop();
  resetState();
  emit recordingModeChanged(impl->m_recordingMode);
}

// -------------------------------------------------------------------------------------------------
int InputMapper::keyEventInterval() const
{
  return impl->m_seqTimer->interval();
}

// -------------------------------------------------------------------------------------------------
void InputMapper::setKeyEventInterval(int interval)
{
  impl->m_seqTimer->setInterval(std::min(Settings::inputSequenceIntervalRange().max,
                                std::max(Settings::inputSequenceIntervalRange().min, interval)));
}

// -------------------------------------------------------------------------------------------------
void InputMapper::addEvents(const input_event* input_events, size_t num)
{
  if (num == 0) return;

  // If no key mapping is configured ...
  if (!impl->m_recordingMode && !impl->m_keymap.hasConfig()) {
    if (impl->m_vdev) { // ... forward events to virtual device if it exists...
      impl->m_vdev->emitEvents(input_events, num);
    } // ... end return
    return;
  }

  if (input_events[num-1].type != EV_SYN) {
    logWarning(input) << tr("Input mapper expects events seperated by SYN event.");
    return;
  } else if (num == 1) {
    logWarning(input) << tr("Ignoring single SYN event received.");
    return;
  }

  // For mouse button press ignore MSC_SCAN events
  if (num == 3
      && input_events[1].type == EV_KEY
      && (input_events[1].code == BTN_LEFT
          || input_events[1].code == BTN_RIGHT
          || input_events[1].code == BTN_MIDDLE)
      && input_events[0].type == EV_MSC && input_events[0].code == MSC_SCAN)
  {
    ++input_events; --num;
  }

  if (impl->m_recordingMode)
  {
    logDebug(input) << "Recorded device event:" << KeyEvent{input_events, input_events + num - 1};
    impl->record(input_events, num-1); // exclude closing syn event for recording
    return;
  }

  const auto res = impl->m_keymap.feed(input_events, num-1); // exclude syn event for keymap feed

  if (res == DeviceKeyMap::Result::Miss)
  { // key sequence miss, send all buffered events so far + current event
    impl->m_seqTimer->stop();
    if (impl->m_vdev)
    {
      if (impl->m_events.size()) {
        impl->m_vdev->emitEvents(impl->m_events);
        impl->m_events.resize(0);
      }
      impl->m_vdev->emitEvents(input_events, num);
    }
    impl->m_keymap.resetState();
  }
  else if (res == DeviceKeyMap::Result::Valid)
  { // KeyEvent is part of valid key sequence.
    impl->m_lastState = std::make_pair(res, impl->m_keymap.state());
    impl->m_seqTimer->start();
    if (impl->m_vdev) {
      impl->m_events.reserve(impl->m_events.size() + num);
      std::copy(input_events, input_events + num, std::back_inserter(impl->m_events));
    }
  }
  else if (res == DeviceKeyMap::Result::Hit)
  { // Found a valid key sequence
    impl->m_seqTimer->stop();
    if (impl->m_vdev)
    {
      if (impl->m_keymap.state()->second)
      {
        // TODO run action(s) / send mapped key events
        logDebug(input) << "Hit: Emitting Key Sequence"
                        << impl->m_keymap.state()->second->mappedKeys.keySequence();
        impl->emitQKeySequence(impl->m_keymap.state()->second->mappedKeys);
      }
      else
      {
        if (impl->m_events.size()) impl->m_vdev->emitEvents(impl->m_events);
        impl->m_vdev->emitEvents(input_events, num);
      }
    }
    impl->resetState();
  }
  else if (res == DeviceKeyMap::Result::AmbigiouslyHit)
  { // Found a valid key sequence, but are still more valid sequences possible -> start timer
    impl->m_lastState = std::make_pair(res, impl->m_keymap.state());
    impl->m_seqTimer->start();
    if (impl->m_vdev) {
      impl->m_events.reserve(impl->m_events.size() + num);
      std::copy(input_events, input_events + num, std::back_inserter(impl->m_events));
    }
  }
}

// -------------------------------------------------------------------------------------------------
void InputMapper::resetState()
{
  impl->resetState();
}

// -------------------------------------------------------------------------------------------------
void InputMapper::setConfiguration(const InputMapConfig& config)
{
  impl->m_config = config;
  impl->resetState();
  impl->m_keymap.reconfigure(impl->m_config);
}

// -------------------------------------------------------------------------------------------------
void InputMapper::setConfiguration(InputMapConfig&& config)
{
  impl->m_config.swap(config);
  impl->resetState();
  impl->m_keymap.reconfigure(impl->m_config);
}

// -------------------------------------------------------------------------------------------------
const InputMapConfig& InputMapper::configuration() const
{
  return impl->m_config;
}


