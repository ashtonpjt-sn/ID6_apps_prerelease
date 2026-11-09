// Performance Timer: automatic 0-60, quarter mile, rolling and braking runs
// (worker: perf_timer.c). Results stay in perf-state.txt beside the worker.
#include "text_app.h"

namespace {
const unsigned kPerfPoll=0x50455246u; // PERF
const char kPerfWorker[]=ID6_PLUGIN_ROOT "/PerfTimer/perf-timer-qnx";
const char kPerfState[]=ID6_PLUGIN_ROOT "/PerfTimer/perf-state.txt";
const char* const kRunArgs[]={"--state",kPerfState,0};
const char* const kResetArgs[]={"--state",kPerfState,"--reset",0};
}

class PerfTimer : public TextApp<PerfTimer> {
    bool visible_;
    bool reset_pending_;

    void launchWorker() {
        // Reset once, then keep the saved results on any later restart.
        runContinuously(reset_pending_ ? kResetArgs : kRunArgs,kRunArgs,50u);
        reset_pending_=false;
    }
    bool visibility(const bmw::Parcel& event) {
        bool show;
        if (!event.readBool(show) || show==visible_) return true;
        visible_=show;
        if (show) launchWorker();
        else stopWorker();
        return true;
    }
    bool reset(const bmw::Parcel&) {
        reset_pending_=true;
        text(50u,"Best results cleared");
        if (visible_) launchWorker();
        return true;
    }

public:
    PerfTimer(bmw::IDispatcher& d,bmw::BasicString<char> name,unsigned maj,unsigned min,
              bmw::RHMIInterfaceVersion version,const bmw::Bundle& cfg,const bmw::Bundle& res)
        : TextApp<PerfTimer>(d,name,maj,min,version,cfg,res,kPerfWorker,kPerfPoll,50u,56u),
          visible_(false),reset_pending_(false) {}
    virtual ~PerfTimer() { shutdownWorker(); }
    virtual bool onFinalize() { visible_=false; shutdownWorker(); return true; }
    virtual bool onInit() {
        visible_=false;
        shutdownWorker();
        typedef bmw::Delegate<bool,const bmw::Parcel&> Action;
        Action action;
        action.m_obj=this;
        action.m_exec=&Action::execImpl<PerfTimer,&PerfTimer::visibility>;
        addHandler(100u,11u,action);
        action.m_exec=&Action::execImpl<PerfTimer,&PerfTimer::reset>;
        addHandler(41u,action);
        text(50u,"Connecting to the engine...");
        return true;
    }
};

extern "C" {
PluginAppInfo pluginAppInfo={1,"PerfTimer","owner-development",1,create_app<PerfTimer>,destroy_app};
}
