#pragma once

#include <leah/types.hpp>

// AC'97 audio, over the two I/O port ranges the controller exposes.
//
// Chosen over Intel HD Audio because it is the smaller honest thing: a mixer
// behind one port pair, a bus master behind another, and a list of buffers in
// physical memory that the card walks on its own. HD Audio would need a command
// ring and a codec graph to make the same noise.
//
// One format, everywhere: 16-bit signed samples, two channels, 48 kHz. That is
// AC'97's native rate, so nothing has to resample and there is no variable-rate
// negotiation to get wrong. A program that wants something else converts before
// it gets here.

namespace audio {

constexpr u32 kSampleRate = 48000;
constexpr u32 kChannels   = 2;

// True once a controller has been found and reset.
bool available();

// A word about who this belongs to, for an about box.
const char* device_name();

// Queue interleaved stereo samples. Returns how many were taken, which may be
// fewer than offered - the ring is finite and this never blocks, because the
// caller is usually a window server client that must keep drawing. Zero means
// full: wait and offer again.
usize play(const i16* samples, usize count);

// How many samples could be taken right now.
usize space();

// Hand over a part-filled buffer. play() only submits whole ones, so a program
// that has finished - a notification sound, the end of a file - has to say so,
// or its last few milliseconds sit in the ring and never play.
void flush();

// Stop immediately and drop whatever is queued.
void stop();

// Master volume, 0..100. Muting is volume 0 - a separate mute flag would be a
// second piece of state saying the same thing.
void set_volume(u32 percent);
u32  volume();

// Diagnostics: what the card says it is doing. Exposed so a boot self-test can
// tell "queued and playing" from "queued and ignored", which look identical
// from the outside until the speakers stay silent.
struct Status {
    u8  current_index;
    u8  last_valid;
    u8  status_reg;
    u8  control_reg;
    u16 position;           // samples left in the buffer being played
    u32 queued_buffers;
};
Status status();

bool init();

} // namespace audio
