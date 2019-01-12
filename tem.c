#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_xrm.h>
#include <xcb/xcbext.h>
#include <X11/keysym.h>
#include <string.h>
#include <locale.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <stdio.h>
#include <poll.h>
#include <err.h>

#include <pty.h> /* XXX */
#include "arg.h"

#define POLLTIMEOUT 50
#define DEBUG(...)	warnx(__VA_ARGS__)


#define FOREACH_CELL(X)	for (X = 0; X < term.width * term.height; X++)

struct xt_cursor {
	int x, y;
};

enum {
	BOLD = 1 << 1
};

struct tattr {
	uint16_t ch;
	int8_t fg, bg, attr;
};

struct font_s {
	xcb_font_t ptr;
	int descent, height, width;
	uint16_t char_max;
	uint16_t char_min;
	xcb_charinfo_t *width_lut;
};

typedef struct term_s {
	int width, height;
	int fg, bg;
	int default_fg, default_bg;
	struct xt_cursor cursor;
	/* uint16_t *map, map_curs; */
	uint16_t map_curs;
	struct tattr *map;

	int padding;
	uint16_t cursor_char;
	char fontline[BUFSIZ];
	char wants_redraw, esc;
	char *esc_str;
	uint8_t fi, bi, attr;
} term_t;

uint32_t colors[255] = {
	/* http://www.calmar.ws/vim/256-xterm-24bit-rgb-color-chart.html */

	/* base 16 */
	[ 0] = 0x000000,
	[ 1] = 0x800000,
	[ 2] = 0x008000,
	[ 3] = 0x808000,
	[ 4] = 0x000080,
	[ 5] = 0x800080,
	[ 6] = 0x008080,
	[ 7] = 0xc0c0c0,
	[ 8] = 0x808080,
	[ 9] = 0xff0000,
	[10] = 0x00ff00,
	[11] = 0xffff00,
	[12] = 0x0000ff,
	[13] = 0xff00ff,
	[14] = 0x00ffff,
	[15] = 0xffffff,

	/* 216 mod */
	[ 16] = 0x000000,
	[ 17] = 0x00005f,
	[ 18] = 0x000087,
	[ 19] = 0x0000af,
	[ 20] = 0x0000d7,
	[ 21] = 0x0000ff,
	[ 22] = 0x005f00,
	[ 23] = 0x005f5f,
	[ 24] = 0x005f87,
	[ 25] = 0x005faf,
	[ 26] = 0x005fd7,
	[ 27] = 0x005fff,
	[ 28] = 0x008700,
	[ 29] = 0x00875f,
	[ 30] = 0x008787,
	[ 31] = 0x0087af,
	[ 32] = 0x0087d7,
	[ 33] = 0x0087ff,
	[ 34] = 0x00af00,
	[ 35] = 0x00af5f,
	[ 36] = 0x00af87,
	[ 37] = 0x00afaf,
	[ 38] = 0x00afd7,
	[ 39] = 0x00afff,
	[ 40] = 0x00d700,
	[ 41] = 0x00d75f,
	[ 42] = 0x00d787,
	[ 43] = 0x00d7af,
	[ 44] = 0x00d7d7,
	[ 45] = 0x00d7ff,
	[ 46] = 0x00ff00,
	[ 47] = 0x00ff5f,
	[ 48] = 0x00ff87,
	[ 49] = 0x00ffaf,
	[ 50] = 0x00ffd7,
	[ 51] = 0x00ffff,
	[ 52] = 0x5f0000,
	[ 53] = 0x5f005f,
	[ 54] = 0x5f0087,
	[ 55] = 0x5f00af,
	[ 56] = 0x5f00d7,
	[ 57] = 0x5f00ff,
	[ 58] = 0x5f5f00,
	[ 59] = 0x5f5f5f,
	[ 60] = 0x5f5f87,
	[ 61] = 0x5f5faf,
	[ 62] = 0x5f5fd7,
	[ 63] = 0x5f5fff,
	[ 64] = 0x5f8700,
	[ 65] = 0x5f875f,
	[ 66] = 0x5f8787,
	[ 67] = 0x5f87af,
	[ 68] = 0x5f87d7,
	[ 69] = 0x5f87ff,
	[ 70] = 0x5faf00,
	[ 71] = 0x5faf5f,
	[ 72] = 0x5faf87,
	[ 73] = 0x5fafaf,
	[ 74] = 0x5fafd7,
	[ 75] = 0x5fafff,
	[ 76] = 0x5fd700,
	[ 77] = 0x5fd75f,
	[ 78] = 0x5fd787,
	[ 79] = 0x5fd7af,
	[ 80] = 0x5fd7d7,
	[ 81] = 0x5fd7ff,
	[ 82] = 0x5fff00,
	[ 83] = 0x5fff5f,
	[ 84] = 0x5fff87,
	[ 85] = 0x5fffaf,
	[ 86] = 0x5fffd7,
	[ 87] = 0x5fffff,
	[ 88] = 0x870000,
	[ 89] = 0x87005f,
	[ 90] = 0x870087,
	[ 91] = 0x8700af,
	[ 92] = 0x8700d7,
	[ 93] = 0x8700ff,
	[ 94] = 0x875f00,
	[ 95] = 0x875f5f,
	[ 96] = 0x875f87,
	[ 97] = 0x875faf,
	[ 98] = 0x875fd7,
	[ 99] = 0x875fff,
	[100] = 0x878700,
	[101] = 0x87875f,
	[102] = 0x878787,
	[103] = 0x8787af,
	[104] = 0x8787d7,
	[105] = 0x8787ff,
	[106] = 0x87af00,
	[107] = 0x87af5f,
	[108] = 0x87af87,
	[109] = 0x87afaf,
	[110] = 0x87afd7,
	[111] = 0x87afff,
	[112] = 0x87d700,
	[113] = 0x87d75f,
	[114] = 0x87d787,
	[115] = 0x87d7af,
	[116] = 0x87d7d7,
	[117] = 0x87d7ff,
	[118] = 0x87ff00,
	[119] = 0x87ff5f,
	[120] = 0x87ff87,
	[121] = 0x87ffaf,
	[122] = 0x87ffd7,
	[123] = 0x87ffff,
	[124] = 0xaf0000,
	[125] = 0xaf005f,
	[126] = 0xaf0087,
	[127] = 0xaf00af,
	[128] = 0xaf00d7,
	[129] = 0xaf00ff,
	[130] = 0xaf5f00,
	[131] = 0xaf5f5f,
	[132] = 0xaf5f87,
	[133] = 0xaf5faf,
	[134] = 0xaf5fd7,
	[135] = 0xaf5fff,
	[136] = 0xaf8700,
	[137] = 0xaf875f,
	[138] = 0xaf8787,
	[139] = 0xaf87af,
	[140] = 0xaf87d7,
	[141] = 0xaf87ff,
	[142] = 0xafaf00,
	[143] = 0xafaf5f,
	[144] = 0xafaf87,
	[145] = 0xafafaf,
	[146] = 0xafafd7,
	[147] = 0xafafff,
	[148] = 0xafd700,
	[149] = 0xafd75f,
	[150] = 0xafd787,
	[151] = 0xafd7af,
	[152] = 0xafd7d7,
	[153] = 0xafd7ff,
	[154] = 0xafff00,
	[155] = 0xafff5f,
	[156] = 0xafff87,
	[157] = 0xafffaf,
	[158] = 0xafffd7,
	[159] = 0xafffff,
	[160] = 0xd70000,
	[161] = 0xd7005f,
	[162] = 0xd70087,
	[163] = 0xd700af,
	[164] = 0xd700d7,
	[165] = 0xd700ff,
	[166] = 0xd75f00,
	[167] = 0xd75f5f,
	[168] = 0xd75f87,
	[169] = 0xd75faf,
	[170] = 0xd75fd7,
	[171] = 0xd75fff,
	[172] = 0xd78700,
	[173] = 0xd7875f,
	[174] = 0xd78787,
	[175] = 0xd787af,
	[176] = 0xd787d7,
	[177] = 0xd787ff,
	[178] = 0xd7af00,
	[179] = 0xd7af5f,
	[180] = 0xd7af87,
	[181] = 0xd7afaf,
	[182] = 0xd7afd7,
	[183] = 0xd7afff,
	[184] = 0xd7d700,
	[185] = 0xd7d75f,
	[186] = 0xd7d787,
	[187] = 0xd7d7af,
	[188] = 0xd7d7d7,
	[189] = 0xd7d7ff,
	[190] = 0xd7ff00,
	[191] = 0xd7ff5f,
	[192] = 0xd7ff87,
	[193] = 0xd7ffaf,
	[194] = 0xd7ffd7,
	[195] = 0xd7ffff,
	[196] = 0xff0000,
	[197] = 0xff005f,
	[198] = 0xff0087,
	[199] = 0xff00af,
	[200] = 0xff00d7,
	[201] = 0xff00ff,
	[202] = 0xff5f00,
	[203] = 0xff5f5f,
	[204] = 0xff5f87,
	[205] = 0xff5faf,
	[206] = 0xff5fd7,
	[207] = 0xff5fff,
	[208] = 0xff8700,
	[209] = 0xff875f,
	[210] = 0xff8787,
	[211] = 0xff87af,
	[212] = 0xff87d7,
	[213] = 0xff87ff,
	[214] = 0xffaf00,
	[215] = 0xffaf5f,
	[216] = 0xffaf87,
	[217] = 0xffafaf,
	[218] = 0xffafd7,
	[219] = 0xffafff,
	[220] = 0xffd700,
	[221] = 0xffd75f,
	[222] = 0xffd787,
	[223] = 0xffd7af,
	[224] = 0xffd7d7,
	[225] = 0xffd7ff,
	[226] = 0xffff00,
	[227] = 0xffff5f,
	[228] = 0xffff87,
	[229] = 0xffffaf,
	[230] = 0xffffd7,
	[231] = 0xffffff,
};

