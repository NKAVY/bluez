/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2009  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <assert.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sco.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "logging.h"
#include "device.h"
#include "manager.h"
#include "error.h"
#include "telephony.h"
#include "headset.h"
#include "glib-helper.h"
#include "btio.h"
#include "dbus-common.h"
#include "../src/adapter.h"
#include "../src/device.h"

#define DC_TIMEOUT 3

#define RING_INTERVAL 3

#define BUF_SIZE 1024

#define HEADSET_GAIN_SPEAKER 'S'
#define HEADSET_GAIN_MICROPHONE 'M'

static struct {
	gboolean telephony_ready;	/* Telephony plugin initialized */
	uint32_t features;		/* HFP AG features */
	const struct indicator *indicators;	/* Available HFP indicators */
	int er_mode;			/* Event reporting mode */
	int er_ind;			/* Event reporting for indicators */
	int rh;				/* Response and Hold state */
	char *number;			/* Incoming phone number */
	int number_type;		/* Incoming number type */
	guint ring_timer;		/* For incoming call indication */
	const char *chld;		/* Response to AT+CHLD=? */
} ag = {
	.telephony_ready = FALSE,
	.features = 0,
	.er_mode = 3,
	.er_ind = 0,
	.rh = -1,
	.number = NULL,
	.number_type = 0,
	.ring_timer = 0,
};

static gboolean sco_hci = TRUE;

static GSList *active_devices = NULL;

static char *str_state[] = {
	"HEADSET_STATE_DISCONNECTED",
	"HEADSET_STATE_CONNECT_IN_PROGRESS",
	"HEADSET_STATE_CONNECTED",
	"HEADSET_STATE_PLAY_IN_PROGRESS",
	"HEADSET_STATE_PLAYING",
};

struct connect_cb {
	unsigned int id;
	headset_stream_cb_t cb;
	void *cb_data;
};

struct pending_connect {
	DBusMessage *msg;
	DBusPendingCall *call;
	GIOChannel *io;
	int err;
	headset_state_t target_state;
	GSList *callbacks;
};

struct headset {
	uint32_t hsp_handle;
	uint32_t hfp_handle;

	int rfcomm_ch;

	GIOChannel *rfcomm;
	GIOChannel *tmp_rfcomm;
	GIOChannel *sco;
	guint sco_id;

	gboolean auto_dc;

	guint dc_timer;

	char buf[BUF_SIZE];
	int data_start;
	int data_length;

	gboolean hfp_active;
	gboolean search_hfp;
	gboolean cli_active;
	gboolean cme_enabled;
	gboolean cwa_enabled;
	gboolean pending_ring;
	gboolean nrec;
	gboolean nrec_req;

	headset_state_t state;
	struct pending_connect *pending;

	int sp_gain;
	int mic_gain;

	unsigned int hf_features;
	headset_lock_t lock;
};

struct event {
	const char *cmd;
	int (*callback) (struct audio_device *device, const char *buf);
};

static inline DBusMessage *invalid_args(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".InvalidArguments",
			"Invalid arguments in method call");
}

static DBusHandlerResult error_not_supported(DBusConnection *conn,
							DBusMessage *msg)
{
	return error_common_reply(conn, msg, ERROR_INTERFACE ".NotSupported",
							"Not supported");
}

static DBusHandlerResult error_connection_attempt_failed(DBusConnection *conn,
						DBusMessage *msg, int err)
{
	return error_common_reply(conn, msg,
			ERROR_INTERFACE ".ConnectionAttemptFailed",
			err > 0 ? strerror(err) : "Connection attempt failed");
}

static int rfcomm_connect(struct audio_device *device, headset_stream_cb_t cb,
				void *user_data, unsigned int *cb_id);
static int get_records(struct audio_device *device, headset_stream_cb_t cb,
			void *user_data, unsigned int *cb_id);

static void print_ag_features(uint32_t features)
{
	GString *gstr;
	char *str;

	if (features == 0) {
		debug("HFP AG features: (none)");
		return;
	}

	gstr = g_string_new("HFP AG features: ");

	if (features & AG_FEATURE_THREE_WAY_CALLING)
		g_string_append(gstr, "\"Three-way calling\" ");
	if (features & AG_FEATURE_EC_ANDOR_NR)
		g_string_append(gstr, "\"EC and/or NR function\" ");
	if (features & AG_FEATURE_VOICE_RECOGNITION)
		g_string_append(gstr, "\"Voice recognition function\" ");
	if (features & AG_FEATURE_INBAND_RINGTONE)
		g_string_append(gstr, "\"In-band ring tone capability\" ");
	if (features & AG_FEATURE_ATTACH_NUMBER_TO_VOICETAG)
		g_string_append(gstr, "\"Attach a number to a voice tag\" ");
	if (features & AG_FEATURE_REJECT_A_CALL)
		g_string_append(gstr, "\"Ability to reject a call\" ");
	if (features & AG_FEATURE_ENHANCED_CALL_STATUS)
		g_string_append(gstr, "\"Enhanced call status\" ");
	if (features & AG_FEATURE_ENHANCED_CALL_CONTROL)
		g_string_append(gstr, "\"Enhanced call control\" ");
	if (features & AG_FEATURE_EXTENDED_ERROR_RESULT_CODES)
		g_string_append(gstr, "\"Extended Error Result Codes\" ");

	str = g_string_free(gstr, FALSE);

	debug("%s", str);

	g_free(str);
}

static void print_hf_features(uint32_t features)
{
	GString *gstr;
	char *str;

	if (features == 0) {
		debug("HFP HF features: (none)");
		return;
	}

	gstr = g_string_new("HFP HF features: ");

	if (features & HF_FEATURE_EC_ANDOR_NR)
		g_string_append(gstr, "\"EC and/or NR function\" ");
	if (features & HF_FEATURE_CALL_WAITING_AND_3WAY)
		g_string_append(gstr, "\"Call waiting and 3-way calling\" ");
	if (features & HF_FEATURE_CLI_PRESENTATION)
		g_string_append(gstr, "\"CLI presentation capability\" ");
	if (features & HF_FEATURE_VOICE_RECOGNITION)
		g_string_append(gstr, "\"Voice recognition activation\" ");
	if (features & HF_FEATURE_REMOTE_VOLUME_CONTROL)
		g_string_append(gstr, "\"Remote volume control\" ");
	if (features & HF_FEATURE_ENHANCED_CALL_STATUS)
		g_string_append(gstr, "\"Enhanced call status\" ");
	if (features & HF_FEATURE_ENHANCED_CALL_CONTROL)
		g_string_append(gstr, "\"Enhanced call control\" ");

	str = g_string_free(gstr, FALSE);

	debug("%s", str);

	g_free(str);
}

static int headset_send_valist(struct headset *hs, char *format, va_list ap)
{
	char rsp[BUF_SIZE];
	ssize_t total_written, written, count;
	int fd;

	count = vsnprintf(rsp, sizeof(rsp), format, ap);

	if (count < 0)
		return -EINVAL;

	if (!hs->rfcomm) {
		error("headset_send: the headset is not connected");
		return -EIO;
	}

	written = total_written = 0;
	fd = g_io_channel_unix_get_fd(hs->rfcomm);

	while (total_written < count) {
		written = write(fd, rsp + total_written, count - total_written);
		if (written < 0)
			return -errno;

		total_written += written;
	}

	return 0;
}

static int headset_send(struct headset *hs, char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = headset_send_valist(hs, format, ap);
	va_end(ap);

	return ret;
}

static int supported_features(struct audio_device *device, const char *buf)
{
	struct headset *hs = device->headset;
	int err;

	if (strlen(buf) < 9)
		return -EINVAL;

	hs->hf_features = strtoul(&buf[8], NULL, 10);

	print_hf_features(hs->hf_features);

	err = headset_send(hs, "\r\n+BRSF: %u\r\n", ag.features);
	if (err < 0)
		return err;

	return headset_send(hs, "\r\nOK\r\n");
}

static char *indicator_ranges(const struct indicator *indicators)
{
	int i;
	GString *gstr;

	gstr = g_string_new("\r\n+CIND: ");

	for (i = 0; indicators[i].desc != NULL; i++) {
		if (i == 0)
			g_string_append_printf(gstr, "(\"%s\",(%s))",
						indicators[i].desc,
						indicators[i].range);
		else
			g_string_append_printf(gstr, ",(\"%s\",(%s))",
						indicators[i].desc,
						indicators[i].range);
	}

	g_string_append(gstr, "\r\n");

	return g_string_free(gstr, FALSE);
}

