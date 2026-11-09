// Definitions for the SDK symbols the plugins import, recording into mock::.
#include "mock_bmw.h"
#include "plugin_factory.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <deque>

extern "C" PluginAppInfo pluginAppInfo;

namespace {
typedef bmw::Delegate<bool, const bmw::Parcel&> Action;
std::map<unsigned, Action> actions;
std::map<std::pair<unsigned, unsigned>, Action> events;
struct Posted { unsigned values[4]; unsigned count; };
std::deque<Posted> queue;
// Mock Parcel layout: up to four 32-bit values, then count | position << 8.
struct ParcelData { unsigned values[4]; unsigned meta; };
ParcelData& data(const bmw::Parcel& p) {
    return *reinterpret_cast<ParcelData*>(const_cast<bmw::Parcel*>(&p));
}
bool next(const bmw::Parcel& p, unsigned& value) {
    ParcelData& d = data(p);
    unsigned count = d.meta & 0xff, position = d.meta >> 8;
    if (position >= count) return false;
    value = d.values[position];
    d.meta = count | (position + 1) << 8;
    return true;
}
void sleep_ms(unsigned ms) {
    struct timespec t = {static_cast<time_t>(ms / 1000), static_cast<long>(ms % 1000) * 1000000L};
    nanosleep(&t, 0);
}
}

namespace mock {
std::map<unsigned, std::string> text;
std::map<unsigned, unsigned> image;

bmw::PluginApp* create(const char* name) {
    static char dispatcher[64], bundle[64];
    bmw::PluginApp* app = pluginAppInfo.create(*reinterpret_cast<bmw::IDispatcher*>(dispatcher), name,
                                               *reinterpret_cast<bmw::Bundle*>(bundle),
                                               *reinterpret_cast<bmw::Bundle*>(bundle));
    assert(app && app->onInit());
    return app;
}

bool press(unsigned action) {
    std::map<unsigned, Action>::iterator i = actions.find(action);
    assert(i != actions.end());
    bmw::Parcel empty;
    return i->second.m_exec(i->second.m_obj, empty);
}

bool visible(unsigned state, bool shown) {
    std::map<std::pair<unsigned, unsigned>, Action>::iterator i = events.find(std::make_pair(state, 11u));
    assert(i != events.end());
    bmw::Parcel event(shown);
    return i->second.m_exec(i->second.m_obj, event);
}

void pump(bmw::PluginApp* app, unsigned ms) {
    for (unsigned waited = 0; waited <= ms; waited += 20) {
        std::deque<Posted> due;
        due.swap(queue);
        for (size_t k = 0; k < due.size(); ++k) {
            bmw::Parcel parcel;
            ParcelData& d = data(parcel);
            memcpy(d.values, due[k].values, sizeof d.values);
            d.meta = due[k].count;
            unsigned key;
            if (next(parcel, key)) app->invokeDataUpdate(key, parcel);
        }
        sleep_ms(20);
    }
}

bool wait_text(bmw::PluginApp* app, unsigned model, const char* needle, unsigned ms) {
    for (unsigned waited = 0; waited <= ms; waited += 100) {
        if (text.count(model) && text[model].find(needle) != std::string::npos) return true;
        pump(app, 100);
    }
    return false;
}
}

