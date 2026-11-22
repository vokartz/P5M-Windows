/* shim.c — implementation of ps5shim.h over the chiaki engine.
 * Built into chiaki.dll (target chiaki-engine) with PS5SHIM_BUILD defined.
 * Only ps5_* symbols are exported (CHIAKI_EXPORT is empty).
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef PS5SHIM_BUILD
#define PS5SHIM_BUILD
#endif
#include <chiaki/ps5shim.h>

#include <chiaki/session.h>
#include <chiaki/ffmpegdecoder.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/remote/holepunch.h>
#include <chiaki/time.h>

#include <libavutil/hwcontext.h>
#include <libavutil/frame.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Ps5Session {
	ChiakiLog log;
	ChiakiSession session;
	ChiakiFfmpegDecoder decoder;
	ChiakiOpusDecoder opus;
	ChiakiHolepunchSession holepunch;
	AVFrame *sw_frame;           /* reused for hw -> RAM transfer */
	Ps5EventCb ev;
	Ps5FrameCb frame;
	Ps5AudioCb audio;
	void *user;
	uint32_t audio_channels;
	int session_inited, decoder_inited, opus_inited;
};

static void log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
	Ps5Session *s = user;
	if(s->ev)
		s->ev(PS5_EV_LOG, (int32_t)level, 0, msg, s->user);
}

/* ---- video ---- */
static void frame_available_cb(ChiakiFfmpegDecoder *dec, void *user)
{
	Ps5Session *s = user;
	int32_t lost = 0;
	ChiakiFfmpegFrame f = chiaki_ffmpeg_decoder_pull_frame(dec, &lost);
	if(!f.frame)
		return;
	uint64_t t0 = chiaki_time_now_monotonic_us();
	AVFrame *src = f.frame;
	uint32_t transfer_us = 0;
	if(src->hw_frames_ctx)
	{
		if(!s->sw_frame)
			s->sw_frame = av_frame_alloc();
		av_frame_unref(s->sw_frame);
		if(!s->sw_frame || av_hwframe_transfer_data(s->sw_frame, src, 0) < 0)
		{
			CHIAKI_LOGE(&s->log, "shim: hw frame transfer failed");
			av_frame_free(&f.frame);
			return;
		}
		src = s->sw_frame;
		transfer_us = (uint32_t)(chiaki_time_now_monotonic_us() - t0);
	}
	Ps5Frame out = {0};
	out.width = src->width;
	out.height = src->height;
	if(src->format == AV_PIX_FMT_NV12)
		out.format = PS5_PIX_NV12;
	else if(src->format == AV_PIX_FMT_YUV420P || src->format == AV_PIX_FMT_YUVJ420P)
		out.format = PS5_PIX_YUV420P;
	else
	{
		CHIAKI_LOGE(&s->log, "shim: unsupported pixel format %d", src->format);
		av_frame_free(&f.frame);
		return;
	}
	for(int i = 0; i < 3; i++)
	{
		out.data[i] = src->data[i];
		out.linesize[i] = src->linesize[i];
	}
	out.frames_lost = lost;
	out.recovered = f.recovered;
	out.decoded_us = t0;
	out.transfer_us = transfer_us;
	if(s->frame)
		s->frame(&out, s->user);
	av_frame_free(&f.frame);
}

/* ---- audio ---- */
static void audio_settings_cb(uint32_t channels, uint32_t rate, void *user)
{
	Ps5Session *s = user;
	s->audio_channels = channels;
	if(s->ev)
		s->ev(PS5_EV_AUDIO_FORMAT, (int32_t)channels, (int32_t)rate, NULL, s->user);
}

static void audio_frame_cb(int16_t *buf, size_t samples_count, void *user)
{
	Ps5Session *s = user;
	if(s->audio)
		s->audio(buf, (uint32_t)samples_count, s->user);
}

/* ---- session events ---- */
static void event_cb(ChiakiEvent *e, void *user)
{
	Ps5Session *s = user;
	if(!s->ev)
		return;
	switch(e->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			s->ev(PS5_EV_CONNECTED, 0, 0, NULL, s->user);
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			s->ev(PS5_EV_PIN_REQUEST, e->login_pin_request.pin_incorrect, 0, NULL, s->user);
			break;
		case CHIAKI_EVENT_HOLEPUNCH:
			s->ev(PS5_EV_HOLEPUNCH, e->data_holepunch.finished, 0, NULL, s->user);
			break;
		case CHIAKI_EVENT_QUIT:
			s->ev(PS5_EV_QUIT, (int32_t)e->quit.reason, chiaki_quit_reason_is_error(e->quit.reason),
					e->quit.reason_str ? e->quit.reason_str : chiaki_quit_reason_string(e->quit.reason), s->user);
			break;
		case CHIAKI_EVENT_RUMBLE:
			s->ev(PS5_EV_RUMBLE, e->rumble.left, e->rumble.right, NULL, s->user);
			break;
		default:
			break;
	}
}