static char *indicator_values(const struct indicator *indicators)
{
	int i;
	GString *gstr;

	gstr = g_string_new("\r\n+CIND: ");

	for (i = 0; indicators[i].desc != NULL; i++) {
		if (i == 0)
			g_string_append_printf(gstr, "%d", indicators[i].val);
		else
			g_string_append_printf(gstr, ",%d", indicators[i].val);
	}

	g_string_append(gstr, "\r\n");

	return g_string_free(gstr, FALSE);
}

static int report_indicators(struct audio_device *device, const char *buf)
{
	struct headset *hs = device->headset;
	int err;
	char *str;

	if (strlen(buf) < 8)
		return -EINVAL;

	if (buf[7] == '=')
		str = indicator_ranges(ag.indicators);
	else
		str = indicator_values(ag.indicators);

	err = headset_send(hs, str);

	g_free(str);

	if (err < 0)
		return err;

	return headset_send(hs, "\r\nOK\r\n");
}

static void pending_connect_complete(struct connect_cb *cb, struct audio_device *dev)
{
	struct headset *hs = dev->headset;

	if (hs->pending->err)
		cb->cb(NULL, cb->cb_data);
	else
		cb->cb(dev, cb->cb_data);
}

static void pending_connect_finalize(struct audio_device *dev)
{
	struct headset *hs = dev->headset;
	struct pending_connect *p = hs->pending;

	g_slist_foreach(p->callbacks, (GFunc) pending_connect_complete, dev);

	g_slist_foreach(p->callbacks, (GFunc) g_free, NULL);
	g_slist_free(p->callbacks);

	if (p->io) {
		g_io_channel_close(p->io);
		g_io_channel_unref(p->io);
	}

	if (p->msg)
		dbus_message_unref(p->msg);

	if (p->call) {
		dbus_pending_call_cancel(p->call);
		dbus_pending_call_unref(p->call);
	}

	g_free(p);

	hs->pending = NULL;
}

static void pending_connect_init(struct headset *hs, headset_state_t target_state)
{
	if (hs->pending) {
		if (hs->pending->target_state < target_state)
			hs->pending->target_state = target_state;
		return;
	}

	hs->pending = g_new0(struct pending_connect, 1);
	hs->pending->target_state = target_state;
}

static unsigned int connect_cb_new(struct headset *hs,
					headset_state_t target_state,
					headset_stream_cb_t func,
					void *user_data)
{
	struct connect_cb *cb;
	unsigned int free_cb_id = 1;

	pending_connect_init(hs, target_state);

	cb = g_new(struct connect_cb, 1);

	cb->cb = func;
	cb->cb_data = user_data;
	cb->id = free_cb_id++;

	hs->pending->callbacks = g_slist_append(hs->pending->callbacks,
						cb);

	return cb->id;
}

static void send_foreach_headset(GSList *devices,
					int (*cmp) (struct headset *hs),
					char *format, ...)
{
	GSList *l;
	va_list ap;

	for (l = devices; l != NULL; l = l->next) {
		struct audio_device *device = l->data;
		struct headset *hs = device->headset;
		int ret;

		assert(hs != NULL);

		if (cmp && cmp(hs) != 0)
			continue;

		va_start(ap, format);
		ret = headset_send_valist(hs, format, ap);
		if (ret < 0)
			error("Failed to send to headset: %s (%d)",
					strerror(-ret), -ret);
		va_end(ap);
	}
}

static int cli_cmp(struct headset *hs)
{
	if (!hs->hfp_active)
		return -1;

	if (hs->cli_active)
		return 0;
	else
		return -1;
}

static gboolean ring_timer_cb(gpointer data)
{
	send_foreach_headset(active_devices, NULL, "\r\nRING\r\n");

	if (ag.number)
		send_foreach_headset(active_devices, cli_cmp,
					"\r\n+CLIP: \"%s\",%d\r\n",
					ag.number, ag.number_type);

	return TRUE;
}

static void sco_connect_cb(GIOChannel *chan, GError *err, gpointer user_data)
{
	int sk;
	struct audio_device *dev = user_data;
	struct headset *hs = dev->headset;
	struct pending_connect *p = hs->pending;

	if (err) {
		error("%s", err->message);

		if (p->msg)
			error_connection_attempt_failed(dev->conn, p->msg, p->err);

		pending_connect_finalize(dev);
		if (hs->rfcomm)
			headset_set_state(dev, HEADSET_STATE_CONNECTED);
		else
			headset_set_state(dev, HEADSET_STATE_DISCONNECTED);

		return;
	}

	debug("SCO socket opened for headset %s", dev->path);

	sk = g_io_channel_unix_get_fd(chan);

	debug("SCO fd=%d", sk);
	hs->sco = chan;
	p->io = NULL;

	if (p->msg) {
		DBusMessage *reply = dbus_message_new_method_return(p->msg);
		g_dbus_send_message(dev->conn, reply);
	}

	pending_connect_finalize(dev);

	fcntl(sk, F_SETFL, 0);

	headset_set_state(dev, HEADSET_STATE_PLAYING);

	if (hs->pending_ring) {
		ring_timer_cb(NULL);
		ag.ring_timer = g_timeout_add_seconds(RING_INTERVAL,
						ring_timer_cb,
						NULL);
		hs->pending_ring = FALSE;
	}
}

static int sco_connect(struct audio_device *dev, headset_stream_cb_t cb,
			void *user_data, unsigned int *cb_id)
{
	struct headset *hs = dev->headset;
	GError *err = NULL;
	GIOChannel *io;

	if (hs->state != HEADSET_STATE_CONNECTED)
		return -EINVAL;

	io = bt_io_connect(BT_IO_SCO, sco_connect_cb, dev, NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR, &dev->src,
				BT_IO_OPT_DEST_BDADDR, &dev->dst,
				BT_IO_OPT_INVALID);
	if (!io) {
		error("%s", err->message);
		g_error_free(err);
		return -EIO;
	}

	g_io_channel_unref(io);

	headset_set_state(dev, HEADSET_STATE_PLAY_IN_PROGRESS);

	pending_connect_init(hs, HEADSET_STATE_PLAYING);

	if (cb) {
		unsigned int id = connect_cb_new(hs, HEADSET_STATE_PLAYING,
							cb, user_data);
		if (cb_id)
			*cb_id = id;
	}

	return 0;
}

static int hfp_cmp(struct headset *hs)
{
	if (hs->hfp_active)
		return 0;
	else
		return -1;
}

static void hfp_slc_complete(struct audio_device *dev)
{
	struct headset *hs = dev->headset;
	struct pending_connect *p = hs->pending;

	debug("HFP Service Level Connection established");

	headset_set_state(dev, HEADSET_STATE_CONNECTED);

	if (p == NULL)
		return;

	if (p->target_state == HEADSET_STATE_CONNECTED) {
		if (p->msg) {
			DBusMessage *reply = dbus_message_new_method_return(p->msg);
			g_dbus_send_message(dev->conn, reply);
		}
		pending_connect_finalize(dev);
		return;
	}

	p->err = sco_connect(dev, NULL, NULL, NULL);
	if (p->err < 0) {
		if (p->msg)
			error_connection_attempt_failed(dev->conn, p->msg, p->err);
		pending_connect_finalize(dev);
	}
}

static int telephony_generic_rsp(struct audio_device *device, cme_error_t err)
{
	struct headset *hs = device->headset;

	if (err != CME_ERROR_NONE) {
		if (hs->cme_enabled)
			return headset_send(hs, "\r\n+CME ERROR: %d\r\n", err);
		else
			return headset_send(hs, "\r\nERROR\r\n");
	}

	return headset_send(hs, "\r\nOK\r\n");
}

int telephony_event_reporting_rsp(void *telephony_device, cme_error_t err)
{
	struct audio_device *device = telephony_device;
	struct headset *hs = device->headset;
	int ret;

	if (err != CME_ERROR_NONE)
		return telephony_generic_rsp(telephony_device, err);

	ret = headset_send(hs, "\r\nOK\r\n");
	if (ret < 0)
		return ret;

	if (hs->state != HEADSET_STATE_CONNECT_IN_PROGRESS)
		return 0;

	if (hs->hf_features & HF_FEATURE_CALL_WAITING_AND_3WAY &&
			ag.features & AG_FEATURE_THREE_WAY_CALLING)
		return 0;

	hfp_slc_complete(device);

	return 0;
}

