/* aaudio_mixer.c -- AAudio output for libib3.so, mixed into ONE SDL device.
 *
 * Why this exists
 * ---------------
 * The first AAudio shim opened a separate SDL audio device for every AAudio
 * stream. On Switch that is broken: SDL's switch driver creates a private
 * audrv driver per device on top of the single shared audren renderer, so two
 * open devices keep overwriting each other's renderer state (voice 0, sinks,
 * mem pools). Closing one stream (the runtime does this on pause, scene and
 * cutscene changes) tears down state the other stream still uses. The audible
 * result was audio that never starts, cuts out, and only comes back after the
 * game is restarted.
 *
 * SDL also sets its own audio thread to priority 0x3B on Switch, below every
 * game thread (0x2C), and leaves it on the process default core next to the
 * busy emulator threads, so it was regularly starved and the output underran.
 *
 * Design
 * ------
 *   - one SDL device (48 kHz, stereo, S16), opened once and never closed until
 *     the process exits;
 *   - every AAudio stream is a ring buffer of float stereo frames at the
 *     stream's own rate; the SDL callback resamples (linear) and mixes them;
 *   - AAudioStream_write blocks (up to its timeout) until there is room, which
 *     paces the producer exactly as real AAudio does;
 *   - the SDL callback thread raises its priority above the game threads and
 *     moves to a core the game is not using. It only runs this mixer code, so
 *     the higher priority cannot starve guest locks.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <switch.h>

#include "ib3_shim.h"
#include "util.h"

/* AAudio result codes (aaudio/AAudio.h). */
#define AAUDIO_OK 0
#define AAUDIO_ERROR_DISCONNECTED (-899)
#define AAUDIO_ERROR_ILLEGAL_ARGUMENT (-898)
#define AAUDIO_ERROR_INVALID_STATE (-895)
#define AAUDIO_ERROR_UNAVAILABLE (-889)
#define AAUDIO_ERROR_NO_MEMORY (-887)

#define AAUDIO_FORMAT_PCM_I16 1
#define AAUDIO_FORMAT_PCM_FLOAT 2

#define AAUDIO_BURST_FRAMES 256
#define MIXER_RATE 48000
#define MIXER_DEVICE_FRAMES 1024
#define MAX_STREAMS 16
/* Per-stream buffering. Big enough to ride out the producer thread being
 * descheduled for a frame or two on the busy game core, small enough that
 * sword hits still line up with the picture. */
#define STREAM_BUFFER_MS 80
/* Priority for the SDL callback thread: above the game's 0x2C threads. */
#define MIXER_THREAD_PRIORITY 0x2A

typedef struct {
  int32_t format;
  int32_t channels;
  int32_t rate;
} AAudioBuilder;

typedef struct {
  int32_t format;
  int32_t channels;
  int32_t rate;
  int32_t in_bytes_per_frame;

  float *ring; /* interleaved stereo float frames at `rate` */
  uint32_t capacity;
  uint32_t head;
  uint32_t count;

  /* resampler state */
  float prev[2];
  double frac;
  double step;

  int started;
  int closing;
  int writers;
  uint64_t underruns;
} MixStream;

static Mutex g_lock;
static CondVar g_space_cv; /* signalled when the mixer consumed frames */
static SDL_AudioDeviceID g_device;
static int g_device_failed;
static MixStream *g_streams[MAX_STREAMS];
static float g_mix[MIXER_DEVICE_FRAMES * 4 * 2];
static volatile int g_callback_thread_tuned;

static void tune_callback_thread(void) {
  if (g_callback_thread_tuned)
    return;
  g_callback_thread_tuned = 1;

  Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, MIXER_THREAD_PRIORITY);

  /* Prefer core 2, then core 1 (core 0 is the game's default core). Keep every
   * application core in the mask so the kernel can still migrate the thread
   * if that core is busy. */
  u64 allowed = 0;
  Result mask_rc = svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  if (R_SUCCEEDED(mask_rc)) {
    const u64 app_mask = allowed & 0x7;
    int core = -1;
    if (app_mask & 0x4)
      core = 2;
    else if (app_mask & 0x2)
      core = 1;
    if (core >= 0)
      mask_rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, (u32)app_mask);
  }
  debugPrintf("audio: mixer thread priority=0x%x (%08x) affinity (%08x)\n",
              MIXER_THREAD_PRIORITY, rc, mask_rc);
}

