#ifndef ID6_DIAGNOSTIC_GATEWAY_H
#define ID6_DIAGNOSTIC_GATEWAY_H

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

// Where the HMI loads plugins from. Host tests point it at a scratch folder.
#ifndef ID6_PLUGIN_ROOT
#define ID6_PLUGIN_ROOT "/var/opt/hmi/data/ID5/Plugins"
#endif

class DiagnosticGateway {
    char host_[16];

public:
    explicit DiagnosticGateway(const char* settings) {
        strcpy(host_, "169.254.21.3");
        FILE* file = fopen(settings, "r");
        if (!file) return;
        char candidate[16];
        in_addr address;
        if (fscanf(file, "%15s", candidate) == 1 && inet_pton(AF_INET, candidate, &address) == 1)
            strcpy(host_, candidate);
        fclose(file);
    }

    const char* host() const { return host_; }
};
#endif