static int event_reporting(struct audio_device *dev, const char *buf)
{
	char **tokens; /* <mode>, <keyp>, <disp>, <ind>, <bfr> */

	if (strlen(buf) < 13)
		return -EINVAL;

	tokens = g_strsplit(&buf[8], ",", 5);
	if (g_strv_length(tokens) < 4) {
		g_strfreev(tokens);
		return -EINVAL;
	}

	ag.er_mode = atoi(tokens[0]);
	ag.er_ind = atoi(tokens[3]);

	g_strfreev(tokens);
	tokens = NULL;

	debug("Event reporting (CMER): mode=%d, ind=%d",
			ag.er_mode, ag.er_ind);

	switch (ag.er_ind) {
	case 0:
	case 1:
		telephony_event_reporting_req(dev, ag.er_ind);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int call_hold(struct audio_device *dev, const char *buf)
{
	struct headset *hs = dev->headset;
	int err;

	if (strlen(buf) < 9)
		return -EINVAL;

	if (buf[8] != '?') {
		telephony_call_hold_req(dev, &buf[8]);
		return 0;
	}

	err = headset_send(hs, "\r\n+CHLD: (%s)\r\n", ag.chld);
	if (err < 0)
		return err;

	err = headset_send(hs, "\r\nOK\r\n");
	if (err < 0)
		return err;

	if (hs->state != HEADSET_STATE_CONNECT_IN_PROGRESS)
		return 0;

	hfp_slc_complete(dev);

	return 0;
}

int telephony_key_press_rsp(void *telephony_device, cme_error_t err)
{
	return telephony_generic_rsp(telephony_device, err);
}

static int key_press(struct audio_device *device, const char *buf)
{
	if (strlen(buf) < 9)
		return -EINVAL;

	g_dbus_emit_signal(device->conn, device->path,
			AUDIO_HEADSET_INTERFACE, "AnswerRequested",
			DBUS_TYPE_INVALID);

	if (ag.ring_timer) {
		g_source_remove(ag.ring_timer);
		ag.ring_timer = 0;
	}

	telephony_key_press_req(device, &buf[8]);

	return 0;
}

int telephony_answer_call_rsp(void *telephony_device, cme_error_t err)
{
	return telephony_generic_rsp(telephony_device, err);
}

static int answer_call(struct audio_device *device, const char *buf)
{
	if (ag.ring_timer) {
		g_source_remove(ag.ring_timer);
		ag.ring_timer = 0;
	}

	if (ag.number) {
		g_free(ag.number);
		ag.number = NULL;
	}

	telephony_answer_call_req(device);

	return 0;
}

int telephony_terminate_call_rsp(void *telephony_device,
					cme_error_t err)
{
	struct audio_device *device = telephony_device;
	struct headset *hs = device->headset;

	if (err != CME_ERROR_NONE)
		return telephony_generic_rsp(telephony_device, err);

	g_dbus_emit_signal(device->conn, device->path,
			AUDIO_HEADSET_INTERFACE, "CallTerminated",
			DBUS_TYPE_INVALID);

	return headset_send(hs, "\r\nOK\r\n");
}

static int terminate_call(struct audio_device *device, const char *buf)
{
	if (ag.number) {
		g_free(ag.number);
		ag.number = NULL;
	}

	if (ag.ring_timer) {
		g_source_remove(ag.ring_timer);
		ag.ring_timer = 0;
	}

	telephony_terminate_call_req(device);

	return 0;
}

static int cli_notification(struct audio_device *device, const char *buf)
{
	struct headset *hs = device->headset;

	if (strlen(buf) < 9)
		return -EINVAL;

	hs->cli_active = buf[8] == '1' ? TRUE : FALSE;

	return headset_send(hs, "\r\nOK\r\n");
}

int telephony_response_and_hold_rsp(void *telephony_device, cme_error_t err)
{
	return telephony_generic_rsp(telephony_device, err);
}

static int response_and_hold(struct audio_device *device, const char *buf)
{
	struct headset *hs = device->headset;

	if (strlen(buf) < 8)
		return -EINVAL;

	if (buf[7] == '=') {
		telephony_response_and_hold_req(device, atoi(&buf[8]) < 0);
		return 0;
	}

	if (ag.rh >= 0)
		headset_send(hs, "\r\n+BTRH: %d\r\n", ag.rh);

	return headset_send(hs, "\r\nOK\r\n", ag.rh);
}

int telephony_last_dialed_number_rsp(void *telephony_device, cme_error_t err)
{
	return telephony_generic_rsp(telephony_device, err);
}

static int last_dialed_number(struct audio_device *device, const char *buf)
{
	telephony_last_dialed_number_req(device);

	return 0;
}

int telephony_dial_number_rsp(void *telephony_device, cme_error_t err)
{
	return telephony_generic_rsp(telephony_device, err);
}

static int dial_number(struct audio_device *device, const char *buf)
{
	char number[BUF_SIZE];
	size_t buf_len;

	buf_len = strlen(buf);

	if (buf[buf_len - 1] != ';') {
		debug("Rejecting non-voice call dial request");
		return -EINVAL;
	}

	memset(number, 0, sizeof(number));
	strncpy(number, &buf[3], buf_len - 4);

	telephony_dial_number_req(device, number);

	return 0;
}

static int signal_gain_setting(struct audio_device *device, const char *buf)
{
	struct headset *hs = device->headset;
	const char *property;
	const char *name;
	dbus_uint16_t gain;

	if (strlen(buf) < 8) {
		error("Too short string for Gain setting");
		return -EINVAL;
	}

	gain = (dbus_uint16_t) strtol(&buf[7], NULL, 10);

	if (gain > 15) {
		error("Invalid gain value received: %u", gain);
		return -EINVAL;
	}

	switch (buf[5]) {
	case HEADSET_GAIN_SPEAKER:
		if (hs->sp_gain == gain)
			goto ok;
		name = "SpeakerGainChanged";
		property = "SpeakerGain";
		hs->sp_gain = gain;
		break;
	case HEADSET_GAIN_MICROPHONE:
		if (hs->mic_gain == gain)
			goto ok;
		name = "MicrophoneGainChanged";
		property = "MicrophoneGain";
		hs->mic_gain = gain;
		break;
	default:
		error("Unknown gain setting");
		return -EINVAL;
	}

	g_dbus_emit_signal(device->conn, device->path,
				AUDIO_HEADSET_INTERFACE, name,
				DBUS_TYPE_UINT16, &gain,
				DBUS_TYPE_INVALID);

	emit_property_changed(device->conn, device->path,
				AUDIO_HEADSET_INTERFACE, property,
				DBUS_TYPE_UINT16, &gain);

ok:
	return headset_send(hs, "\r\nOK\r\n");
}

int telephony_transmit_dtmf_rsp(void *telephony_device, cme_error_t err)
{
	return telephony_generic_rsp(telephony_device, err);
}

static int dtmf_tone(struct audio_device *device, const char *buf)
{
	if (strlen(buf) < 8) {
		error("Too short string for DTMF tone");
		return -EINVAL;
	}

	telephony_transmit_dtmf_req(device, buf[7]);

	return 0;
}

int telephony_subscriber_number_rsp(void *telephony_device, cme_error_t err)
{
	return telephony_generic_rsp(telephony_device, err);
}

static int subscriber_number(struct audio_device *device, const char *buf)
{
	telephony_subscriber_number_req(device);

	return 0;
}

int telephony_list_current_calls_rsp(void *telephony_device, cme_error_t err)
{
	return telephony_generic_rsp(telephony_device, err);
}

static int list_current_calls(struct audio_device *device, const char *buf)
{
	telephony_list_current_calls_req(device);

	return 0;
}

static int extended_errors(struct audio_device *device, const char *buf)
{
	struct headset *hs = device->headset;

	if (strlen(buf) < 9)
		return -EINVAL;

	if (buf[8] == '1') {
		hs->cme_enabled = TRUE;
		debug("CME errors enabled for headset %p", hs);
	} else {
		hs->cme_enabled = FALSE;
		debug("CME errors disabled for headset %p", hs);
	}

	return headset_send(hs, "\r\nOK\r\n");
}

static int call_waiting_notify(struct audio_device *device, const char *buf)
{
	struct headset *hs = device->headset;

	if (strlen(buf) < 9)
		return -EINVAL;

	if (buf[8] == '1') {
		hs->cwa_enabled = TRUE;
		debug("Call waiting notification enabled for headset %p", hs);
	} else {
		hs->cwa_enabled = FALSE;
		debug("Call waiting notification disabled for headset %p", hs);
	}

	return headset_send(hs, "\r\nOK\r\n");
}

int telephony_operator_selection_rsp(void *telephony_device, cme_error_t err)
{
	return telephony_generic_rsp(telephony_device, err);
}

int telephony_call_hold_rsp(void *telephony_device, cme_error_t err)
{
	return telephony_generic_rsp(telephony_device, err);
}

int telephony_nr_and_ec_rsp(void *telephony_device, cme_error_t err)
{
	struct audio_device *device = telephony_device;
	struct headset *hs = device->headset;

	if (err == CME_ERROR_NONE)
		hs->nrec = hs->nrec_req;

	return telephony_generic_rsp(telephony_device, err);
}

int telephony_operator_selection_ind(int mode, const char *oper)
{
	if (!active_devices)
		return -ENODEV;

	send_foreach_headset(active_devices, hfp_cmp, "\r\n+COPS: %d,0,%s\r\n",
				mode, oper);
	return 0;
}

static int operator_selection(struct audio_device *device, const char *buf)
{
	struct headset *hs = device->headset;

	if (strlen(buf) < 8)
		return -EINVAL;

	switch (buf[7]) {
	case '?':
		telephony_operator_selection_req(device);
		break;
	case '=':
		return headset_send(hs, "\r\nOK\r\n");
	default:
		return -EINVAL;
	}

	return 0;
}

static int nr_and_ec(struct audio_device *device, const char *buf)
{
	struct headset *hs = device->headset;

	if (strlen(buf) < 9)
		return -EINVAL;

	if (buf[8] == '0')
		hs->nrec_req = FALSE;
	else
		hs->nrec_req = TRUE;

	telephony_nr_and_ec_req(device, hs->nrec_req);

	return 0;
}

static struct event event_callbacks[] = {
	{ "ATA", answer_call },
	{ "ATD", dial_number },
	{ "AT+VG", signal_gain_setting },
	{ "AT+BRSF", supported_features },
	{ "AT+CIND", report_indicators },
	{ "AT+CMER", event_reporting },
	{ "AT+CHLD", call_hold },
	{ "AT+CHUP", terminate_call },
	{ "AT+CKPD", key_press },
	{ "AT+CLIP", cli_notification },
	{ "AT+BTRH", response_and_hold },
	{ "AT+BLDN", last_dialed_number },
	{ "AT+VTS", dtmf_tone },
	{ "AT+CNUM", subscriber_number },
	{ "AT+CLCC", list_current_calls },
	{ "AT+CMEE", extended_errors },
	{ "AT+CCWA", call_waiting_notify },
	{ "AT+COPS", operator_selection },
	{ "AT+NREC", nr_and_ec },
	{ 0 }
};

static int handle_event(struct audio_device *device, const char *buf)
{
	struct event *ev;

	debug("Received %s", buf);

	for (ev = event_callbacks; ev->cmd; ev++) {
		if (!strncmp(buf, ev->cmd, strlen(ev->cmd)))
			return ev->callback(device, buf);
	}

	return -EINVAL;
}

static void close_sco(struct audio_device *device)
{
	struct headset *hs = device->headset;

	if (hs->sco) {
		g_source_remove(hs->sco_id);
		hs->sco_id = 0;
		g_io_channel_close(hs->sco);
		g_io_channel_unref(hs->sco);
		hs->sco = NULL;
	}
}

static gboolean rfcomm_io_cb(GIOChannel *chan, GIOCondition cond,
				struct audio_device *device)
{
	struct headset *hs;
	unsigned char buf[BUF_SIZE];
	gsize bytes_read = 0;
	gsize free_space;

	if (cond & G_IO_NVAL)
		return FALSE;

	hs = device->headset;

	if (cond & (G_IO_ERR | G_IO_HUP))
		goto failed;

	if (g_io_channel_read(chan, (gchar *) buf, sizeof(buf) - 1,
				&bytes_read) != G_IO_ERROR_NONE)
		return TRUE;

	free_space = sizeof(hs->buf) - hs->data_start - hs->data_length - 1;

	if (free_space < bytes_read) {
		/* Very likely that the HS is sending us garbage so
		 * just ignore the data and disconnect */
		error("Too much data to fit incomming buffer");
		goto failed;
	}

	memcpy(&hs->buf[hs->data_start], buf, bytes_read);
	hs->data_length += bytes_read;

	/* Make sure the data is null terminated so we can use string
	 * functions */
	hs->buf[hs->data_start + hs->data_length] = '\0';

	while (hs->data_length > 0) {
		char *cr;
		int err;
		off_t cmd_len;

		cr = strchr(&hs->buf[hs->data_start], '\r');
		if (!cr)
			break;

		cmd_len = 1 + (off_t) cr - (off_t) &hs->buf[hs->data_start];
		*cr = '\0';

		if (cmd_len > 1)
			err = handle_event(device, &hs->buf[hs->data_start]);
		else
			/* Silently skip empty commands */
			err = 0;

		if (err == -EINVAL) {
			error("Badly formated or unrecognized command: %s",
					&hs->buf[hs->data_start]);
			err = headset_send(hs, "\r\nERROR\r\n");
		} else if (err < 0)
			error("Error handling command %s: %s (%d)",
						&hs->buf[hs->data_start],
						strerror(-err), -err);

		hs->data_start += cmd_len;
		hs->data_length -= cmd_len;

		if (!hs->data_length)
			hs->data_start = 0;
	}

	return TRUE;

failed:
	headset_set_state(device, HEADSET_STATE_DISCONNECTED);

	return FALSE;
}

static gboolean sco_cb(GIOChannel *chan, GIOCondition cond,
			struct audio_device *device)
{
	struct headset *hs;

	if (cond & G_IO_NVAL)
		return FALSE;

	hs = device->headset;

	error("Audio connection got disconnected");

	headset_set_state(device, HEADSET_STATE_CONNECTED);

	return FALSE;
}

static void rfcomm_connect_cb(GIOChannel *chan, GError *err, gpointer user_data)
{
	struct audio_device *dev = user_data;
	struct headset *hs = dev->headset;
	struct pending_connect *p = hs->pending;
	char hs_address[18];

	if (err) {
		error("%s", err->message);
		goto failed;
	}

	ba2str(&dev->dst, hs_address);
	hs->rfcomm = g_io_channel_ref(chan);
	p->io = NULL;

	if (server_is_enabled(&dev->src, HANDSFREE_SVCLASS_ID) &&
			hs->hfp_handle != 0)
		hs->hfp_active = TRUE;
	else
		hs->hfp_active = FALSE;

	g_io_add_watch(chan, G_IO_IN | G_IO_ERR | G_IO_HUP| G_IO_NVAL,
			(GIOFunc) rfcomm_io_cb, dev);

	debug("%s: Connected to %s", dev->path, hs_address);

	/* In HFP mode wait for Service Level Connection */
	if (hs->hfp_active)
		return;

	headset_set_state(dev, HEADSET_STATE_CONNECTED);

	if (p->target_state == HEADSET_STATE_PLAYING) {
		p->err = sco_connect(dev, NULL, NULL, NULL);
		if (p->err < 0)
			goto failed;
		return;
	}

	if (p->msg) {
		DBusMessage *reply = dbus_message_new_method_return(p->msg);
		g_dbus_send_message(dev->conn, reply);
	}

	pending_connect_finalize(dev);

	return;

failed:
	if (p->msg)
		error_connection_attempt_failed(dev->conn, p->msg, p->err);
	pending_connect_finalize(dev);
	if (hs->rfcomm)
		headset_set_state(dev, HEADSET_STATE_CONNECTED);
	else
		headset_set_state(dev, HEADSET_STATE_DISCONNECTED);
}

static void get_record_cb(sdp_list_t *recs, int err, gpointer user_data)
{
	struct audio_device *dev = user_data;
	struct headset *hs = dev->headset;
	struct pending_connect *p = hs->pending;
	int ch = -1;
	sdp_record_t *record = NULL;
	sdp_list_t *protos, *classes = NULL;
	uuid_t uuid;

	if (err < 0) {
		error("Unable to get service record: %s (%d)", strerror(-err),
			-err);
		goto failed_not_supported;
	}

	if (!recs || !recs->data) {
		error("No records found");
		goto failed_not_supported;
	}

	record = recs->data;

	if (sdp_get_service_classes(record, &classes) < 0) {
		error("Unable to get service classes from record");
		goto failed_not_supported;
	}

	memcpy(&uuid, classes->data, sizeof(uuid));

	if (!sdp_uuid128_to_uuid(&uuid) || uuid.type != SDP_UUID16) {
		error("Not a 16 bit UUID");
		goto failed_not_supported;
	}

	if (hs->search_hfp) {
		if (uuid.value.uuid16 != HANDSFREE_SVCLASS_ID) {
			error("Service record didn't contain the HFP UUID");
			goto failed_not_supported;
		}
		hs->hfp_handle = record->handle;
	} else {
		if (uuid.value.uuid16 != HEADSET_SVCLASS_ID) {
			error("Service record didn't contain the HSP UUID");
			goto failed_not_supported;
		}
		hs->hsp_handle = record->handle;
	}

	if (!sdp_get_access_protos(record, &protos)) {
		ch = sdp_get_proto_port(protos, RFCOMM_UUID);
		sdp_list_foreach(protos, (sdp_list_func_t) sdp_list_free,
					NULL);
		sdp_list_free(protos, NULL);
		protos = NULL;
	}

	if (ch == -1) {
		error("Unable to extract RFCOMM channel from service record");
		goto failed_not_supported;
	}

	hs->rfcomm_ch = ch;

	err = rfcomm_connect(dev, NULL, NULL, NULL);
	if (err < 0) {
		error("Unable to connect: %s (%s)", strerror(-err), -err);
		p->err = -err;
		error_connection_attempt_failed(dev->conn, p->msg, p->err);
		goto failed;
	}

	sdp_list_free(classes, free);

	return;

failed_not_supported:
	if (p->msg)
		error_not_supported(dev->conn, p->msg);
failed:
	if (classes)
		sdp_list_free(classes, free);
	pending_connect_finalize(dev);
	headset_set_state(dev, HEADSET_STATE_DISCONNECTED);
}

static int get_records(struct audio_device *device, headset_stream_cb_t cb,
			void *user_data, unsigned int *cb_id)
{
	struct headset *hs = device->headset;
	uuid_t uuid;

	sdp_uuid16_create(&uuid, hs->search_hfp ? HANDSFREE_SVCLASS_ID :
				HEADSET_SVCLASS_ID);

	headset_set_state(device, HEADSET_STATE_CONNECT_IN_PROGRESS);

	pending_connect_init(hs, HEADSET_STATE_CONNECTED);

	if (cb) {
		unsigned int id;
		id = connect_cb_new(hs, HEADSET_STATE_CONNECTED,
					cb, user_data);
		if (cb_id)
			*cb_id = id;
	}

	return bt_search_service(&device->src, &device->dst, &uuid,
			get_record_cb, device, NULL);
}

static int rfcomm_connect(struct audio_device *dev, headset_stream_cb_t cb,
				void *user_data, unsigned int *cb_id)
{
	struct headset *hs = dev->headset;
	char address[18];
	GError *err = NULL;
	GIOChannel *io;

	if (hs->rfcomm_ch < 0)
		return get_records(dev, cb, user_data, cb_id);

	ba2str(&dev->dst, address);

	debug("%s: Connecting to %s channel %d", dev->path, address,
		hs->rfcomm_ch);

	io = bt_io_connect(BT_IO_RFCOMM, rfcomm_connect_cb, dev,
				NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR, &dev->src,
				BT_IO_OPT_DEST_BDADDR, &dev->dst,
				BT_IO_OPT_CHANNEL, hs->rfcomm_ch,
				BT_IO_OPT_INVALID);
	if (!io) {
		error("%s", err->message);
		g_error_free(err);
		return -EIO;
	}

	g_io_channel_unref(io);

	headset_set_state(dev, HEADSET_STATE_CONNECT_IN_PROGRESS);

	pending_connect_init(hs, HEADSET_STATE_CONNECTED);

	if (cb) {
		unsigned int id = connect_cb_new(hs, HEADSET_STATE_CONNECTED,
							cb, user_data);
		if (cb_id)
			*cb_id = id;
	}

	return 0;
}

static DBusMessage *hs_stop(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct audio_device *device = data;
	struct headset *hs = device->headset;
	DBusMessage *reply = NULL;

	if (hs->state < HEADSET_STATE_PLAY_IN_PROGRESS)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".NotConnected",
						"Device not Connected");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	headset_set_state(device, HEADSET_STATE_CONNECTED);

	return reply;
}

static DBusMessage *hs_is_playing(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct audio_device *device = data;
	struct headset *hs = device->headset;
	DBusMessage *reply;
	dbus_bool_t playing;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	playing = (hs->state == HEADSET_STATE_PLAYING);

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &playing,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *hs_disconnect(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct audio_device *device = data;
	struct headset *hs = device->headset;
	DBusMessage *reply = NULL;
	char hs_address[18];

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	if (hs->state == HEADSET_STATE_DISCONNECTED)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".NotConnected",
						"Device not Connected");

	headset_set_state(device, HEADSET_STATE_DISCONNECTED);
	ba2str(&device->dst, hs_address);
	info("Disconnected from %s, %s", hs_address, device->path);

	return reply;
}

static DBusMessage *hs_is_connected(DBusConnection *conn,
						DBusMessage *msg,
						void *data)
{
	struct audio_device *device = data;
	DBusMessage *reply;
	dbus_bool_t connected;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	connected = (device->headset->state >= HEADSET_STATE_CONNECTED);

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &connected,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *hs_connect(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct audio_device *device = data;
	struct headset *hs = device->headset;
	int err;

	if (hs->state == HEADSET_STATE_CONNECT_IN_PROGRESS)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".InProgress",
						"Connect in Progress");
	else if (hs->state > HEADSET_STATE_CONNECT_IN_PROGRESS)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".AlreadyConnected",
						"Already Connected");

	if (hs->hfp_handle && !ag.telephony_ready)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".NotReady",
					"Telephony subsystem not ready");

	if (!manager_allow_headset_connection(&device->src))
		return g_dbus_create_error(msg, ERROR_INTERFACE ".NotAllowed",
						"Too many connected devices");

	err = rfcomm_connect(device, NULL, NULL, NULL);
	if (err < 0)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".ConnectAttemptFailed",
						"Connect Attempt Failed");

	hs->auto_dc = FALSE;

	hs->pending->msg = dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *hs_ring(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct audio_device *device = data;
	struct headset *hs = device->headset;
	DBusMessage *reply = NULL;
	int err;

	if (hs->state < HEADSET_STATE_CONNECTED)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".NotConnected",
						"Device not Connected");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	if (ag.ring_timer) {
		debug("IndicateCall received when already indicating");
		goto done;
	}

	err = headset_send(hs, "\r\nRING\r\n");
	if (err < 0) {
		dbus_message_unref(reply);
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
						"%s", strerror(-err));
	}

	ring_timer_cb(NULL);
	ag.ring_timer = g_timeout_add_seconds(RING_INTERVAL, ring_timer_cb,
						NULL);