void clrscr();
void xt_rectfill(int, int);


static xcb_connection_t *conn;
static xcb_screen_t *scr;
static xcb_window_t win;
static struct font_s *font;
static term_t term;
static xcb_gcontext_t gc;
/* static char fontline[BUFSIZ] = "-*-gohufont-medium-*-*-*-11-*-*-*-*-*-*-1"; */
/* static char fontline[BUFSIZ] = "-*-fixed-*-r-normal-*-10-*-*-*-*-*-*-1"; */
static int d;

/*
 * thanks for wmdia for doing what xcb devs can't
 */
xcb_void_cookie_t
xcb_poly_text_16_simple(xcb_connection_t *c, xcb_drawable_t drawable,
		xcb_gcontext_t gc, int16_t x, int16_t y, uint32_t len,
		const uint16_t *str)
{
	struct iovec xcb_parts[7];
	static const xcb_protocol_request_t xcb_req = {
		5,                /* count  */
		0,                /* ext    */
		XCB_POLY_TEXT_16, /* opcode */
		1                 /* isvoid */
	};
	uint8_t xcb_lendelta[2];
	xcb_void_cookie_t xcb_ret;
	xcb_poly_text_8_request_t xcb_out;

	xcb_out.pad0 = 0;
	xcb_out.drawable = drawable;
	xcb_out.gc = gc;
	xcb_out.x = x;
	xcb_out.y = y;

	xcb_lendelta[0] = len;
	xcb_lendelta[1] = 0;

	xcb_parts[2].iov_base = (char *)&xcb_out;
	xcb_parts[2].iov_len = sizeof(xcb_out);
	xcb_parts[3].iov_base = 0;
	xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

	xcb_parts[4].iov_base = xcb_lendelta;
	xcb_parts[4].iov_len = sizeof(xcb_lendelta);
	xcb_parts[5].iov_base = (char *)str;
	xcb_parts[5].iov_len = len * sizeof(int16_t);

	xcb_parts[6].iov_base = 0;
	xcb_parts[6].iov_len = -(xcb_parts[4].iov_len + xcb_parts[5].iov_len)
		& 3;

	xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);

	return xcb_ret;
}