/* Pull and resample one stream into the float mix buffer. Lock held. */
static void mix_stream(MixStream *s, float *out, int frames) {
  for (int i = 0; i < frames; i++) {
    while (s->frac >= 1.0) {
      if (!s->count)
        goto underrun;
      const float *next = &s->ring[(size_t)s->head * 2];
      s->prev[0] = next[0];
      s->prev[1] = next[1];
      s->head = (s->head + 1) % s->capacity;
      s->count--;
      s->frac -= 1.0;
    }
    if (!s->count)
      goto underrun;
    const float *next = &s->ring[(size_t)s->head * 2];
    const float t = (float)s->frac;
    out[i * 2 + 0] += s->prev[0] + (next[0] - s->prev[0]) * t;
    out[i * 2 + 1] += s->prev[1] + (next[1] - s->prev[1]) * t;
    s->frac += s->step;
  }
  return;

underrun:
  s->underruns++;
}

static void SDLCALL mixer_callback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  tune_callback_thread();

  int16_t *out = (int16_t *)stream;
  int frames = len / (int)(2 * sizeof(int16_t));
  const int max_frames = (int)(sizeof(g_mix) / sizeof(g_mix[0]) / 2);
  if (frames > max_frames)
    frames = max_frames;
  memset(g_mix, 0, (size_t)frames * 2 * sizeof(float));

  mutexLock(&g_lock);
  for (int i = 0; i < MAX_STREAMS; i++) {
    MixStream *s = g_streams[i];
    if (s && s->started && !s->closing)
      mix_stream(s, g_mix, frames);
  }
  condvarWakeAll(&g_space_cv);
  mutexUnlock(&g_lock);

  for (int i = 0; i < frames * 2; i++) {
    float v = g_mix[i];
    if (v > 1.0f)
      v = 1.0f;
    else if (v < -1.0f)
      v = -1.0f;
    out[i] = (int16_t)(v * 32767.0f);
  }
  if (frames * 2 * (int)sizeof(int16_t) < len)
    memset((uint8_t *)stream + frames * 2 * sizeof(int16_t), 0,
           (size_t)len - (size_t)frames * 2 * sizeof(int16_t));
}

/* Lock held. */
static int ensure_device(void) {
  if (g_device)
    return 1;
  if (g_device_failed)
    return 0;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    debugPrintf("audio: SDL audio init failed: %s\n", SDL_GetError());
    g_device_failed = 1;
    return 0;
  }
  SDL_AudioSpec want;
  SDL_AudioSpec have;
  SDL_zero(want);
  SDL_zero(have);
  want.freq = MIXER_RATE;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = MIXER_DEVICE_FRAMES;
  want.callback = mixer_callback;
  g_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!g_device) {
    debugPrintf("audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    g_device_failed = 1;
    return 0;
  }
  debugPrintf("audio: shared mixer device open %d Hz %d ch %d frames\n",
              have.freq, have.channels, have.samples);
  SDL_PauseAudioDevice(g_device, 0);
  return 1;
}

void ib3_audio_shutdown(void) {
  mutexLock(&g_lock);
  const SDL_AudioDeviceID device = g_device;
  g_device = 0;
  g_device_failed = 1;
  for (int i = 0; i < MAX_STREAMS; i++)
    if (g_streams[i])
      g_streams[i]->closing = 1;
  condvarWakeAll(&g_space_cv);
  mutexUnlock(&g_lock);
  if (device) {
    SDL_PauseAudioDevice(device, 1);
    SDL_CloseAudioDevice(device);
  }
}

/* ------------------------------------------------------------------------ */

