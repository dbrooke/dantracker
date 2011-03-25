#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <termios.h>
#include <getopt.h>
#include <netdb.h>

#include <fap.h>
#include <iniparser.h>

#include "ui.h"
#include "util.h"
#include "serial.h"
#include "nmea.h"

#ifndef BUILD
#define BUILD 0
#endif

#ifndef REVISION
#define REVISION "Unknown"
#endif

#define MYPOS(s) (&s->mypos[s->mypos_idx])

#define KEEP_PACKETS 8
#define KEEP_POSITS  4

#define DO_TYPE_NONE 0
#define DO_TYPE_WX   1
#define DO_TYPE_PHG  2

#define TZ_OFFSET (-8)

struct smart_beacon_point {
	float int_sec;
	float speed;
};

struct state {
	struct {
		char *tnc;
		int tnc_rate;
		char *gps;
		int gps_rate;
		char *tel;
		int tel_rate;

		char *gps_type;
		int testing;
		int verbose;
		char *icon;

		char *digi_path;

		int power;
		int height;
		int gain;
		int directivity;

		int atrest_rate;
		struct smart_beacon_point sb_low;
		struct smart_beacon_point sb_high;
		int course_change_min;
		int course_change_slope;
		int after_stop;

		unsigned int do_types;

		char **comments;
		int comments_count;

		char *config;

		double static_lat, static_lon, static_alt;
		double static_spd, static_crs;

		struct sockaddr display_to;
	} conf;

	struct posit mypos[KEEP_POSITS];
	int mypos_idx;

	struct posit last_beacon_pos;

	struct {
		double temp1;
		double voltage;

		time_t last_tel_beacon;
		time_t last_tel;
	} tel;

	char *mycall;

	fap_packet_t *last_packet; /* In case we don't store it below */
	fap_packet_t *recent[KEEP_PACKETS];
	int recent_idx;

	char gps_buffer[128];
	int gps_idx;
	time_t last_gps_update;
	time_t last_gps_data;
	time_t last_beacon;
	time_t last_time_set;
	time_t last_moving;
	time_t last_status;

	int comment_idx;
	int other_beacon_idx;

	uint8_t digi_quality;
};

int ui_send(struct state *state, const char *name, const char *value)
{
	return set_value_to(&state->conf.display_to,
			    sizeof(state->conf.display_to),
			    name, value);
}

char *format_time(time_t t)
{
	static char str[32];

	if (t > 3600)
		snprintf(str, sizeof(str), "%luh%lum", t / 3600, t % 3600);
	else if (t > 60)
		if (t % 60)
			snprintf(str, sizeof(str), "%lum%lus", t / 60, t % 60);
		else
			snprintf(str, sizeof(str), "%lu min", t / 60);
	else
		snprintf(str, sizeof(str), "%lu sec", t);

	return str;
}

void display_wx(struct state *state, fap_packet_t *_fap)
{
	fap_wx_report_t *fap = _fap->wx_report;
	char *wind = NULL;
	char *temp = NULL;
	char *rain = NULL;
	char *humid = NULL;
	char *pres = NULL;
	char *report = NULL;

	if (fap->wind_gust && fap->wind_dir && fap->wind_speed)
		asprintf(&wind, "Wind %s %.0fmph (%.0f gst) ",
			 direction(*fap->wind_dir),
			 MS_TO_MPH(*fap->wind_speed),
			 MS_TO_MPH(*fap->wind_gust));
	else if (fap->wind_dir && fap->wind_speed)
		asprintf(&wind, "Wind %s %.0f mph ",
			 direction(*fap->wind_dir),
			 MS_TO_MPH(*fap->wind_speed));

	if (fap->temp)
		asprintf(&temp, "%.0fF ", C_TO_F(*fap->temp));

	if (fap->rain_1h && *fap->rain_24h)
		asprintf(&rain, "Rain %.2f\"h%.2f\"d ",
			 MM_TO_IN(*fap->rain_1h),
			 MM_TO_IN(*fap->rain_24h));
	else if (fap->rain_1h)
		asprintf(&rain, "Rain %.2f\"h ", MM_TO_IN(*fap->rain_1h));
	else if (fap->rain_24h)
		asprintf(&rain, "Rain %.2f\"d ", MM_TO_IN(*fap->rain_24h));

	if (fap->humidity)
		asprintf(&humid, "Hum. %2i%% ", *fap->humidity);

	asprintf(&report, "%s%s%s%s",
		 wind ? wind : "",
		 temp ? temp : "",
		 rain ? rain : "",
		 humid ? humid : "");

	ui_send(state, "AI_COMMENT", report);

	/* Comment is used for larger WX report, so report the
	 * comment (if any) in the smaller course field
	 */
	if (_fap->comment_len) {
		char buf[512];

		strncpy(buf, _fap->comment, _fap->comment_len);
		buf[_fap->comment_len] = 0;
		ui_send(state, "AI_COURSE", buf);
	} else
		ui_send(state, "AI_COURSE", "");

	free(report);
	free(pres);
	free(humid);
	free(rain);
	free(temp);
	free(wind);
}

void display_telemetry(struct state *state, fap_telemetry_t *fap)
{
	ui_send(state, "AI_COURSE", "(Telemetry)");
	ui_send(state, "AI_COMMENT", "");
}