done:
	return reply;
}

static DBusMessage *hs_cancel_call(DBusConnection *conn,
					DBusMessage *msg,
					void *data)
{
	struct audio_device *device = data;
	struct headset *hs = device->headset;
	DBusMessage *reply = NULL;

	if (hs->state < HEADSET_STATE_CONNECTED)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".NotConnected",
						"Device not Connected");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	if (ag.ring_timer) {
		g_source_remove(ag.ring_timer);
		ag.ring_timer = 0;
	} else
		debug("Got CancelCall method call but no call is active");

	return reply;
}

static DBusMessage *hs_play(DBusConnection *conn, DBusMessage *msg,
				void *data)
{
	struct audio_device *device = data;
	struct headset *hs = device->headset;
	int err;

	if (sco_hci) {
		error("Refusing Headset.Play() because SCO HCI routing "
				"is enabled");
		return g_dbus_create_error(msg, ERROR_INTERFACE ".NotAvailable",
						"Operation not Available");
	}

	switch (hs->state) {
	case HEADSET_STATE_DISCONNECTED:
	case HEADSET_STATE_CONNECT_IN_PROGRESS:
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".NotConnected",
						"Device not Connected");
	case HEADSET_STATE_PLAY_IN_PROGRESS:
		if (hs->pending && hs->pending->msg == NULL) {
			hs->pending->msg = dbus_message_ref(msg);
			return NULL;
		}
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".InProgress",
						"Play in Progress");
	case HEADSET_STATE_PLAYING:
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".AlreadyConnected",
						"Device Already Connected");
	case HEADSET_STATE_CONNECTED:
	default:
		break;
	}

	err = sco_connect(device, NULL, NULL, NULL);
	if (err < 0)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
						"%s", strerror(-err));

	hs->pending->msg = dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *hs_get_speaker_gain(DBusConnection *conn,
					DBusMessage *msg,
					void *data)
{
	struct audio_device *device = data;
	struct headset *hs = device->headset;
	DBusMessage *reply;
	dbus_uint16_t gain;

	if (hs->state < HEADSET_STATE_CONNECTED || hs->sp_gain < 0)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".NotAvailable",
						"Operation not Available");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	gain = (dbus_uint16_t) hs->sp_gain;

	dbus_message_append_args(reply, DBUS_TYPE_UINT16, &gain,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *hs_get_mic_gain(DBusConnection *conn,
					DBusMessage *msg,
					void *data)
{
	struct audio_device *device = data;
	struct headset *hs = device->headset;
	DBusMessage *reply;
	dbus_uint16_t gain;

	if (hs->state < HEADSET_STATE_CONNECTED || hs->mic_gain < 0)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".NotAvailable",
						"Operation not Available");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	gain = (dbus_uint16_t) hs->mic_gain;

	dbus_message_append_args(reply, DBUS_TYPE_UINT16, &gain,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *hs_set_gain(DBusConnection *conn,
				DBusMessage *msg,
				void *data, uint16_t gain,
				char type)
{
	struct audio_device *device = data;
	struct headset *hs = device->headset;
	DBusMessage *reply;
	int err;

	if (hs->state < HEADSET_STATE_CONNECTED)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".NotConnected",
						"Device not Connected");

	if (gain > 15)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".InvalidArgument",
						"Must be less than or equal to 15");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	if (hs->state != HEADSET_STATE_PLAYING)
		goto done;

	err = headset_send(hs, "\r\n+VG%c=%u\r\n", type, gain);
	if (err < 0) {
		dbus_message_unref(reply);
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
						"%s", strerror(-err));
	}

