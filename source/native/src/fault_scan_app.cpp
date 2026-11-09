// Fault Scanner: read fault codes from every control unit and, after a second
// press within five seconds, clear them (worker: fault_scan.c).
#include "text_app.h"

namespace {
const unsigned kScanPoll=0x46534341u; // FSCA
const char kScanWorker[]=ID6_PLUGIN_ROOT "/FaultScan/fault-scan-qnx";
const char kScanFile[]=ID6_PLUGIN_ROOT "/FaultScan/last-scan.txt";
const unsigned kStatus=50u, kWarning=60u, kResult=61u;
const unsigned kClearEngine=1u, kClearAll=2u;
const unsigned kConfirmMs=5000u;
}

class FaultScan : public TextApp<FaultScan> {
    long page_;
    unsigned armed_;
    uint64_t armed_until_;
    char page_arg_[8];
    const char* args_[6];

    bool busy(unsigned model) {
        if (!workerBusy()) return false;
        text(model,"Busy. Wait for the current task to finish.");
        return true;
    }
    void run(const char* mode,const char* value,unsigned model) {
        args_[0]="--file"; args_[1]=kScanFile; args_[2]=mode; args_[3]=value; args_[4]=0;
        startWorker(args_,model);
    }
    void show(long page) {
        if (page<0) page=0;
        if (page>999) page=999;
        snprintf(page_arg_,sizeof page_arg_,"%ld",page);
        run("--show",page_arg_,kStatus);
    }
    bool rootVisibility(const bmw::Parcel& event) {
        bool show;
        if (event.readBool(show) && show && !workerBusy()) this->show(page_);
        return true;
    }
    bool clearVisibility(const bmw::Parcel& event) {
        bool show;
        if (!event.readBool(show) || !show) return true;
        armed_=0;
        text(kWarning,"Clearing erases stored fault codes and their freeze-frame data. "
                      "The car must be stopped; engine off with ignition on works best.");
        if (!workerBusy()) text(kResult,"");
        return true;
    }
    bool scan(const bmw::Parcel&) {
        if (busy(kStatus)) return true;
        page_=0;
        run("--scan",0,kStatus);
        return true;
    }
    bool nextPage(const bmw::Parcel&) {
        if (!busy(kStatus)) show(page_+1);
        return true;
    }
    bool previousPage(const bmw::Parcel&) {
        if (!busy(kStatus)) show(page_-1);
        return true;
    }
    void requestClear(unsigned which) {
        if (busy(kResult)) return;
        uint64_t now=bd_mono_ms();
        if (armed_!=which || now>armed_until_) {
            armed_=which;
            armed_until_=now+kConfirmMs;
            text(kResult,which==kClearAll
                 ? "Press again within 5 seconds to clear codes in ALL control units"
                 : "Press again within 5 seconds to clear the engine (DME) codes");
            return;
        }
        armed_=0;
        text(kResult,"Checking the car is stopped...");
        page_=0;
        run("--clear",which==kClearAll ? "all" : "12",kResult);
    }
    bool clearEngine(const bmw::Parcel&) { requestClear(kClearEngine); return true; }
    bool clearAll(const bmw::Parcel&) { requestClear(kClearAll); return true; }

public:
    FaultScan(bmw::IDispatcher& d,bmw::BasicString<char> name,unsigned maj,unsigned min,
              bmw::RHMIInterfaceVersion version,const bmw::Bundle& cfg,const bmw::Bundle& res)
        : TextApp<FaultScan>(d,name,maj,min,version,cfg,res,kScanWorker,kScanPoll,50u,61u),
          page_(0),armed_(0),armed_until_(0) {
        page_arg_[0]=0;
        for (unsigned i=0;i<6;++i) args_[i]=0;
    }
    virtual ~FaultScan() { shutdownWorker(); }
    virtual bool onFinalize() { shutdownWorker(); return true; }
    virtual bool onInit() {
        shutdownWorker();
        page_=0;
        armed_=0;
        typedef bmw::Delegate<bool,const bmw::Parcel&> Action;
        Action action;
        action.m_obj=this;
        action.m_exec=&Action::execImpl<FaultScan,&FaultScan::rootVisibility>;
        addHandler(100u,11u,action);
        action.m_exec=&Action::execImpl<FaultScan,&FaultScan::clearVisibility>;
        addHandler(101u,11u,action);
        action.m_exec=&Action::execImpl<FaultScan,&FaultScan::scan>;
        addHandler(41u,action);
        action.m_exec=&Action::execImpl<FaultScan,&FaultScan::nextPage>;
        addHandler(42u,action);
        action.m_exec=&Action::execImpl<FaultScan,&FaultScan::previousPage>;
        addHandler(43u,action);
        action.m_exec=&Action::execImpl<FaultScan,&FaultScan::clearEngine>;
        addHandler(44u,action);
        action.m_exec=&Action::execImpl<FaultScan,&FaultScan::clearAll>;
        addHandler(45u,action);
        text(kStatus,"Press OPTIONS, then Scan all modules.");
        return true;
    }
    // The worker reports the page it actually showed.
    void workerValue(unsigned key,long value) { if (key==1u) page_=value; }
};

extern "C" {
PluginAppInfo pluginAppInfo={1,"FaultScan","owner-development",1,create_app<FaultScan>,destroy_app};
}