void display_phg(struct state *state, fap_packet_t *fap)
{
	int power, gain, dir;
	char height;
	int ret;
	char *buf = NULL;

	ret = sscanf(fap->phg, "%1d%c%1d%1d",
		     &power, &height, &gain, &dir);
	if (ret != 4) {
		ui_send(state, "AI_COURSE", "(Broken PHG)");
		return;
	}

	asprintf(&buf, "Power %iW at %.0fft (%idB gain @ %s)",
		 power*power,
		 pow(2, height - '0') * 10,
		 gain,
		 dir ? direction(dir) : "omni");
	ui_send(state, "AI_COMMENT", buf);
	free(buf);

	if (fap->comment) {
		buf = strndup(fap->comment, fap->comment_len);
		ui_send(state, "AI_COURSE", buf);
		free(buf);
	} else
		ui_send(state, "AI_COURSE", "");
}

void display_posit(struct state *state, fap_packet_t *fap, int isnew)
{
	char buf[512];

	if (fap->speed && fap->course && (*fap->speed > 0.0)) {
		snprintf(buf, sizeof(buf), "%.0f MPH %2s",
			 KPH_TO_MPH(*fap->speed),
			 direction(*fap->course));
		ui_send(state, "AI_COURSE", buf);
	} else if (isnew)
		ui_send(state, "AI_COURSE", "");

	if (fap->type && (*fap->type == fapSTATUS)) {
		strncpy(buf, fap->status, fap->status_len);
		buf[fap->status_len] = 0;
		ui_send(state, "AI_COMMENT", buf);
	} else if (fap->comment_len) {
		strncpy(buf, fap->comment, fap->comment_len);
		buf[fap->comment_len] = 0;
		ui_send(state, "AI_COMMENT", buf);
	} else if (isnew)
		ui_send(state, "AI_COMMENT", "");
}

void display_dist_and_dir(struct state *state, fap_packet_t *fap)
{
	char buf[512] = "";
	char via[32] = "Direct";
	int i;
	struct posit *mypos = MYPOS(state);

	for (i = 0; i < fap->path_len; i++)
		if (strchr(fap->path[i], '*'))
			strcpy(via, fap->path[i]);
	if (strchr(via, '*'))
		*strchr(via, '*') = 0; /* Nuke the asterisk */

	if (STREQ(fap->src_callsign, state->mycall))
		snprintf(buf, sizeof(buf), "via %s", via);
	else if (fap->latitude && fap->longitude)
		snprintf(buf, sizeof(buf), "%5.1fmi %2s <small>via %s</small>",
			 KPH_TO_MPH(fap_distance(mypos->lon, mypos->lat,
						 *fap->longitude,
						 *fap->latitude)),
			 direction(get_direction(mypos->lon, mypos->lat,
						 *fap->longitude,
						 *fap->latitude)),
			 via);
	else if (fap->latitude && fap->longitude && fap->altitude)
		snprintf(buf, 512, "%5.1fmi %2s (%4.0f ft)",
			 KPH_TO_MPH(fap_distance(mypos->lon, mypos->lat,
						 *fap->longitude,
						 *fap->latitude)),
			 direction(get_direction(mypos->lon, mypos->lat,
						 *fap->longitude,
						 *fap->latitude)),
			 M_TO_FT(*fap->altitude));
	ui_send(state, "AI_DISTANCE", buf);
}

void display_packet(struct state *state, fap_packet_t *fap)
{
	char buf[512];
	static char last_callsign[10] = "";
	int isnew = 1;

	if (STREQ(fap->src_callsign, last_callsign))
		isnew = 1;

	ui_send(state, "AI_CALLSIGN", fap->src_callsign);
	strncpy(last_callsign, fap->src_callsign, 9);
	last_callsign[9] = 0;

	display_dist_and_dir(state, fap);

	if (fap->wx_report)
		display_wx(state, fap);
	else if (fap->telemetry)
		display_telemetry(state, fap->telemetry);
	else if (fap->phg)
		display_phg(state, fap);
	else
		display_posit(state, fap, isnew);

	snprintf(buf, sizeof(buf), "%c%c", fap->symbol_table, fap->symbol_code);
	ui_send(state, "AI_ICON", buf);
}

int stored_packet_desc(fap_packet_t *fap, int index,
		       double mylat, double mylon,
		       char *buf, int len)
{
	if (fap->latitude && fap->longitude)
		snprintf(buf, len,
			 "%i: %-9s <small>%3.0fmi %-2s</small>",
			 index, fap->src_callsign,
			 KPH_TO_MPH(fap_distance(mylon, mylat,
						 *fap->longitude,
						 *fap->latitude)),
			 direction(get_direction(mylon, mylat,
						 *fap->longitude,
						 *fap->latitude)));
	else
		snprintf(buf, len,
			 "%i: %-9s", index, fap->src_callsign);

	return 0;
}

int update_packets_ui(struct state *state)
{
	int i, j;
	char name[] = "AL_00";
	char buf[64];
	struct posit *mypos = MYPOS(state);

	if (state->last_packet)
		display_dist_and_dir(state, state->last_packet);

	for (i = KEEP_PACKETS, j = state->recent_idx + 1; i > 0; i--, j++) {
		fap_packet_t *p = state->recent[j % KEEP_PACKETS];

		sprintf(name, "AL_%02i", i-1);
		if (p)
			stored_packet_desc(p, i,
					   mypos->lat, mypos->lon,
					   buf, sizeof(buf));
		else
			sprintf(buf, "%i:", i);
		ui_send(state, name, buf);
	}

	return 0;
}

/* Move packets below @index to @index */
int move_packets(struct state *state, int index)
{
	int i;
	const int max = KEEP_PACKETS;
	int end = (state->recent_idx +1 ) % max;

	fap_free(state->recent[index]);

	for (i = index; i != end; i -= 1) {
		if (i == 0)
			i = KEEP_PACKETS; /* Zero now, KEEP-1 next */
		state->recent[i % max] = state->recent[(i - 1) % max];
	}

	/* This made a hole at the bottom */
	state->recent[end] = NULL;

	return 0;
}