done:
	if (type == HEADSET_GAIN_SPEAKER) {
		hs->sp_gain = gain;
		g_dbus_emit_signal(conn, device->path,
					AUDIO_HEADSET_INTERFACE,
					"SpeakerGainChanged",
					DBUS_TYPE_UINT16, &gain,
					DBUS_TYPE_INVALID);
	} else {
		hs->mic_gain = gain;
		g_dbus_emit_signal(conn, device->path,
					AUDIO_HEADSET_INTERFACE,
					"MicrophoneGainChanged",
					DBUS_TYPE_UINT16, &gain,
					DBUS_TYPE_INVALID);
	}

	return reply;
}

static DBusMessage *hs_set_speaker_gain(DBusConnection *conn,
					DBusMessage *msg,
					void *data)
{
	uint16_t gain;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT16, &gain,
				DBUS_TYPE_INVALID))
		return NULL;

	return hs_set_gain(conn, msg, data, gain, HEADSET_GAIN_SPEAKER);
}

static DBusMessage *hs_set_mic_gain(DBusConnection *conn,
					DBusMessage *msg,
					void *data)
{
	uint16_t gain;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT16, &gain,
				DBUS_TYPE_INVALID))
		return NULL;

	return hs_set_gain(conn, msg, data, gain, HEADSET_GAIN_MICROPHONE);
}

