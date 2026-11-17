#ifndef DRIVE_FRAME_H
#define DRIVE_FRAME_H

#include <stdint.h>

enum { DRIVE_PORT = 42863, DRIVE_STALE_MS = 750 };

struct drive_frame {
    uint64_t sampled_ms;
    int32_t rpm;
    int32_t kph;
    int32_t gear;     /* 1..8, 0 = unknown */
    int32_t reserved; /* keeps the frame 24 bytes on every ABI */
};

#endif
