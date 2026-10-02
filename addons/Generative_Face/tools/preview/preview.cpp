// Host preview renderer for Dayprint. Compiles the exact same art and face
// code the watch runs (src/art/*) with the host compiler and writes PNGs.
//
//   tools/preview/build.sh                      # builds tools/preview/preview
//   preview art   2026-10-01 6000 out.png       # artwork only
//   preview face  2026-10-01 6000 10:42 out.png # watch face as the watch shows it
//   preview grid  2026-10-01 7 0,3000,8000,16000 out.png [face]
//   preview year  2026 out.png                  # a simulated year contact sheet
//   preview hash  2026-10-01 6000               # prints the ARTHASH fingerprint
//   preview snapstream 2026-10-01 6000 out.bin  # fake SNAP stream for tools/snap.py
//
// Pixels go through the same RGB565 + ordered-dither path as the watch for
// "face" output, so previews show the real colour depth.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>
#include "gf_art.h"
#ifdef GF_HAVE_FACE
#include "gf_face.h"
#endif

using namespace gf;

static void writePng(const char *path, const std::vector<uint8_t> &rgb, int w, int h) {
  std::vector<uint8_t> raw;
  raw.reserve((size_t)(w * 3 + 1) * h);
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + (size_t)y * w * 3, rgb.begin() + (size_t)(y + 1) * w * 3);
  }
  uLongf zlen = compressBound(raw.size());
  std::vector<uint8_t> z(zlen);
  compress2(z.data(), &zlen, raw.data(), raw.size(), 9);
  z.resize(zlen);
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); exit(1); }
  auto be32 = [&](uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; fwrite(b, 1, 4, f); };
  auto chunk = [&](const char *type, const uint8_t *data, uint32_t n) {
    be32(n);
    std::vector<uint8_t> buf((const uint8_t *)type, (const uint8_t *)type + 4);
    buf.insert(buf.end(), data, data + n);
    fwrite(buf.data(), 1, buf.size(), f);
    be32((uint32_t)crc32(0, buf.data(), (uInt)buf.size()));
  };
  const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                       (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                       8, 2, 0, 0, 0 };
  chunk("IHDR", ihdr, 13);
  chunk("IDAT", z.data(), (uint32_t)z.size());
  chunk("IEND", nullptr, 0);
  fclose(f);
}

struct Bufs {
  std::vector<uint32_t> art = std::vector<uint32_t>(kW * kH);
  std::vector<uint8_t> scratch = std::vector<uint8_t>(kScratchBytes);
  std::vector<uint16_t> out565 = std::vector<uint16_t>(kW * kH);
  std::vector<uint8_t> maskA = std::vector<uint8_t>(kW * kH);
  std::vector<uint8_t> maskB = std::vector<uint8_t>(kW * kH);
};

