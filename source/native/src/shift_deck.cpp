#include "plugin_factory.h"
#include "diagnostic_worker.h"
#include "diagnostic_gateway.h"
#include "drive_frame.h"
#include "gauge_core.h"
#include "shift_assets.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>

namespace {
const unsigned kShiftPoll=0x53484654u;
const char kShiftWorker[]=ID6_PLUGIN_ROOT "/ShiftDeck/shift-deck-qnx";
// The learned gear calibration survives restarts beside the worker.
const char kGearCalibration[]=ID6_PLUGIN_ROOT "/ShiftDeck/gear-calibration.txt";
#ifdef NATIVE_SHIFT_SMOOTH
const unsigned kSweepSteps=128, kSweepBase=56400, kSweepAlert=56529;
const unsigned kEaseMs=50;
#endif
}

class ShiftDeck : public bmw::PluginApp {
    int socket_;
    pid_t child_;
    unsigned generation_;
    bool visible_;
    unsigned target_;
    uint64_t retry_at_;
    drive_frame frame_;
    unsigned bar_, lights_;
    int rpm_,kph_,gear_;
    bool shift_;
#ifdef NATIVE_SHIFT_SMOOTH
    bool metric_;
    double rpm_from_;
    uint64_t rpm_started_;
    double animatedRpm(uint64_t now) const {
        if (!rpm_started_ || now-rpm_started_>=kEaseMs) return frame_.rpm;
        const double remaining=1.-static_cast<double>(now-rpm_started_)/kEaseMs;
        return frame_.rpm+(rpm_from_-frame_.rpm)*remaining*remaining;
    }
#endif

