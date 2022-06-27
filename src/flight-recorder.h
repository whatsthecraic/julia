/*
 * flight-recorder.h
 *
 *  Created on: 17 Jun 2022
 *      Author: Dean De Leo
 */

#pragma once

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Macro to record a message in the flight recorder, for the C troglodites
#define FR(...) { char* buffer = NULL; uint64_t buffer_sz = 0;  \
    bool is_enabled = flight_recorder_insert(__FILE__, __LINE__, __FUNCTION__, &buffer, &buffer_sz); \
    if(is_enabled){ snprintf(buffer, buffer_sz, __VA_ARGS__); }\
    } // end of the block for the FR

// Whether to enable or no the recording of the messages in the flight recorder
void flight_recorder_enable(bool value);

// Insert a message in the flight recorder. Return true if the FR is enabled, false otherwise.
// Implementation for the macro FR
bool flight_recorder_insert(const char* source, uint64_t line, const char* function, char** out_buffer, uint64_t* out_buffer_sz);

// Dump the content of the flight recorder to stdout
void flight_recorder_dump();

// Dump up to N entries in the flight recorder
void flight_recorder_dump_n(uint64_t n);

#ifdef __cplusplus
} // extern "C"
#endif


