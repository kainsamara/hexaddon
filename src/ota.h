#pragma once
#include <Arduino.h>

// Release-check cache lifetime: the UI polls on every page load, GitHub's
// API is rate limited (60 req/h per IP without a token) - do not query more
// often than this.
#define CHECK_TTL_MS 600000UL

// Online update from the GitHub release feed (see .github/workflows/release.yml).
// The firmware itself polls the repo, so the browser never talks to GitHub
// directly (no CORS worries) and the same code path works from the display.

struct OtaState {
  // release check (background task, cached ~10 min)
  bool checking = false;    // a check is in flight
  bool checked = false;     // a check has finished at least once
  bool netError = false;    // github unreachable / no releases / rate limited
  uint32_t checkedAt = 0;   // millis() of the last finished check
  String latest;            // tag without the leading 'v', e.g. "0.9.3"
  String url;               // direct download url of the firmware.bin asset

  // download + flash (background task)
  String stage = "idle";    // idle | download | done | error
  int progress = 0;         // 0..100 while downloading
  String error;             // last error text (stage == "error")
};

// The state lives behind a mutex: background tasks write it, async_tcp
// handlers read it — a plain reference would let a reader observe a
// half-reassigned String (use-after-free). Always take a snapshot copy.
OtaState otaState();

// Atomic flag, set right before the download task is spawned and cleared at
// every task exit: /api/update (multipart upload) must refuse a second
// writer even inside the gap before the task publishes stage="download"
// (Update.h itself is not thread safe).
bool otaDownloadActive();

// Kick off a background release check (no-op if one is already running).
void otaCheckAsync();

// Is `latest` newer than the compiled-in FW_VERSION? Semantic x.y.z compare.
bool otaNewerThan(const String &latest);

// Start downloading + flashing the cached release url in a background task.
// Returns false (and fills `err`) if it is already running or no url is known.
bool otaStart(String &err);
