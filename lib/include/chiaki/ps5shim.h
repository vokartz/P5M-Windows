/* ps5shim.h — flat C interface over the chiaki engine for ps5-rs.
 * Rust sees only this header: opaque handle, fixed-size POD structs, callbacks.
 * Lives in P5M-Windows/lib (branch engine-dll) as lib/include/chiaki/ps5shim.h;
 * this copy is the design draft. SPDX-License-Identifier: AGPL-3.0-only */
#ifndef PS5SHIM_H
#define PS5SHIM_H

#include <stdint.h>

#ifdef _WIN32
#  ifdef PS5SHIM_BUILD
#    define PS5SHIM_API __declspec(dllexport)
#  else
#    define PS5SHIM_API __declspec(dllimport)
#  endif
#else
#  define PS5SHIM_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PS5SHIM_VERSION 2

typedef struct Ps5Session Ps5Session;

/* ---- connect ---- */
typedef struct {
	int32_t ps5;                 /* 1 = PS5 */
	const char *host;            /* LAN address; NULL when connecting over PSN */
	uint8_t regist_key[16];      /* zero padded */
	uint8_t morning[16];
	const char *duid_hex;        /* PSN: console DUID as hex; NULL for LAN */
	const char *psn_token;       /* PSN: OAuth2 access token (never logged) */
	uint8_t psn_account_id[8];
	uint32_t width, height, fps, bitrate_kbps;
	int32_t codec;               /* 0 H264, 1 H265, 2 H265 HDR */
	const char *hw_decoder;      /* "d3d11va" or NULL for software */
	double packet_loss_max;
	int32_t port_guessing;       /* PSN option, 0/1 */
	int32_t log_level_mask;      /* chiaki log mask, 0 = default (INFO|WARN|ERR) */
	int32_t zero_copy;           /* with d3d11va: convert on the GPU to an RGBA texture (PS5_PIX_D3D11_RGBA), no copy to RAM */
} Ps5ConnectInfo;

/* ---- events ---- */
enum {
	PS5_EV_LOG = 0,          /* a = level, msg = text */
	PS5_EV_HOLEPUNCH = 1,    /* a = 1 finished */
	PS5_EV_CONNECTED = 2,
	PS5_EV_PIN_REQUEST = 3,  /* a = 1 previous pin incorrect */
	PS5_EV_QUIT = 4,         /* a = ChiakiQuitReason, b = is error, msg = reason */
	PS5_EV_RUMBLE = 5,       /* a = left (low), b = right (high) */
	PS5_EV_AUDIO_FORMAT = 6, /* a = channels, b = rate */
};
typedef void (*Ps5EventCb)(int32_t type, int32_t a, int32_t b, const char *msg, void *user);

/* ---- video ---- */
/* PS5_PIX_D3D11_RGBA: data[0] = ID3D11Texture2D* (RGBA8, full range, owned by the session,
 * one of PS5_D3D11_RING), data[1] = its ID3D11Device*. Register it with WGL_NV_DX_interop2. */
enum { PS5_PIX_YUV420P = 0, PS5_PIX_NV12 = 1, PS5_PIX_D3D11_RGBA = 2 };
#define PS5_D3D11_RING 4
typedef struct {
	int32_t width, height, format;
	const uint8_t *data[3];
	int32_t linesize[3];
	int32_t frames_lost;     /* since previous frame */
	int32_t recovered;
	uint64_t decoded_us;     /* monotonic time the decoder returned the frame */
	uint32_t transfer_us;    /* GPU->RAM copy (or GPU convert submit for D3D11_RGBA) time, 0 for sw */
} Ps5Frame;
/* Called on the decoder thread. Planes are valid only during the call: copy/upload them. */
typedef void (*Ps5FrameCb)(const Ps5Frame *frame, void *user);

/* ---- audio: interleaved s16, rate/channels from PS5_EV_AUDIO_FORMAT ---- */
typedef void (*Ps5AudioCb)(const int16_t *pcm, uint32_t frames, void *user);

/* ---- controller: same layout as ChiakiControllerState, minus engine internals ---- */
typedef struct {
	uint32_t buttons;            /* ChiakiControllerButton bits */
	uint8_t l2, r2;
	int16_t lx, ly, rx, ry;
	/* touches: id -1 = up; x 0..1919, y 0..1079 (PS5 touchpad) */
	int8_t touch_id[2];
	uint16_t touch_x[2], touch_y[2];
	float gyro[3], accel[3], orient[4];
} Ps5Pad;

/* ---- stats, polled ---- */
typedef struct {
	double measured_mbps;
	double target_mbps;      /* console encoder target */
	double rtt_ms;
	double packet_loss;      /* 0..1 */
	int32_t frames_lost_total;
} Ps5Stats;

PS5SHIM_API int32_t ps5_version(void);
PS5SHIM_API const char *ps5_error_string(int32_t err);

/* Creates the session. On PSN this also does the hole punch (blocking, seconds):
 * call it off the UI thread. Returns NULL on failure; reason via event cb (PS5_EV_LOG). */
PS5SHIM_API Ps5Session *ps5_session_create(const Ps5ConnectInfo *info,
		Ps5EventCb ev, Ps5FrameCb frame, Ps5AudioCb audio, void *user);
PS5SHIM_API int32_t ps5_session_start(Ps5Session *s);
PS5SHIM_API void ps5_session_stop(Ps5Session *s);      /* async; QUIT event follows */
PS5SHIM_API void ps5_session_destroy(Ps5Session *s);   /* joins threads */
PS5SHIM_API void ps5_session_cancel_holepunch(Ps5Session *s);

PS5SHIM_API int32_t ps5_session_set_pad(Ps5Session *s, const Ps5Pad *pad);
PS5SHIM_API int32_t ps5_session_set_pin(Ps5Session *s, const char *pin);
PS5SHIM_API int32_t ps5_session_request_idr(Ps5Session *s);
PS5SHIM_API int32_t ps5_session_goto_bed(Ps5Session *s);
PS5SHIM_API void ps5_session_stats(Ps5Session *s, Ps5Stats *out);

/* PSN console list (needs a valid token). Returns count written, <0 on error. */
typedef struct {
	char name[32];
	char duid_hex[65];
	int32_t ps5;
	int32_t remoteplay_enabled;
} Ps5Device;
PS5SHIM_API int32_t ps5_psn_list_devices(const char *psn_token, int32_t ps5, Ps5Device *out, int32_t max);

#ifdef __cplusplus
}
#endif
#endif
