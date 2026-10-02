#include "ics_parser.h"
#include "mc_text.h"
#include <string.h>
#include <stdlib.h>

namespace mc {

// ===========================================================================
// small helpers
// ===========================================================================
static char upperc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static bool eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static int wdFromCode(const char *s, size_t n) {
  static const char *k[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
  if (n < 2) return -1;
  for (int i = 0; i < 7; i++)
    if (upperc(s[0]) == k[i][0] && upperc(s[1]) == k[i][1]) return i;
  return -1;
}

static bool parseIntTok(const char *s, size_t n, int &out) {
  if (n == 0) return false;
  size_t i = 0;
  int sign = 1;
  if (s[0] == '+' || s[0] == '-') { if (s[0] == '-') sign = -1; i++; }
  if (i >= n) return false;
  long v = 0;
  for (; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
    if (v > 1000000) return false;
  }
  out = (int)(sign * v);
  return true;
}

// Calls fn(token, len) for each comma-separated token.
template <typename F>
static void forEachToken(const char *s, size_t n, F fn) {
  size_t start = 0;
  for (size_t i = 0; i <= n; i++) {
    if (i == n || s[i] == ',') {
      if (i > start) fn(s + start, i - start);
      start = i + 1;
    }
  }
}

static bool isUtcName(const char *tz) {
  static const char *k[] = {"UTC", "ETC/UTC", "GMT", "ETC/GMT", "Z", "ZULU", "UNIVERSAL",
                            "ETC/UNIVERSAL", "COORDINATED UNIVERSAL TIME", "UTC+00:00"};
  for (const char *n : k) {
    size_t l = strlen(n);
    if (strlen(tz) == l && equalsNoCase(tz, n, l)) return true;
  }
  return false;
}

// Day-of-month of the ord-th weekday wd in (y, m); ord<0 counts from the end.
// Returns 0 if it does not exist.
static int nthWeekdayOfMonth(int y, int m, int ord, int wd) {
  int dim = daysInMonth(y, m);
  if (ord > 0) {
    int w1 = weekdayFromDays(daysFromCivil(y, m, 1));
    int d = 1 + ((wd - w1 + 7) % 7) + (ord - 1) * 7;
    return d <= dim ? d : 0;
  }
  if (ord < 0) {
    int wl = weekdayFromDays(daysFromCivil(y, m, dim));
    int d = dim - ((wl - wd + 7) % 7) - (-ord - 1) * 7;
    return d >= 1 ? d : 0;
  }
  return 0;
}

// ===========================================================================
// RRULE
// ===========================================================================
bool parseRRule(const char *s, RRule &r) {
  r = RRule();
  bool haveFreq = false;
  const char *p = s;
  while (*p) {
    const char *semi = strchr(p, ';');
    size_t len = semi ? (size_t)(semi - p) : strlen(p);
    const char *eqp = (const char *)memchr(p, '=', len);
    if (eqp) {
      size_t klen = (size_t)(eqp - p);
      const char *v = eqp + 1;
      size_t vlen = len - klen - 1;
      char key[16];
      size_t kl = klen < sizeof(key) - 1 ? klen : sizeof(key) - 1;
      for (size_t i = 0; i < kl; i++) key[i] = upperc(p[i]);
      key[kl] = '\0';
      if (eq(key, "FREQ")) {
        haveFreq = true;
        if (vlen == 5 && equalsNoCase(v, "DAILY", 5)) r.freq = 1;
        else if (vlen == 6 && equalsNoCase(v, "WEEKLY", 6)) r.freq = 2;
        else if (vlen == 7 && equalsNoCase(v, "MONTHLY", 7)) r.freq = 3;
        else if (vlen == 6 && equalsNoCase(v, "YEARLY", 6)) r.freq = 4;
        else r.unsupported = true;                       // HOURLY, MINUTELY, ...
      } else if (eq(key, "INTERVAL")) {
        int iv;
        if (parseIntTok(v, vlen, iv) && iv >= 1) r.interval = iv > 1000 ? 1000 : iv;
      } else if (eq(key, "COUNT")) {
        int c;
        if (parseIntTok(v, vlen, c) && c >= 1) r.count = c > 100000 ? 100000 : c;
      } else if (eq(key, "UNTIL")) {
        r.hasUntil = parseIcsDateTime(v, vlen, r.until);
      } else if (eq(key, "WKST")) {
        int wd = wdFromCode(v, vlen);
        if (wd >= 0) r.wkst = (uint8_t)wd;
      } else if (eq(key, "BYDAY")) {
        forEachToken(v, vlen, [&](const char *t, size_t tl) {
          if (tl < 2) return;
          int wd = wdFromCode(t + tl - 2, 2);
          if (wd < 0) { r.unsupported = true; return; }
          int ord = 0;
          if (tl > 2 && !parseIntTok(t, tl - 2, ord)) { r.unsupported = true; return; }
          if (ord == 0) {
            r.byDayMask |= (uint8_t)(1u << wd);
          } else if (r.nByDayOrd < 8) {
            r.byDayOrd[r.nByDayOrd] = (int8_t)ord;
            r.byDayOrdWd[r.nByDayOrd] = (int8_t)wd;
            r.nByDayOrd++;
          }
        });
      } else if (eq(key, "BYMONTHDAY")) {
        forEachToken(v, vlen, [&](const char *t, size_t tl) {
          int d;
          if (parseIntTok(t, tl, d) && d != 0 && d >= -31 && d <= 31 && r.nByMonthDay < 8)
            r.byMonthDay[r.nByMonthDay++] = (int8_t)d;
        });
      } else if (eq(key, "BYMONTH")) {
        forEachToken(v, vlen, [&](const char *t, size_t tl) {
          int m;
          if (parseIntTok(t, tl, m) && m >= 1 && m <= 12) r.byMonthMask |= (uint16_t)(1u << m);
        });
      } else if (eq(key, "BYSETPOS")) {
        forEachToken(v, vlen, [&](const char *t, size_t tl) {
          int sp;
          if (parseIntTok(t, tl, sp) && sp != 0 && r.nBySetPos < 4) r.bySetPos[r.nBySetPos++] = (int16_t)sp;
        });
      } else if (eq(key, "BYWEEKNO") || eq(key, "BYYEARDAY") || eq(key, "BYHOUR") ||
                 eq(key, "BYMINUTE") || eq(key, "BYSECOND")) {
        r.unsupported = true;
      }
    }
    if (!semi) break;
    p = semi + 1;
  }
  // Ordinals only make sense for MONTHLY/YEARLY; elsewhere treat as plain days.
  if (r.freq == 1 || r.freq == 2) {
    for (int i = 0; i < r.nByDayOrd; i++) r.byDayMask |= (uint8_t)(1u << r.byDayOrdWd[i]);
    r.nByDayOrd = 0;
  }
  // YEARLY with an ordinal BYDAY but no BYMONTH means "Nth weekday of the
  // year": not supported.
  if (r.freq == 4 && r.nByDayOrd && !r.byMonthMask) r.unsupported = true;
  return haveFreq && r.freq != 0;
}

// Days (day numbers, ascending) of one recurrence period.
struct PeriodCtx {
  const RRule *r;
  int64_t d0, w0;
  int y0, m0, md0;
};

// Days of month (y, m) selected by BYMONTHDAY/BYDAY (or the DTSTART day).
static int monthDays(const RRule &r, int y, int m, int md0, int64_t *out, int cap) {
  int dim = daysInMonth(y, m);
  uint32_t setMD = 0, setBD = 0;
  bool haveMD = r.nByMonthDay > 0, haveBD = r.nByDayOrd > 0 || r.byDayMask;
  for (int i = 0; i < r.nByMonthDay; i++) {
    int d = r.byMonthDay[i] > 0 ? r.byMonthDay[i] : dim + r.byMonthDay[i] + 1;
    if (d >= 1 && d <= dim) setMD |= 1u << d;
  }
  if (haveBD) {
    for (int i = 0; i < r.nByDayOrd; i++) {
      int d = nthWeekdayOfMonth(y, m, r.byDayOrd[i], r.byDayOrdWd[i]);
      if (d) setBD |= 1u << d;
    }
    if (r.byDayMask) {
      int w1 = weekdayFromDays(daysFromCivil(y, m, 1));
      for (int d = 1; d <= dim; d++)
        if (r.byDayMask & (1u << ((w1 + d - 1) % 7))) setBD |= 1u << d;
    }
  }
  uint32_t set;
  if (haveMD && haveBD) set = setMD & setBD;
  else if (haveMD) set = setMD;
  else if (haveBD) set = setBD;
  else set = (md0 <= dim) ? (1u << md0) : 0;
  int n = 0;
  int64_t base = daysFromCivil(y, m, 1);
  for (int d = 1; d <= dim && n < cap; d++)
    if (set & (1u << d)) out[n++] = base + d - 1;
  return n;
}

static void applySetPos(const RRule &r, int64_t *days, int &n) {
  if (!r.nBySetPos || n == 0) return;
  int64_t keep[8];
  int k = 0;
  for (int i = 0; i < r.nBySetPos && k < 8; i++) {
    int sp = r.bySetPos[i];
    int idx = sp > 0 ? sp - 1 : n + sp;
    if (idx >= 0 && idx < n) keep[k++] = days[idx];
  }
  // sort + dedupe
  for (int i = 1; i < k; i++) {
    int64_t t = keep[i];
    int j = i - 1;
    while (j >= 0 && keep[j] > t) { keep[j + 1] = keep[j]; j--; }
    keep[j + 1] = t;
  }
  n = 0;
  for (int i = 0; i < k; i++)
    if (n == 0 || days[n - 1] != keep[i]) days[n++] = keep[i];
}

// Returns the number of days in period k; also reports the period's first day.
static int periodDays(const PeriodCtx &c, int64_t k, int64_t *out, int cap, int64_t &periodStart) {
  const RRule &r = *c.r;
  int n = 0;
  switch (r.freq) {
    case 1: {  // DAILY
      int64_t d = c.d0 + k * r.interval;
      periodStart = d;
      int y, m, md;
      civilFromDays(d, y, m, md);
      bool ok = true;
      if (r.byDayMask && !(r.byDayMask & (1u << weekdayFromDays(d)))) ok = false;
      if (r.byMonthMask && !(r.byMonthMask & (1u << m))) ok = false;
      if (ok && r.nByMonthDay) {
        int dim = daysInMonth(y, m);
        bool hit = false;
        for (int i = 0; i < r.nByMonthDay; i++) {
          int want = r.byMonthDay[i] > 0 ? r.byMonthDay[i] : dim + r.byMonthDay[i] + 1;
          if (want == md) hit = true;
        }
        ok = hit;
      }
      if (ok && cap > 0) out[n++] = d;
      break;
    }
    case 2: {  // WEEKLY
      int64_t ws = c.w0 + k * 7 * r.interval;
      periodStart = ws;
      uint8_t mask = r.byDayMask ? r.byDayMask : (uint8_t)(1u << weekdayFromDays(c.d0));
      for (int i = 0; i < 7 && n < cap; i++) {
        int64_t d = ws + i;
        if (!(mask & (1u << weekdayFromDays(d)))) continue;
        if (r.byMonthMask) {
          int y, m, md;
          civilFromDays(d, y, m, md);
          if (!(r.byMonthMask & (1u << m))) continue;
        }
        out[n++] = d;
      }
      break;
    }
    case 3: {  // MONTHLY
      int64_t total = (int64_t)c.y0 * 12 + (c.m0 - 1) + k * r.interval;
      int y = (int)floorDiv(total, 12), m = (int)floorMod(total, 12) + 1;
      periodStart = daysFromCivil(y, m, 1);
      if (r.byMonthMask && !(r.byMonthMask & (1u << m))) break;
      n = monthDays(r, y, m, c.md0, out, cap);
      break;
    }
    case 4: {  // YEARLY
      int y = c.y0 + (int)(k * r.interval);
      periodStart = daysFromCivil(y, 1, 1);
      bool plain = !r.nByMonthDay && !r.nByDayOrd && !r.byDayMask;
      for (int m = 1; m <= 12 && n < cap; m++) {
        bool monthOn = r.byMonthMask ? (r.byMonthMask & (1u << m)) != 0 : (m == c.m0);
        if (!monthOn) continue;
        if (plain) {
          if (c.md0 <= daysInMonth(y, m)) out[n++] = daysFromCivil(y, m, c.md0);
        } else {
          n += monthDays(r, y, m, c.md0, out + n, cap - n);
        }
      }
      break;
    }
    default:
      periodStart = c.d0;
      break;
  }
  applySetPos(r, out, n);
  return n;
}

// ===========================================================================
// parser lifecycle
// ===========================================================================
void IcsParser::begin(const IcsOptions &opt) {
  opt_ = opt;
  if (opt_.maxEvents > kMaxOut) opt_.maxEvents = kMaxOut;
  if (opt_.maxEvents < 1) opt_.maxEvents = 1;
  if (opt_.maxAllDay < 0) opt_.maxAllDay = 0;
  stats_ = IcsStats();
  lst_ = L_NAME;
  prop_ = P_IGNORE;
  nlPending_ = crSeen_ = inQuote_ = false;
  bomSkip_ = 0;
  nameLen_ = pnameLen_ = pvalLen_ = 0;
  valLen_ = 0;
  valOverflow_ = false;
  lineHasContent_ = false;
  tzParam_[0] = valueParam_[0] = '\0';
  depth_ = 0;
  deepOverflow_ = 0;
  nZones_ = 0;
  ocurValid_ = false;
  nCand_ = nOvr_ = nOut_ = 0;
  resetCur();
}

void IcsParser::resetCur() {
  memset((void *)&cur_, 0, sizeof(cur_));
}

void IcsParser::feed(const char *data, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char c = data[i];
    stats_.bytes++;
    // Skip a UTF-8 byte-order mark at the very start of the stream.
    if (stats_.bytes <= 3) {
      static const char kBom[3] = {(char)0xEF, (char)0xBB, (char)0xBF};
      if (c == kBom[stats_.bytes - 1] && bomSkip_ == stats_.bytes - 1) { bomSkip_++; continue; }
    }
    if (c == '\r') { nlPending_ = true; crSeen_ = true; continue; }
    if (c == '\n') {
      if (crSeen_) { crSeen_ = false; continue; }
      nlPending_ = true;
      continue;
    }
    crSeen_ = false;
    if (nlPending_) {
      nlPending_ = false;
      if (c == ' ' || c == '\t') continue;      // folded continuation line
      endLine();
    }
    lexChar(c);
  }
}

void IcsParser::finish() {
  if (lineHasContent_) endLine();
  // A truncated stream may leave an event open; finalize what we have.
  while (depth_ > 0) endComp();

  // Drop master instances replaced or cancelled by a RECURRENCE-ID override.
  int w = 0;
  for (int i = 0; i < nCand_; i++) {
    bool kill = false;
    if (!cand_[i].isOverride) {
      for (int j = 0; j < nOvr_; j++) {
        if (ovr_[j].uid == cand_[i].ev.uid && ovr_[j].orig == cand_[i].orig) { kill = true; break; }
      }
    }
    if (!kill) cand_[w++] = cand_[i];
  }
  nCand_ = w;

  // Order candidates by start (all-day starts are wall values; the error of
  // mixing scales is at most the UTC offset, which is fine for ranking).
  for (int i = 1; i < nCand_; i++) {
    Cand t = cand_[i];
    int j = i - 1;
    while (j >= 0 && eventLess(t.ev, cand_[j].ev)) { cand_[j + 1] = cand_[j]; j--; }
    cand_[j + 1] = t;
  }
  int allDayTotal = 0;
  for (int i = 0; i < nCand_; i++) if (isAllDay(cand_[i].ev)) allDayTotal++;
  int allDayKeep = allDayTotal < opt_.maxAllDay ? allDayTotal : opt_.maxAllDay;
  int timedKeep = opt_.maxEvents - allDayKeep;
  int nAll = 0, nTimed = 0;
  nOut_ = 0;
  for (int i = 0; i < nCand_ && nOut_ < opt_.maxEvents; i++) {
    const Event &e = cand_[i].ev;
    if (isAllDay(e)) { if (nAll >= allDayKeep) continue; nAll++; }
    else { if (nTimed >= timedKeep) continue; nTimed++; }
    out_[nOut_++] = e;
  }
  sortEvents(out_, nOut_);

  if (stats_.calTz[0]) {
    int off;
    if (zoneOffsetAt(stats_.calTz, opt_.windowStart, off)) {
      stats_.calTzResolved = true;
      stats_.calTzOffsetMin = off;
    }
  }
  stats_.zones = nZones_;
}

// ===========================================================================
// lexer
// ===========================================================================
void IcsParser::lexChar(char c) {
  lineHasContent_ = true;
  switch (lst_) {
    case L_NAME:
      if (c == ':') { name_[nameLen_] = '\0'; identify(); lst_ = L_VALUE; }
      else if (c == ';') { name_[nameLen_] = '\0'; identify(); lst_ = L_PNAME; pnameLen_ = 0; }
      else if (nameLen_ < sizeof(name_) - 1) name_[nameLen_++] = upperc(c);
      break;
    case L_PNAME:
      if (c == '=') { pname_[pnameLen_] = '\0'; lst_ = L_PVAL; pvalLen_ = 0; inQuote_ = false; }
      else if (c == ':') { lst_ = L_VALUE; }
      else if (c == ';') { pnameLen_ = 0; }
      else if (pnameLen_ < sizeof(pname_) - 1) pname_[pnameLen_++] = upperc(c);
      break;
    case L_PVAL:
      if (c == '"') { inQuote_ = !inQuote_; break; }
      if (inQuote_) {
        if (pvalLen_ < sizeof(pval_) - 1) pval_[pvalLen_++] = c;
        break;
      }
      if (c == ';') { commitParam(); lst_ = L_PNAME; pnameLen_ = 0; }
      else if (c == ':') { commitParam(); lst_ = L_VALUE; }
      else if (c == ',') { commitParam(); pvalLen_ = 0; }
      else if (pvalLen_ < sizeof(pval_) - 1) pval_[pvalLen_++] = c;
      break;
    case L_VALUE:
      switch (prop_) {
        case P_IGNORE:
          return;
        case P_UID:
          cur_.uid = fnv1aByte((uint8_t)c, cur_.uid);
          return;
        case P_EXDATE:
        case P_RDATE:
        case P_OBS_RDATE:
          if (c == ',') { listItem(); return; }
          // fall through - accumulate the current list item
        default:
          if (valLen_ < sizeof(val_) - 1) {
            val_[valLen_++] = c;
          } else if (!valOverflow_) {
            valOverflow_ = true;
            stats_.truncated++;
          }
          return;
      }
  }
}

void IcsParser::commitParam() {
  pval_[pvalLen_] = '\0';
  if (eq(pname_, "TZID")) {
    // Only the first value of a (malformed) multi-valued TZID is used.
    if (!tzParam_[0]) copyStr(tzParam_, sizeof(tzParam_), pval_);
  } else if (eq(pname_, "VALUE")) {
    copyStr(valueParam_, sizeof(valueParam_), pval_);
  }
  pvalLen_ = 0;
}

void IcsParser::identify() {
  Comp top = depth_ ? comp_[depth_ - 1] : C_NONE;
  const char *n = name_;
  prop_ = P_IGNORE;
  if (eq(n, "BEGIN")) { prop_ = P_BEGIN; return; }
  if (eq(n, "END"))   { prop_ = P_END; return; }
  if (deepOverflow_) return;
  switch (top) {
    case C_EVENT:
      if      (eq(n, "SUMMARY"))       prop_ = P_SUMMARY;
      else if (eq(n, "LOCATION"))      prop_ = P_LOCATION;
      else if (eq(n, "DTSTART"))       prop_ = P_DTSTART;
      else if (eq(n, "DTEND"))         prop_ = P_DTEND;
      else if (eq(n, "DURATION"))      prop_ = P_DURATION;
      else if (eq(n, "RRULE"))         prop_ = P_RRULE;
      else if (eq(n, "EXDATE"))        prop_ = P_EXDATE;
      else if (eq(n, "RDATE"))         prop_ = P_RDATE;
      else if (eq(n, "RECURRENCE-ID")) prop_ = P_RECURID;
      else if (eq(n, "UID"))           { prop_ = P_UID; cur_.uid = kFnvSeed; cur_.hasUid = true; }
      else if (eq(n, "STATUS"))        prop_ = P_STATUS;
      else if (eq(n, "X-APPLE-TRAVEL-DURATION"))      prop_ = P_TRAVEL;
      else if (eq(n, "X-MICROSOFT-CDO-ALLDAYEVENT"))  prop_ = P_MSALLDAY;
      break;
    case C_TZ:
      if (eq(n, "TZID")) prop_ = P_TZID;
      break;
    case C_TZOBS:
      if      (eq(n, "DTSTART"))      prop_ = P_OBS_DTSTART;
      else if (eq(n, "TZOFFSETFROM")) prop_ = P_OBS_FROM;
      else if (eq(n, "TZOFFSETTO"))   prop_ = P_OBS_TO;
      else if (eq(n, "RRULE"))        prop_ = P_OBS_RRULE;
      else if (eq(n, "RDATE"))        prop_ = P_OBS_RDATE;
      break;
    case C_CAL:
      if      (eq(n, "X-WR-CALNAME"))  prop_ = P_CALNAME;
      else if (eq(n, "X-WR-TIMEZONE")) prop_ = P_CALTZ;
      break;
    default:
      break;
  }
}

void IcsParser::endLine() {
  if (lst_ == L_VALUE) dispatch();
  if (lineHasContent_) stats_.lines++;
  lst_ = L_NAME;
  prop_ = P_IGNORE;
  nameLen_ = pnameLen_ = pvalLen_ = 0;
  valLen_ = 0;
  valOverflow_ = false;
  inQuote_ = false;
  tzParam_[0] = valueParam_[0] = '\0';
  lineHasContent_ = false;
}

static size_t trimInPlace(char *s, size_t n) {
  size_t a = 0;
  while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
  while (n > a && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
  if (a) memmove(s, s + a, n - a);
  s[n - a] = '\0';
  return n - a;
}

void IcsParser::dispatch() {
  val_[valLen_] = '\0';
  switch (prop_) {
    case P_BEGIN: {
      size_t n = trimInPlace(val_, valLen_);
      for (size_t i = 0; i < n; i++) val_[i] = upperc(val_[i]);
      beginComp(val_);
      break;
    }
    case P_END:
      endComp();
      break;
    case P_SUMMARY: {
      size_t n = icsUnescape(val_, valLen_);
      foldToAscii(val_, n, cur_.title, sizeof(cur_.title));
      break;
    }
    case P_LOCATION: {
      size_t n = icsUnescape(val_, valLen_);
      foldToAscii(val_, n, cur_.location, sizeof(cur_.location));
      break;
    }
    case P_DTSTART:
      if (parseIcsDateTime(val_, valLen_, cur_.dtstart)) {
        copyStr(cur_.tzStart, sizeof(cur_.tzStart), tzParam_);
      } else {
        cur_.dtstart.valid = false;
      }
      break;
    case P_DTEND:
      if (parseIcsDateTime(val_, valLen_, cur_.dtend)) {
        cur_.hasEnd = true;
        copyStr(cur_.tzEnd, sizeof(cur_.tzEnd), tzParam_);
      }
      break;
    case P_DURATION: {
      int64_t d;
      if (parseIcsDuration(val_, valLen_, d)) { cur_.duration = d; cur_.hasDuration = true; }
      break;
    }
    case P_RRULE:
      if (!cur_.hasRrule && !valOverflow_) {
        size_t n = trimInPlace(val_, valLen_);
        if (n < sizeof(cur_.rrule)) {
          for (size_t i = 0; i <= n; i++) cur_.rrule[i] = upperc(val_[i]);
          cur_.hasRrule = true;
        }
      }
      break;
    case P_EXDATE:
    case P_RDATE:
    case P_OBS_RDATE:
      listItem();
      break;
    case P_RECURID:
      if (parseIcsDateTime(val_, valLen_, cur_.recurId)) {
        cur_.hasRecurId = true;
        copyStr(cur_.tzRecur, sizeof(cur_.tzRecur), tzParam_);
      }
      break;
    case P_STATUS: {
      size_t n = trimInPlace(val_, valLen_);
      if (n == 9 && equalsNoCase(val_, "CANCELLED", 9)) cur_.cancelled = true;
      else if (n == 9 && equalsNoCase(val_, "TENTATIVE", 9)) cur_.tentative = true;
      break;
    }
    case P_TRAVEL: {
      int64_t d;
      size_t n = trimInPlace(val_, valLen_);
      if (parseIcsDuration(val_, n, d) && d > 0) {
        int64_t m = (d + 59) / 60;
        cur_.leaveMin = (uint8_t)(m > 240 ? 240 : m);
      }
      break;
    }
    case P_MSALLDAY: {
      size_t n = trimInPlace(val_, valLen_);
      if (n == 4 && equalsNoCase(val_, "TRUE", 4)) cur_.allDayX = true;
      break;
    }
    case P_TZID: {
      size_t n = trimInPlace(val_, valLen_);
      (void)n;
      copyStr(zcur_.name, sizeof(zcur_.name), val_);
      break;
    }
    case P_OBS_DTSTART: {
      IcsDateTime dt;
      if (parseIcsDateTime(val_, valLen_, dt)) ocur_.dtstartWall = dt.wall;
      else ocurValid_ = false;
      break;
    }
    case P_OBS_FROM: {
      int m;
      if (parseUtcOffset(val_, valLen_, m)) { ocur_.offFrom = (int16_t)m; obsHaveFrom_ = true; }
      break;
    }
    case P_OBS_TO: {
      int m;
      if (parseUtcOffset(val_, valLen_, m)) { ocur_.offTo = (int16_t)m; obsHaveTo_ = true; }
      break;
    }
    case P_OBS_RRULE: {
      size_t n = trimInPlace(val_, valLen_);
      if (n < sizeof(ocurRrule_)) {
        for (size_t i = 0; i <= n; i++) ocurRrule_[i] = upperc(val_[i]);
      }
      break;
    }
    case P_CALNAME: {
      size_t n = icsUnescape(val_, valLen_);
      foldToAscii(val_, n, stats_.calName, sizeof(stats_.calName));
      break;
    }
    case P_CALTZ: {
      size_t n = trimInPlace(val_, valLen_);
      (void)n;
      copyStr(stats_.calTz, sizeof(stats_.calTz), val_);
      break;
    }
    default:
      break;
  }
}

// One EXDATE/RDATE list value (comma-separated values arrive one by one).
void IcsParser::listItem() {
  size_t n = trimInPlace(val_, valLen_);
  valLen_ = 0;
  if (n == 0) return;
  char *slash = strchr(val_, '/');                  // RDATE;VALUE=PERIOD:start/end
  if (slash) { *slash = '\0'; n = (size_t)(slash - val_); }
  IcsDateTime dt;
  if (!parseIcsDateTime(val_, n, dt)) { stats_.badDates++; return; }

  if (prop_ == P_OBS_RDATE) {
    // Keep only onsets near the window (some generators list every
    // historical transition as an RDATE).
    int64_t lo = opt_.windowStart - 400 * kDay, hi = opt_.windowEnd + 400 * kDay;
    if (dt.wall >= lo && dt.wall <= hi && ocur_.nRdates < 4) ocur_.rdates[ocur_.nRdates++] = dt.wall;
    return;
  }
  // Values far from the window can never matter.
  int64_t lo = opt_.windowStart - 3 * kDay, hi = opt_.windowEnd + 3 * kDay;
  if (dt.wall < lo || dt.wall > hi) return;

  if (prop_ == P_EXDATE) {
    if (dt.isDate) {
      if (cur_.nExDay < 16) cur_.exDay[cur_.nExDay++] = floorDiv(dt.wall, kDay);
    } else if (dt.isUtc || tzParam_[0]) {
      if (cur_.nExUtc < 40) cur_.exUtc[cur_.nExUtc++] = toUtc(dt, tzParam_, true);
    } else {
      if (cur_.nExFloat < 16) cur_.exFloat[cur_.nExFloat++] = dt.wall;
    }
  } else if (prop_ == P_RDATE) {
    if (dt.isDate) {
      if (cur_.nRdDay < 8) cur_.rdDay[cur_.nRdDay++] = floorDiv(dt.wall, kDay);
    } else if (dt.isUtc || tzParam_[0]) {
      if (cur_.nRdUtc < 12) cur_.rdUtc[cur_.nRdUtc++] = toUtc(dt, tzParam_, true);
    } else {
      if (cur_.nRdFloat < 8) cur_.rdFloat[cur_.nRdFloat++] = dt.wall;
    }
  }
}

// ===========================================================================
// components
// ===========================================================================
void IcsParser::beginComp(const char *name) {
  if (depth_ >= 8) { deepOverflow_++; return; }
  Comp top = depth_ ? comp_[depth_ - 1] : C_NONE;
  Comp c = C_OTHER;
  if (eq(name, "VCALENDAR") && top == C_NONE) {
    c = C_CAL;
    stats_.sawCalendar = true;
  } else if (eq(name, "VEVENT") && (top == C_CAL || top == C_NONE)) {
    c = C_EVENT;
    resetCur();
  } else if (eq(name, "VTIMEZONE") && (top == C_CAL || top == C_NONE)) {
    c = C_TZ;
    memset((void *)&zcur_, 0, sizeof(zcur_));
  } else if ((eq(name, "STANDARD") || eq(name, "DAYLIGHT")) && top == C_TZ) {
    c = C_TZOBS;
    memset((void *)&ocur_, 0, sizeof(ocur_));
    ocur_.untilUtc = kNever;
    ocurRrule_[0] = '\0';
    ocurValid_ = true;
    obsHaveFrom_ = obsHaveTo_ = false;
  }
  comp_[depth_++] = c;
}

void IcsParser::endComp() {
  if (deepOverflow_) { deepOverflow_--; return; }
  if (!depth_) return;
  Comp c = comp_[--depth_];
  if (c == C_EVENT) {
    finalizeEvent();
  } else if (c == C_TZOBS) {
    if (!ocurValid_ || !obsHaveTo_) return;
    if (!obsHaveFrom_) ocur_.offFrom = ocur_.offTo;
    if (ocurRrule_[0]) {
      RRule r;
      if (parseRRule(ocurRrule_, r) && r.freq == 4 && !r.unsupported) {
        ocur_.hasRule = true;
        int y, m, d;
        civilFromDays(floorDiv(ocur_.dtstartWall, kDay), y, m, d);
        ocur_.month = (int8_t)m;
        for (int mm = 1; mm <= 12; mm++)
          if (r.byMonthMask & (1u << mm)) { ocur_.month = (int8_t)mm; break; }
        ocur_.wday = -1;
        if (r.nByDayOrd) { ocur_.ord = r.byDayOrd[0]; ocur_.wday = r.byDayOrdWd[0]; }
        else if (r.byDayMask) {
          for (int wd = 0; wd < 7; wd++)
            if (r.byDayMask & (1u << wd)) { ocur_.wday = (int8_t)wd; break; }
        }
        ocur_.nMdays = r.nByMonthDay > 7 ? 7 : r.nByMonthDay;
        for (int i = 0; i < ocur_.nMdays; i++) ocur_.mdays[i] = r.byMonthDay[i];
        if (r.hasUntil) {
          ocur_.untilUtc = r.until.isUtc ? r.until.wall
                                         : r.until.wall - (int64_t)ocur_.offFrom * 60;
        }
      }
    }
    if (zcur_.nObs < kMaxObs) {
      zcur_.obs[zcur_.nObs++] = ocur_;
    } else {
      // Keep the newest observances: replace the oldest if this one is newer.
      int oldest = 0;
      for (int i = 1; i < zcur_.nObs; i++)
        if (zcur_.obs[i].dtstartWall < zcur_.obs[oldest].dtstartWall) oldest = i;
      if (ocur_.dtstartWall > zcur_.obs[oldest].dtstartWall) zcur_.obs[oldest] = ocur_;
    }
  } else if (c == C_TZ) {
    if (!zcur_.name[0] || zcur_.nObs == 0) return;
    int existing = findZone(zcur_.name);
    if (existing >= 0) { zones_[existing] = zcur_; return; }
    if (nZones_ < kMaxZones) zones_[nZones_++] = zcur_;
  }
}

// ===========================================================================
// time zones
// ===========================================================================
int IcsParser::findZone(const char *name) const {
  if (!name || !name[0]) return -1;
  size_t l = strlen(name);
  for (int i = 0; i < nZones_; i++)
    if (eq(zones_[i].name, name)) return i;
  for (int i = 0; i < nZones_; i++)
    if (strlen(zones_[i].name) == l && equalsNoCase(zones_[i].name, name, l)) return i;
  return -1;
}

// Wall time of a yearly observance's onset in year y (0 if none).
static bool obsOnsetWall(int8_t month, int8_t ord, int8_t wday, const int8_t *mdays,
                         uint8_t nMdays, int64_t dtstartWall, int y, int64_t &wall) {
  int64_t d0 = floorDiv(dtstartWall, kDay);
  int64_t tod = dtstartWall - d0 * kDay;
  int sy, sm, sd;
  civilFromDays(d0, sy, sm, sd);
  int m = month ? month : sm;
  int dim = daysInMonth(y, m);
  int day = 0;
  if (ord != 0 && wday >= 0) {
    day = nthWeekdayOfMonth(y, m, ord, wday);
  } else if (nMdays && wday >= 0) {
    for (int i = 0; i < nMdays && !day; i++) {
      int md = mdays[i] > 0 ? mdays[i] : dim + mdays[i] + 1;
      if (md >= 1 && md <= dim && weekdayFromDays(daysFromCivil(y, m, md)) == wday) day = md;
    }
  } else if (nMdays) {
    day = mdays[0] > 0 ? mdays[0] : dim + mdays[0] + 1;
  } else {
    day = sd;
  }
  if (day < 1 || day > dim) return false;
  wall = daysFromCivil(y, m, day) * kDay + tod;
  return true;
}

int IcsParser::zoneOffsetAtUtc(const TzZone &z, int64_t utc) const {
  int64_t best = INT64_MIN, earliest = INT64_MAX;
  int bestOff = 0, earliestFrom = z.nObs ? z.obs[0].offFrom : 0;
  Civil c = secondsToCivil(utc);
  for (int i = 0; i < z.nObs; i++) {
    const TzObs &o = z.obs[i];
    auto consider = [&](int64_t wall) {
      int64_t t = wall - (int64_t)o.offFrom * 60;
      if (t <= utc && t > best) { best = t; bestOff = o.offTo; }
      if (t < earliest) { earliest = t; earliestFrom = o.offFrom; }
    };
    consider(o.dtstartWall);
    if (o.hasRule) {
      int startYear = secondsToCivil(o.dtstartWall).year;
      for (int y = c.year - 1; y <= c.year; y++) {
        if (y < startYear) continue;
        int64_t w;
        if (!obsOnsetWall(o.month, o.ord, o.wday, o.mdays, o.nMdays, o.dtstartWall, y, w)) continue;
        if (w < o.dtstartWall) continue;
        if (w - (int64_t)o.offFrom * 60 > o.untilUtc) continue;
        consider(w);
      }
    }
    for (int k = 0; k < o.nRdates; k++) consider(o.rdates[k]);
  }
  return best != INT64_MIN ? bestOff : earliestFrom;
}

int64_t IcsParser::zoneWallToUtcIdx(int zi, int64_t wall) const {
  const TzZone &z = zones_[zi];
  int off1 = zoneOffsetAtUtc(z, wall - (int64_t)(z.nObs ? z.obs[0].offTo : 0) * 60);
  int64_t utc = wall - (int64_t)off1 * 60;
  int off2 = zoneOffsetAtUtc(z, utc);
  if (off2 != off1) utc = wall - (int64_t)off2 * 60;
  return utc;
}

bool IcsParser::zoneOffsetAt(const char *tzid, int64_t utc, int &offMin) const {
  int zi = findZone(tzid);
  if (zi < 0) return false;
  offMin = zoneOffsetAtUtc(zones_[zi], utc);
  return true;
}

bool IcsParser::zoneWallToUtc(const char *tzid, int64_t wall, int64_t &utc) const {
  int zi = findZone(tzid);
  if (zi < 0) return false;
  utc = zoneWallToUtcIdx(zi, wall);
  return true;
}

int64_t IcsParser::toUtc(const IcsDateTime &dt, const char *tzid, bool countUnknown) {
  if (dt.isUtc) return dt.wall;
  if (tzid && tzid[0] && !dt.isDate) {
    int zi = findZone(tzid);
    if (zi >= 0) return zoneWallToUtcIdx(zi, dt.wall);
    if (isUtcName(tzid)) return dt.wall;
    if (countUnknown) stats_.unknownTz++;
  }
  return dt.wall - (int64_t)opt_.fallbackOffsetMin * 60;
}

int64_t IcsParser::occToUtc(int64_t wall, bool allDay, int zoneIdx, bool utcBased) const {
  if (allDay || utcBased) return wall;
  if (zoneIdx >= 0) return zoneWallToUtcIdx(zoneIdx, wall);
  return wall - (int64_t)opt_.fallbackOffsetMin * 60;
}

// ===========================================================================
// events
// ===========================================================================
void IcsParser::addOverride(uint32_t uid, int64_t orig) {
  int64_t lo = opt_.windowStart - 3 * kDay, hi = opt_.windowEnd + 3 * kDay;
  if (orig < lo || orig > hi) return;
  for (int i = 0; i < nOvr_; i++)
    if (ovr_[i].uid == uid && ovr_[i].orig == orig) return;
  if (nOvr_ < kMaxOvr) ovr_[nOvr_++] = {uid, orig};
  else stats_.dropped++;
}

void IcsParser::addCandidate(const Event &ev, int64_t orig, bool isOverride) {
  for (int i = 0; i < nCand_; i++) {
    Cand &c = cand_[i];
    if (c.ev.uid == ev.uid && c.ev.start == ev.start) {
      if (isOverride && !c.isOverride) { c.ev = ev; c.orig = orig; c.isOverride = true; }
      return;
    }
  }
  if (nCand_ < kMaxCand) {
    cand_[nCand_].ev = ev;
    cand_[nCand_].orig = orig;
    cand_[nCand_].isOverride = isOverride;
    nCand_++;
    return;
  }
  // Full: keep the earliest kMaxCand; evict the latest-starting candidate.
  stats_.dropped++;
  int worst = 0;
  for (int i = 1; i < nCand_; i++)
    if (cand_[i].ev.start > cand_[worst].ev.start) worst = i;
  if (ev.start < cand_[worst].ev.start) {
    cand_[worst].ev = ev;
    cand_[worst].orig = orig;
    cand_[worst].isOverride = isOverride;
  }
}

bool IcsParser::excluded(int64_t occWall, int64_t occUtc, bool allDay) const {
  int64_t day = floorDiv(occWall, kDay);
  for (int i = 0; i < cur_.nExDay; i++) if (cur_.exDay[i] == day) return true;
  if (allDay) {
    // Date-time EXDATEs on an all-day series: compare the date part.
    for (int i = 0; i < cur_.nExFloat; i++) if (floorDiv(cur_.exFloat[i], kDay) == day) return true;
    return false;
  }
  for (int i = 0; i < cur_.nExUtc; i++) if (cur_.exUtc[i] == occUtc) return true;
  return false;
}

void IcsParser::emitInstance(int64_t occWall, int64_t occUtc, int64_t durSec, bool allDay,
                             bool recurring, int64_t origKey) {
  // Window test (all-day dates are placed with the watch's own offset).
  int64_t s = allDay ? occWall - (int64_t)opt_.fallbackOffsetMin * 60 : occUtc;
  int64_t e = s + (durSec > 0 ? durSec : 1);
  if (!(s < opt_.windowEnd && e > opt_.windowStart)) return;

  Event ev;
  memset((void *)&ev, 0, sizeof(ev));
  ev.start = allDay ? occWall : occUtc;
  ev.end = ev.start + durSec;
  ev.uid = cur_.uid;
  ev.key = instanceKey(cur_.uid, origKey);
  ev.flags = (uint8_t)((allDay ? EF_ALLDAY : 0) | (recurring ? EF_RECURRING : 0) |
                       (cur_.hasRecurId ? EF_OVERRIDE : 0) | (cur_.tentative ? EF_TENTATIVE : 0));
  ev.source = opt_.source;
  ev.feed = opt_.feedIndex;
  ev.leaveMin = allDay ? 0 : cur_.leaveMin;
  copyStr(ev.title, sizeof(ev.title), cur_.title[0] ? cur_.title : "(no title)");
  copyStr(ev.location, sizeof(ev.location), cur_.location);
  stats_.instances++;
  addCandidate(ev, origKey, cur_.hasRecurId);
}

void IcsParser::expandRule(const RRule &r, int64_t dtWall, int64_t durSec, bool allDay,
                           int zoneIdx, bool utcBased) {
  PeriodCtx c;
  c.r = &r;
  c.d0 = floorDiv(dtWall, kDay);
  int64_t tod = dtWall - c.d0 * kDay;
  civilFromDays(c.d0, c.y0, c.m0, c.md0);
  c.w0 = c.d0 - floorMod(weekdayFromDays(c.d0) - r.wkst, 7);

  // Day-number bounds of the window, widened for UTC offsets and durations.
  int64_t durDays = durSec / kDay;
  int64_t winStartDay = floorDiv(opt_.windowStart, kDay) - 2 - durDays;
  int64_t winEndDay = floorDiv(opt_.windowEnd, kDay) + 2;

  auto pastUntil = [&](int64_t occWall) -> bool {
    if (!r.hasUntil) return false;
    if (r.until.isDate) return floorDiv(occWall, kDay) > floorDiv(r.until.wall, kDay);
    if (r.until.isUtc) return occToUtc(occWall, allDay, zoneIdx, utcBased) > r.until.wall;
    return occWall > r.until.wall;
  };
  auto emit = [&](int64_t occWall) {
    int64_t occUtc = occToUtc(occWall, allDay, zoneIdx, utcBased);
    if (excluded(occWall, occUtc, allDay)) return;
    emitInstance(occWall, occUtc, durSec, allDay, true, allDay ? occWall : occUtc);
  };

  // With COUNT every occurrence since DTSTART must be counted; otherwise
  // jump straight to the periods around the window.
  int64_t kStart = 0;
  if (r.count == 0) {
    switch (r.freq) {
      case 1: kStart = floorDiv(winStartDay - c.d0, r.interval); break;
      case 2: kStart = floorDiv(winStartDay - 7 - c.w0, 7LL * r.interval); break;
      case 3: {
        int wy, wm, wd;
        civilFromDays(winStartDay, wy, wm, wd);
        int64_t months = ((int64_t)wy * 12 + wm) - ((int64_t)c.y0 * 12 + c.m0) - 1;
        kStart = floorDiv(months, r.interval);
        break;
      }
      case 4: {
        int wy, wm, wd;
        civilFromDays(winStartDay, wy, wm, wd);
        kStart = floorDiv((int64_t)(wy - 1 - c.y0), r.interval);
        break;
      }
    }
    if (kStart < 0) kStart = 0;
  }

  int64_t days[64];
  int64_t pstart;
  int64_t produced = 0;

  // RFC 5545: DTSTART is always the first instance, even if the rule itself
  // would not generate it.
  int n0 = periodDays(c, 0, days, 64, pstart);
  bool d0InRule = false;
  for (int i = 0; i < n0; i++) if (days[i] == c.d0) d0InRule = true;
  if (!d0InRule) {
    produced++;
    emit(dtWall);
    if (r.count && produced >= r.count) return;
  }

  // COUNT normally forces counting from DTSTART. For the plain daily and
  // weekly shapes the count before a period is arithmetic, so long-running
  // series (a standup since 2015 with COUNT=5000) jump straight to the window.
  if (r.count && !r.byMonthMask && !r.nByMonthDay && !r.nBySetPos) {
    if (r.freq == 1 && !r.byDayMask) {
      int64_t k = floorDiv(winStartDay - c.d0, r.interval);
      if (k > 0) {
        kStart = k;
        produced = k;                             // one instance per earlier period
        if (produced >= r.count) return;
      }
    } else if (r.freq == 2) {
      int64_t k = floorDiv(winStartDay - 7 - c.w0, 7LL * r.interval);
      if (k > 0) {
        unsigned mask = r.byDayMask ? r.byDayMask : (1u << weekdayFromDays(c.d0));
        int perWeek = 0;
        for (int wd = 0; wd < 7; wd++) if (mask & (1u << wd)) perWeek++;
        int week0 = 0;
        for (int i = 0; i < n0; i++) if (days[i] >= c.d0) week0++;
        kStart = k;
        produced += week0 + (k - 1) * perWeek;
        if (produced >= r.count) return;
      }
    }
  }

  const int kMaxIter = 5000;
  for (int iter = 0; iter < kMaxIter; iter++) {
    int64_t k = kStart + iter;
    int n = periodDays(c, k, days, 64, pstart);
    for (int i = 0; i < n; i++) {
      int64_t d = days[i];
      if (d < c.d0) continue;
      int64_t occWall = d * kDay + tod;
      if (pastUntil(occWall)) return;
      if (d > winEndDay) return;                 // days only increase from here
      produced++;
      if (d >= winStartDay) emit(occWall);
      if (r.count && produced >= r.count) return;
    }
    if (pstart > winEndDay) return;
  }
  stats_.dropped++;                               // iteration cap hit
}

void IcsParser::finalizeEvent() {
  stats_.events++;
  if (!cur_.dtstart.valid) { stats_.badDates++; return; }
  if (!cur_.hasUid) {
    // No UID: synthesize one from the title and start so instances still
    // dedupe and alerts can be tracked.
    cur_.uid = fnv1a(cur_.title, strlen(cur_.title)) ^ (uint32_t)cur_.dtstart.wall;
  }

  bool allDay = cur_.dtstart.isDate;
  int64_t dtWall = cur_.dtstart.wall;
  if (!allDay && cur_.allDayX) {
    // Outlook marks all-day events with a flag but sends date-times.
    allDay = true;
    dtWall = floorDiv(dtWall, kDay) * kDay;
  }

  int zoneIdx = -1;
  bool utcBased = cur_.dtstart.isUtc;
  if (!allDay && !utcBased && cur_.tzStart[0]) {
    zoneIdx = findZone(cur_.tzStart);
    if (zoneIdx < 0) {
      if (isUtcName(cur_.tzStart)) utcBased = true;
      else stats_.unknownTz++;
    }
  }

  // Floating EXDATE/RDATE values follow DTSTART's time zone.
  for (int i = 0; i < cur_.nExFloat && !allDay; i++) {
    if (cur_.nExUtc < 40) cur_.exUtc[cur_.nExUtc++] = occToUtc(cur_.exFloat[i], false, zoneIdx, utcBased);
  }
  int64_t startUtc = occToUtc(dtWall, allDay, zoneIdx, utcBased);

  int64_t dur;
  if (cur_.hasEnd && cur_.dtend.valid) {
    if (allDay) {
      int64_t endWall = cur_.dtend.isDate ? cur_.dtend.wall
                                          : floorDiv(cur_.dtend.wall + kDay - 1, kDay) * kDay;
      dur = endWall - dtWall;
    } else {
      const char *tz = cur_.tzEnd[0] ? cur_.tzEnd : cur_.tzStart;
      int64_t endUtc;
      if (cur_.dtend.isUtc) endUtc = cur_.dtend.wall;
      else if (!cur_.tzEnd[0] && utcBased) endUtc = cur_.dtend.wall;
      else endUtc = toUtc(cur_.dtend, tz, false);
      dur = endUtc - startUtc;
    }
  } else if (cur_.hasDuration) {
    dur = cur_.duration;
  } else {
    dur = allDay ? kDay : 0;
  }
  if (dur < 0) dur = 0;
  if (allDay && dur < kDay) dur = kDay;
  if (dur > 31 * kDay) dur = 31 * kDay;

  if (cur_.hasRecurId) {
    stats_.overrides++;
    int64_t orig;
    if (cur_.recurId.isDate) {
      orig = allDay ? floorDiv(cur_.recurId.wall, kDay) * kDay
                    : cur_.recurId.wall - (int64_t)opt_.fallbackOffsetMin * 60;
    } else if (allDay) {
      orig = floorDiv(cur_.recurId.wall, kDay) * kDay;
    } else if (cur_.recurId.isUtc) {
      orig = cur_.recurId.wall;
    } else {
      const char *tz = cur_.tzRecur[0] ? cur_.tzRecur : cur_.tzStart;
      orig = (tz[0] || !utcBased) ? toUtc(cur_.recurId, tz, false) : cur_.recurId.wall;
    }
    addOverride(cur_.uid, orig);
    if (cur_.cancelled) { stats_.cancelled++; return; }
    emitInstance(dtWall, startUtc, dur, allDay, false, orig);
    return;
  }
  if (cur_.cancelled) { stats_.cancelled++; return; }

  bool expanded = false;
  if (cur_.hasRrule) {
    RRule r;
    if (parseRRule(cur_.rrule, r) && !r.unsupported) {
      stats_.recurring++;
      expandRule(r, dtWall, dur, allDay, zoneIdx, utcBased);
      expanded = true;
    } else {
      stats_.unsupportedRules++;
    }
  }
  if (!expanded) {
    if (!excluded(dtWall, startUtc, allDay))
      emitInstance(dtWall, startUtc, dur, allDay, false, allDay ? dtWall : startUtc);
  }
  // RDATE: extra instances of the same series.
  int64_t tod = dtWall - floorDiv(dtWall, kDay) * kDay;
  for (int i = 0; i < cur_.nRdUtc && !allDay; i++) {
    int64_t u = cur_.rdUtc[i];
    if (!excluded(u, u, false)) emitInstance(u, u, dur, false, true, u);
  }
  for (int i = 0; i < cur_.nRdFloat && !allDay; i++) {
    int64_t u = occToUtc(cur_.rdFloat[i], false, zoneIdx, utcBased);
    if (!excluded(cur_.rdFloat[i], u, false)) emitInstance(cur_.rdFloat[i], u, dur, false, true, u);
  }
  for (int i = 0; i < cur_.nRdDay; i++) {
    int64_t w = cur_.rdDay[i] * kDay + (allDay ? 0 : tod);
    int64_t u = occToUtc(w, allDay, zoneIdx, utcBased);
    if (!excluded(w, u, allDay)) emitInstance(w, u, dur, allDay, true, allDay ? w : u);
  }
}

}  // namespace mc
