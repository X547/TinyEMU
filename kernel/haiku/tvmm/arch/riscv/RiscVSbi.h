#pragma once

#include <SupportDefs.h>


// The SBI as the guests call it.
namespace Sbi {


constexpr int64 kSuccess = 0;
constexpr int64 kErrFailed = -1;
constexpr int64 kErrNotSupported = -2;
constexpr int64 kErrInvalidParam = -3;
constexpr int64 kErrAlreadyAvailable = -6;

// v2.0
constexpr uint64 kSpecVersion = 2 << 24;
// not a registered implementation ID: "TVMM"
constexpr uint64 kImplId = 0x54564d4d;
constexpr uint64 kImplVersion = 1;

constexpr uint32 kExtLegacyLast = 0x08;
constexpr uint32 kExtBase = 0x10;
constexpr uint32 kExtTime = 0x54494d45;
constexpr uint32 kExtIpi = 0x735049;
constexpr uint32 kExtRfence = 0x52464e43;
constexpr uint32 kExtHsm = 0x48534d;
constexpr uint32 kExtSrst = 0x53525354;
constexpr uint32 kExtDbcn = 0x4442434e;

constexpr uint32 kLegacySetTimer = 0;
constexpr uint32 kLegacyPutchar = 1;
constexpr uint32 kLegacyGetchar = 2;
constexpr uint32 kLegacyClearIpi = 3;
constexpr uint32 kLegacySendIpi = 4;
constexpr uint32 kLegacyFenceI = 5;
constexpr uint32 kLegacySfenceVma = 6;
constexpr uint32 kLegacySfenceAsid = 7;
constexpr uint32 kLegacyShutdown = 8;

constexpr uint64 kHsmSuspendRetentive = 0x00000000;

constexpr uint64 kSrstShutdown = 0;
constexpr uint64 kSrstColdReboot = 1;
constexpr uint64 kSrstWarmReboot = 2;
constexpr uint64 kSrstReasonNone = 0;
constexpr uint64 kSrstReasonFailure = 1;


} // Sbi