    void label(unsigned id,const char* value) { updateModel(id,bmw::BasicString<char>(value)); }
    void reap() {
        if (child_<=0) return;
        pid_t ended=waitpid(child_,0,WNOHANG);
        if (ended==child_ || (ended<0 && errno==ECHILD)) child_=-1;
    }
    void stopChild() {
        reap();
        if (child_>0) kill(child_,SIGTERM);
    }
    void shutdown() {
        visible_=false;
        ++generation_;
        stopChild();
        if (socket_>=0) { close(socket_); socket_=-1; }
    }
    void postPoll() {
        bmw::Parcel data;
        data.writeUInt32(kShiftPoll);
        data.writeUInt32(generation_);
        post(bmw::Message(0x40000003u,data),visible_ ? 16u : 150u);
    }
    void digits(unsigned model, unsigned width, const char* text, unsigned base, bool center) {
        unsigned length=static_cast<unsigned>(strlen(text));
        unsigned offset=center && length<width ? (width-length)/2 : 0;
        for (unsigned i=0;i<width;++i) {
            char c=i>=offset && i-offset<length ? text[i-offset] : ' ';
            updateModel(model+i,bmw::ImageID(base+(c==' ' ? 11u : c=='-' ? 10u : c-'0')));
        }
    }
    void draw(uint64_t now) {
        bool live=visible_ && frame_.sampled_ms && frame_.sampled_ms<=now &&
                  now-frame_.sampled_ms<DRIVE_STALE_MS;
        int rpm=live ? frame_.rpm : -1;
        int kph=live ? frame_.kph : -1;
        int target=static_cast<int>(kShiftTargets[target_]);
        bool shift=rpm>=target;
        int displayed_rpm=rpm;
#ifdef NATIVE_SHIFT_SMOOTH
        if (rpm>=0) displayed_rpm=static_cast<int>(animatedRpm(now)+.5);
        unsigned bars=displayed_rpm>0 ? (displayed_rpm*kSweepSteps+4000)/8000 : 0;
        if (bars>kSweepSteps) bars=kSweepSteps;
        unsigned image=(shift ? kSweepAlert : kSweepBase)+bars;
#else
        unsigned bars=rpm>0 ? static_cast<unsigned>(rpm+125)/250 : 0;
        if (bars>32) bars=32;
        unsigned image=(shift ? kBarWarn : kBarBase)+bars;
#endif
        if (image!=bar_) {
            updateModel(54u,bmw::ImageID(image));
            bar_=image;
        }
        int approach=rpm-(target-2000);
        unsigned lights=shift ? 13u : approach>0 ? static_cast<unsigned>(approach)*12/2000 : 0u;
        if (lights!=lights_) {
            updateModel(55u,bmw::ImageID(kLightBase+lights));
            lights_=lights;
        }
        char text[48];
        if (displayed_rpm!=rpm_ || shift!=shift_) {
            if (displayed_rpm<0) strcpy(text,"--");
            else snprintf(text,sizeof text,"%d",displayed_rpm>9999 ? 9999 : displayed_rpm);
            digits(60u,4u,text,shift ? kDigitWarn : kDigitBase,true);
            rpm_=displayed_rpm;
        }
        if (kph!=kph_) {
            if (kph<0) strcpy(text,"--");
            else {
            int speed=(kph*621371+500000)/1000000;
#ifdef NATIVE_SHIFT_SMOOTH
            if (metric_) speed=kph;
#endif
            snprintf(text,sizeof text,"%d",speed);
        }
            digits(70u,3u,text,kDigitBase,false);
            kph_=kph;
        }
        // The worker's gear estimate, "-" until it has learned the car (model 74, legend on 75).
        int gear=live && frame_.gear>=1 && frame_.gear<=8 ? frame_.gear : 0;
        if (gear!=gear_) {
            const char digit[2]={gear ? static_cast<char>('0'+gear) : '-',0};
            digits(74u,1u,digit,kDigitBase,false);
            gear_=gear;
        }
        if (shift!=shift_) {
            updateModel(57u,bmw::ImageID(shift ? kShiftOn : kShiftOff));
            shift_=shift;
        }
    }
    bool visibility(const bmw::Parcel& event) {
        bool show;
        if (!event.readBool(show) || show==visible_) return true;
        visible_=show;
        frame_.sampled_ms=0;
#ifdef NATIVE_SHIFT_SMOOTH
        rpm_started_=0;
#endif
        if (show) retry_at_=0;
        else stopChild();
        draw(bd_mono_ms());
        return true;
    }
    template <unsigned Index> bool setTarget(const bmw::Parcel&) {
        target_=Index;
        showTarget();
        draw(bd_mono_ms());
        return true;
    }
#ifdef NATIVE_SHIFT_SMOOTH
    template <bool Metric> bool setUnits(const bmw::Parcel&) {
        metric_=Metric;
        kph_=-2;
        updateModel(64u,bmw::ImageID(metric_ ? 56660u : 55916u));
        draw(bd_mono_ms());
        return true;
    }
#endif
    void showTarget() {
        char text[48];
        snprintf(text,sizeof text,"Shift light at %u RPM",kShiftTargets[target_]);
        label(53u,text);
        updateModel(56u,bmw::ImageID(kTargetBase+target_));
        updateModel(68u,bmw::ImageID(kMarkBase+target_));
    }

public:
    ShiftDeck(bmw::IDispatcher& d,bmw::BasicString<char> name,unsigned maj,unsigned min,
              bmw::RHMIInterfaceVersion version,const bmw::Bundle& cfg,const bmw::Bundle& res)
        : PluginApp(d,name,maj,min,version,cfg,res),socket_(-1),child_(-1),generation_(0),
          visible_(false),target_(kDefaultTarget),retry_at_(0),bar_(0),lights_(0),
          rpm_(-2),kph_(-2),gear_(-1),shift_(false) {
        memset(&frame_,0,sizeof frame_);
#ifdef NATIVE_SHIFT_SMOOTH
        metric_=false;
        rpm_from_=0;
        rpm_started_=0;
#endif
    }
    virtual ~ShiftDeck() { shutdown(); }
    virtual bool onFinalize() { shutdown(); return true; }
    virtual bool onInit() {
        shutdown();
        memset(&frame_,0,sizeof frame_);
        bar_=0;
        lights_=static_cast<unsigned>(-1);
        rpm_=kph_=-2;
        gear_=-1;
        shift_=false;
        socket_=socket(AF_INET,SOCK_DGRAM,0);
        if (socket_>=0) {
            struct sockaddr_in address;
            memset(&address,0,sizeof address);
            address.sin_family=AF_INET;
            address.sin_port=htons(DRIVE_PORT);
            address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if (fcntl(socket_,F_SETFL,O_NONBLOCK)<0 || fcntl(socket_,F_SETFD,FD_CLOEXEC)<0 ||
                bind(socket_,reinterpret_cast<struct sockaddr*>(&address),sizeof address)<0) {
                close(socket_);
                socket_=-1;
            }
        }
        typedef bmw::Delegate<bool,const bmw::Parcel&> Action;
        Action action;
        action.m_obj=this;
        action.m_exec=&Action::execImpl<ShiftDeck,&ShiftDeck::visibility>;
        addHandler(100u,11u,action);
        action.m_exec=&Action::execImpl<ShiftDeck,&ShiftDeck::setTarget<0> >;
        addHandler(41u,action);
        action.m_exec=&Action::execImpl<ShiftDeck,&ShiftDeck::setTarget<1> >;
        addHandler(42u,action);
        action.m_exec=&Action::execImpl<ShiftDeck,&ShiftDeck::setTarget<2> >;
        addHandler(43u,action);
        action.m_exec=&Action::execImpl<ShiftDeck,&ShiftDeck::setTarget<3> >;
        addHandler(44u,action);
        action.m_exec=&Action::execImpl<ShiftDeck,&ShiftDeck::setTarget<4> >;
        addHandler(45u,action);
#ifdef NATIVE_SHIFT_SMOOTH
        action.m_exec=&Action::execImpl<ShiftDeck,&ShiftDeck::setUnits<false> >;
        addHandler(46u,action);
        action.m_exec=&Action::execImpl<ShiftDeck,&ShiftDeck::setUnits<true> >;
        addHandler(47u,action);
        updateModel(64u,bmw::ImageID(metric_ ? 56660u : 55916u));
#endif
        updateModel(57u,bmw::ImageID(kShiftOff));
        showTarget();
        draw(bd_mono_ms());
        if (socket_>=0) postPoll();
        return true;
    }
    virtual bool invokeDataUpdate(unsigned key,const bmw::Parcel& data) {
        if (key!=kShiftPoll) return PluginApp::invokeDataUpdate(key,data);
        unsigned generation;
        if (!data.readUInt32(generation)) return false;
        if (generation!=generation_ || socket_<0) return true;
        reap();
        uint64_t now=bd_mono_ms();
        if (visible_ && child_<=0 && now>=retry_at_) {
            const DiagnosticGateway gateway(ID6_PLUGIN_ROOT "/ShiftDeck/gateway-ip.txt");
            const char* args[]={kShiftWorker,"--host",gateway.host(),"--calibration",kGearCalibration,0};
            child_=launch(args,-1);
            retry_at_=now+1000;
        }
        for (unsigned i=0;i<8;++i) {
            drive_frame next;
            ssize_t n=recv(socket_,&next,sizeof next,0);
            if (n<=0) break;
            if (n==sizeof next && visible_ && next.sampled_ms>=frame_.sampled_ms &&
                next.sampled_ms<=now && next.rpm>=-1 && next.rpm<=16383 &&
                next.kph>=-1 && next.kph<=255 && next.gear>=0 && next.gear<=8) {
#ifdef NATIVE_SHIFT_SMOOTH
                if (next.rpm!=frame_.rpm) {
                    const bool continuous=next.rpm>=0 && frame_.rpm>=0 && frame_.sampled_ms &&
                                          now-frame_.sampled_ms<DRIVE_STALE_MS;
                    rpm_from_=continuous ? animatedRpm(now) : next.rpm;
                    rpm_started_=continuous ? now : 0;
                }
#endif
                frame_=next;
            }
        }
        draw(now);
        postPoll();
        return true;
    }
};

extern "C" {
PluginAppInfo pluginAppInfo={1,"ShiftDeck","owner-development",1,
                            create_app<ShiftDeck>,destroy_app};
}