static DBusMessage *hs_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct audio_device *device = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	gboolean value;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);


	/* Playing */
	value = (device->headset->state == HEADSET_STATE_PLAYING);
	dict_append_entry(&dict, "Playing", DBUS_TYPE_BOOLEAN, &value);

	/* Connected */
	value = (device->headset->state >= HEADSET_STATE_CONNECTED);
	dict_append_entry(&dict, "Connected", DBUS_TYPE_BOOLEAN, &value);

	if (!value)
		goto done;

	/* SpeakerGain */
	dict_append_entry(&dict, "SpeakerGain",
				DBUS_TYPE_UINT16, &device->headset->sp_gain);

	/* MicrophoneGain */
	dict_append_entry(&dict, "MicrophoneGain",
				DBUS_TYPE_UINT16, &device->headset->mic_gain);

done:
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *hs_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *property;
	DBusMessageIter iter;
	DBusMessageIter sub;
	uint16_t gain;

	if (!dbus_message_iter_init(msg, &iter))
		return invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return invalid_args(msg);
	dbus_message_iter_recurse(&iter, &sub);

	if (g_str_equal("SpeakerGain", property)) {
		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_UINT16)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &gain);
		return hs_set_gain(conn, msg, data, gain,
					HEADSET_GAIN_SPEAKER);
	} else if (g_str_equal("MicrophoneGain", property)) {
		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_UINT16)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &gain);
		return hs_set_gain(conn, msg, data, gain,
					HEADSET_GAIN_MICROPHONE);
	}

	return invalid_args(msg);
}
static GDBusMethodTable headset_methods[] = {
	{ "Connect",		"",	"",	hs_connect,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "Disconnect",		"",	"",	hs_disconnect },
	{ "IsConnected",	"",	"b",	hs_is_connected },
	{ "IndicateCall",	"",	"",	hs_ring },
	{ "CancelCall",		"",	"",	hs_cancel_call },
	{ "Play",		"",	"",	hs_play,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "Stop",		"",	"",	hs_stop },
	{ "IsPlaying",		"",	"b",	hs_is_playing,
						G_DBUS_METHOD_FLAG_DEPRECATED },
	{ "GetSpeakerGain",	"",	"q",	hs_get_speaker_gain,
						G_DBUS_METHOD_FLAG_DEPRECATED },
	{ "GetMicrophoneGain",	"",	"q",	hs_get_mic_gain,
						G_DBUS_METHOD_FLAG_DEPRECATED },
	{ "SetSpeakerGain",	"q",	"",	hs_set_speaker_gain,
						G_DBUS_METHOD_FLAG_DEPRECATED },
	{ "SetMicrophoneGain",	"q",	"",	hs_set_mic_gain,
						G_DBUS_METHOD_FLAG_DEPRECATED },
	{ "GetProperties",	"",	"a{sv}",hs_get_properties },
	{ "SetProperty",	"sv",	"",	hs_set_property },
	{ NULL, NULL, NULL, NULL }
};

static GDBusSignalTable headset_signals[] = {
	{ "Connected",			"",	G_DBUS_SIGNAL_FLAG_DEPRECATED },
	{ "Disconnected",		"",	G_DBUS_SIGNAL_FLAG_DEPRECATED },
	{ "AnswerRequested",		""	},
	{ "Stopped",			"",	G_DBUS_SIGNAL_FLAG_DEPRECATED },
	{ "Playing",			"",	G_DBUS_SIGNAL_FLAG_DEPRECATED },
	{ "SpeakerGainChanged",		"q",	G_DBUS_SIGNAL_FLAG_DEPRECATED },
	{ "MicrophoneGainChanged",	"q",	G_DBUS_SIGNAL_FLAG_DEPRECATED },
	{ "CallTerminated",		""	},
	{ "PropertyChanged",		"sv"	},
	{ NULL, NULL }
};

static void headset_set_channel(struct headset *headset,
				const sdp_record_t *record, uint16_t svc)
{
	int ch;
	sdp_list_t *protos;

	if (sdp_get_access_protos(record, &protos) < 0) {
		error("Unable to get access protos from headset record");
		return;
	}

	ch = sdp_get_proto_port(protos, RFCOMM_UUID);

	sdp_list_foreach(protos, (sdp_list_func_t) sdp_list_free, NULL);
	sdp_list_free(protos, NULL);

	if (ch > 0) {
		headset->rfcomm_ch = ch;
		debug("Discovered %s service on RFCOMM channel %d",
			svc == HEADSET_SVCLASS_ID ? "Headset" : "Handsfree",
			ch);
	} else
		error("Unable to get RFCOMM channel from Headset record");
}

void headset_update(struct audio_device *dev, uint16_t svc,
			const char *uuidstr)
{
	struct headset *headset = dev->headset;
	const sdp_record_t *record;

	record = btd_device_get_record(dev->btd_dev, uuidstr);
	if (!record)
		return;

	switch (svc) {
	case HANDSFREE_SVCLASS_ID:
		if (headset->hfp_handle &&
				(headset->hfp_handle != record->handle)) {
			error("More than one HFP record found on device");
			return;
		}

		headset->hfp_handle = record->handle;
		break;

	case HEADSET_SVCLASS_ID:
		if (headset->hsp_handle &&
				(headset->hsp_handle != record->handle)) {
			error("More than one HSP record found on device");
			return;
		}

		headset->hsp_handle = record->handle;

		/* Ignore this record if we already have access to HFP */
		if (headset->hfp_handle)
			return;

		break;

	default:
		debug("Invalid record passed to headset_update");
		return;
	}

	headset_set_channel(headset, record, svc);
}

static void headset_free(struct audio_device *dev)
{
	struct headset *hs = dev->headset;

	if (hs->dc_timer) {
		g_source_remove(hs->dc_timer);
		hs->dc_timer = 0;
	}

	if (hs->sco) {
		g_io_channel_close(hs->sco);
		g_io_channel_unref(hs->sco);
	}

	if (hs->rfcomm) {
		g_io_channel_close(hs->rfcomm);
		g_io_channel_unref(hs->rfcomm);
	}

	g_free(hs);
	dev->headset = NULL;
}

static void path_unregister(void *data)
{
	struct audio_device *dev = data;
	struct headset *hs = dev->headset;

	if (hs->state > HEADSET_STATE_DISCONNECTED) {
		debug("Headset unregistered while device was connected!");
		headset_set_state(dev, HEADSET_STATE_DISCONNECTED);
	}

	info("Unregistered interface %s on path %s",
		AUDIO_HEADSET_INTERFACE, dev->path);

	headset_free(dev);
}

void headset_unregister(struct audio_device *dev)
{
	g_dbus_unregister_interface(dev->conn, dev->path,
		AUDIO_HEADSET_INTERFACE);
}

