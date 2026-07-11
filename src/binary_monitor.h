// Commander X16 Emulator (Box16)
// VICE binary monitor protocol (API v2) server, so debugger front-ends
// written for VICE -- e.g. the VS64 VSCode extension -- can drive Box16.
// All rights reserved. License: 2-clause BSD

#pragma once
#ifndef BINARY_MONITOR_H
#	define BINARY_MONITOR_H

#	include <string>

// Parse an address of the form "ip4://127.0.0.1:6502" (the VICE
// -binarymonitoraddress syntax) and start listening. Returns false if the
// address cannot be parsed or the socket cannot be opened.
bool binary_monitor_init(const std::string &address);
void binary_monitor_shutdown();

// Pump the monitor: accept a client, service protocol requests, and emit
// STOPPED/RESUMED/CHECKPOINT events on pause-state transitions. Call from
// the emulator main loop; cheap no-op when not initialized. `emulation_paused`
// tells the monitor which branch of the loop it is being called from so it
// can skip its own throttling while the machine is stopped.
void binary_monitor_process(bool emulation_paused);

#endif
