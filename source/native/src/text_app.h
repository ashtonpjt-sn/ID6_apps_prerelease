// Shared base for the text-driven apps (Warm-Up, Performance Timer, Fault
// Scanner and the BoostDeck vitals page). All vehicle work and formatting
// happens in a separate worker process; the plugin only forwards its output
// lines to RHMI models, so a slow control unit can never stall HmiMain3D.
//
// Worker output, one update per line:
//   T<model> <text>    set a Richtext model
//   I<model> <image>   set an image model
//   V<key> <number>    a value for the plugin itself, passed to workerValue()
// Lines for models outside the app's [first, last] range are ignored.
#ifndef TEXT_APP_H
#define TEXT_APP_H

#include "plugin_factory.h"
#include "diagnostic_worker.h"
#include "gauge_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#ifndef ID6_PLUGIN_ROOT
#define ID6_PLUGIN_ROOT "/var/opt/hmi/data/ID5/Plugins"
#endif

template <class Derived>
class TextApp : public bmw::PluginApp {
    pid_t child_;
    pid_t stale_[4]; // stopped workers still to be reaped
    int pipe_;
    unsigned generation_;
    bool polling_;
    bool discarding_;
    size_t used_;
    char line_[512];
    const char* worker_;
    unsigned poll_key_, first_, last_;

    void postPoll(unsigned delay) {
        if (polling_) return;
        polling_=true;
        bmw::Parcel data;
        data.writeUInt32(poll_key_);
        data.writeUInt32(generation_);
        post(bmw::Message(0x40000003u,data),delay);
    }
    static bool ended(pid_t pid) {
        pid_t result=waitpid(pid,0,WNOHANG);
        return result==pid || (result<0 && errno==ECHILD);
    }
    void reap() {
        if (child_>0 && ended(child_)) child_=-1;
        for (unsigned i=0;i<4;++i)
            if (stale_[i]>0 && ended(stale_[i])) stale_[i]=-1;
    }
    bool staleLeft() const {
        for (unsigned i=0;i<4;++i) if (stale_[i]>0) return true;
        return false;
    }
    void retire() {
        // Signal the current worker and keep its pid until it has exited.
        if (child_<=0) return;
        kill(child_,SIGTERM);
        if (ended(child_)) { child_=-1; return; }
        for (unsigned i=0;i<4;++i)
            if (stale_[i]<=0) { stale_[i]=child_; child_=-1; return; }
        child_=-1; // all slots busy: the HMI's SIGCHLD handling reaps it later
    }
    void closePipe() {
        if (pipe_>=0) { close(pipe_); pipe_=-1; }
        used_=0;
        discarding_=false;
    }
    void apply(char* line) {
        char kind=line[0];
        if (kind!='T' && kind!='I' && kind!='V') return;
        char* end;
        unsigned long model=strtoul(line+1,&end,10);
        if (kind=='V') {
            if (end==line+1 || *end!=' ') return;
            long number=strtol(end+1,&end,10);
            if (!*end) static_cast<Derived*>(this)->workerValue(static_cast<unsigned>(model),number);
            return;
        }
        if (end==line+1 || model<first_ || model>last_) return;
        const char* value=*end==' ' ? end+1 : end;
        if (kind=='T') {
            updateModel(static_cast<unsigned>(model),bmw::BasicString<char>(value));
        } else {
            unsigned long image=strtoul(value,&end,10);
            if (end!=value && !*end) updateModel(static_cast<unsigned>(model),
                                                 bmw::ImageID(static_cast<unsigned>(image)));
        }
    }
    void readPipe() {
        // A bounded drain: at most 8 reads of the line buffer per UI tick.
        for (unsigned reads=0;pipe_>=0 && reads<8;++reads) {
            char chunk[256];
            ssize_t n=read(pipe_,chunk,sizeof chunk);
            if (n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) return;
            if (n<=0) {
                closePipe();
                static_cast<Derived*>(this)->workerFinished();
                return;
            }
            for (ssize_t i=0;i<n;++i) {
                char c=chunk[i];
                if (c=='\n') {
                    if (!discarding_) {
                        line_[used_]=0;
                        apply(line_);
                    }
                    used_=0;
                    discarding_=false;
                } else if (c!='\r' && !discarding_) {
                    if (used_<sizeof line_-1) line_[used_++]=c;
                    else discarding_=true; // overlong line: drop it whole
                }
            }
        }
    }

protected:
    TextApp(bmw::IDispatcher& d,bmw::BasicString<char> name,unsigned maj,unsigned min,
            bmw::RHMIInterfaceVersion version,const bmw::Bundle& cfg,const bmw::Bundle& res,
            const char* worker,unsigned pollKey,unsigned firstModel,unsigned lastModel)
        : PluginApp(d,name,maj,min,version,cfg,res),child_(-1),pipe_(-1),generation_(0),
          polling_(false),discarding_(false),used_(0),worker_(worker),poll_key_(pollKey),
          first_(firstModel),last_(lastModel) {
        for (unsigned i=0;i<4;++i) stale_[i]=-1;
    }

    void text(unsigned model,const char* value) {
        updateModel(model,bmw::BasicString<char>(value));
    }
    bool workerBusy() {
        reap();
        return child_>0 || pipe_>=0;
    }
    // Start the worker with "--host GATEWAY" followed by up to 6 extra arguments.
    // Returns false (after showing why on statusModel) if it could not start.
    bool startWorker(const char* const* extra,unsigned statusModel) {
        stopWorker();
        char host[16];
        if (!installed_gateway(worker_,host)) {
            text(statusModel,"Missing gateway setting. Run the installer again.");
            return false;
        }
        const char* args[10]={worker_,"--host",host,0,0,0,0,0,0,0};
        for (unsigned i=0;extra && extra[i] && i<6;++i) args[3+i]=extra[i];
        int channel[2];
        if (pipe(channel)<0) { text(statusModel,"Could not start"); return false; }
        if (fcntl(channel[0],F_SETFL,O_NONBLOCK)>=0 &&
            fcntl(channel[0],F_SETFD,FD_CLOEXEC)>=0 && fcntl(channel[1],F_SETFD,FD_CLOEXEC)>=0)
            child_=launch(args,channel[1]);
        close(channel[1]);
        if (child_<0) {
            close(channel[0]);
            text(statusModel,"Could not start");
            return false;
        }
        pipe_=channel[0];
        used_=0;
        discarding_=false;
        postPoll(50u);
        return true;
    }
    void stopWorker() {
        closePipe();
        reap();
        retire();
        if (staleLeft()) postPoll(100u); // keep reaping until it has exited
    }
    // For onInit/onFinalize: also invalidates queued polls.
    void shutdownWorker() {
        ++generation_;
        polling_=false;
        closePipe();
        reap();
        retire();
    }
    // Hook: the worker closed its output. A derived class may declare its own
    // public workerFinished(), which then hides this one.
    void workerFinished() {}
    // Hook: a V line from the worker. Same hiding rule as workerFinished().
    void workerValue(unsigned, long) {}

public:
    virtual bool invokeDataUpdate(unsigned key,const bmw::Parcel& data) {
        if (key!=poll_key_) return PluginApp::invokeDataUpdate(key,data);
        unsigned generation;
        if (!data.readUInt32(generation)) return false;
        if (generation!=generation_) return true;
        polling_=false;
        readPipe();
        reap();
        if (pipe_>=0) postPoll(80u);
        else if (child_>0 || staleLeft()) postPoll(250u);
        return true;
    }
};

#endif