int AAudio_createStreamBuilder_fake(void **builder) {
  if (!builder)
    return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
  AAudioBuilder *state = calloc(1, sizeof(*state));
  if (!state)
    return AAUDIO_ERROR_NO_MEMORY;
  state->format = AAUDIO_FORMAT_PCM_I16;
  state->channels = 2;
  state->rate = MIXER_RATE;
  *builder = state;
  return AAUDIO_OK;
}

void AAudioStreamBuilder_setFormat_fake(void *builder, int32_t format) {
  if (builder && format > 0)
    ((AAudioBuilder *)builder)->format = format;
}
void AAudioStreamBuilder_setChannelCount_fake(void *builder, int32_t channels) {
  if (builder && channels > 0)
    ((AAudioBuilder *)builder)->channels = channels;
}
void AAudioStreamBuilder_setSampleRate_fake(void *builder, int32_t rate) {
  /* 0 = AAUDIO_UNSPECIFIED: keep the default. */
  if (builder && rate > 0)
    ((AAudioBuilder *)builder)->rate = rate;
}
void AAudioStreamBuilder_setPerformanceMode_fake(void *builder, int32_t mode) {
  (void)builder;
  (void)mode;
}
void AAudioStreamBuilder_setUsage_fake(void *builder, int32_t usage) {
  (void)builder;
  (void)usage;
}

int AAudioStreamBuilder_openStream_fake(void *builder, void **out) {
  const AAudioBuilder *state = builder;
  if (!state || !out)
    return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
  *out = NULL;

  const int32_t format = state->format == AAUDIO_FORMAT_PCM_FLOAT
                             ? AAUDIO_FORMAT_PCM_FLOAT
                             : AAUDIO_FORMAT_PCM_I16;
  int32_t channels = state->channels;
  if (channels < 1 || channels > 8)
    channels = 2;
  int32_t rate = state->rate;
  if (rate < 8000 || rate > 192000)
    rate = MIXER_RATE;

  MixStream *s = calloc(1, sizeof(*s));
  if (!s)
    return AAUDIO_ERROR_NO_MEMORY;
  s->format = format;
  s->channels = channels;
  s->rate = rate;
  s->in_bytes_per_frame =
      channels * (format == AAUDIO_FORMAT_PCM_FLOAT ? 4 : 2);
  s->capacity = (uint32_t)((int64_t)rate * STREAM_BUFFER_MS / 1000);
  if (s->capacity < AAUDIO_BURST_FRAMES * 4)
    s->capacity = AAUDIO_BURST_FRAMES * 4;
  s->ring = calloc((size_t)s->capacity * 2, sizeof(float));
  if (!s->ring) {
    free(s);
    return AAUDIO_ERROR_NO_MEMORY;
  }
  s->step = (double)rate / (double)MIXER_RATE;
  s->frac = 1.0; /* first output frame pulls the first input frame */

  mutexLock(&g_lock);
  if (!ensure_device()) {
    mutexUnlock(&g_lock);
    free(s->ring);
    free(s);
    return AAUDIO_ERROR_UNAVAILABLE;
  }
  int slot = -1;
  for (int i = 0; i < MAX_STREAMS; i++) {
    if (!g_streams[i]) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    mutexUnlock(&g_lock);
    debugPrintf("audio: too many open streams\n");
    free(s->ring);
    free(s);
    return AAUDIO_ERROR_UNAVAILABLE;
  }
  g_streams[slot] = s;
  mutexUnlock(&g_lock);

  debugPrintf("audio: stream %d opened %d Hz %d ch %s buffer=%u frames\n", slot,
              rate, channels,
              format == AAUDIO_FORMAT_PCM_FLOAT ? "float" : "s16",
              s->capacity);
  *out = s;
  return AAUDIO_OK;
}

int AAudioStreamBuilder_delete_fake(void *builder) {
  free(builder);
  return AAUDIO_OK;
}

int AAudioStream_requestStart_fake(void *stream_ptr) {
  MixStream *s = stream_ptr;
  if (!s)
    return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
  mutexLock(&g_lock);
  s->started = 1;
  mutexUnlock(&g_lock);
  return AAUDIO_OK;
}

