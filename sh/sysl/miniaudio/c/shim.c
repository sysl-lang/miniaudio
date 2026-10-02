#include "shim.h"

void sysl_ma_device_config_init(ma_device_config *out, ma_device_type type)
{
    *out = ma_device_config_init(type);
}