/* ---- PSN ---- */
static int hex_to_bytes(const char *hex, uint8_t *out, size_t max)
{
	size_t n = strlen(hex);
	if(n % 2 || n / 2 > max)
		return -1;
	for(size_t i = 0; i < n / 2; i++)
	{
		unsigned v;
		if(sscanf(hex + 2 * i, "%2x", &v) != 1)
			return -1;
		out[i] = (uint8_t)v;
	}
	return (int)(n / 2);
}

static ChiakiErrorCode psn_connect(Ps5Session *s, const Ps5ConnectInfo *info)
{
	s->holepunch = chiaki_holepunch_session_init(info->psn_token, &s->log);
	if(!s->holepunch)
		return CHIAKI_ERR_MEMORY;
	chiaki_holepunch_session_force_port_guessing(s->holepunch, info->port_guessing != 0);

	uint8_t duid[64];
	int duid_len = hex_to_bytes(info->duid_hex, duid, sizeof(duid));
	if(duid_len <= 0)
		return CHIAKI_ERR_INVALID_DATA;
	ChiakiHolepunchConsoleType type = info->ps5 ? CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5 : CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4;

	ChiakiErrorCode err;
	if((err = chiaki_holepunch_upnp_discover(s->holepunch)) != CHIAKI_ERR_SUCCESS) return err;
	if((err = chiaki_holepunch_session_create(s->holepunch)) != CHIAKI_ERR_SUCCESS) return err;
	if((err = holepunch_session_create_offer(s->holepunch)) != CHIAKI_ERR_SUCCESS) return err;
	if((err = chiaki_holepunch_session_start(s->holepunch, duid, type)) != CHIAKI_ERR_SUCCESS) return err;
	return chiaki_holepunch_session_punch_hole(s->holepunch, CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL);
}

/* ---- API ---- */
PS5SHIM_API int32_t ps5_version(void) { return PS5SHIM_VERSION; }

PS5SHIM_API const char *ps5_error_string(int32_t err) { return chiaki_error_string((ChiakiErrorCode)err); }

PS5SHIM_API Ps5Session *ps5_session_create(const Ps5ConnectInfo *info,
		Ps5EventCb ev, Ps5FrameCb frame, Ps5AudioCb audio, void *user)
{
	static int lib_inited = 0;
	if(!lib_inited)
	{
		if(chiaki_lib_init() != CHIAKI_ERR_SUCCESS)
			return NULL;
		lib_inited = 1;
	}
	Ps5Session *s = calloc(1, sizeof(*s));
	if(!s)
		return NULL;
	s->ev = ev; s->frame = frame; s->audio = audio; s->user = user;
	uint32_t mask = info->log_level_mask ? (uint32_t)info->log_level_mask
		: (CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR);
	chiaki_log_init(&s->log, mask, log_cb, s);

	const int psn = info->duid_hex && info->duid_hex[0];
	ChiakiConnectInfo ci;
	memset(&ci, 0, sizeof(ci));
	ci.ps5 = info->ps5 != 0;
	ci.host = info->host;
	memcpy(ci.regist_key, info->regist_key, sizeof(ci.regist_key));
	memcpy(ci.morning, info->morning, sizeof(ci.morning));
	ci.video_profile.width = info->width;
	ci.video_profile.height = info->height;
	ci.video_profile.max_fps = info->fps;
	ci.video_profile.bitrate = info->bitrate_kbps;
	ci.video_profile.codec = (ChiakiCodec)info->codec;
	ci.video_profile_auto_downgrade = true;
	ci.enable_keyboard = false;
	ci.enable_dualsense = false;
	ci.packet_loss_max = info->packet_loss_max > 0 ? info->packet_loss_max : 0.05;
	ci.enable_idr_on_fec_failure = true;

	ChiakiErrorCode err = chiaki_ffmpeg_decoder_init(&s->decoder, &s->log, ci.video_profile.codec,
			info->fps, info->hw_decoder, NULL, frame_available_cb, s);
	if(err != CHIAKI_ERR_SUCCESS)
		goto fail;
	s->decoder_inited = 1;

	chiaki_opus_decoder_init(&s->opus, &s->log);
	chiaki_opus_decoder_set_cb(&s->opus, audio_settings_cb, audio_frame_cb, s);
	s->opus_inited = 1;

	if(psn)
	{
		memcpy(ci.psn_account_id, info->psn_account_id, CHIAKI_PSN_ACCOUNT_ID_SIZE);
		err = psn_connect(s, info);   /* hole punch before session init, as P5M does */
		if(err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(&s->log, "shim: PSN connection failed: %s", chiaki_error_string(err));
			goto fail;
		}
		ci.holepunch_session = s->holepunch;
	}

	err = chiaki_session_init(&s->session, &ci, &s->log);
	if(err != CHIAKI_ERR_SUCCESS)
		goto fail;
	s->session_inited = 1;
	s->holepunch = NULL; /* owned by the session now */

	ChiakiAudioSink sink;
	chiaki_opus_decoder_get_sink(&s->opus, &sink);
	chiaki_session_set_audio_sink(&s->session, &sink);
	chiaki_session_set_video_sample_cb(&s->session, chiaki_ffmpeg_decoder_video_sample_cb, &s->decoder);
	chiaki_session_set_event_cb(&s->session, event_cb, s);
	return s;

fail:
	ps5_session_destroy(s);
	return NULL;
}