int32_t AAudioStream_getSampleRate_fake(void *stream_ptr) {
  const MixStream *s = stream_ptr;
  return s ? s->rate : MIXER_RATE;
}

int32_t AAudioStream_getFramesPerBurst_fake(void *stream_ptr) {
  (void)stream_ptr;
  return AAUDIO_BURST_FRAMES;
}

/* Convert `frames` input frames into the ring at its tail. Lock held. */
static void push_frames(MixStream *s, const uint8_t *src, uint32_t frames) {
  uint32_t tail = (s->head + s->count) % s->capacity;
  for (uint32_t i = 0; i < frames; i++) {
    float l, r;
    if (s->format == AAUDIO_FORMAT_PCM_FLOAT) {
      const float *f = (const float *)src;
      l = f[0];
      r = s->channels > 1 ? f[1] : l;
    } else {
      int16_t a, b;
      memcpy(&a, src, sizeof(a));
      b = a;
      if (s->channels > 1)
        memcpy(&b, src + 2, sizeof(b));
      l = (float)a * (1.0f / 32768.0f);
      r = (float)b * (1.0f / 32768.0f);
    }
    s->ring[(size_t)tail * 2 + 0] = l;
    s->ring[(size_t)tail * 2 + 1] = r;
    tail = tail + 1 == s->capacity ? 0 : tail + 1;
    src += s->in_bytes_per_frame;
  }
  s->count += frames;
}

int32_t AAudioStream_write_fake(void *stream_ptr, const void *buffer,
                                int32_t frames, int64_t timeout_ns) {
  MixStream *s = stream_ptr;
  if (!s || frames < 0 || (frames > 0 && !buffer))
    return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
  if (frames == 0)
    return 0;

  const uint64_t start = armTicksToNs(armGetSystemTick());
  const uint8_t *src = buffer;
  int32_t written = 0;
  int32_t result = 0;

  mutexLock(&g_lock);
  s->writers++;
  while (written < frames) {
    if (s->closing || !g_device) {
      result = AAUDIO_ERROR_DISCONNECTED;
      break;
    }
    const uint32_t space = s->capacity - s->count;
    if (space == 0) {
      if (timeout_ns <= 0)
        break;
      const uint64_t elapsed = armTicksToNs(armGetSystemTick()) - start;
      if ((int64_t)elapsed >= timeout_ns)
        break;
      uint64_t wait = (uint64_t)timeout_ns - elapsed;
      if (wait > 20000000ull)
        wait = 20000000ull; /* re-check closing/device periodically */
      condvarWaitTimeout(&g_space_cv, &g_lock, wait);
      continue;
    }
    uint32_t n = (uint32_t)(frames - written);
    if (n > space)
      n = space;
    push_frames(s, src, n);
    src += (size_t)n * (size_t)s->in_bytes_per_frame;
    written += (int32_t)n;
  }
  s->writers--;
  if (s->closing)
    condvarWakeAll(&g_space_cv);
  mutexUnlock(&g_lock);

  /* A partial write is reported as success, like AAudio does. */
  return written > 0 ? written : result;
}

int AAudioStream_close_fake(void *stream_ptr) {
  MixStream *s = stream_ptr;
  if (!s)
    return AAUDIO_OK;
  mutexLock(&g_lock);
  s->closing = 1;
  condvarWakeAll(&g_space_cv);
  /* A writer on another thread may still be inside write(); let it leave. */
  while (s->writers > 0)
    condvarWaitTimeout(&g_space_cv, &g_lock, 5000000ull);
  for (int i = 0; i < MAX_STREAMS; i++)
    if (g_streams[i] == s)
      g_streams[i] = NULL;
  const uint64_t underruns = s->underruns;
  mutexUnlock(&g_lock);
  debugPrintf("audio: stream closed (%llu underruns)\n",
              (unsigned long long)underruns);
  free(s->ring);
  free(s);
  return AAUDIO_OK;
}