void
set_fg(int fg) {
	uint32_t mask;
	uint32_t values[3];

	mask = XCB_GC_FOREGROUND;
	values[0] = fg;
	xcb_change_gc(conn, gc, mask, values);
	term.fg = fg;

	return;
}

void
set_bg(int bg) {
	uint32_t mask;
	uint32_t values[2];

	mask = XCB_CW_BACK_PIXEL;
	values[0] = bg;
	xcb_change_window_attributes(conn, win, mask, values);
}

void
set_color_fg(int c) {
	set_fg(colors[c]);
}

void
set_color_bg(int c) {
	set_bg(colors[c]);
}

int
redraw() {
	int x, y;
	int i;

	clrscr();

	for (x = 0; x < term.width; x++) {
		for (y = 0; y < term.height; y++) {
			i = x + (y * term.width);

			if (term.map[i].ch) {
				if (term.map[i].fg) {
					if (term.map[i].fg != term.fg)
						set_color_fg(term.map[i].fg);
				} else {
					set_fg(term.default_fg);
				}

				xcb_poly_text_16_simple(conn, win, gc,
						(term.padding * 2) + ((x + 1) * font->width),
						(term.padding * 2) + ((y + 1) * font->height),
						1, &term.map[x + (y * term.width)].ch
				);
			}
		}
	}

	xcb_flush(conn);
	return 0;
}
void
scroll(int dir) {
	/* add first line to history queue */
	memmove(term.map, term.map + term.width, term.width * term.height * sizeof(*term.map));
	term.cursor.y -= 1;
	clrscr();
}