PS5SHIM_API int32_t ps5_session_start(Ps5Session *s) { return chiaki_session_start(&s->session); }

PS5SHIM_API void ps5_session_stop(Ps5Session *s) { if(s->session_inited) chiaki_session_stop(&s->session); }

PS5SHIM_API void ps5_session_cancel_holepunch(Ps5Session *s)
{
	ChiakiHolepunchSession hp = s->session_inited ? s->session.holepunch_session : s->holepunch;
	if(hp)
		chiaki_holepunch_main_thread_cancel(hp, true);
}

PS5SHIM_API void ps5_session_destroy(Ps5Session *s)
{
	if(!s)
		return;
	if(s->session_inited)
	{
		chiaki_session_join(&s->session);
		chiaki_session_fini(&s->session);
	}
	else if(s->holepunch)
		chiaki_holepunch_session_fini(s->holepunch);
	if(s->opus_inited)
		chiaki_opus_decoder_fini(&s->opus);
	if(s->decoder_inited)
		chiaki_ffmpeg_decoder_fini(&s->decoder);
	av_frame_free(&s->sw_frame);
	free(s);
}

PS5SHIM_API int32_t ps5_session_set_pad(Ps5Session *s, const Ps5Pad *p)
{
	ChiakiControllerState st;
	chiaki_controller_state_set_idle(&st);
	st.buttons = p->buttons;
	st.l2_state = p->l2;
	st.r2_state = p->r2;
	st.left_x = p->lx; st.left_y = p->ly;
	st.right_x = p->rx; st.right_y = p->ry;
	for(int i = 0; i < 2; i++)
	{
		st.touches[i].id = p->touch_id[i];
		st.touches[i].x = p->touch_x[i];
		st.touches[i].y = p->touch_y[i];
	}
	st.gyro_x = p->gyro[0]; st.gyro_y = p->gyro[1]; st.gyro_z = p->gyro[2];
	st.accel_x = p->accel[0]; st.accel_y = p->accel[1]; st.accel_z = p->accel[2];
	st.orient_x = p->orient[0]; st.orient_y = p->orient[1];
	st.orient_z = p->orient[2]; st.orient_w = p->orient[3];
	return chiaki_session_set_controller_state(&s->session, &st);
}

PS5SHIM_API int32_t ps5_session_set_pin(Ps5Session *s, const char *pin)
{
	return chiaki_session_set_login_pin(&s->session, (const uint8_t *)pin, strlen(pin));
}

PS5SHIM_API int32_t ps5_session_request_idr(Ps5Session *s) { return chiaki_session_request_idr(&s->session); }

PS5SHIM_API int32_t ps5_session_goto_bed(Ps5Session *s) { return chiaki_session_goto_bed(&s->session); }

PS5SHIM_API void ps5_session_stats(Ps5Session *s, Ps5Stats *out)
{
	memset(out, 0, sizeof(*out));
	if(!s->session_inited)
		return;
	/* Read without locks: doubles/ints written by engine threads; a torn read only skews one log line. */
	ChiakiStreamConnection *sc = &s->session.stream_connection;
	out->measured_mbps = sc->measured_bitrate;
	out->target_mbps = sc->quality_target_bitrate / 1e6;
	out->rtt_ms = sc->quality_rtt_ms;
	out->packet_loss = sc->congestion_control.packet_loss;
	if(sc->video_receiver)
		out->frames_lost_total = chiaki_video_receiver_get_frames_lost_total(sc->video_receiver);
}

PS5SHIM_API int32_t ps5_psn_list_devices(const char *psn_token, int32_t ps5, Ps5Device *out, int32_t max)
{
	ChiakiLog log;
	chiaki_log_init(&log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING, NULL, NULL);
	ChiakiHolepunchDeviceInfo *devs = NULL;
	size_t count = 0;
	ChiakiErrorCode err = chiaki_holepunch_list_devices(psn_token,
			ps5 ? CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5 : CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4, &devs, &count, &log);
	if(err != CHIAKI_ERR_SUCCESS)
		return -(int32_t)err;
	int32_t n = 0;
	for(size_t i = 0; i < count && n < max; i++, n++)
	{
		memset(&out[n], 0, sizeof(out[n]));
		snprintf(out[n].name, sizeof(out[n].name), "%.31s", devs[i].device_name);
		for(int b = 0; b < 32; b++)
			snprintf(out[n].duid_hex + 2 * b, 3, "%02x", devs[i].device_uid[b]);
		out[n].ps5 = devs[i].type == CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5;
		out[n].remoteplay_enabled = devs[i].remoteplay_enabled;
	}
	chiaki_holepunch_free_device_list(&devs);
	return n;
}
