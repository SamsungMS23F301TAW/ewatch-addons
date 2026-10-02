// Tilt Parallax — optional serial console (115200, newline-terminated).
//
// loop() calls parallaxSerialPoll(), which collects a line and queues it;
// the face executes queued commands on the render task (the only task that
// may draw). Nothing here is needed for normal use. Type "help" for the list.
#pragma once
#include <stdint.h>

static const int kCmdLen = 40;
struct ParallaxCmd { char line[kCmdLen]; };

void parallaxSerialPoll();                 // loopTask
bool parallaxCmdPop(ParallaxCmd &out);     // render task