void
cursor_next(struct xt_cursor *curs) {
	if (curs->x + 1 >= term.width) {
		curs->y++;

		curs->x = 0;
	} else
		curs->x++;

	if (curs->y + 1 > term.height)
		scroll(+1);

	term.map_curs = curs->x + (curs->y * term.width);
}

static xcb_keysym_t
xcb_get_keysym(xcb_keycode_t keycode, uint16_t state)
{
	xcb_key_symbols_t *keysyms;

	if (!(keysyms = xcb_key_symbols_alloc(conn)))
		return 0;

	xcb_keysym_t keysym = xcb_key_symbols_get_keysym(keysyms, keycode, state);
	xcb_key_symbols_free(keysyms);

	return keysym;
}

struct font_s *
load_font(xcb_gcontext_t gc, const char *name) {
	xcb_query_font_cookie_t queryreq;
	xcb_query_font_reply_t *font_info;
	xcb_void_cookie_t cookie;
	xcb_font_t font;
	struct font_s *r;

	font = xcb_generate_id(conn);

	cookie = xcb_open_font_checked(conn, font, strlen(name), name);
	if (xcb_request_check(conn, cookie)) {
		warnx("could not load font '%s'", name);
		return NULL;
	}

	r = malloc(sizeof(struct font_s));
	if (r == NULL) {
		warn("malloc");
		return NULL;
	}

	queryreq = xcb_query_font(conn, font);
	font_info = xcb_query_font_reply(conn, queryreq, NULL);

	r->ptr = font;
	r->descent = font_info->font_descent;
	r->height = font_info->font_ascent + font_info->font_descent;
	r->width = font_info->max_bounds.character_width;
	r->char_max = font_info->max_byte1 << 8 | font_info->max_char_or_byte2;
	r->char_min = font_info->min_byte1 << 8 | font_info->min_char_or_byte2;

	xcb_change_gc(conn, gc, XCB_GC_FONT, &font);

	DEBUG("loaded font '%s'", term.fontline);
	return r;
}

int
utf_len(char *str) {
	uint8_t *utf = (uint8_t *)str;

	if (utf[0] < 0x80)
		return 1;
	else if ((utf[0] & 0xe0) == 0xc0)
		return 2;
	else if ((utf[0] & 0xf0) == 0xe0)
		return 3;
	else if ((utf[0] & 0xf8) == 0xf0)
		return 4;
	else if ((utf[0] & 0xfc) == 0xf8)
		return 5;
	else if ((utf[0] & 0xfe) == 0xfc)
		return 6;

	return 1;
}

uint16_t
utf_combine(char *str) {
	uint16_t c;
	uint8_t *utf = (uint8_t *)str;

	switch (utf_len(str)) {
	case 1:
		c = utf[0];
		break;
	case 2:
		c = (utf[0] & 0x1f) << 6 | (utf[1] & 0x3f);
		break;
	case 3:
		c = (utf[0] & 0xf) << 12 | (utf[1] & 0x3f) << 6 | (utf[2] & 0x3f);
		break;
	case 4:
	case 5:
	case 6:
		c = 0xfffd;
		break;
	}

	return c >> 8 | c << 8;
}

void
set_cell(int x, int y, char *str) {
	uint16_t c;
	uint8_t *utf = (uint8_t *)str;

	c = utf_combine(str);
	term.map[x + (y * term.width)].ch = c;
	term.map[x + (y * term.width)].fg = term.fi;
}

