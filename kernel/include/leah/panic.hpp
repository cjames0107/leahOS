#pragma once

#include <leah/interrupts.hpp>
#include <leah/types.hpp>

// Unrecoverable failure. Prints what it can and stops the machine - never
// reboots, because a reboot loop destroys the evidence.

[[noreturn]] void panic(const char* message);
[[noreturn]] void panic(const char* message, const interrupts::Frame& frame);