static double renderArt(Bufs &b, uint16_t day, uint16_t bucket, ArtSpec &spec) {
  Canvas cv; cv.px = b.art.data();
  auto t0 = std::chrono::steady_clock::now();
  ArtJob job;
  job.begin(&cv, b.scratch.data(), day, bucket);
  while (!job.run(64)) {}
  auto t1 = std::chrono::steady_clock::now();
  spec = job.spec();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

static void artToRgb(const Bufs &b, std::vector<uint8_t> &rgb) {
  rgb.resize(kW * kH * 3);
  for (int i = 0; i < kW * kH; i++) {
    uint32_t c = b.art[i];
    rgb[i * 3] = (uint8_t)(c >> 16); rgb[i * 3 + 1] = (uint8_t)(c >> 8); rgb[i * 3 + 2] = (uint8_t)c;
  }
}

static void c565ToRgb(const uint16_t *p, std::vector<uint8_t> &rgb) {
  rgb.resize(kW * kH * 3);
  for (int i = 0; i < kW * kH; i++) {
    uint16_t v = p[i];
    uint32_t r5 = (v >> 11) & 31, g6 = (v >> 5) & 63, b5 = v & 31;   // bit replication
    uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t bb = (uint8_t)((b5 << 3) | (b5 >> 2));
    rgb[i * 3] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = bb;
  }
}

#ifdef GF_HAVE_FACE
static void composeFace(Bufs &b, const ArtSpec &spec, uint16_t day, uint32_t steps,
                        int hh, int mm, uint8_t mode) {
  FaceInputs in;
  in.day = day;
  in.hour = (uint8_t)hh; in.minute = (uint8_t)mm;
  in.rtcOk = true;
  in.steps = steps;
  in.goal = 8000;
  in.mode = mode;
  in.batteryLow = false;
  in.ambient = false;
  FaceLayer layer;
  layer.maskText = b.maskA.data();
  layer.maskHalo = b.maskB.data();
  Canvas cv; cv.px = b.art.data();
  uint8_t tones = chooseTones(cv, spec, mode, spec.pal.darkBg ? 0 : 3);
  buildFaceLayer(spec, in, tones, layer);
  composeFrame(cv, spec, in, layer, b.out565.data(), 0, kH);
}
#endif

static uint16_t parseDay(const char *s) {
  uint16_t d;
  if (!parseIsoDate(s, d)) { fprintf(stderr, "bad date %s\n", s); exit(2); }
  return d;
}

static void blit(std::vector<uint8_t> &dst, int dw, const std::vector<uint8_t> &src,
                 int sw, int sh, int ox, int oy) {
  for (int y = 0; y < sh; y++)
    memcpy(&dst[((size_t)(oy + y) * dw + ox) * 3], &src[(size_t)y * sw * 3], (size_t)sw * 3);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: see preview.cpp header\n"); return 2; }
  std::string cmd = argv[1];
  Bufs b;
  std::vector<uint8_t> rgb;
  if (cmd == "art" && argc >= 5) {
    uint16_t day = parseDay(argv[2]);
    uint32_t steps = (uint32_t)atoi(argv[3]);
    ArtSpec spec;
    double ms = renderArt(b, day, bucketForSteps(steps), spec);
    artToRgb(b, rgb);
    writePng(argv[4], rgb, kW, kH);
    Canvas cv; cv.px = b.art.data();
    printf("%s %s/%s hash=%08x %.2f ms ops\n", argv[2], familyName(spec.family),
           variantName(spec.family, spec.variant), canvasHash(cv), ms);
    return 0;
  }
#ifdef GF_HAVE_FACE
  if (cmd == "face" && argc >= 6) {
    uint16_t day = parseDay(argv[2]);
    uint32_t steps = (uint32_t)atoi(argv[3]);
    int hh = 10, mm = 42;
    sscanf(argv[4], "%d:%d", &hh, &mm);
    uint8_t mode = argc >= 7 ? (uint8_t)atoi(argv[6]) : 0;
    ArtSpec spec;
    renderArt(b, day, bucketForSteps(steps), spec);
    composeFace(b, spec, day, steps, hh, mm, mode);
    c565ToRgb(b.out565.data(), rgb);
    writePng(argv[5], rgb, kW, kH);
    return 0;
  }
#endif
  if (cmd == "grid" && argc >= 6) {
    uint16_t day0 = parseDay(argv[2]);
    int ndays = atoi(argv[3]);
    std::vector<uint32_t> stepsList;
    for (char *p = strtok(argv[4], ","); p; p = strtok(nullptr, ",")) stepsList.push_back((uint32_t)atoi(p));
    bool face = argc >= 7 && std::string(argv[6]) == "face";
    int gap = 6;
    int cols = (int)stepsList.size(), rows = ndays;
    int W = cols * kW + (cols + 1) * gap, H = rows * kH + (rows + 1) * gap;
    std::vector<uint8_t> sheet((size_t)W * H * 3, 24);
    double total = 0, worst = 0;
    for (int r = 0; r < rows; r++) {
      for (int c = 0; c < cols; c++) {
        uint16_t day = (uint16_t)(day0 + r);
        ArtSpec spec;
        double ms = renderArt(b, day, bucketForSteps(stepsList[c]), spec);
        total += ms; if (ms > worst) worst = ms;
#ifdef GF_HAVE_FACE
        if (face) { composeFace(b, spec, day, stepsList[c], 9 + r, 41, 0); c565ToRgb(b.out565.data(), rgb); }
        else
#endif
        { (void)face; artToRgb(b, rgb); }
        blit(sheet, W, rgb, kW, kH, gap + c * (kW + gap), gap + r * (kH + gap));
        if (c == 0) {
          char iso[11]; formatIsoDate(day, iso);
          printf("%s %-9s %-10s %s\n", iso, familyName(spec.family),
                 variantName(spec.family, spec.variant), schemeName(spec.pal.scheme));
        }
      }
    }
    writePng(argv[5], sheet, W, H);
    printf("render avg %.2f ms, worst %.2f ms\n", total / (rows * cols), worst);
    return 0;
  }
  if (cmd == "year" && argc >= 4) {
    // 12 rows (months) x 31 columns of quarter-size thumbnails with
    // synthetic step counts: the "year collection" poster.
    int year = atoi(argv[2]);
    int tw = kW / 4, th = kH / 4, gap = 4;
    int W = 31 * tw + 32 * gap, H = 12 * th + 13 * gap;
    std::vector<uint8_t> sheet((size_t)W * H * 3, 18);
    std::vector<uint8_t> thumb((size_t)tw * th * 3);
    uint32_t lcg = 12345;
    for (int m = 1; m <= 12; m++) {
      for (int d = 1; d <= daysInMonth((uint16_t)year, (uint8_t)m); d++) {
        uint16_t day = dayIndex((uint16_t)year, (uint8_t)m, (uint8_t)d);
        lcg = lcg * 1103515245u + 12345u;
        uint32_t steps = 2500 + (lcg >> 8) % 11000;
        ArtSpec spec;
        renderArt(b, day, bucketForSteps(steps), spec);
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
          uint32_t r = 0, g = 0, bl = 0;
          for (int yy = 0; yy < 4; yy++) for (int xx = 0; xx < 4; xx++) {
            uint32_t c = b.art[(y * 4 + yy) * kW + x * 4 + xx];
            r += (c >> 16) & 255; g += (c >> 8) & 255; bl += c & 255;
          }
          uint8_t *o = &thumb[((size_t)y * tw + x) * 3];
          o[0] = (uint8_t)(r / 16); o[1] = (uint8_t)(g / 16); o[2] = (uint8_t)(bl / 16);
        }
        blit(sheet, W, thumb, tw, th, gap + (d - 1) * (tw + gap), gap + (m - 1) * (th + gap));
      }
    }
    writePng(argv[3], sheet, W, H);
    return 0;
  }
#ifdef GF_HAVE_FACE
  if (cmd == "snapstream" && argc >= 5) {
    // Byte-for-byte what the watch sends for "SNAP <date>" (gallery label),
    // for testing tools/snap.py without a watch.
    uint16_t day = parseDay(argv[2]);
    uint32_t steps = (uint32_t)atoi(argv[3]);
    ArtSpec spec;
    renderArt(b, day, bucketForSteps(steps), spec);
    FaceInputs in;
    in.day = day; in.steps = steps; in.mode = kModeGallery;
    FaceLayer layer; layer.maskText = b.maskA.data(); layer.maskHalo = b.maskB.data();
    Canvas cv; cv.px = b.art.data();
    uint8_t tones = chooseTones(cv, spec, in.mode, spec.pal.darkBg ? 0 : 3);
    buildFaceLayer(spec, in, tones, layer);
    composeFrame(cv, spec, in, layer, b.out565.data(), 0, kH);
    FILE *f = fopen(argv[4], "wb");
    fprintf(f, "Steps  : some unrelated log line\r\n");
    fprintf(f, "SNAP BEGIN w=%d h=%d fmt=RGB565LE bytes=%d day=%s steps=%u algo=%u family=%s\n",
            kW, kH, kW * kH * 2, argv[2], (unsigned)steps, (unsigned)ALGO_VERSION, familyName(spec.family));
    std::vector<uint8_t> bytes(kW * kH * 2);
    for (int i = 0; i < kW * kH; i++) { bytes[2 * i] = (uint8_t)b.out565[i]; bytes[2 * i + 1] = (uint8_t)(b.out565[i] >> 8); }
    fwrite(bytes.data(), 1, bytes.size(), f);
    fprintf(f, "\nSNAP END crc32=%08lx\n", (unsigned long)crc32(0, bytes.data(), (uInt)bytes.size()));
    fclose(f);
    c565ToRgb(b.out565.data(), rgb);
    return 0;
  }
#endif
#ifdef GF_HAVE_FACE
  if (cmd == "remix" && argc >= 4) {
    // The Gallery's weekly remix for the week starting at a Monday, with a
    // synthetic week of steps.
    uint16_t monday = parseDay(argv[2]);
    uint16_t days[7]; uint32_t steps[7];
    for (int k = 0; k < 7; k++) { days[k] = (uint16_t)(monday + k); steps[k] = 4000 + (uint32_t)k * 1300 + (k == 5 ? 9000 : 0); }
    ArtSpec spec; uint16_t bucket;
    describeWeekRemix(monday, days, steps, 7, spec, bucket);
    Canvas cv; cv.px = b.art.data();
    ArtJob job; job.beginSpec(&cv, b.scratch.data(), spec, bucket);
    while (!job.run(1u << 20)) {}
    uint32_t total = 0; for (uint32_t v : steps) total += v;
    char t1[32], t2[48], num[16];
    uint16_t y; uint8_t m, d; dayToDate(monday, y, m, d);
    snprintf(t1, sizeof(t1), "WEEK OF %u %s", d, monthShort(m));
    formatSteps(total, num);
    snprintf(t2, sizeof(t2), "REMIX %c %c %s %c %s", kGlyphDot, kGlyphSteps, num, kGlyphDot, familyName(spec.family));
    FaceInputs in; in.day = monday; in.steps = total; in.mode = kModeGallery; in.caption1 = t1; in.caption2 = t2;
    FaceLayer layer; layer.maskText = b.maskA.data(); layer.maskHalo = b.maskB.data();
    buildFaceLayer(spec, in, chooseTones(cv, spec, in.mode, spec.pal.darkBg ? 0 : 3), layer);
    composeFrame(cv, spec, in, layer, b.out565.data(), 0, kH);
    c565ToRgb(b.out565.data(), rgb);
    writePng(argv[3], rgb, kW, kH);
    return 0;
  }
#endif
  if (cmd == "hash" && argc >= 4) {
    uint16_t day = parseDay(argv[2]);
    uint32_t steps = (uint32_t)atoi(argv[3]);
    ArtSpec spec;
    renderArt(b, day, bucketForSteps(steps), spec);
    Canvas cv; cv.px = b.art.data();
    printf("ARTHASH %s %u v%u %08x\n", argv[2], (unsigned)steps, (unsigned)ALGO_VERSION,
           canvasHash(cv));
    return 0;
  }
  fprintf(stderr, "unknown command\n");
  return 2;
}