int find_packet(struct state *state, fap_packet_t *fap)
{
	int i;

	for (i = 0; i < KEEP_PACKETS; i++)
		if (state->recent[i] &&
		    STREQ(state->recent[i]->src_callsign, fap->src_callsign))
			return i;

	return -1;
}

#define SWAP_VAL(new, old, value)				\
	do {							\
		if (old->value && !new->value) {		\
			new->value = old->value;		\
			old->value = 0;				\
		}						\
	} while (0);

int merge_packets(fap_packet_t *new, fap_packet_t *old)
{
	SWAP_VAL(new, old, speed);
	SWAP_VAL(new, old, course);
	SWAP_VAL(new, old, latitude);
	SWAP_VAL(new, old, longitude);
	SWAP_VAL(new, old, altitude);
	SWAP_VAL(new, old, symbol_table);
	SWAP_VAL(new, old, symbol_code);

	if (old->comment_len && !new->comment_len) {
		new->comment_len = old->comment_len;
		new->comment = old->comment;
		old->comment_len = 0;
		old->comment = NULL;
	}

	if (old->status_len && !new->status_len) {
		new->status_len = old->status_len;
		new->status = old->status;
		old->status_len = 0;
		old->status = NULL;
	}

	return 0;
}

int store_packet(struct state *state, fap_packet_t *fap)
{
	int i;

	if (STREQ(fap->src_callsign, state->mycall))
		return 0; /* Don't store our own packets */

	i = find_packet(state, fap);
	if (i != -1) {
		merge_packets(fap, state->recent[i]);
		move_packets(state, i);
	}
	state->recent_idx = (state->recent_idx + 1) % KEEP_PACKETS;

	/* If found in spot X, remove and shift all up, then
	 * replace at top
	 */

	if (state->recent[state->recent_idx])
		fap_free(state->recent[state->recent_idx]);
	state->recent[state->recent_idx] = fap;

	update_packets_ui(state);

	return 0;
}

int update_mybeacon_status(struct state *state)
{
	char buf[512];
	time_t delta = (time(NULL) - state->last_beacon);
	uint8_t quality = state->digi_quality;
	int count = 1;
	int i;

	for (i = 1; i < 8; i++)
		count += (quality >> i) & 0x01;

	snprintf(buf, sizeof(buf), "%i", count / 2);
	ui_send(state, "G_SIGBARS", buf);

	if (state->last_beacon)
		snprintf(buf, sizeof(buf), "%s ago", format_time(delta));
	else
		snprintf(buf, sizeof(buf), "Never");
	ui_send(state, "G_LASTBEACON", buf);

	return 0;
}

int handle_incoming_packet(int fd, struct state *state)
{
	char packet[512];
	unsigned int len = sizeof(packet);
	fap_packet_t *fap;
	int ret;

	memset(packet, 0, len);

	ret = get_packet(fd, packet, &len);
	if (!ret)
		return -1;

	printf("%s\n", packet);
	fap = fap_parseaprs(packet, len, 1);
	if (!fap->error_code) {
		if (STREQ(fap->src_callsign, state->mycall)) {
			state->digi_quality |= 1;
			update_mybeacon_status(state);
			if (state->last_packet &&
			    STREQ(state->last_packet->src_callsign,
				  state->mycall))
				/* Special case: if the last packet is also
				 * ours, merge with the new one since we
				 * don't store our own
				 */
				merge_packets(fap, state->last_packet);
		}
		display_packet(state, fap);
		state->last_packet = fap;
		store_packet(state, fap);
		ui_send(state, "I_RX", "1000");
	}

	return 0;
}

int parse_gps_string(struct state *state)
{
	char *str = state->gps_buffer;

	if (*str == '\n')
		str++;

	if (!valid_checksum(str))
		return 0;

	if (strncmp(str, "$GPGGA", 6) == 0) {
		return parse_gga(MYPOS(state), str);
	} else if (strncmp(str, "$GPRMC", 6) == 0) {
		state->mypos_idx = (state->mypos_idx + 1) % KEEP_POSITS;
		return parse_rmc(MYPOS(state), str);
	}

	return 0;
}

int display_gps_info(struct state *state)
{
	char buf[512];
	struct posit *mypos = MYPOS(state);
	const char *status = mypos->qual != 0 ?
		"Locked" :
		"<span background='red'>INVALID</span>";
	int hour, min, sec;

	hour = (mypos->tstamp / 10000) + TZ_OFFSET;
	min = (mypos->tstamp / 100) % 100;
	sec = mypos->tstamp % 100;

	if (hour < 0)
		hour += 24;

	sprintf(buf, "%7.5f %8.5f   Time %02i:%02i:%02i   %s: %2i sats",
		mypos->lat, mypos->lon,
		hour, min, sec,
		status,
		mypos->sats);
	ui_send(state, "G_LATLON", buf);

	if (mypos->speed > 1.0)
		sprintf(buf, "%.0f MPH %2s, Alt %.0f ft",
			KTS_TO_MPH(mypos->speed),
			direction(mypos->course),
			mypos->alt);
	else
		sprintf(buf, "Stationary, Alt %.0f ft", mypos->alt);
	ui_send(state, "G_SPD", buf);

	ui_send(state, "G_MYCALL", state->mycall);

	return 0;
}