struct headset *headset_init(struct audio_device *dev, uint16_t svc,
				const char *uuidstr)
{
	struct headset *hs;
	const sdp_record_t *record;

	hs = g_new0(struct headset, 1);
	hs->rfcomm_ch = -1;
	hs->sp_gain = -1;
	hs->mic_gain = -1;
	hs->search_hfp = server_is_enabled(&dev->src, HANDSFREE_SVCLASS_ID);
	hs->hfp_active = FALSE;
	hs->cli_active = FALSE;
	hs->nrec = TRUE;

	record = btd_device_get_record(dev->btd_dev, uuidstr);
	if (!record)
		goto register_iface;

	switch (svc) {
	case HANDSFREE_SVCLASS_ID:
		hs->hfp_handle = record->handle;
		break;

	case HEADSET_SVCLASS_ID:
		hs->hsp_handle = record->handle;
		break;

	default:
		debug("Invalid record passed to headset_init");
		g_free(hs);
		return NULL;
	}

	headset_set_channel(hs, record, svc);
register_iface:
	if (!g_dbus_register_interface(dev->conn, dev->path,
					AUDIO_HEADSET_INTERFACE,
					headset_methods, headset_signals, NULL,
					dev, path_unregister)) {
		g_free(hs);
		return NULL;
	}

	info("Registered interface %s on path %s",
		AUDIO_HEADSET_INTERFACE, dev->path);

	return hs;
}

uint32_t headset_config_init(GKeyFile *config)
{
	GError *err = NULL;
	char *str;

	/* Use the default values if there is no config file */
	if (config == NULL)
		return ag.features;

	str = g_key_file_get_string(config, "General", "SCORouting",
					&err);
	if (err) {
		debug("audio.conf: %s", err->message);
		g_clear_error(&err);
	} else {
		if (strcmp(str, "PCM") == 0)
			sco_hci = FALSE;
		else if (strcmp(str, "HCI") == 0)
			sco_hci = TRUE;
		else
			error("Invalid Headset Routing value: %s", str);
		g_free(str);
	}

	return ag.features;
}

static gboolean hs_dc_timeout(struct audio_device *dev)
{
	headset_set_state(dev, HEADSET_STATE_DISCONNECTED);
	return FALSE;
}

gboolean headset_cancel_stream(struct audio_device *dev, unsigned int id)
{
	struct headset *hs = dev->headset;
	struct pending_connect *p = hs->pending;
	GSList *l;
	struct connect_cb *cb = NULL;

	if (!p)
		return FALSE;

	for (l = p->callbacks; l != NULL; l = l->next) {
		struct connect_cb *tmp = l->data;

		if (tmp->id == id) {
			cb = tmp;
			break;
		}
	}

	if (!cb)
		return FALSE;

	p->callbacks = g_slist_remove(p->callbacks, cb);
	g_free(cb);

	if (p->callbacks || p->msg)
		return TRUE;

	if (hs->auto_dc) {
		if (hs->rfcomm)
			hs->dc_timer = g_timeout_add_seconds(DC_TIMEOUT,
						(GSourceFunc) hs_dc_timeout,
						dev);
		else
			headset_set_state(dev, HEADSET_STATE_DISCONNECTED);
	}

	return TRUE;
}

static gboolean dummy_connect_complete(struct audio_device *dev)
{
	pending_connect_finalize(dev);
	return FALSE;
}

unsigned int headset_request_stream(struct audio_device *dev,
					headset_stream_cb_t cb,
					headset_lock_t lock,
					void *user_data)
{
	struct headset *hs = dev->headset;
	unsigned int id;

	if (hs->rfcomm && hs->sco) {
		id = connect_cb_new(hs, HEADSET_STATE_PLAYING, cb, user_data);
		g_idle_add((GSourceFunc) dummy_connect_complete, dev);
		return id;
	}

	if (hs->dc_timer) {
		g_source_remove(hs->dc_timer);
		hs->dc_timer = 0;
	}

	if (hs->state == HEADSET_STATE_CONNECT_IN_PROGRESS ||
			hs->state == HEADSET_STATE_PLAY_IN_PROGRESS)
		return connect_cb_new(hs, HEADSET_STATE_PLAYING, cb, user_data);

	if (hs->rfcomm == NULL) {
		if (rfcomm_connect(dev, cb, user_data, &id) < 0)
			return 0;
		hs->auto_dc = TRUE;
	} else {
		if (sco_connect(dev, cb, user_data, &id) < 0)
			return 0;
	}

	hs->pending->target_state = HEADSET_STATE_PLAYING;

	return id;
}

unsigned int headset_config_stream(struct audio_device *dev,
					headset_stream_cb_t cb,
					headset_lock_t lock,
					void *user_data)
{
	struct headset *hs = dev->headset;
	unsigned int id;

	if (hs->lock & lock)
		return 0;

	if (hs->dc_timer) {
		g_source_remove(hs->dc_timer);
		hs->dc_timer = 0;
	}

	if (hs->state == HEADSET_STATE_CONNECT_IN_PROGRESS)
		return connect_cb_new(hs, HEADSET_STATE_CONNECTED, cb,
					user_data);

	if (hs->rfcomm)
		goto done;

	if (rfcomm_connect(dev, cb, user_data, &id) < 0)
		return 0;

	hs->auto_dc = TRUE;
	hs->pending->target_state = HEADSET_STATE_CONNECTED;

	return id;

done:
	id = connect_cb_new(hs, HEADSET_STATE_CONNECTED, cb, user_data);
	g_idle_add((GSourceFunc) dummy_connect_complete, dev);
	return id;
}

unsigned int headset_suspend_stream(struct audio_device *dev,
					headset_stream_cb_t cb,
					headset_lock_t lock,
					void *user_data)
{
	struct headset *hs = dev->headset;
	unsigned int id;

	if (hs->lock & ~lock || !hs->rfcomm || !hs->sco)
		return 0;

	if (hs->dc_timer) {
		g_source_remove(hs->dc_timer);
		hs->dc_timer = 0;
	}

	close_sco(dev);

	id = connect_cb_new(hs, HEADSET_STATE_CONNECTED, cb, user_data);
	g_idle_add((GSourceFunc) dummy_connect_complete, dev);
	return id;
}

gboolean get_hfp_active(struct audio_device *dev)
{
	struct headset *hs = dev->headset;

	return hs->hfp_active;
}

void set_hfp_active(struct audio_device *dev, gboolean active)
{
	struct headset *hs = dev->headset;

	hs->hfp_active = active;
}

int headset_connect_rfcomm(struct audio_device *dev, GIOChannel *io)
{
	struct headset *hs = dev->headset;

	hs->tmp_rfcomm = g_io_channel_ref(io);

	return hs->tmp_rfcomm ? 0 : -EINVAL;
}

int headset_connect_sco(struct audio_device *dev, GIOChannel *io)
{
	struct headset *hs = dev->headset;

	if (hs->sco)
		return -EISCONN;

	hs->sco = io;

	if (hs->pending_ring) {
		ring_timer_cb(NULL);
		ag.ring_timer = g_timeout_add_seconds(RING_INTERVAL,
						ring_timer_cb,
						NULL);
		hs->pending_ring = FALSE;
	}

	return 0;
}

static int headset_close_rfcomm(struct audio_device *dev)
{
	struct headset *hs = dev->headset;
	GIOChannel *rfcomm = hs->tmp_rfcomm ? hs->tmp_rfcomm : hs->rfcomm;

	if (rfcomm) {
		g_io_channel_close(rfcomm);
		g_io_channel_unref(rfcomm);
		hs->tmp_rfcomm = NULL;
		hs->rfcomm = NULL;
	}

	hs->data_start = 0;
	hs->data_length = 0;

	hs->nrec = TRUE;

	return 0;
}

void headset_set_authorized(struct audio_device *dev)
{
	struct headset *hs = dev->headset;

	/* For HFP telephony isn't ready just disconnect */
	if (hs->hfp_active && !ag.telephony_ready) {
		error("Unable to accept HFP connection since the telephony "
				"subsystem isn't initialized");
		headset_set_state(dev, HEADSET_STATE_DISCONNECTED);
		return;
	}

	hs->rfcomm = hs->tmp_rfcomm;
	hs->tmp_rfcomm = NULL;

	g_io_add_watch(hs->rfcomm,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			(GIOFunc) rfcomm_io_cb, dev);

	hs->auto_dc = FALSE;

	/* For HSP (no special SLC setup) move to CONNECTED state */
	if (!hs->hfp_active)
		headset_set_state(dev, HEADSET_STATE_CONNECTED);
}

