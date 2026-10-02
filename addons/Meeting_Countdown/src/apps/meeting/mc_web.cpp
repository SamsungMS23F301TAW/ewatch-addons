// Meeting Countdown — web pages. See mc_web.h.
#include "mc_web.h"

#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <new>
#include <string.h>
#include "mc_app.h"
#include "mc_sync.h"
#include "mc_proto.h"
#include "mc_text.h"
#include "ics_parser.h"
#include "http_parse.h"
#include "model.h"
#include "storage.h"
#include "controller.h"

using namespace mc;

static WebServer *gSrv = nullptr;
// Page scratch (wifi task only), PSRAM.
static Event *listBuf() {
  static Event *p = nullptr;
  if (!p) p = mcapp::allocEvents(mcapp::kMaxMerged);
  return p;
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static void send(const char *s) { gSrv->sendContent(s); }
static void send(const String &s) { gSrv->sendContent(s); }

static String esc(const char *in) {
  String out;
  out.reserve(strlen(in) + 8);
  for (const char *p = in; *p; p++) {
    switch (*p) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += *p;
    }
  }
  return out;
}

static void redirect(const char *query) {
  String loc = "/meet";
  if (query && *query) { loc += "?"; loc += query; }
  gSrv->sendHeader("Location", loc);
  gSrv->send(303);
}

static String urlEncode(const char *s) {
  static const char *hex = "0123456789ABCDEF";
  String o;
  for (const char *p = s; *p; p++) {
    char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_') o += c;
    else { o += '%'; o += hex[(c >> 4) & 15]; o += hex[c & 15]; }
  }
  return o;
}

static void redirectMsg(const char *kind, const char *text) {
  String q = kind;
  q += "=";
  q += urlEncode(text);
  redirect(q.c_str());
}

static void fmtLocal(int64_t utc, int tz, char *buf, size_t n, bool withDay) {
  Civil c = secondsToCivil(utc + (int64_t)tz * 60);
  static const char *wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  if (withDay) snprintf(buf, n, "%s %02d:%02d", wd[c.weekday], c.hour, c.minute);
  else snprintf(buf, n, "%02d:%02d", c.hour, c.minute);
}

// ---------------------------------------------------------------------------
// page
// ---------------------------------------------------------------------------
static const char kHead[] PROGMEM =
  "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Meeting Countdown</title><style>"
  "body{font-family:-apple-system,system-ui,sans-serif;max-width:480px;margin:1em auto;padding:0 1em;"
  "background:#0d1014;color:#e8ecf0}h1{font-weight:300;margin:.2em 0}"
  "a{color:#7ef0cf}label{display:block;margin:.6em 0 .2em;font-size:.9em;color:#9fb6c8}"
  "input,select{font-size:1em;padding:.45em;width:100%;box-sizing:border-box;background:#1a1f26;"
  "color:#e8ecf0;border:1px solid #333b45;border-radius:6px}"
  "input[type=checkbox]{width:auto;margin-right:.45em}"
  ".row{display:flex;gap:.6em}.row>*{flex:1}"
  "button{margin-top:1em;padding:.7em;font-size:1.05em;border:0;border-radius:8px;background:#1fa883;"
  "color:#fff;width:100%;cursor:pointer}button.sec{background:#2c3540}button.del{background:#7a2b2b;"
  "width:auto;margin:0;padding:.3em .7em;font-size:.85em}"
  "fieldset{border:1px solid #273039;border-radius:10px;padding:.6em 1em;margin:1em 0}"
  "legend{padding:0 .4em;color:#9fb6c8}.muted{color:#8593a0;font-size:.85em}"
  ".chk{display:flex;align-items:center;margin:.35em 0}.chk label{margin:0;color:#e8ecf0}"
  ".ev{display:flex;align-items:center;gap:.6em;padding:.5em 0;border-bottom:1px solid #1d242c}"
  ".ev .n{flex:1;min-width:0}.ev .t{font-variant-numeric:tabular-nums;font-size:.85em;color:#9fb6c8}"
  ".tag{font-size:.7em;padding:.1em .45em;border-radius:6px;background:#273039;color:#9fb6c8;margin-left:.3em;white-space:nowrap}"
  ".ok{background:#103b33;color:#7ef0cf;padding:.6em .8em;border-radius:8px}"
  ".err{background:#3b1414;color:#ffb0b0;padding:.6em .8em;border-radius:8px}"
  ".feed{padding:.5em 0;border-bottom:1px solid #1d242c}.feed code{word-break:break-all;color:#cfe}"
  ".offs{display:flex;flex-wrap:wrap;gap:.2em 1em}"
  "#st{font-weight:600}"
  "</style>";