int set_time(struct state *state)
{
	struct posit *mypos = MYPOS(state);
	time_t tstamp = mypos->tstamp;
	time_t dstamp = mypos->dstamp;
	char timestr[64];
	int ret;
	int hour, min, sec;
	int day, mon, year;

	if (mypos->qual == 0)
		return 1; /* No fix, no set */
	else if (mypos->sats < 3)
		return 1; /* Not enough sats, don't set */
	else if (!HAS_BEEN(state->last_time_set, 120))
		return 1; /* Too recent */

	hour = (tstamp / 10000);
	min = (tstamp / 100) % 100;
	sec = tstamp % 100;

	day = (dstamp / 10000);
	mon = (dstamp / 100) % 100;
	year = dstamp % 100;

	snprintf(timestr, sizeof(timestr),
		 "date -u %02i%02i%02i%02i20%02i.%02i",
		 mon, day,
		 hour, min,
		 year, sec);

	ret = system(timestr);
	printf("Setting date %s: %s\n", timestr, ret == 0 ? "OK" : "FAIL");
	state->last_time_set = time(NULL);

	return 0;
}

int handle_gps_data(int fd, struct state *state)
{
	char buf[33];
	int ret;
	char *cr;

	ret = read(fd, buf, 32);
	buf[ret] = 0; /* Safe because size is +1 */

	if (ret < 0) {
		perror("gps");
		return -errno;
	} else if (ret == 0)
		return 0;

	if (state->gps_idx + ret > sizeof(state->gps_buffer)) {
		printf("Clearing overrun buffer\n");
		state->gps_idx = 0;
	}

	cr = strchr(buf, '\r');
	if (cr) {
		*cr = 0;
		strcpy(&state->gps_buffer[state->gps_idx], buf);
		if (parse_gps_string(state))
			state->last_gps_data = time(NULL);
		strcpy(state->gps_buffer, cr+1);
		state->gps_idx = strlen(state->gps_buffer);
	} else {
		memcpy(&state->gps_buffer[state->gps_idx], buf, ret);
		state->gps_idx += ret;
	}

	if (MYPOS(state)->speed > 0)
		state->last_moving = time(NULL);

	if (HAS_BEEN(state->last_gps_update, 1)) {
		display_gps_info(state);
		state->last_gps_update = time(NULL);
		set_time(state);
		update_mybeacon_status(state);
		update_packets_ui(state);
	}

	return 0;
}

int handle_telemetry(int fd, struct state *state)
{
	char _buf[512] = "";
	int i = 0;
	int ret;
	char *buf = _buf;
	char *space;

	while (i < sizeof(_buf)) {
		ret = read(fd, &buf[i], 1);
		if (buf[i] == '\n')
			break;
		if (ret < 0)
			return -ret;
		else if (ret == 1)
			i++;
	}

	while (buf && *buf != '\n') {
		char name[16];
		char value[16];

		space = strchr(buf, ' ');
		if (space)
			*space = 0;

		ret = sscanf(buf, "%16[^=]=%16s", (char*)&name, (char*)&value);
		if (ret != 2) {
			printf("Invalid telemetry: %s\n", buf);
			return -EINVAL;
		}

		buf = space+1;

		if (STREQ(name, "temp1"))
			state->tel.temp1 = atof(value);
		else if (STREQ(name, "voltage"))
			state->tel.voltage = atof(value);
		else
			printf("Unknown telemetry value %s\n", name);
	}

	snprintf(_buf, sizeof(_buf), "%.1fV", state->tel.voltage);
	ui_send(state, "T_VOLTAGE", _buf);

	snprintf(_buf, sizeof(_buf), "%.0fF", state->tel.temp1);
	ui_send(state, "T_TEMP1", _buf);

	state->tel.last_tel = time(NULL);

	return 0;
}

/* Get a substitution value for a given key (result must be free()'d) */
char *get_subst(struct state *state, char *key)
{
	char *value;
	struct tm tm;
	char timestr[16];
	time_t t;

	t = time(NULL);
	localtime_r(&t, &tm);

	if (STREQ(key, "index"))
		asprintf(&value, "%i",
			 state->comment_idx++ % state->conf.comments_count);
	else if (STREQ(key, "mycall"))
		value = strdup(state->mycall);
	else if (STREQ(key, "temp1"))
		asprintf(&value, "%.0f", state->tel.temp1);
	else if (STREQ(key, "voltage"))
		asprintf(&value, "%.1f", state->tel.voltage);
	else if (STREQ(key, "sats"))
		asprintf(&value, "%i", MYPOS(state)->sats);
	else if (STREQ(key, "ver"))
		asprintf(&value, "v0.1.%04i (%s)", BUILD, REVISION);
	else if (STREQ(key, "time")) {
		strftime(timestr, sizeof(timestr), "%H:%M:%S", &tm);
		value = strdup(timestr);
	} else if (STREQ(key, "date")) {
		strftime(timestr, sizeof(timestr), "%m/%d/%Y", &tm);
		value = strdup(timestr);
	} else
		printf("Unknown substitution `%s'", key);

	return value;
}

/* Given a string with substition variables, do the substitutions
 * and return the new result (which must be free()'d)
 */
char *process_subst(struct state *state, char *src)
{
	char *str;
	char *ptr1;
	char *ptr2;

	/* FIXME: might overrun! */
	str = malloc(strlen(src) * 4);
	if (!str)
		return str;
	str[0] = 0;

	for (ptr1 = src; *ptr1; ptr1++) {
		char subst[16] = "";
		char *value = NULL;

		ptr2 = strchr(ptr1, '$');
		if (!ptr2) {
			/* No more substs */
			strcat(str, ptr1);
			break;
		}

		/* Copy up to the next variable */
		strncat(str, ptr1, ptr2-ptr1);

		ptr1 = ptr2+1;
		ptr2 = strchr(ptr1, '$');
		if (!ptr2) {
			printf("Bad substitution `%s'\n", ptr1);
			goto err;
		}

		strncpy(subst, ptr1, ptr2-ptr1);
		ptr1 = ptr2;

		value = get_subst(state, subst);
		if (value) {
			strcat(str, value);
			free(value);
		}
	}

	return str;
 err:
	free(str);
	return NULL;
}

