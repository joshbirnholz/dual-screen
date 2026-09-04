// Not libnx. Just enough of its shape, with the signatures taken verbatim
// from the real header, to type-check the __SWITCH__ path of motion.cpp on a
// machine with no devkitPro. Catches exactly the class of mistake that broke
// the Switch build: an argument spelled as the integer underneath an enum.
#pragma once
#include <cstddef>
#include <cstdint>

typedef uint32_t Result;
typedef int32_t s32;
typedef uint32_t u32;

#define R_SUCCEEDED(res) ((res) == 0)

typedef enum { HidNpadIdType_No1 = 0, HidNpadIdType_Handheld = 0x20 } HidNpadIdType;
typedef enum {
    HidNpadStyleTag_NpadFullKey = (1U << 0),
    HidNpadStyleTag_NpadHandheld = (1U << 1),
    HidNpadStyleTag_NpadJoyDual = (1U << 2),
} HidNpadStyleTag;

typedef struct { u32 type_holder[4]; } HidSixAxisSensorHandle;
typedef struct { float x, y, z; } HidVector;
typedef struct {
    uint64_t delta_time;
    uint64_t sampling_number;
    HidVector acceleration;
    HidVector angular_velocity;
    HidVector angle;
    float direction[9];
    u32 attributes;
    u32 reserved;
} HidSixAxisSensorState;

Result hidGetSixAxisSensorHandles(HidSixAxisSensorHandle* handles, s32 total_handles,
                                  HidNpadIdType id, HidNpadStyleTag style);
Result hidStartSixAxisSensor(HidSixAxisSensorHandle handle);
Result hidStopSixAxisSensor(HidSixAxisSensorHandle handle);
size_t hidGetSixAxisSensorStates(HidSixAxisSensorHandle handle, HidSixAxisSensorState* states,
                                 size_t count);