static void sendSelect(const char *name, const int *vals, const char *const *labels, int n, int cur) {
  String h = "<select name=";
  h += name;
  h += ">";
  for (int i = 0; i < n; i++) {
    h += "<option value=";
    h += vals[i];
    if (vals[i] == cur) h += " selected";
    h += ">";
    h += labels[i];
    h += "</option>";
  }
  h += "</select>";
  send(h);
}

static void handlePage() {
  mcapp::keepAwakeFor(120000);
  int64_t now = 0;
  bool haveNow = mcapp::nowUtc(now);
  int tz = mcapp::tzOffsetMin();
  mcapp::Settings set = mcapp::settings();
  mcapp::SyncInfo si = mcapp::syncInfo();
  Event *gList = listBuf();
  int n = gList ? mcapp::snapshot(gList, mcapp::kMaxMerged) : 0;

  gSrv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  gSrv->send(200, "text/html", "");
  send(FPSTR(kHead));
  send("<h1>Meeting Countdown</h1>");

  if (gSrv->hasArg("msg")) { send("<p class=ok>"); send(esc(gSrv->arg("msg").c_str())); send("</p>"); }
  if (gSrv->hasArg("err")) { send("<p class=err>"); send(esc(gSrv->arg("err").c_str())); send("</p>"); }

  // ---- status ----
  {
    String h = "<p>";
    char line[64];
    bool warn;
    FaceInfo fi = computeFace(gList, n, now, tz, set.leadMin);
    if (!haveNow) {
      h += "The watch clock is not set yet.";
    } else if (fi.mode == FaceMode::InMeeting) {
      char d[24];
      fmtDuration(fi.remaining, d, sizeof(d));
      h += "Now: <b>"; h += esc(gList[fi.cur].title); h += "</b> ("; h += d; h += " left)";
    } else if (fi.next >= 0) {
      char w[16], d[24];
      fmtWhen(gList[fi.next].start, now, tz, w, sizeof(w));
      fmtCountdown(gList[fi.next].start - now, d, sizeof(d));
      h += "Next: <b>"; h += esc(gList[fi.next].title); h += "</b> at "; h += w; h += " ("; h += d; h += ")";
    } else {
      h += "All clear: nothing coming up.";
    }
    h += "</p>";
    send(h);
    mcapp::statusLine(now, line, sizeof(line), warn);
    mcsync::Progress pr = mcsync::progress();
    h = "<p class=muted>Sync: <span id=st>";
    if (pr.running) h += esc(pr.step);
    else if (line[0]) h += esc(line);
    else if (mcapp::feedCount() == 0) h += "no calendar feed yet";
    else h += "not synced yet";
    h += "</span>";
    if (si.calName[0]) { h += " &middot; calendar &ldquo;"; h += esc(si.calName); h += "&rdquo;"; }
    h += "</p><form method=POST action=/meet/sync><button>Sync now</button></form>";
    if (pr.running) {
      h += "<script>(function p(){fetch('/meet/status.json').then(r=>r.json()).then(j=>{"
           "document.getElementById('st').textContent=j.step||j.status;"
           "if(j.syncing)setTimeout(p,800);else location.replace('/meet?msg='+encodeURIComponent(j.status));"
           "}).catch(()=>setTimeout(p,1500))})()</script>";
    }
    send(h);
  }

  // ---- feeds ----
  send("<fieldset><legend>Calendar feeds (iCal / .ics)</legend><form method=POST action=/meet/feeds>");
  for (int i = 0; i < mcapp::kMaxFeeds; i++) {
    char lbl[96];
    mcapp::feedLabel(i, lbl, sizeof(lbl));
    String h = "<div class=feed><label>Feed ";
    h += (i + 1);
    h += "</label>";
    if (lbl[0]) {
      h += "<code>"; h += esc(lbl); h += "</code>";
      const mcapp::FeedStatus &fs = si.feed[i];
      if (fs.result) {
        h += "<div class=muted>";
        h += mcapp::syncResultText((mcapp::SyncResult)fs.result);
        if (fs.http && fs.http != 200) { h += " (HTTP "; h += fs.http; h += ")"; }
        if (fs.result == (uint8_t)mcapp::SyncResult::Ok) {
          h += " &middot; "; h += fs.events; h += " events &middot; "; h += (unsigned)(fs.bytes / 1024); h += " KB";
        }
        h += "</div>";
      }
      h += "<input name=f"; h += i; h += " type=url placeholder='paste a new address to replace' autocomplete=off>";
      h += "<div class=chk><input type=checkbox id=rm"; h += i; h += " name=rm"; h += i;
      h += "><label for=rm"; h += i; h += ">Remove this feed</label></div>";
    } else {
      h += "<input name=f"; h += i; h += " type=url placeholder='https://...  or  webcal://...' autocomplete=off>";
    }
    h += "</div>";
    send(h);
  }
  send("<button>Save feeds</button></form>"
       "<p class=muted>Use your calendar's private iCal address. Google Calendar: Settings &rarr; your "
       "calendar &rarr; <i>Secret address in iCal format</i>. Outlook: Settings &rarr; Calendar &rarr; Shared "
       "calendars &rarr; <i>Publish</i> &rarr; ICS link. iCloud: share the calendar as a public calendar. "
       "The watch keeps the next 48 hours (up to 16 events) and never shows the full address again.</p>"
       "</fieldset>");

  // ---- upcoming ----
  {
    send("<fieldset><legend>Upcoming</legend>");
    if (n == 0) send("<p class=muted>Nothing yet.</p>");
    for (int i = 0; i < n; i++) {
      const Event &e = gList[i];
      char when[24], end[8];
      String h = "<div class=ev><div class=n><div class=t>";
      static const char *wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
      int64_t today = floorDiv(now + (int64_t)tz * 60, kDay);
      if (isAllDay(e)) {
        Civil c = secondsToCivil(e.start);
        snprintf(when, sizeof(when), "%s, all day", floorDiv(e.start, kDay) == today ? "Today" : wd[c.weekday]);
        h += when;
      } else {
        bool isToday = floorDiv(e.start + (int64_t)tz * 60, kDay) == today;
        fmtLocal(e.start, tz, when, sizeof(when), !isToday);
        fmtLocal(e.end, tz, end, sizeof(end), false);
        h += when; h += "&ndash;"; h += end;
      }
      h += "</div><b>";
      h += esc(e.title);
      h += "</b>";
      if (e.location[0]) { h += " <span class=muted>"; h += esc(e.location); h += "</span>"; }
      if (e.leaveMin) { h += " <span class=tag>leave "; h += e.leaveMin; h += " min early</span>"; }
      h += e.source == (uint8_t)Source::Feed ? " <span class=tag>calendar</span>"
         : e.source == (uint8_t)Source::Manual ? " <span class=tag>added here</span>"
                                                : " <span class=tag>USB / import</span>";
      h += "</div>";
      if (e.source != (uint8_t)Source::Feed) {
        char key[12];
        snprintf(key, sizeof(key), "%08lx", (unsigned long)e.key);
        h += "<form method=POST action=/meet/del><input type=hidden name=k value=";
        h += key;
        h += "><button class=del>Delete</button></form>";
      }
      h += "</div>";
      send(h);
    }
    send("</fieldset>");
  }

  // ---- add ----
  {
    char today[12] = "";
    if (haveNow) {
      Civil c = secondsToCivil(now + (int64_t)tz * 60);
      snprintf(today, sizeof(today), "%04d-%02d-%02d", c.year, c.month, c.day);
    }
    String h = "<fieldset><legend>Add an event</legend><form method=POST action=/meet/add>"
               "<label>Title</label><input name=title maxlength=60 required>"
               "<div class=row><div><label>Date</label><input name=date type=date value=";
    h += today;
    h += " required></div><div><label>Starts</label><input name=start type=time></div>"
         "<div><label>Ends</label><input name=end type=time></div></div>"
         "<div class=row><div><label>Leave early (min)</label><input name=leave type=number min=0 max=240 value=0></div>"
         "<div><label>Where</label><input name=loc maxlength=40></div></div>"
         "<p class=muted>No start time = all-day. No end time = 30 minutes.</p>"
         "<button>Add event</button></form></fieldset>";
    send(h);
  }

  // ---- import ----
  send("<fieldset><legend>Import an .ics file</legend>"
       "<form method=POST action=/meet/import enctype=multipart/form-data>"
       "<input type=file name=ics accept='.ics,text/calendar' required>"
       "<button class=sec>Import (replaces earlier imports and USB events)</button></form>"
       "<p class=muted>Recurring events, time zones and exceptions are handled like a feed; the next 7 "
       "days are kept (up to 16 events).</p></fieldset>");

  // ---- options ----
  {
    send("<fieldset><legend>Alerts &amp; watch face</legend><form method=POST action=/meet/settings>");
    String h = "<div class=chk><input type=checkbox id=al name=alerts";
    if (set.alertsOn) h += " checked";
    h += "><label for=al>Buzz before events (also when the watch is asleep)</label></div><label>Remind me</label><div class=offs>";
    for (int b = 0; b < kNumAlertOffsets; b++) {
      h += "<div class=chk><input type=checkbox id=o"; h += b; h += " name=o"; h += b;
      if (set.alertMask & (1u << b)) h += " checked";
      h += "><label for=o"; h += b; h += ">";
      if (kAlertOffsets[b] == 0) h += "at start";
      else { h += kAlertOffsets[b]; h += " min"; }
      h += "</label></div>";
    }
    h += "</div>";
    send(h);
    static const int snoozeV[] = {1, 2, 3, 5, 10};
    static const char *const snoozeL[] = {"1 min", "2 min", "3 min", "5 min", "10 min"};
    send("<div class=row><div><label>Snooze</label>");
    sendSelect("snooze", snoozeV, snoozeL, 5, set.snoozeMin);
    static const int leadV[] = {30, 45, 60, 90, 120};
    static const char *const leadL[] = {"30 min", "45 min", "60 min", "90 min", "2 hours"};
    send("</div><div><label>Ring window</label>");
    sendSelect("lead", leadV, leadL, 5, set.leadMin);
    send("</div></div>");
#if defined(MC_BACKGROUND_SYNC) && MC_BACKGROUND_SYNC
    static const int syncV[] = {0, 15, 30, 60, 120};
    static const char *const syncL[] = {"off (manual)", "every 15 min", "every 30 min", "every hour", "every 2 hours"};
    send("<label>Background sync while asleep</label>");
    sendSelect("sync", syncV, syncL, 5, set.syncMin);
#endif
    h = "<div class=chk><input type=checkbox id=np name=night";
    if (set.nightPause) h += " checked";
    h += "><label for=np>Pause background sync 23:00&ndash;06:00</label></div>"
         "<div class=chk><input type=checkbox id=tz name=autotz";
    if (set.autoTz) h += " checked";
    h += "><label for=tz>Follow my calendar&rsquo;s time zone (daylight saving)</label></div>"
         "<div class=chk><input type=checkbox id=in name=insecure";
    if (set.insecureTls) h += " checked";
    h += "><label for=in>Accept untrusted HTTPS certificates (self-hosted calendars only)</label></div>";
    send(h);
    static const int faceV[] = {0, 1};
    static const char *const faceL[] = {"Meeting ring", "Classic BaseOS face"};
    send("<label>Watch face</label>");
    sendSelect("face", faceV, faceL, 2, set.faceClassic);
    send("<button>Save</button></form></fieldset>");
  }

  // ---- clock ----
  {
    char off[24], wt[40] = "unknown";
    snprintf(off, sizeof(off), "UTC%+03d:%02d", tz / 60, abs(tz % 60));
    if (haveNow) formatIsoLocal(now, tz, wt, sizeof(wt));
    String h = "<fieldset><legend>Watch clock</legend><p>Watch: <b>";
    h += haveNow ? String(wt).substring(11, 16) : String("--:--");
    h += "</b> ";
    h += off;
    h += "<br>This phone: <b id=pt>&hellip;</b> <span id=pz></span></p><p id=cw class=muted></p>";
    if (si.calTzResolved && si.calTz[0]) {
      char coff[24];
      snprintf(coff, sizeof(coff), "UTC%+03d:%02d", si.calTzOffset / 60, abs(si.calTzOffset % 60));
      h += "<p class=muted>Calendar time zone: ";
      h += esc(si.calTz);
      h += " ("; h += coff; h += ")";
      if (si.calTzOffset != tz) {
        int d = si.calTzOffset - tz;
        h += d >= -60 && d <= 60 ? " &middot; the watch follows it at the next sync"
                                 : " &middot; differs from the watch: if you live there, use the button below";
      }
      h += "</p>";
    }
    h += "<form method=POST action=/meet/clock id=cf><input type=hidden name=utc id=cu>"
         "<input type=hidden name=tz id=cz><button class=sec>Set watch time &amp; zone from this phone</button></form>"
         "<p class=muted>Alerts are only as good as the watch clock. Each sync also sets it from the internet.</p>"
         "</fieldset><script>(function(){var w=";
    h += haveNow ? String((long)(now - (int64_t)946684800)) : String("null");
    h += ",wz=";
    h += tz;
    h += ",ld=Date.now()/1000;function f(n){return(n<10?'0':'')+n}function t(){var d=new Date();"
         "document.getElementById('pt').textContent=f(d.getHours())+':'+f(d.getMinutes())+':'+f(d.getSeconds());"
         "var z=-d.getTimezoneOffset();document.getElementById('pz').textContent='UTC'+(z<0?'-':'+')+f(Math.floor(Math.abs(z)/60))+':'+f(Math.abs(z)%60);"
         "document.getElementById('cu').value=Math.floor(d.getTime()/1000);document.getElementById('cz').value=z;"
         "if(w!==null){var diff=Math.round((ld-946684800-w)/60);var m='';"
         "if(z!==wz)m='The watch uses a different time zone from this phone.';"
         "else if(Math.abs(diff)>2)m='The watch clock is '+Math.abs(diff)+' min '+(diff>0?'behind':'ahead')+'.';"
         "document.getElementById('cw').textContent=m;}}t();setInterval(t,1000)})()</script>";
    send(h);
  }
  send("<p><a href=/>&larr; Watch settings</a></p>");
  send("");
}

