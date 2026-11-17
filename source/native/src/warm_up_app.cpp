// Warm-Up: oil and coolant temperature with a rising rev limit (worker: warm_up.c).
#include "text_app.h"

namespace {
const unsigned kWarmPoll=0x57524d55u; // WRMU
const char kWarmWorker[]=ID6_PLUGIN_ROOT "/WarmUp/warm-up-qnx";
const char* const kNoArgs[]={0};
}

class WarmUp : public TextApp<WarmUp> {
    bool visible_;

    bool visibility(const bmw::Parcel& event) {
        bool show;
        if (!event.readBool(show) || show==visible_) return true;
        visible_=show;
        if (show) runContinuously(kNoArgs,kNoArgs,52u);
        else stopWorker();
        return true;
    }

public:
    WarmUp(bmw::IDispatcher& d,bmw::BasicString<char> name,unsigned maj,unsigned min,
           bmw::RHMIInterfaceVersion version,const bmw::Bundle& cfg,const bmw::Bundle& res)
        : TextApp<WarmUp>(d,name,maj,min,version,cfg,res,kWarmWorker,kWarmPoll,50u,60u),
          visible_(false) {}
    virtual ~WarmUp() { shutdownWorker(); }
    virtual bool onFinalize() { visible_=false; shutdownWorker(); return true; }
    virtual bool onInit() {
        visible_=false;
        shutdownWorker();
        typedef bmw::Delegate<bool,const bmw::Parcel&> Action;
        Action action;
        action.m_obj=this;
        action.m_exec=&Action::execImpl<WarmUp,&WarmUp::visibility>;
        addHandler(100u,11u,action);
        text(52u,"Connecting to the engine...");
        return true;
    }
};

extern "C" {
PluginAppInfo pluginAppInfo={1,"WarmUp","owner-development",1,create_app<WarmUp>,destroy_app};
}