void
xt_rectfill(int x, int y) {
	xcb_rectangle_t rect;
	int rx, ry;

	rx = x * font->width;
	ry = y * font->height;

	rect.x = rx;
	rect.y = ry;
	rect.width = font->width;
	rect.height = font->height;

	xcb_poly_fill_rectangle(conn, win, gc, 1, &rect);
}

int
valid_xy(int x, int y) {
	if (x > term.width || x < 0)
		return 0;

	if (y > term.height || y < 0)
		return 0;

	return 1;
}

struct tattr *
map_pos_now() {
	return term.map + term.cursor.x + (term.cursor.y * term.cursor.x);
}

void
sgr(char *buf, size_t n) {
	int c;
	char *p;
	size_t i;
	int longfmt;

	i = n;
	p = buf;
	longfmt = 0;

	n -= 2;

	DEBUG("'%s'", p);

	while (sscanf(p, "%u", &c)) {
		DEBUG("in loop (c:%d)", c);

		switch (c) {
		case 0:
			DEBUG("resetting colors");
			term.attr = term.bi = term.fi = 0;
			break;
		case 1:
			term.attr |= BOLD;
			break;
		case 30:
		case 31:
		case 32:
		case 33:
		case 34:
		case 35:
		case 36:
		case 37:
			DEBUG("setting fg to %d", c - 30);
			term.fi =  term.attr & BOLD ? c - 30 + 8 : c - 30;
			break;
		case 38:
			longfmt = 1;
			break;
		case 40:
		case 41:
		case 42:
		case 43:
		case 44:
		case 45:
		case 46:
		case 47:
			term.bi =  term.attr & BOLD ? c - 40 + 8 : c - 40;
			break;
		case 48:
			longfmt = 1;
			break;
		}

		if (longfmt) {
			int r, g, b, f1, f2, f3, ret;
			DEBUG("longfmt");

			if (sscanf(p, "%u;%u;%u;%u;%u;%u", &f1, &f2, &f3, &r, &g, &b) > 1) {
				DEBUG("sscanf match");

				if (f1 != 38)
					return;

				switch (f2) {
				case 2:
					/* rgb */
					break;
				case 5:
					/* 256 */
					DEBUG("setting 256 fg to %d", f3);
					term.fi = f3;
					break;
				}
			}
		}

		while (*p != ';')
			(void)*p++;
	}

}

void
csiseq(char *esc, size_t n) {
	char *p;

	p = esc;

	if (p[1] != '[')
		return;

	DEBUG("csi length %ld, type '%c'", n, p[n]);

	*p++;
	*p++;
	//*p += 2;

	for (int i = 0; i < n; i++)
		DEBUG("%d: %lc", i, p[i]);

	switch (p[n]) {
	case 'J': {
		uint16_t *off;
		off_t len;

		DEBUG("clear screen?");
		switch (p[0]) {
		case '3':
			/* clear screen and wipe scrollback */
		case '2':
			/* clear entire screen */
		case '1':
			/* clear from cursor to beginning of screen */
		case 'J':
		case '0':
			/* clear from cursor to end of screen (default) */

			/* XXX */
			DEBUG("clearing screen");
			memset(term.map, 0, term.width * term.height * sizeof(*term.map));
			term.cursor.x = term.cursor.y = 0;
			break;
		}
	}	break;
	case 'H': {
		/* CUP: n ; m H */
		unsigned int row, col;

		row = col = 0;

		if (memchr(p, ';', n)) {
			/* n + m */
			sscanf(p, "%u:%u", &row, &col);
			DEBUG("cup(%d, %d);", col, row);
		} else {
			/* n only */
			DEBUG("cup(0, %d);", row);
			sscanf(p, "%u", &row);
		}

		if (valid_xy(row, col)) {
			term.cursor.x = col - 1;
			term.cursor.y = row - 1;
		}
	}	break;
	case 'm': {
		/* sgr */
		int c;
		int i;

		for (i = c = 0; i < n; i++)
			if (p[i] == ';')
				c++;

		DEBUG("%d", c);
		sgr(p, c);
	}	break;
	default:
		DEBUG("unknown escape type: '%lc' (0x%x)", p[n], p[n]);
		break;
	}
}