static void handleClock() {
  mcapp::keepAwakeFor(120000);
  long long utc = atoll(gSrv->arg("utc").c_str());
  long tzMin = gSrv->arg("tz").toInt();
  // Sanity: a 2024..2100 clock and a real-world offset.
  if (utc < 1704067200LL || utc > 4102444800LL || tzMin < -720 || tzMin > 840) {
    redirectMsg("err", "That did not look like a valid time.");
    return;
  }
  { ModelLock lk; model.tzOffsetMin = (int16_t)tzMin; model.revision++; }
  Storage::save();
  Civil c = secondsToCivil((int64_t)utc + (int64_t)tzMin * 60);
  requestSetRTC((uint8_t)c.hour, (uint8_t)c.minute, (uint8_t)c.second, (uint8_t)c.weekday,
                (uint8_t)c.day, (uint8_t)c.month, (uint16_t)c.year);
  // taskIO writes the RTC on its next cycle and reads it back every 80 ms:
  // re-plan alerts once the model holds the new time.
  vTaskDelay(pdMS_TO_TICKS(200));
  mcapp::armAwakeTimer();
  redirectMsg("msg", "Watch time and time zone set from this phone.");
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
static void handleFeeds() {
  mcapp::keepAwakeFor(120000);
  int changed = 0;
  for (int i = 0; i < mcapp::kMaxFeeds; i++) {
    String rm = "rm";
    rm += i;
    String f = "f";
    f += i;
    if (gSrv->hasArg(rm)) { mcapp::setFeedUrl(i, ""); changed++; continue; }
    String v = gSrv->arg(f);
    v.trim();
    if (!v.length()) continue;
    Url u;
    if (v.length() >= mcapp::kUrlMax || !parseUrl(v.c_str(), u)) {
      redirectMsg("err", "That address doesn't look right: it should start with https:// or webcal://");
      return;
    }
    mcapp::setFeedUrl(i, v.c_str());
    changed++;
  }
  if (!changed) { redirect(""); return; }
  bool conn;
  WifiMode mode;
  { ModelLock lk; conn = model.wifiConnected; mode = model.wifiMode; }
  if (conn && mode == WifiMode::Client && mcsync::startAsync()) { redirect(""); return; }
  redirectMsg("msg", Storage::knownCount() ? "Saved. The watch syncs as soon as it goes to sleep."
                                           : "Saved. Add your WiFi network on the main settings page so the watch can sync.");
}

static void handleSettings() {
  mcapp::keepAwakeFor(120000);
  mcapp::Settings s = mcapp::settings();
  s.alertsOn = gSrv->hasArg("alerts");
  uint8_t mask = 0;
  for (int b = 0; b < kNumAlertOffsets; b++) {
    String k = "o";
    k += b;
    if (gSrv->hasArg(k)) mask |= (uint8_t)(1u << b);
  }
  s.alertMask = mask ? mask : kAlertDefaultMask;
  if (!mask) s.alertsOn = 0;
  int v = gSrv->arg("snooze").toInt();
  if (v >= 1 && v <= 30) s.snoozeMin = (uint8_t)v;
  v = gSrv->arg("lead").toInt();
  if (v >= 15 && v <= 180) s.leadMin = (uint8_t)v;
  if (gSrv->hasArg("sync")) {
    v = gSrv->arg("sync").toInt();
    if (v >= 0 && v <= 240) s.syncMin = (uint8_t)v;
  }
  s.nightPause = gSrv->hasArg("night");
  s.autoTz = gSrv->hasArg("autotz");
  s.insecureTls = gSrv->hasArg("insecure");
  s.faceClassic = gSrv->arg("face").toInt() == 1;
  mcapp::setSettings(s);
  { ModelLock lk; model.revision++; }
  redirectMsg("msg", "Saved.");
}

static void handleAdd() {
  mcapp::keepAwakeFor(120000);
  Event e;
  const char *err = "";
  static uint32_t salt = 0;
  salt += (uint32_t)millis() | 1u;
  bool ok = makeEvent(gSrv->arg("title").c_str(), gSrv->arg("date").c_str(), gSrv->arg("start").c_str(),
                      gSrv->arg("end").c_str(), 0, (int)gSrv->arg("leave").toInt(), gSrv->arg("loc").c_str(),
                      mcapp::tzOffsetMin(), Source::Manual, salt, e, &err);
  if (ok) ok = mcapp::addEvent(e, &err);
  if (!ok) { redirectMsg("err", err && *err ? err : "could not add the event"); return; }
  { ModelLock lk; model.revision++; }
  redirectMsg("msg", "Added.");
}

static void handleDel() {
  mcapp::keepAwakeFor(120000);
  uint32_t key = (uint32_t)strtoul(gSrv->arg("k").c_str(), nullptr, 16);
  bool ok = mcapp::deleteEvent(key);
  { ModelLock lk; model.revision++; }
  redirectMsg(ok ? "msg" : "err", ok ? "Deleted." : "That event is already gone.");
}

static void handleSync() {
  mcapp::keepAwakeFor(120000);
  if (mcapp::feedCount() == 0) { redirectMsg("err", "Add a calendar feed first."); return; }
  if (Storage::knownCount() == 0) { redirectMsg("err", "Save your WiFi network first (main settings page)."); return; }
  bool ap;
  { ModelLock lk; ap = model.wifiEnabled && model.wifiMode == WifiMode::AP; }
  if (ap) {
    // Syncing needs the radio in client mode, which would drop this page.
    mcapp::requestSync();
    redirectMsg("msg", "The watch will sync as soon as it goes to sleep (the setup hotspot needs the radio until then).");
    return;
  }
  if (!mcsync::startAsync() && !mcsync::running()) { redirectMsg("err", "Could not start a sync."); return; }
  redirect("");
}

static void handleStatus() {
  mcapp::keepAwakeFor(60000);
  mcsync::Progress p = mcsync::progress();
  int64_t now = 0;
  mcapp::nowUtc(now);
  char line[64];
  bool warn;
  mcapp::statusLine(now, line, sizeof(line), warn);
  mcapp::SyncInfo si = mcapp::syncInfo();
  String j = "{\"syncing\":";
  j += p.running ? "true" : "false";
  j += ",\"step\":\"";
  j += esc(p.step);
  j += "\",\"status\":\"";
  j += p.running ? esc(p.step) : (line[0] ? esc(line) : String(mcapp::syncResultText(si.last)));
  j += "\",\"result\":\"";
  j += mcapp::syncResultText(si.last);
  j += "\"}";
  gSrv->send(200, "application/json", j);
}

static void handleEventsJson() {
  int64_t now = 0;
  mcapp::nowUtc(now);
  int tz = mcapp::tzOffsetMin();
  Event *gList = listBuf();
  int n = gList ? mcapp::snapshot(gList, mcapp::kMaxMerged) : 0;
  gSrv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  gSrv->send(200, "application/json", "");
  send("{\"events\":[");
  for (int i = 0; i < n; i++) {
    const Event &e = gList[i];
    char a[32], b[32];
    if (isAllDay(e)) {
      Civil c1 = secondsToCivil(e.start), c2 = secondsToCivil(e.end);
      snprintf(a, sizeof(a), "%04d-%02d-%02d", c1.year, c1.month, c1.day);
      snprintf(b, sizeof(b), "%04d-%02d-%02d", c2.year, c2.month, c2.day);
    } else {
      formatIsoLocal(e.start, tz, a, sizeof(a));
      formatIsoLocal(e.end, tz, b, sizeof(b));
    }
    char key[12];
    snprintf(key, sizeof(key), "%08lx", (unsigned long)e.key);
    String h = i ? "," : "";
    h += "{\"id\":\""; h += key; h += "\",\"start\":\""; h += a; h += "\",\"end\":\""; h += b;
    h += "\",\"allDay\":"; h += isAllDay(e) ? "true" : "false";
    h += ",\"title\":\""; h += esc(e.title); h += "\",\"location\":\""; h += esc(e.location);
    h += "\",\"source\":\"";
    h += e.source == (uint8_t)Source::Feed ? "feed" : e.source == (uint8_t)Source::Manual ? "web" : "usb";
    h += "\"}";
    send(h);
  }
  send("]}");
  send("");
}

// ---- .ics upload ----
static IcsParser *gUp = nullptr;
static bool gUpOk = false;
static int gUpCount = 0;

static void handleImportChunk() {
  mcapp::keepAwakeFor(120000);
  esp_task_wdt_reset();
  HTTPUpload &up = gSrv->upload();
  if (up.status == UPLOAD_FILE_START) {
    gUpOk = false;
    gUpCount = 0;
    if (!gUp) {
      void *mem = heap_caps_malloc(sizeof(IcsParser), MALLOC_CAP_SPIRAM);
      if (!mem) mem = malloc(sizeof(IcsParser));
      if (mem) gUp = new (mem) IcsParser();
    }
    if (!gUp) return;
    int64_t now = 0;
    mcapp::nowUtc(now);
    IcsOptions o;
    o.windowStart = now;
    o.windowEnd = now + 7 * 86400;
    o.fallbackOffsetMin = mcapp::tzOffsetMin();
    o.source = (uint8_t)Source::Pushed;
    o.maxEvents = mcapp::kMaxPushed;
    o.maxAllDay = 4;
    gUp->begin(o);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (gUp) gUp->feed((const char *)up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (!gUp) return;
    gUp->finish();
    if (gUp->stats().sawCalendar) {
      // The parser's own output array is contiguous: hand it over directly.
      int n = gUp->count() > mcapp::kMaxPushed ? mcapp::kMaxPushed : gUp->count();
      mcapp::replaceSource(Source::Pushed, n ? &gUp->event(0) : nullptr, n);
      gUpOk = true;
      gUpCount = n;
    }
  }
}

static void handleImportDone() {
  { ModelLock lk; model.revision++; }
  if (!gUpOk) { redirectMsg("err", "That file is not an iCalendar (.ics) file."); return; }
  char m[64];
  snprintf(m, sizeof(m), "Imported %d event%s.", gUpCount, gUpCount == 1 ? "" : "s");
  redirectMsg("msg", m);
}

void mcWebRegister(WebServer &server) {
  gSrv = &server;
  server.on("/meet", HTTP_GET, handlePage);
  server.on("/meet/feeds", HTTP_POST, handleFeeds);
  server.on("/meet/settings", HTTP_POST, handleSettings);
  server.on("/meet/add", HTTP_POST, handleAdd);
  server.on("/meet/del", HTTP_POST, handleDel);
  server.on("/meet/sync", HTTP_POST, handleSync);
  server.on("/meet/status.json", HTTP_GET, handleStatus);
  server.on("/meet/events.json", HTTP_GET, handleEventsJson);
  server.on("/meet/import", HTTP_POST, handleImportDone, handleImportChunk);
  server.on("/meet/clock", HTTP_POST, handleClock);
}

#endif  // EWATCH_ENABLE_WIFI
