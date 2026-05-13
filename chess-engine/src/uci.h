#pragma once

#include <string>

namespace UCI {

extern bool usePolicy;
extern bool policyTimeMod;
extern int threads;
extern int policyScale;
extern int policyTimeMargin;
extern int policyTimeBoost;
extern int moveOverhead;
extern std::string policyFile;

// Policy v2 surface. v1 options above are preserved for A/B and CCRL.
// Mode values: 0 = off, 1 = root_bonus (v1 behaviour), 2 = quiet_residual
//              (bonus applied only to non-capture / non-promotion moves).
extern int policyMode;
extern bool usePolicyV2;
extern std::string policyV2File;
extern int policyV2Mode;
extern int policyV2Scale;
extern int policyV2QuietScale;
extern int policyV2EndgameScale;
extern int policyV2BonusClamp;

void loop();

} // namespace UCI