void
xcb_printf(char *fmt, ...) {
	va_list args;
	char buf[BUFSIZ];
	char *p;
	size_t n = 0;

	va_start(args, fmt);
	vsprintf(buf, fmt, args);

	buf[BUFSIZ - 1] = 0;

	p = buf;

	while (*p) {
		if (term.esc) {
			//DEBUG("%ld %c", p - buf, *p);

			if (p - buf == 1 && *p == '[')
				;
			else
				switch (*p) {
				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
				case ';':
					n++;
					/* accepted characters */
					break;
				case '[':
					break;
				default:
					csiseq(term.esc_str, n);
					term.esc = 0;
					break;
				}

			(void)*p++;
			if (p - buf >= BUFSIZ)
				return;

			continue;
		}

		switch (*p) {
		case '\t': {
			int i;

			for (i = 0; i < 8 - (term.cursor.x % 8); i++)
				;

			xcb_printf("%*s", i, "");
		 }	break;
		case '\r':
			term.cursor.x = 0;
			break;
		case '\n':
			term.cursor.y++;
			break;
		case 0x1b:
			term.esc = 1;
			term.esc_str = p;
			n = 0;
			break;
		default:
			set_cell(term.cursor.x, term.cursor.y, p);
			cursor_next(&term.cursor);
			break;
		}

		p += utf_len(p);
	}
}

void
resize(int x, int y) {
	uint32_t values[3];
	uint32_t mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	struct tattr *rmap;
	int i;

	values[0] = font->width * (x - 1);
	values[1] = font->height * (y - 1);

	xcb_configure_window(conn, win, mask, values);

	rmap = malloc(sizeof(struct tattr) * (x * y));
	if (rmap == NULL)
		err(1, "malloc");

	memset(rmap, 0, x * y * sizeof(uint16_t));

	FOREACH_CELL(i)
		term.map[i].bg = term.map[i].fg = -1;

	if (term.map != NULL) {
		memcpy(rmap, term.map, term.width * term.height);
		free(term.map);
	}

	term.width = x - 1;
	term.height = y - 1;
	term.map = rmap;
}

void
load_config() {
	xcb_xrm_database_t *db;
	char *xrm_buf;

	db = xcb_xrm_database_from_default(conn);
	if (db != NULL) {
		xcb_xrm_resource_get_string(db, "xt.font", NULL, &xrm_buf);
		if (xrm_buf != NULL) {
			strncpy(term.fontline, xrm_buf, BUFSIZ);
			free(xrm_buf);
		}

		xcb_xrm_resource_get_string(db, "xt.foreground", NULL, &xrm_buf);
		if (xrm_buf != NULL) {
			puts("loaded xt.foreground");
			if (xrm_buf[0] == '#')
				xrm_buf[0] = ' ';

			term.fg = strtoul(xrm_buf, NULL, 16);
			free(xrm_buf);
		}

		xcb_xrm_resource_get_string(db, "xt.background", NULL, &xrm_buf);
		if (xrm_buf != NULL) {
			puts("loaded xt.background");
			if (xrm_buf[0] == '#')
				xrm_buf[0] = ' ';

			term.bg = strtoul(xrm_buf, NULL, 16);
			free(xrm_buf);
		}
	}
}

void
clrscr() {
	xcb_clear_area(conn, 0, win, 0, 0,
			term.width * font->width,
			term.height * font->height
	);
}

static char wdata[BUFSIZ];
static char *wdatap = wdata;
void
keypress(xcb_keycode_t keycode, xcb_keysym_t keysym) {
	switch (keysym) {
	case (0xffe2):
	case (0xffe4):
	case (0xfe03):
	case (0xffe9):
	case (0xffe3):
	case (0xffe1):
		/* lone modifiers, do nothing */
		break;
	case XK_Tab:
		break;
	case 0x1b3: /* ^L */
		clrscr();
		redraw();
		break;
	case (0xff0d): /* enter */
		*wdatap++ = '\n';
		write(d, &wdata, wdatap - wdata);
		memset(wdata, 0, wdatap - wdata);
		wdatap = wdata;
		term.cursor.y++;
		term.cursor.x = 0;
		//strcpy(wdata, "1+1\n");
		//write(d, &wdata, 4);
		break;
	case XK_BackSpace: /* backspace */
		puts("backspace");
		if (wdatap == wdata)
			break;

		*wdatap-- = 0;
		term.cursor.x--;
		set_cell(term.cursor.x, term.cursor.y, "  ");
		break;
	default:
		xcb_printf("%lc", keysym);
		//DEBUG("unknown keysym: '%lc' (0x%x)", keysym, keysym);
		*wdatap++ = keysym;
		break;
	}
}

