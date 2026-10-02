// Dayprint generative art: public API.
//
// The artwork is a pure function of (day, step bucket, ALGO_VERSION):
//   * the DATE picks the composition family (a weekly shuffle so every week
//     shows all seven), a variant, a harmonious palette and every parameter;
//   * the STEP BUCKET (250 steps each) sets how far the piece has grown;
//   * ALGO_VERSION is stored in every day record so future changes can never
//     silently alter past days.
//
// Rendering is incremental: an ArtJob executes a fixed, deterministic
// sequence of small "ops" (each well under a few ms on the watch). Growth
// only ever appends ops, so raising the bucket continues the job instead of
// restarting it, and chunking never changes the result.
#pragma once
#include <stdint.h>
#include "gf_core.h"
#include "gf_color.h"
#include "gf_raster.h"
#include "gf_date.h"

namespace gf {

static const uint8_t  ALGO_VERSION     = 1;
static const uint16_t STEPS_PER_BUCKET = 250;
static const uint16_t FULL_BUCKET      = 64;    // 16,000 steps: fully grown
static const uint16_t MAX_BUCKET       = 160;   // 40,000 steps: flourish cap

enum Family : uint8_t {
  kCurrents = 0, kOrbits, kWeave, kGarden, kContours, kMosaic, kDunes,
  kFamilyCount
};
const char *familyName(uint8_t family);
const char *variantName(uint8_t family, uint8_t variant);

// Everything the date alone decides.
struct ArtSpec {
  uint16_t   day = 0;
  uint8_t    algo = ALGO_VERSION;
  uint8_t    family = 0;
  uint8_t    variant = 0;
  SpecialDay special = kOrdinary;
  Palette    pal;
  uint64_t   seed = 0;
};
void describeDay(uint16_t day, uint8_t algo, ArtSpec &out);

// The weekly remix: a new piece for the Monday-based week starting at
// `monday`, built from that week's days (day indices and final steps).
// Family of the most-walked day, palette around the week's mean hue, growth
// from the average steps. Deterministic in its inputs.
void describeWeekRemix(uint16_t monday, const uint16_t *days, const uint32_t *steps,
                       int32_t n, ArtSpec &out, uint16_t &bucketOut);

uint16_t bucketForSteps(uint32_t steps);
int32_t  growthPermille(uint16_t bucket);        // 0..1000, eases out

// Scratch memory a job needs (heightfields, queues, site maps).
static const uint32_t kScratchBytes = 160 * 1024;

struct FamState;     // per-family state, defined in gf_family.h

class ArtJob {
 public:
  ArtJob();
  ~ArtJob();
  // Starts a fresh render of `day` grown to `bucket` into `canvas`.
  // `scratch` must hold kScratchBytes and stay valid for the job's life.
  void begin(Canvas *canvas, uint8_t *scratch, uint16_t day, uint16_t bucket,
             uint8_t algo = ALGO_VERSION);
  // Same with a prepared spec (the weekly remix).
  void beginSpec(Canvas *canvas, uint8_t *scratch, const ArtSpec &spec, uint16_t bucket);
  // Raises the growth target. Returns false (and changes nothing) if the new
  // bucket is lower, which needs a fresh begin() instead.
  bool extendTo(uint16_t bucket);
  // Runs up to maxOps ops. Returns true once the job is complete.
  bool run(uint32_t maxOps);
  bool done() const { return phase_ == kDone; }
  bool active() const { return phase_ != kIdle; }
  uint32_t opsDone() const { return ops_; }
  uint16_t day() const { return spec_.day; }
  uint16_t bucket() const { return bucket_; }
  const ArtSpec &spec() const { return spec_; }

  // Convenience: render the whole thing in one call (host tools, tests).
  static void renderFull(Canvas *canvas, uint8_t *scratch, uint16_t day,
                         uint16_t bucket, uint8_t algo = ALGO_VERSION);

 private:
  enum Phase : uint8_t { kIdle, kPrep, kElements, kFlourish, kDone };
  void refreshTargets();

  ArtSpec   spec_;
  Canvas   *cv_ = nullptr;
  uint8_t  *scratch_ = nullptr;
  FamState *st_ = nullptr;
  Phase     phase_ = kIdle;
  uint16_t  bucket_ = 0;
  int32_t   prepCount_ = 0, prepDone_ = 0;
  int32_t   elemTarget_ = 0, elemDone_ = 0;
  int32_t   flourTarget_ = 0, flourDone_ = 0;
  uint32_t  ops_ = 0;
  Rng       famRng_, elemRng_, flourRng_;
};

// FNV-1a over the canvas pixels: the determinism fingerprint used by the
// tests, the host preview tool and the watch's ARTHASH serial command.
uint32_t canvasHash(const Canvas &c);

}  // namespace gf