char *get_comment(struct state *state)
{
	int cmt = state->comment_idx++ % state->conf.comments_count;

	return process_subst(state, state->conf.comments[cmt]);
}

/*
 * Choose a comment out of the list, and choose a type
 * of (phg, wx, normal) from the list of configured types
 * and construct it.
 */
char *choose_data(struct state *state, char *req_icon)
{
	char *data = NULL;
	char *comment;

	comment = get_comment(state);
	if (!comment)
		comment = strdup("Error");

	switch (state->other_beacon_idx++ % 3) {
	case DO_TYPE_WX:
		if ((state->conf.do_types & DO_TYPE_WX) &&
		    (!HAS_BEEN(state->tel.last_tel, 30))) {
			*req_icon = '_';
			asprintf(&data,
				 ".../...t%03.0f%s",
				 state->tel.temp1,
				 comment);
			break;
		}
	case DO_TYPE_PHG:
		if (state->conf.do_types & DO_TYPE_PHG) {
			asprintf(&data,
				 "PHG%1d%1d%1d%1d%s",
				 state->conf.power,
				 state->conf.height,
				 state->conf.gain,
				 state->conf.directivity,
				 comment);
			break;
		}
	case DO_TYPE_NONE:
		data = strdup(comment);
		break;
	}

	free(comment);
	return data;
}

void separate_minutes(double minutes, unsigned char *min, unsigned char *hun)
{
	double _min, _hun;

	_hun = modf(minutes, &_min);
	*min = (unsigned char)_min;
	*hun = (unsigned char)(_hun * 100);

	printf("min: %hhd hun: %hhd\n", *min, *hun);
}

/* Get the @digith digit of a base-ten number
 *
 * 1234
 * |||^-- 0
 * ||^--- 1
 * |^---- 2
 * ^----- 3
 */
unsigned char get_digit(int value, int digit)
{
	value /= pow(10, digit);
	return value % 10;
}

char *make_mice_beacon(struct state *state)
{
	char *str = NULL;

	struct posit *mypos = MYPOS(state);
	double ldeg, lmin;
	double Ldeg, Lmin;
	int lat;
	unsigned char north = mypos->lat > 0 ? 0x50 : 0x30;
	unsigned char lonsc = fabs(mypos->lon) > 100 ? 0x50 : 0x30;
	unsigned char west = mypos->lon > 0 ? 0x30 : 0x50;

	unsigned char lon_deg, lon_min, lon_hun;

	unsigned char spd_htk;
	unsigned char spd_crs;
	unsigned char crs_tud;

	lmin = modf(fabs(mypos->lat), &ldeg) * 60;
	Lmin = modf(fabs(mypos->lon), &Ldeg) * 60;

	/* Latitude DDMMmm encoded in base-10 */
	lat = (ldeg * 10000) + (lmin * 100);

	/* Longitude degrees encoded per APRS spec */
	if (Ldeg <= 9)
		lon_deg = (int)Ldeg + 118;
	else if (Ldeg <= 99)
		lon_deg = (int)Ldeg + 28;
	else if (Ldeg <= (int)109)
		lon_deg = (int)Ldeg + 108;
	else if (Ldeg <= 179)
		lon_deg = ((int)Ldeg - 100) + 28;

	/* Minutes and hundredths of a minute encoded per APRS spec */
	separate_minutes(Lmin, &lon_min, &lon_hun);
	if (Lmin > 10)
		lon_min += 28;
	else
		lon_min += 88;
	lon_hun += 28;

	/* Speed, hundreds and tens of knots */
	spd_htk = (mypos->speed / 10) + 108;

	/* Units of speed and course hundreds of degrees */
	spd_crs = 32 + \
		(((int)mypos->speed % 10) * 10) + \
		((int)mypos->course / 100);

	/* Course tens and units of degrees */
	crs_tud = ((int)mypos->course % 100) + 28;

	asprintf(&str,
		 "%s>%c%c%c%c%c%c,%s:`%c%c%c%c%c%c%c%c",
		 state->mycall,
		 get_digit(lat, 5) | 0x50,
		 get_digit(lat, 4) | 0x30,
		 get_digit(lat, 3) | 0x50,
		 get_digit(lat, 2) | north,
		 get_digit(lat, 1) | lonsc,
		 get_digit(lat, 0) | west,
		 state->conf.digi_path,
		 lon_deg,
		 lon_min,
		 lon_hun,
		 spd_htk,
		 spd_crs,
		 crs_tud,
		 state->conf.icon[1],
		 state->conf.icon[0]);

	return str;
}

char *make_status_beacon(struct state *state)
{
	char *packet = NULL;
	char *data = get_comment(state);

	asprintf(&packet,
		 "%s>%s,%s:>%s",
		 state->mycall, "APZDMS", state->conf.digi_path,
		 data);
	free(data);

	return packet;
}