void
cleanup() {
	xcb_disconnect(conn);
	free(term.map);
	free(font);
	_exit(0);
}

int
main(int argc, char **argv) {
	uint32_t mask;
	uint32_t values[3];

	char *p;
	char *argv0;

	ARGBEGIN {
	case 'f':
		strncpy(term.fontline, ARGF(), BUFSIZ);
		break;
	} ARGEND

	/* defaults */
	term.bg = term.default_bg = 0x000000;
	term.fg = term.default_fg = 0xFFFFFF;
	term.padding = -2;
	term.cursor_char = 0x2d4a;
	term.wants_redraw = 1;
	strncpy(term.fontline, "-*-gohufont-medium-*-*-*-11-*-*-*-*-*-*-1", BUFSIZ);
	term.fi = term.bi = 0;

	(void)setlocale(LC_ALL, "");

	conn = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(conn))
		err(1, "xcb_connection_has_error");

	scr = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;

	load_config();
	mask = XCB_CW_EVENT_MASK;
	values[0] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;

	win = xcb_generate_id(conn);
	xcb_create_window (conn,
				XCB_COPY_FROM_PARENT,
				win,
				scr->root,
				0, 0,
				1, 1,
				10,
				XCB_WINDOW_CLASS_INPUT_OUTPUT,
				scr->root_visual,
				mask, values);

	xcb_map_window(conn, win);

	mask = XCB_GC_GRAPHICS_EXPOSURES;
	values[0] = 0;
	gc = xcb_generate_id(conn);
	xcb_create_gc(conn, gc, win, mask, values);

	font = load_font(gc, term.fontline);
	resize(80, 24);
	set_bg(term.bg);
	set_fg(term.fg);

	xcb_flush(conn);
	atexit(cleanup);
	signal(SIGCHLD, cleanup);

	set_color_fg(13);
	xcb_flush(conn);

	xcb_generic_event_t *ev;
	int s;
	ssize_t n;
	char buf[BUFSIZ];

	/* poll */
	struct pollfd fds[1];

	pid_t pid;
	struct winsize ws;
	struct termios tio;

	ws.ws_col = term.width - 1;
	ws.ws_row = term.height - 1;

	pid = forkpty(&d, NULL, NULL, &ws);
	if (pid < 0)
		err(1, "forkpty");

	if (pid == 0) {
		/* child */
		char *args[] = { "sh", NULL };
		execvp("/bin/sh", args);
	} else {
		/* parent */
		fds[0].fd = d;
		fds[0].events = POLLIN | POLLPRI;

		tcgetattr(d, &tio);
		tio.c_lflag &= ~ECHO;
		tcsetattr(d, TCSAFLUSH, &tio);

	}


	for (;;) {
		struct winsize ws;
		pid_t pid;

		/* select */

		ev = xcb_poll_for_event(conn);
		if (ev == NULL) {
			if (xcb_connection_has_error(conn))
				break;
			else {
				s = poll(fds, 1, POLLTIMEOUT);
				if (s < 0)
					err(1, "poll");

				if (s && fds[0].revents & POLLIN) {
					memset(buf, 0, BUFSIZ);
					n = read(d, &buf, BUFSIZ);
					xcb_printf("%*s", n, buf);
					term.wants_redraw = 1;
				}

				if (term.wants_redraw)
					term.wants_redraw = redraw();

				continue;
			}
		}

		switch (ev->response_type & ~0x80) {
		case XCB_EXPOSE: {
			/* setup stuff */
			term.wants_redraw = 1;
		} break;
		case XCB_KEY_PRESS: {
			xcb_key_press_event_t *e = (xcb_key_press_event_t *)ev;
			keypress(e->detail, xcb_get_keysym(e->detail, e->state));
			term.wants_redraw = 1;
		} break;
		default:
			DEBUG("unknown event %d", ev->response_type & ~0x80);
		}
	}

	return 0;
}
