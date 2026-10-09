#pragma once

#include "Limelight.h"

// Parse the numeric value carried by X-SS-Dynamic-HDR. Invalid, missing, or
// unknown values are treated as DYNAMIC_HDR_FORMAT_NONE. The parser accepts
// the enum names as a compatibility aid for hosts that expose textual values.
int parseDynamicHdrFormatValue(const char* value);

// Parse X-SS-Dynamic-HDR-Fallback. Sunshine normally sends enum names, but
// accepting the stable numeric identities keeps the client interoperable with
// hosts that serialize enums as integers. Unknown values map to NONE.
int parseDynamicHdrFallbackValue(const char* value);