char *make_beacon(struct state *state, char *payload)
{
	char *data = NULL;
	char *packet;
	char _lat[16];
	char _lon[16];
	int ret;
	char icon = state->conf.icon[1];
	struct posit *mypos = MYPOS(state);
	char course_speed[] = ".../...";

	double lat = fabs(mypos->lat);
	double lon = fabs(mypos->lon);

	snprintf(_lat, 16, "%02.0f%05.2f%c",
		 floor(lat),
		 (lat - floor(lat)) * 60,
		 mypos->lat > 0 ? 'N' : 'S');

	snprintf(_lon, 16, "%03.0f%05.2f%c",
		 floor(lon),
		 (lon - floor(lon)) * 60,
		 mypos->lon > 0 ? 'E' : 'W');

	if (mypos->speed > 5)
		snprintf(course_speed, sizeof(course_speed),
			 "%03.0f/%03.0f",
			 mypos->course,
			 mypos->speed);
	else
		course_speed[0] = 0;

	if (!payload)
		payload = data = choose_data(state, &icon);

	ret = asprintf(&packet,
		       "%s>APZDMS,%s:!%s%c%s%c%s%s",
		       state->mycall,
		       state->conf.digi_path,
		       _lat,
		       state->conf.icon[0],
		       _lon,
		       icon,
		       course_speed,
		       payload);

	free(data);

	if (ret < 0)
		return NULL;

	return packet;
}

double sb_course_change_thresh(struct state *state)
{
	double mph = KTS_TO_MPH(MYPOS(state)->speed);
	double slope = state->conf.course_change_slope;
	double min = state->conf.course_change_min;

	return min + (slope / mph);
}

int should_beacon(struct state *state)
{
	struct posit *mypos = MYPOS(state);
	time_t delta = time(NULL) - state->last_beacon;
	time_t sb_min_delta;
	double speed_frac;
	double d_speed = state->conf.sb_high.speed - state->conf.sb_low.speed;
	double d_rate = state->conf.sb_low.int_sec -
		state->conf.sb_high.int_sec;
	double sb_thresh = sb_course_change_thresh(state);
	double sb_change = fabs(state->last_beacon_pos.course - mypos->course);

	char *reason = NULL;

	/* Time required to have passed in order to beacon,
	 * 0 if never, -1 if now
	 */
	time_t req = 0;

	/* NEVER more often than every 10 seconds! */
	if (delta < 10)
		return 0;

	/* The fractional penetration into the lo/hi zone */
	speed_frac = (KTS_TO_MPH(mypos->speed) -
		      state->conf.sb_low.speed) / d_speed;

	/* Determine the fractional that we are slower than the max */
	sb_min_delta = (d_rate * (1 - speed_frac)) +
		state->conf.sb_high.int_sec;

	/* Never when we aren't getting data anymore */
	if (HAS_BEEN(state->last_gps_data, 30)) {
		mypos->qual = mypos->sats = 0;
		reason = "NODATA";
		goto out;
	}

	/* Never when we don't have a fix */
	if (mypos->qual == 0) {
		reason = "NOLOCK";
		goto out;
	}

	/* If we have recently stopped moving, do one beacon */
	if (state->last_moving &&
	    HAS_BEEN(state->last_moving, state->conf.after_stop)) {
		state->last_moving = 0;
		req = -1;
		reason = "STOPPED";
		goto out;
	}

	/* If we're not moving at all, choose the "at rest" rate */
	if (mypos->speed <= 1) {
		req = state->conf.atrest_rate;
		reason = "ATREST";
		goto out;
	}

	/* SmartBeaconing: Course Change (only if moving) */
	if ((sb_change > sb_thresh) && (KTS_TO_MPH(mypos->speed) > 2.0)) {
		printf("SB: Angle changed by %.0f (>%.0f)\n",
		       sb_change, sb_thresh);
		reason = "COURSE";
		req = -1;
		goto out;
	}

	/* SmartBeaconing: Range-based variable speed beaconing */

	/* If we're going below the low point, use that interval */
	if (KTS_TO_MPH(mypos->speed) < state->conf.sb_low.speed) {
		req = state->conf.sb_low.int_sec;
		reason = "SLOWTO";
		goto out;
	}

	/* If we're going above the high point, use that interval */
	if (KTS_TO_MPH(mypos->speed) > state->conf.sb_high.speed) {
		req = state->conf.sb_high.int_sec;
		reason = "FASTTO";
		goto out;
	}

	/* We must be in the speed zone, so adjust interval according
	 * to the fractional penetration of the speed range
	 */
	req = sb_min_delta;
	reason = "FRACTO";
 out:
	if (reason) {
		char tmp[256];
		if (req <= 0)
			strcpy(tmp, reason);
		else
			sprintf(tmp, "Every %s", format_time(req));
		ui_send(state, "G_REASON", tmp);
	}

	if (req == 0) {
		update_mybeacon_status(state);
		return 0;
	} else if (req == -1)
		return 1;
	else
		return delta > req;
}

int send_beacon(int fd, char *packet)
{
	char buf[512];
	int ret;
	unsigned int len = sizeof(buf);

	printf("Sending Packet: %s\n", packet);

	ret = fap_tnc2_to_kiss(packet, strlen(packet), 0, buf, &len);
	if (!ret) {
		printf("Failed to make beacon KISS packet\n");
		return 1;
	}
	return write(fd, buf, len) == len;
}

