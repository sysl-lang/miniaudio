/*
 * The one shape in miniaudio's device surface that sysl has no spelling for.
 *
 * `ma_device_config_init` returns a `ma_device_config` by value, and that struct is several hundred
 * bytes of nested anonymous structs -- one per backend, a resampler configuration with a union in
 * it -- which the binding never lays out. It keeps the config as caller-placed storage it knows only
 * the size of, and writes the dozen fields it sets at their `offsetof`. What it cannot do is receive
 * the struct as a return value without declaring it, so this copies it into storage the caller
 * supplies instead.
 */

#ifndef SYSL_MINIAUDIO_SHIM_H
#define SYSL_MINIAUDIO_SHIM_H

#include "miniaudio.h"

/* `*out = ma_device_config_init(type)`: zero, the device type, and the default resampler. */
void sysl_ma_device_config_init(ma_device_config *out, ma_device_type type);

#endif
