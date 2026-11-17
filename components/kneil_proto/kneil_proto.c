/* KNEIL live protocol: packet encode/decode and text lines. See kneil_proto.h. */
#include <string.h>
#include "kneil_proto.h"

/* format 2 must stay 40 bytes (compile-time check that works in C99 too) */
typedef char kneil_summary_size_check[(sizeof(kneil_summary_t) == KNEIL_SUMMARY_LEN - 1) ? 1 : -1];

/* ---------- little-endian helpers ---------- */
static void put_u16(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *b)
{
	return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t get_u32(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* ---------- binary packets ---------- */
int kneil_live_encode(const kneil_live_t *p, uint8_t *b, size_t size)
{
	if (size < KNEIL_LIVE_V2_LEN) {
		return -1;
	}
	b[0] = KNEIL_PKT_LIVE;
	b[1] = KNEIL_PROTO_VERSION;
	put_u16(b + 2, p->seq);
	put_u32(b + 4, p->t_ms);
	b[8] = p->cls;
	b[9] = p->cls_smooth;
	b[10] = p->vote_pct;
	b[11] = p->flags;
	for (int k = 0; k < 3; k++) {
		put_u16(b + 12 + 2 * k, (uint16_t)p->mean_mg[k]);
	}
	put_u16(b + 18, p->battery_mv);
	b[20] = p->raw_cls;
	b[21] = p->events;
	return KNEIL_LIVE_V2_LEN;
}

int kneil_live_decode(const uint8_t *b, size_t len, kneil_live_t *p)
{
	if (b[0] != KNEIL_PKT_LIVE) {
		return -1;
	}
	const int v2 = (len == KNEIL_LIVE_V2_LEN && b[1] == KNEIL_PROTO_VERSION);
	const int v1 = (len == KNEIL_LIVE_V1_LEN && b[1] == 1);

	if (!v1 && !v2) {
		return -1;
	}
	p->version = v2 ? 2 : 1;
	p->seq = get_u16(b + 2);
	p->t_ms = get_u32(b + 4);
	p->cls = b[8];
	p->cls_smooth = b[9];
	p->vote_pct = b[10];
	p->flags = b[11];
	for (int k = 0; k < 3; k++) {
		p->mean_mg[k] = (int16_t)get_u16(b + 12 + 2 * k);
	}
	p->battery_mv = get_u16(b + 18);
	p->raw_cls = v2 ? b[20] : KNEIL_CLASS_NONE;
	p->events = v2 ? b[21] : 0;
	return 0;
}

int kneil_raw_encode(const kneil_raw_t *p, uint8_t *b, size_t size)
{
	const size_t len = KNEIL_RAW_HDR_LEN + 6U * p->n;

	if (p->n == 0 || p->n > KNEIL_RAW_N_MAX || size < len) {
		return -1;
	}
	b[0] = KNEIL_PKT_RAW;
	b[1] = p->n;
	put_u16(b + 2, p->seq);
	put_u32(b + 4, p->t_ms_first);
	for (int i = 0; i < p->n; i++) {
		for (int k = 0; k < 3; k++) {
			put_u16(b + KNEIL_RAW_HDR_LEN + 6 * i + 2 * k, (uint16_t)p->xyz_mg[i][k]);
		}
	}
	return (int)len;
}

int kneil_raw_decode(const uint8_t *b, size_t len, kneil_raw_t *p)
{
	if (len < KNEIL_RAW_HDR_LEN || b[0] != KNEIL_PKT_RAW) {
		return -1;
	}
	const uint8_t n = b[1];

	if (n == 0 || n > KNEIL_RAW_N_MAX || len != KNEIL_RAW_HDR_LEN + 6U * n) {
		return -1;
	}
	p->n = n;
	p->seq = get_u16(b + 2);
	p->t_ms_first = get_u32(b + 4);
	for (int i = 0; i < n; i++) {
		for (int k = 0; k < 3; k++) {
			p->xyz_mg[i][k] = (int16_t)get_u16(b + KNEIL_RAW_HDR_LEN + 6 * i + 2 * k);
		}
	}
	return 0;
}

int kneil_summary_encode(const kneil_summary_t *s, uint8_t *b, size_t size)
{
	if (size < KNEIL_SUMMARY_LEN) {
		return -1;
	}
	b[0] = KNEIL_PKT_SUMMARY;
	b[1] = s->format;
	b[2] = s->n_classes;
	for (int k = 0; k < KNEIL_SUMMARY_CLASSES; k++) {
		put_u16(b + 3 + 2 * k, s->minutes_by_class[k]);
	}
	put_u16(b + 19, s->minutes_sitting);
	put_u16(b + 21, s->minutes_standing);
	put_u16(b + 23, s->minutes_still_unknown);
	put_u16(b + 25, s->n_sit_to_stand);
	put_u16(b + 27, s->n_stand_to_sit);
	put_u16(b + 29, s->n_bed_transfer);
	put_u16(b + 31, s->sts_seconds_x10);
	put_u16(b + 33, s->steps);
	put_u16(b + 35, s->cadence_x10);
	put_u16(b + 37, s->stairs_cadence_x10);
	put_u16(b + 39, (uint16_t)s->sit_tilt_x10);
	return KNEIL_SUMMARY_LEN;
}

int kneil_summary_decode(const uint8_t *b, size_t len, kneil_summary_t *s)
{
	if (len != KNEIL_SUMMARY_LEN || b[0] != KNEIL_PKT_SUMMARY) {
		return -1;
	}
	s->format = b[1];
	s->n_classes = b[2];
	for (int k = 0; k < KNEIL_SUMMARY_CLASSES; k++) {
		s->minutes_by_class[k] = get_u16(b + 3 + 2 * k);
	}
	s->minutes_sitting = get_u16(b + 19);
	s->minutes_standing = get_u16(b + 21);
	s->minutes_still_unknown = get_u16(b + 23);
	s->n_sit_to_stand = get_u16(b + 25);
	s->n_stand_to_sit = get_u16(b + 27);
	s->n_bed_transfer = get_u16(b + 29);
	s->sts_seconds_x10 = get_u16(b + 31);
	s->steps = get_u16(b + 33);
	s->cadence_x10 = get_u16(b + 35);
	s->stairs_cadence_x10 = get_u16(b + 37);
	s->sit_tilt_x10 = (int16_t)get_u16(b + 39);
	return 0;
}

/* ---------- text lines (no printf: small and the same on every compiler) ---------- */
typedef struct {
	char *p;
	char *end;   /* last usable position (one byte is kept for the NUL) */
	int ok;
} out_t;

static void put_ch(out_t *o, char c)
{
	if (o->p < o->end) {
		*o->p++ = c;
	} else {
		o->ok = 0;
	}
}

static void put_str(out_t *o, const char *s)
{
	while (*s) {
		put_ch(o, *s++);
	}
}

static void put_uint(out_t *o, uint32_t u)
{
	char tmp[10];
	int n = 0;

	do {
		tmp[n++] = (char)('0' + u % 10U);
		u /= 10U;
	} while (u != 0U);
	while (n > 0) {
		put_ch(o, tmp[--n]);
	}
}

static void put_int(out_t *o, int32_t v)
{
	if (v < 0) {
		put_ch(o, '-');
		put_uint(o, (uint32_t)(-(int64_t)v));
	} else {
		put_uint(o, (uint32_t)v);
	}
}

static int finish(out_t *o, char *out)
{
	put_ch(o, '\n');
	if (!o->ok) {
		return -1;
	}
	*o->p = '\0';
	return (int)(o->p - out);
}

int kneil_live_to_line(const kneil_live_t *p, char *out, size_t size)
{
	if (size < 2) {
		return -1;
	}
	out_t o = {out, out + size - 1, 1};

	put_str(&o, "L,");
	put_uint(&o, p->seq);
	put_ch(&o, ',');
	put_uint(&o, p->t_ms);
	put_ch(&o, ',');
	put_uint(&o, p->cls);
	put_ch(&o, ',');
	put_uint(&o, p->cls_smooth);
	put_ch(&o, ',');
	put_uint(&o, p->vote_pct);
	put_ch(&o, ',');
	put_uint(&o, p->flags);
	for (int k = 0; k < 3; k++) {
		put_ch(&o, ',');
		put_int(&o, p->mean_mg[k]);
	}
	put_ch(&o, ',');
	put_uint(&o, p->battery_mv);
	if (p->version >= 2) {          /* v2 adds the raw class and the event flags */
		put_ch(&o, ',');
		put_uint(&o, p->raw_cls);
		put_ch(&o, ',');
		put_uint(&o, p->events);
	}
	return finish(&o, out);
}

int kneil_raw_to_line(const kneil_raw_t *p, char *out, size_t size)
{
	if (size < 2 || p->n > KNEIL_RAW_N_MAX) {
		return -1;
	}
	out_t o = {out, out + size - 1, 1};

	put_str(&o, "R,");
	put_uint(&o, p->seq);
	put_ch(&o, ',');
	put_uint(&o, p->t_ms_first);
	put_ch(&o, ',');
	put_uint(&o, p->n);
	for (int i = 0; i < p->n; i++) {
		for (int k = 0; k < 3; k++) {
			put_ch(&o, ',');
			put_int(&o, p->xyz_mg[i][k]);
		}
	}
	return finish(&o, out);
}

int kneil_summary_to_line(const kneil_summary_t *s, char *out, size_t size)
{
	if (size < 2) {
		return -1;
	}
	out_t o = {out, out + size - 1, 1};

	put_str(&o, "D,");
	put_uint(&o, s->format);
	put_ch(&o, ',');
	put_uint(&o, s->n_classes);
	for (int k = 0; k < KNEIL_SUMMARY_CLASSES; k++) {
		put_ch(&o, ',');
		put_uint(&o, s->minutes_by_class[k]);
	}
	put_ch(&o, ',');
	put_uint(&o, s->minutes_sitting);
	put_ch(&o, ',');
	put_uint(&o, s->minutes_standing);
	put_ch(&o, ',');
	put_uint(&o, s->minutes_still_unknown);
	put_ch(&o, ',');
	put_uint(&o, s->n_sit_to_stand);
	put_ch(&o, ',');
	put_uint(&o, s->n_stand_to_sit);
	put_ch(&o, ',');
	put_uint(&o, s->n_bed_transfer);
	put_ch(&o, ',');
	put_uint(&o, s->sts_seconds_x10);
	put_ch(&o, ',');
	put_uint(&o, s->steps);
	put_ch(&o, ',');
	put_uint(&o, s->cadence_x10);
	put_ch(&o, ',');
	put_uint(&o, s->stairs_cadence_x10);
	put_ch(&o, ',');
	put_int(&o, s->sit_tilt_x10);
	return finish(&o, out);
}

static char printable(uint8_t c)
{
	return (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
}

int kneil_info_encode(const char *text, uint8_t *b, size_t size)
{
	size_t n = 0;

	if (size < 2) {
		return -1;
	}
	while (text[n] != '\0' && n < KNEIL_INFO_TEXT_MAX && n + 1 < size) {
		b[1 + n] = (uint8_t)printable((uint8_t)text[n]);
		n++;
	}
	if (n == 0) {
		return -1;
	}
	b[0] = KNEIL_PKT_INFO;
	return (int)(1 + n);
}

int kneil_info_to_line(const uint8_t *b, size_t len, char *out, size_t size)
{
	if (len < 2 || len > 1 + KNEIL_INFO_TEXT_MAX || b[0] != KNEIL_PKT_INFO || size < 2) {
		return -1;
	}
	out_t o = {out, out + size - 1, 1};

	put_str(&o, "I,");
	for (size_t i = 1; i < len; i++) {
		put_ch(&o, printable(b[i]));
	}
	return finish(&o, out);
}

int kneil_cmd_to_ctrl(const char *line)
{
	if (line[0] == '\0' || line[1] != '\0') {
		return -1;
	}
	switch (line[0]) {
	case 'S':
		return KNEIL_CTRL_RAW_START;
	case 'P':
		return KNEIL_CTRL_RAW_STOP;
	case 'T':
		return KNEIL_CTRL_SELFTEST;
	case 'Q':
		return KNEIL_CTRL_SUMMARY;
	case 'Z':
		return KNEIL_CTRL_RESET;
	default:
		return -1;
	}
}