int beacon(int fd, struct state *state)
{
	char *packet;
	static time_t max_beacon_check = 0;

	/* Don't even check but every half-second */
	if (!HAS_BEEN(max_beacon_check, 0.5))
		return 0;

	max_beacon_check = time(NULL);

	if (!should_beacon(state))
		return 0;

	if (MYPOS(state)->speed > 5) {
		/* Send a short MIC-E position beacon */
		packet = make_mice_beacon(state);
		send_beacon(fd, packet);
		free(packet);

		if (HAS_BEEN(state->last_status, 120)) {
			/* Follow up with a status packet */
			packet = make_status_beacon(state);
			send_beacon(fd, packet);
			free(packet);
			state->last_status = time(NULL);
		}
	} else {
		packet = make_beacon(state, NULL);
		send_beacon(fd, packet);
		free(packet);
	}

	state->last_beacon = time(NULL);
	state->digi_quality <<= 1;
	update_mybeacon_status(state);

	ui_send(state, "I_TX", "1000");

	state->last_beacon_pos = state->mypos[state->mypos_idx];

	return 0;
}

int redir_log()
{
	int fd;

	fd = open("/tmp/aprs.log", O_WRONLY|O_TRUNC|O_CREAT, 0644);
	if (fd < 0) {
		perror("/tmp/aprs.log");
		return -errno;
	}

	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);

	setvbuf(stdout, NULL, _IONBF, 0);

	return 0;
}

int fake_gps_data(struct state *state)
{
	struct posit *mypos = MYPOS(state);

	if (state->conf.testing) {
		//state->conf.static_lat -= 0.01;
		//state->conf.static_lon += 0.01;
		//if (state->conf.static_spd > 0)
		//    state->conf.static_spd -= 1;
		state->conf.static_crs += 0.1;
	}

	mypos->lat = state->conf.static_lat;
	mypos->lon = state->conf.static_lon;
	mypos->alt = state->conf.static_alt;
	mypos->speed = state->conf.static_spd;
	mypos->course = state->conf.static_crs;

	mypos->qual = 1;
	mypos->sats = 0; /* We may claim qual=1, but no sats */

	state->last_gps_data = time(NULL);
	state->tel.temp1 = 75;
	state->tel.voltage = 13.8;
	state->tel.last_tel = time(NULL);

	if ((time(NULL) - state->last_gps_update) > 3) {
		display_gps_info(state);
		state->last_gps_update = time(NULL);
	}

	return 0;
}

int lookup_host(struct state *state, const char *hostname)
{
	struct hostent *host;
	struct sockaddr_in *sa = (struct sockaddr_in *)&state->conf.display_to;

	host = gethostbyname(hostname);
	if (!host) {
		perror(hostname);
		return -errno;
	}

	if (host->h_length < 1) {
		fprintf(stderr, "No address for %s\n", hostname);
		return -EINVAL;
	}

	sa->sin_family = AF_INET;
	sa->sin_port = htons(SOCKPORT);
	memcpy(&sa->sin_addr, host->h_addr_list[0], sizeof(sa->sin_addr));

	return 0;
}

int parse_opts(int argc, char **argv, struct state *state)
{
	static struct option lopts[] = {
		{"tnc",       1, 0, 't'},
		{"gps",       1, 0, 'g'},
		{"telemetry", 1, 0, 'T'},
		{"testing",   0, 0,  1 },
		{"verbose",   0, 0, 'v'},
		{"conf",      1, 0, 'c'},
		{"display",   1, 0, 'd'},
		{NULL,        0, 0,  0 },
	};

	state->conf.display_to.sa_family = AF_UNIX;
	strcpy(((struct sockaddr_un *)&state->conf.display_to)->sun_path,
	       SOCKPATH);

	while (1) {
		int c;
		int optidx;

		c = getopt_long(argc, argv, "t:g:T:c:sv",
				lopts, &optidx);
		if (c == -1)
			break;

		switch(c) {
		case 't':
			state->conf.tnc = optarg;
			break;
		case 'g':
			state->conf.gps = optarg;
			break;
		case 'T':
			state->conf.tel = optarg;
			break;
		case 1:
			state->conf.testing = 1;
			break;
		case 'v':
			state->conf.verbose = 1;
			break;
		case 'c':
			state->conf.config = optarg;
			break;
		case 'd':
			lookup_host(state, optarg);
			break;
		case '?':
			printf("Unknown option\n");
			return -1;
		};
	}

	return 0;
}

char **parse_list(char *string, int *count)
{
	char **list;
	char *ptr;
	int i = 0;

	for (ptr = string; *ptr; ptr++)
		if (*ptr == ',')
			i++;
	*count = i+1;

	list = calloc(*count, sizeof(char **));
	if (!list)
		return NULL;

	for (i = 0; string; i++) {
		ptr = strchr(string, ',');
		if (ptr) {
			*ptr = 0;
			ptr++;
		}
		list[i] = strdup(string);
		string = ptr;
	}

	return list;
}

