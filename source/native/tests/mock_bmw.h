// Host stand-in for the NBT Evo HMI framework, so the plugin classes can be
// driven without a head unit. It records what a plugin shows and delivers the
// messages it posts. It is not a model of the real framework's timing.
#ifndef MOCK_BMW_H
#define MOCK_BMW_H
#include "bmw_list.h"
#include <map>
#include <string>
#include <vector>

namespace mock {
extern std::map<unsigned, std::string> text;   // model -> latest text
extern std::map<unsigned, unsigned> image;     // model -> latest image ID
// Create the plugin through its exported descriptor and run onInit().
bmw::PluginApp* create(const char* name);
// An RA action (button press) or a state event, as the HMI would deliver it.
bool press(unsigned action);
bool visible(unsigned state, bool shown);
// Deliver posted messages for about `ms` milliseconds of wall time.
void pump(bmw::PluginApp* app, unsigned ms);
// Keep delivering until `model` shows text containing `needle`, up to `ms`.
bool wait_text(bmw::PluginApp* app, unsigned model, const char* needle, unsigned ms);
}
#endif
