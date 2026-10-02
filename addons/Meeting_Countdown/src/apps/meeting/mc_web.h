// Meeting Countdown — pages on the watch's settings web server (wifi_svc).
//   /meet              status, feeds, upcoming events, add event, import, options
//   /meet/status.json  sync progress for the page's live status line
//   /meet/events.json  upcoming events for scripts
#pragma once

#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
#include <WebServer.h>
void mcWebRegister(WebServer &server);
#endif