namespace bmw {
template <> BasicString<char>::BasicString(const char* s) : m_data(strdup(s)) {}
template <> BasicString<char>::BasicString(const BasicString& o) : m_data(strdup(o.m_data)) {}
template <> BasicString<char>::~BasicString() { free(m_data); }
template <> BasicString<char>& BasicString<char>::operator=(const BasicString& o) {
    char* copy = strdup(o.m_data);
    free(m_data);
    m_data = copy;
    return *this;
}
ImageID::ImageID(unsigned int value) : id(value), type(0) {}
TextID::TextID(unsigned int) : m_a(0), m_b(0) {}

Parcel::Parcel() { memset(_raw, 0, sizeof _raw); }
Parcel::Parcel(bool value) { memset(_raw, 0, sizeof _raw); _raw[0] = value; _raw[4] = 1; }
Parcel::~Parcel() {}
bool Parcel::readInt32(int& v) const { unsigned u; if (!next(*this, u)) return false; v = static_cast<int>(u); return true; }
bool Parcel::readUInt32(unsigned int& v) const { return next(*this, v); }
bool Parcel::readBool(bool& v) const { unsigned u; if (!next(*this, u)) return false; v = u != 0; return true; }
void Parcel::writeUInt32(unsigned int v) {
    unsigned count = _raw[4] & 0xff;
    assert(count < 4);
    _raw[count] = v;
    _raw[4] = (_raw[4] & ~0xffu) | (count + 1);
}

Message::Message(unsigned int, const Parcel& p) { memcpy(opaque_.storage, &p, sizeof p); }
Message::Message(const Message& o) { memcpy(opaque_.storage, o.opaque_.storage, sizeof opaque_.storage); }
Message::~Message() {}

PluginApp::PluginApp(IDispatcher&, BasicString<char>, unsigned int, unsigned int, RHMIInterfaceVersion,
                     const Bundle&, const Bundle&) {
    memset(_reserved, 0, sizeof _reserved);
}
PluginApp::~PluginApp() {}
void PluginApp::updateModel(unsigned int, const DataTableBase&) {}
void PluginApp::updateModel(unsigned int model, const BasicString<char>& s) {
    mock::text[model] = *reinterpret_cast<char* const*>(&s);
}
void PluginApp::updateModel(unsigned int, int) {}
void PluginApp::updateModel(unsigned int model, const ImageID& i) { mock::image[model] = i.id; }
void PluginApp::updateModel(unsigned int, const TextID&) {}
void PluginApp::addHandler(unsigned int action, const Delegate<bool, const Parcel&>& d) { actions[action] = d; }
void PluginApp::addHandler(unsigned int state, unsigned int event, const Delegate<bool, const Parcel&>& d) {
    events[std::make_pair(state, event)] = d;
}
void PluginApp::addHandlerWithMap(unsigned int, unsigned int, const Delegate<bool, const Map<unsigned int, Variant>&>&) {}
void PluginApp::setProperty(unsigned int, unsigned int, const Parcel&) {}
void PluginApp::triggerEvent(unsigned int) {}
void PluginApp::triggerEvent(unsigned int, const Parcel&) {}
bool PluginApp::start() { return true; }
bool PluginApp::stop() { return true; }
bool PluginApp::activate() { return true; }
bool PluginApp::deactivate() { return true; }
bool PluginApp::init() { return true; }
bool PluginApp::finalize() { return true; }
void PluginApp::post(const Message&) {}
void PluginApp::post(const Message& m, unsigned int) {
    Posted p;
    const unsigned* raw = reinterpret_cast<const unsigned*>(&m);
    memcpy(p.values, raw, sizeof p.values);
    p.count = raw[4] & 0xff;
    queue.push_back(p);
}
void PluginApp::post(Runnable*) {}
void PluginApp::post(Runnable*, unsigned int) {}
bool PluginApp::getName() const { return true; }
bool PluginApp::getResourceFile(ResourceType::ResourceTypes) const { return true; }
bool PluginApp::getVersion(unsigned int&, unsigned int&, unsigned int&) const { return true; }
bool PluginApp::getRhmiVersion(RHMIInterfaceVersion&) const { return true; }
bool PluginApp::invokeCommand(unsigned int, const Parcel&) { return false; }
bool PluginApp::updateModel(unsigned int, const Parcel&, unsigned char) { return false; }
bool PluginApp::handleEvent(unsigned int, unsigned int, const Parcel&) { return false; }
bool PluginApp::invokeDataUpdate(unsigned int, const Parcel&) { return false; }
bool PluginApp::requestDataValue(unsigned int) { return false; }
bool PluginApp::onError(unsigned int, const BasicString<char>&) { return false; }
bool PluginApp::onInit() { return true; }
bool PluginApp::onFinalize() { return true; }
bool PluginApp::getCapabilities(const std::string&) { return false; }
bool PluginApp::onCheckAppAvailable() { return true; }
bool PluginApp::handleMessage(const Message&) { return false; }
bool PluginApp::invokeCommandWithMap(unsigned int, const Map<unsigned int, Variant>&) { return false; }
bool PluginApp::updateModelWithMap(unsigned int, const Map<unsigned int, Variant>&) { return false; }
bool PluginApp::handleEventWithMap(unsigned int, unsigned int, const Map<unsigned int, Variant>&) { return false; }
bool PluginApp::invokeCDSUpdate(BasicString<char>&, BasicString<unsigned short>&) { return false; }
bool PluginApp::invokeVoiceStateChanged(unsigned short) { return false; }
bool PluginApp::invokeAppManagerUpdate(const BasicString<char>&) { return false; }
bool PluginApp::invokeSdsEvent(unsigned int, const Map<unsigned int, Variant>&) { return false; }
}