void headset_set_state(struct audio_device *dev, headset_state_t state)
{
	struct headset *hs = dev->headset;
	gboolean value;

	if (hs->state == state)
		return;

	switch (state) {
	case HEADSET_STATE_DISCONNECTED:
		value = FALSE;
		close_sco(dev);
		headset_close_rfcomm(dev);
		g_dbus_emit_signal(dev->conn, dev->path,
					AUDIO_HEADSET_INTERFACE,
					"Disconnected",
					DBUS_TYPE_INVALID);
		emit_property_changed(dev->conn, dev->path,
					AUDIO_HEADSET_INTERFACE, "Connected",
					DBUS_TYPE_BOOLEAN, &value);
		telephony_device_disconnected(dev);
		active_devices = g_slist_remove(active_devices, dev);
		break;
	case HEADSET_STATE_CONNECT_IN_PROGRESS:
		break;
	case HEADSET_STATE_CONNECTED:
		close_sco(dev);
		if (hs->state < state) {
			value = TRUE;
			g_dbus_emit_signal(dev->conn, dev->path,
						AUDIO_HEADSET_INTERFACE,
						"Connected",
						DBUS_TYPE_INVALID);
			emit_property_changed(dev->conn, dev->path,
						AUDIO_HEADSET_INTERFACE,
						"Connected",
						DBUS_TYPE_BOOLEAN, &value);
			active_devices = g_slist_append(active_devices, dev);
			telephony_device_connected(dev);
		} else if (hs->state == HEADSET_STATE_PLAYING) {
			value = FALSE;
			g_dbus_emit_signal(dev->conn, dev->path,
						AUDIO_HEADSET_INTERFACE,
						"Stopped",
						DBUS_TYPE_INVALID);
			emit_property_changed(dev->conn, dev->path,
						AUDIO_HEADSET_INTERFACE,
						"Playing",
						DBUS_TYPE_BOOLEAN, &value);
		}
		break;
	case HEADSET_STATE_PLAY_IN_PROGRESS:
		break;
	case HEADSET_STATE_PLAYING:
		value = TRUE;
		hs->sco_id = g_io_add_watch(hs->sco,
					G_IO_ERR | G_IO_HUP | G_IO_NVAL,
					(GIOFunc) sco_cb, dev);

		g_dbus_emit_signal(dev->conn, dev->path,
					AUDIO_HEADSET_INTERFACE, "Playing",
					DBUS_TYPE_INVALID);
		emit_property_changed(dev->conn, dev->path,
					AUDIO_HEADSET_INTERFACE, "Playing",
					DBUS_TYPE_BOOLEAN, &value);

		if (hs->sp_gain >= 0)
			headset_send(hs, "\r\n+VGS=%u\r\n", hs->sp_gain);
		if (hs->mic_gain >= 0)
			headset_send(hs, "\r\n+VGM=%u\r\n", hs->mic_gain);
		break;
	}

	debug("State changed %s: %s -> %s", dev->path, str_state[hs->state],
		str_state[state]);
	hs->state = state;
}

headset_state_t headset_get_state(struct audio_device *dev)
{
	struct headset *hs = dev->headset;

	return hs->state;
}

int headset_get_channel(struct audio_device *dev)
{
	struct headset *hs = dev->headset;

	return hs->rfcomm_ch;
}

gboolean headset_is_active(struct audio_device *dev)
{
	struct headset *hs = dev->headset;

	if (hs->state != HEADSET_STATE_DISCONNECTED)
		return TRUE;

	return FALSE;
}

gboolean headset_lock(struct audio_device *dev, headset_lock_t lock)
{
	struct headset *hs = dev->headset;

	if (hs->lock & lock)
		return FALSE;

	hs->lock |= lock;

	return TRUE;
}

gboolean headset_unlock(struct audio_device *dev, headset_lock_t lock)
{
	struct headset *hs = dev->headset;

	if (!(hs->lock & lock))
		return FALSE;

	hs->lock &= ~lock;

	if (hs->lock)
		return TRUE;

	if (hs->state == HEADSET_STATE_PLAYING)
		headset_set_state(dev, HEADSET_STATE_CONNECTED);

	if (hs->auto_dc) {
		if (hs->state == HEADSET_STATE_CONNECTED)
			hs->dc_timer = g_timeout_add_seconds(DC_TIMEOUT,
						(GSourceFunc) hs_dc_timeout,
						dev);
		else
			headset_set_state(dev, HEADSET_STATE_DISCONNECTED);
	}

	return TRUE;
}

gboolean headset_suspend(struct audio_device *dev, void *data)
{
	return TRUE;
}

gboolean headset_play(struct audio_device *dev, void *data)
{
	return TRUE;
}

int headset_get_sco_fd(struct audio_device *dev)
{
	struct headset *hs = dev->headset;

	if (!hs->sco)
		return -1;

	return g_io_channel_unix_get_fd(hs->sco);
}

gboolean headset_get_nrec(struct audio_device *dev)
{
	struct headset *hs = dev->headset;

	return hs->nrec;
}

gboolean headset_get_sco_hci(struct audio_device *dev)
{
	return sco_hci;
}

int telephony_event_ind(int index)
{
	if (!active_devices)
		return -ENODEV;

	if (!ag.er_ind) {
		debug("telephony_report_event called but events are disabled");
		return -EINVAL;
	}

	send_foreach_headset(active_devices, hfp_cmp,
				"\r\n+CIEV: %d,%d\r\n", index + 1,
				ag.indicators[index].val);

	return 0;
}

int telephony_response_and_hold_ind(int rh)
{
	if (!active_devices)
		return -ENODEV;

	ag.rh = rh;

	/* If we aren't in any response and hold state don't send anything */
	if (ag.rh < 0)
		return 0;

	send_foreach_headset(active_devices, hfp_cmp, "\r\n+BTRH: %d\r\n",
				ag.rh);

	return 0;
}

int telephony_incoming_call_ind(const char *number, int type)
{
	struct audio_device *dev;
	struct headset *hs;

	if (!active_devices)
		return -ENODEV;

	/* Get the latest connected device */
	dev = active_devices->data;
	hs = dev->headset;

	if (ag.ring_timer) {
		debug("telephony_incoming_call_ind: already calling");
		return -EBUSY;
	}

	g_free(ag.number);
	ag.number = g_strdup(number);
	ag.number_type = type;

	if (ag.features & AG_FEATURE_INBAND_RINGTONE && hs->hfp_active &&
			hs->state != HEADSET_STATE_PLAYING) {
		if (hs->state == HEADSET_STATE_CONNECTED) {
			int ret;

			ret = sco_connect(dev, NULL, NULL, NULL);
			if (ret < 0)
				return ret;
		}

		hs->pending_ring = TRUE;

		return 0;
	}

	ring_timer_cb(NULL);
	ag.ring_timer = g_timeout_add_seconds(RING_INTERVAL, ring_timer_cb,
						NULL);

	return 0;
}

int telephony_calling_stopped_ind(void)
{
	struct audio_device *dev;

	if (!active_devices)
		return -ENODEV;

	/* In case SCO isn't fully up yet */
	dev = active_devices->data;

	if (!dev->headset->pending_ring && !ag.ring_timer)
		return -EINVAL;

	dev->headset->pending_ring = FALSE;

	if (ag.ring_timer) {
		g_source_remove(ag.ring_timer);
		ag.ring_timer = 0;
	}

	return 0;
}

int telephony_ready_ind(uint32_t features,
			const struct indicator *indicators, int rh,
			const char *chld)
{
	ag.telephony_ready = TRUE;
	ag.features = features;
	ag.indicators = indicators;
	ag.rh = rh;
	ag.chld = g_strdup(chld);

	debug("Telephony plugin initialized");

	print_ag_features(ag.features);

	return 0;
}

int telephony_list_current_call_ind(int idx, int dir, int status, int mode,
					int mprty, const char *number,
					int type)
{
	if (!active_devices)
		return -ENODEV;

	if (number)
		send_foreach_headset(active_devices, hfp_cmp,
					"\r\n+CLCC: %d,%d,%d,%d,%d,%s,%d\r\n",
					idx, dir, status, mode, mprty,
					number, type);
	else
		send_foreach_headset(active_devices, hfp_cmp,
					"\r\n+CLCC: %d,%d,%d,%d,%d\r\n",
					idx, dir, status, mode, mprty);

	return 0;
}

int telephony_subscriber_number_ind(const char *number, int type, int service)
{
	if (!active_devices)
		return -ENODEV;

	send_foreach_headset(active_devices, hfp_cmp,
				"\r\n+CNUM: ,%s,%d,,%d\r\n",
				number, type, service);

	return 0;
}

static int cwa_cmp(struct headset *hs)
{
	if (!hs->hfp_active)
		return -1;

	if (hs->cwa_enabled)
		return 0;
	else
		return -1;
}

int telephony_call_waiting_ind(const char *number, int type)
{
	if (!active_devices)
		return -ENODEV;

	send_foreach_headset(active_devices, cwa_cmp, "\r\n+CCWA: %s,%d\r\n",
				number, type);

	return 0;
}