int parse_ini(char *filename, struct state *state)
{
	dictionary *ini;
	char *tmp;

	ini = iniparser_load(filename);
	if (ini == NULL)
		return -EINVAL;

	if (!state->conf.tnc)
		state->conf.tnc = iniparser_getstring(ini, "tnc:port", NULL);
	state->conf.tnc_rate = iniparser_getint(ini, "tnc:rate", 9600);

	if (!state->conf.gps)
		state->conf.gps = iniparser_getstring(ini, "gps:port", NULL);
	state->conf.gps_type = iniparser_getstring(ini, "gps:type", "static");
	state->conf.gps_rate = iniparser_getint(ini, "gps:rate", 4800);

	if (!state->conf.tel)
		state->conf.tel = iniparser_getstring(ini, "telemetry:port",
						      NULL);
	state->conf.tel_rate = iniparser_getint(ini, "telemetry:rate", 9600);

	state->mycall = iniparser_getstring(ini, "station:mycall", "N0CAL-7");
	state->conf.icon = iniparser_getstring(ini, "station:icon", "/>");

	if (strlen(state->conf.icon) != 2) {
		printf("ERROR: Icon must be two characters, not `%s'\n",
		       state->conf.icon);
		return -1;
	}

	state->conf.digi_path = iniparser_getstring(ini, "station:digi_path",
						    "WIDE1-1,WIDE2-1");

	state->conf.power = iniparser_getint(ini, "station:power", 0);
	state->conf.height = iniparser_getint(ini, "station:height", 0);
	state->conf.gain = iniparser_getint(ini, "station:gain", 0);
	state->conf.directivity = iniparser_getint(ini, "station:directivity",
						   0);

	state->conf.atrest_rate = iniparser_getint(ini,
						   "beaconing:atrest_rate",
						   600);
	state->conf.sb_low.speed = iniparser_getint(ini,
						    "beaconing:min_speed",
						    10);
	state->conf.sb_low.int_sec = iniparser_getint(ini,
						      "beaconing:min_rate",
						      600);
	state->conf.sb_high.speed = iniparser_getint(ini,
						     "beaconing:max_speed",
						     60);
	state->conf.sb_high.int_sec = iniparser_getint(ini,
						       "beaconing:max_rate",
						       60);
	state->conf.course_change_min = iniparser_getint(ini,
						     "beaconing:course_change_min",
						     30);
	state->conf.course_change_slope = iniparser_getint(ini,
							   "beaconing:course_change_slope",
							   255);
	state->conf.after_stop = iniparser_getint(ini,
						  "beaconing:after_stop",
						  180);

	state->conf.static_lat = iniparser_getdouble(ini,
						     "static:lat",
						     0.0);
	state->conf.static_lon = iniparser_getdouble(ini,
						     "static:lon",
						     0.0);
	state->conf.static_alt = iniparser_getdouble(ini,
						     "static:alt",
						     0.0);
	state->conf.static_spd = iniparser_getdouble(ini,
						     "static:speed",
						     0.0);
	state->conf.static_crs = iniparser_getdouble(ini,
						     "static:course",
						     0.0);

	tmp = iniparser_getstring(ini, "station:beacon_types", "posit");
	if (strlen(tmp) != 0) {
		char **types;
		int count;
		int i;

		types = parse_list(tmp, &count);
		if (!types) {
			printf("Failed to parse beacon types\n");
			return -EINVAL;
		}

		for (i = 0; i < count; i++) {
			if (STREQ(types[i], "weather"))
				state->conf.do_types |= DO_TYPE_WX;
			else if (STREQ(types[i], "phg"))
				state->conf.do_types |= DO_TYPE_PHG;
			else
				printf("WARNING: Unknown beacon type %s\n",
				       types[i]);
			free(types[i]);
		}
		free(types);
	}

	tmp = iniparser_getstring(ini, "comments:enabled", "");
	if (strlen(tmp) != 0) {
		int i;

		state->conf.comments = parse_list(tmp,
						  &state->conf.comments_count);
		if (!state->conf.comments)
			return -EINVAL;

		for (i = 0; i < state->conf.comments_count; i++) {
			char section[32];

			snprintf(section, sizeof(section),
				 "comments:%s", state->conf.comments[i]);
			free(state->conf.comments[i]);
			state->conf.comments[i] = iniparser_getstring(ini,
								      section,
								      "INVAL");
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	int tncfd;
	int gpsfd;
	int telfd;
	int i;

	fd_set fds;

	struct state state;
	memset(&state, 0, sizeof(state));

	printf("APRS v0.1.%04i (%s)\n", BUILD, REVISION);

	fap_init();

	if (parse_opts(argc, argv, &state)) {
		printf("Invalid option(s)\n");
		exit(1);
	}

	if (parse_ini(state.conf.config ? state.conf.config : "aprs.ini", &state)) {
		printf("Invalid config\n");
		exit(1);
	}

	if (!state.conf.verbose)
		redir_log();

	if (state.conf.testing)
		state.digi_quality = 0xFF;

	for (i = 0; i < KEEP_PACKETS; i++)
		state.recent[i] = NULL;

	tncfd = serial_open(state.conf.tnc, state.conf.tnc_rate);
	if (tncfd < 0) {
		printf("Failed to open TNC: %m\n");
		exit(1);
	}

	if (state.conf.gps) {
		gpsfd = serial_open(state.conf.gps, state.conf.gps_rate);
		if (gpsfd < 0) {
			perror(state.conf.gps);
			exit(1);
		}
	} else
		gpsfd = -1;

	if (state.conf.tel) {
		telfd = serial_open(state.conf.tel, state.conf.tel_rate);
		if (telfd < 0) {
			perror(state.conf.tel);
			exit(1);
		}
	} else
		telfd = -1;

	FD_ZERO(&fds);

	while (1) {
		int ret;
		struct timeval tv = {1, 0};

		FD_SET(tncfd, &fds);
		if (gpsfd > 0)
			FD_SET(gpsfd, &fds);
		if (telfd > 0)
			FD_SET(telfd, &fds);

		if (STREQ(state.conf.gps_type, "static"))
			fake_gps_data(&state);

		ret = select(100, &fds, NULL, NULL, &tv);
		if (ret == -1) {
			perror("select");
			continue;
		} else if (ret > 0) {
			if (FD_ISSET(tncfd, &fds))
				handle_incoming_packet(tncfd, &state);
			if (FD_ISSET(gpsfd, &fds))
				handle_gps_data(gpsfd, &state);
			if (FD_ISSET(telfd, &fds))
				handle_telemetry(telfd, &state);
		}

		beacon(tncfd, &state);
		fflush(NULL);
	}

	fap_cleanup();
}
