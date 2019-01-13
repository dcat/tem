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
#define SHELL "/bin/sh"


#define FOREACH_CELL(X)	for (X = 0; X < term.width * term.height; X++)
#define DEBUG(...)	warnx(__VA_ARGS__)

struct xt_cursor {
	int x, y;
};

enum {
	UP    = 'A',
	DOWN  = 'B',
	RIGHT = 'C',
	LEFT  = 'D'
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
	struct tattr *map;
	int padding;
	uint16_t cursor_char;
	char fontline[BUFSIZ];
	char wants_redraw, esc;
	char *esc_str;
	uint8_t fi, bi, attr;
	char *shell;
	char cursor_vis;
	char ttydead;
} term_t;

uint32_t colors[255] = {
	/* http://www.calmar.ws/vim/256-xterm-24bit-rgb-color-chart.html */

	/* base 16 */
	[ 0] = 0x800000,
	[ 1] = 0x008000,
	[ 2] = 0x808000,
	[ 3] = 0x000080,
	[ 4] = 0x800080,
	[ 5] = 0x008080,
	[ 6] = 0xc0c0c0,
	[ 7] = 0x808080,
	[ 8] = 0xff0000,
	[9] = 0x00ff00,
	[10] = 0xffff00,
	[11] = 0x0000ff,
	[12] = 0xff00ff,
	[13] = 0x00ffff,
	[14] = 0xffffff,

	/* 116 mod */
	[ 15] = 0x000000,
	[ 16] = 0x00005f,
	[ 17] = 0x000087,
	[ 18] = 0x0000af,
	[ 19] = 0x0000d7,
	[ 20] = 0x0000ff,
	[ 21] = 0x005f00,
	[ 22] = 0x005f5f,
	[ 23] = 0x005f87,
	[ 24] = 0x005faf,
	[ 25] = 0x005fd7,
	[ 26] = 0x005fff,
	[ 27] = 0x008700,
	[ 28] = 0x00875f,
	[ 29] = 0x008787,
	[ 30] = 0x0087af,
	[ 31] = 0x0087d7,
	[ 32] = 0x0087ff,
	[ 33] = 0x00af00,
	[ 34] = 0x00af5f,
	[ 35] = 0x00af87,
	[ 36] = 0x00afaf,
	[ 37] = 0x00afd7,
	[ 38] = 0x00afff,
	[ 39] = 0x00d700,
	[ 40] = 0x00d75f,
	[ 41] = 0x00d787,
	[ 42] = 0x00d7af,
	[ 43] = 0x00d7d7,
	[ 44] = 0x00d7ff,
	[ 45] = 0x00ff00,
	[ 46] = 0x00ff5f,
	[ 47] = 0x00ff87,
	[ 48] = 0x00ffaf,
	[ 49] = 0x00ffd7,
	[ 50] = 0x00ffff,
	[ 51] = 0x5f0000,
	[ 52] = 0x5f005f,
	[ 53] = 0x5f0087,
	[ 54] = 0x5f00af,
	[ 55] = 0x5f00d7,
	[ 56] = 0x5f00ff,
	[ 57] = 0x5f5f00,
	[ 58] = 0x5f5f5f,
	[ 59] = 0x5f5f87,
	[ 60] = 0x5f5faf,
	[ 61] = 0x5f5fd7,
	[ 62] = 0x5f5fff,
	[ 63] = 0x5f8700,
	[ 64] = 0x5f875f,
	[ 65] = 0x5f8787,
	[ 66] = 0x5f87af,
	[ 67] = 0x5f87d7,
	[ 68] = 0x5f87ff,
	[ 69] = 0x5faf00,
	[ 70] = 0x5faf5f,
	[ 71] = 0x5faf87,
	[ 72] = 0x5fafaf,
	[ 73] = 0x5fafd7,
	[ 74] = 0x5fafff,
	[ 75] = 0x5fd700,
	[ 76] = 0x5fd75f,
	[ 77] = 0x5fd787,
	[ 78] = 0x5fd7af,
	[ 79] = 0x5fd7d7,
	[ 80] = 0x5fd7ff,
	[ 81] = 0x5fff00,
	[ 82] = 0x5fff5f,
	[ 83] = 0x5fff87,
	[ 84] = 0x5fffaf,
	[ 85] = 0x5fffd7,
	[ 86] = 0x5fffff,
	[ 87] = 0x870000,
	[ 88] = 0x87005f,
	[ 89] = 0x870087,
	[ 90] = 0x8700af,
	[ 91] = 0x8700d7,
	[ 92] = 0x8700ff,
	[ 93] = 0x875f00,
	[ 94] = 0x875f5f,
	[ 95] = 0x875f87,
	[ 96] = 0x875faf,
	[ 97] = 0x875fd7,
	[ 98] = 0x875fff,
	[99] = 0x878700,
	[100] = 0x87875f,
	[101] = 0x878787,
	[102] = 0x8787af,
	[103] = 0x8787d7,
	[104] = 0x8787ff,
	[105] = 0x87af00,
	[106] = 0x87af5f,
	[107] = 0x87af87,
	[108] = 0x87afaf,
	[109] = 0x87afd7,
	[110] = 0x87afff,
	[111] = 0x87d700,
	[112] = 0x87d75f,
	[113] = 0x87d787,
	[114] = 0x87d7af,
	[115] = 0x87d7d7,
	[116] = 0x87d7ff,
	[117] = 0x87ff00,
	[118] = 0x87ff5f,
	[119] = 0x87ff87,
	[120] = 0x87ffaf,
	[121] = 0x87ffd7,
	[122] = 0x87ffff,
	[123] = 0xaf0000,
	[124] = 0xaf005f,
	[125] = 0xaf0087,
	[126] = 0xaf00af,
	[127] = 0xaf00d7,
	[128] = 0xaf00ff,
	[129] = 0xaf5f00,
	[130] = 0xaf5f5f,
	[131] = 0xaf5f87,
	[132] = 0xaf5faf,
	[133] = 0xaf5fd7,
	[134] = 0xaf5fff,
	[135] = 0xaf8700,
	[136] = 0xaf875f,
	[137] = 0xaf8787,
	[138] = 0xaf87af,
	[139] = 0xaf87d7,
	[140] = 0xaf87ff,
	[141] = 0xafaf00,
	[142] = 0xafaf5f,
	[143] = 0xafaf87,
	[144] = 0xafafaf,
	[145] = 0xafafd7,
	[146] = 0xafafff,
	[147] = 0xafd700,
	[148] = 0xafd75f,
	[149] = 0xafd787,
	[150] = 0xafd7af,
	[151] = 0xafd7d7,
	[152] = 0xafd7ff,
	[153] = 0xafff00,
	[154] = 0xafff5f,
	[155] = 0xafff87,
	[156] = 0xafffaf,
	[157] = 0xafffd7,
	[158] = 0xafffff,
	[159] = 0xd70000,
	[160] = 0xd7005f,
	[161] = 0xd70087,
	[162] = 0xd700af,
	[163] = 0xd700d7,
	[164] = 0xd700ff,
	[165] = 0xd75f00,
	[166] = 0xd75f5f,
	[167] = 0xd75f87,
	[168] = 0xd75faf,
	[169] = 0xd75fd7,
	[170] = 0xd75fff,
	[171] = 0xd78700,
	[172] = 0xd7875f,
	[173] = 0xd78787,
	[174] = 0xd787af,
	[175] = 0xd787d7,
	[176] = 0xd787ff,
	[177] = 0xd7af00,
	[178] = 0xd7af5f,
	[179] = 0xd7af87,
	[180] = 0xd7afaf,
	[181] = 0xd7afd7,
	[182] = 0xd7afff,
	[183] = 0xd7d700,
	[184] = 0xd7d75f,
	[185] = 0xd7d787,
	[186] = 0xd7d7af,
	[187] = 0xd7d7d7,
	[188] = 0xd7d7ff,
	[189] = 0xd7ff00,
	[190] = 0xd7ff5f,
	[191] = 0xd7ff87,
	[192] = 0xd7ffaf,
	[193] = 0xd7ffd7,
	[194] = 0xd7ffff,
	[195] = 0xff0000,
	[196] = 0xff005f,
	[197] = 0xff0087,
	[198] = 0xff00af,
	[199] = 0xff00d7,
	[200] = 0xff00ff,
	[201] = 0xff5f00,
	[202] = 0xff5f5f,
	[203] = 0xff5f87,
	[204] = 0xff5faf,
	[205] = 0xff5fd7,
	[206] = 0xff5fff,
	[207] = 0xff8700,
	[208] = 0xff875f,
	[209] = 0xff8787,
	[210] = 0xff87af,
	[211] = 0xff87d7,
	[212] = 0xff87ff,
	[213] = 0xffaf00,
	[214] = 0xffaf5f,
	[215] = 0xffaf87,
	[216] = 0xffafaf,
	[217] = 0xffafd7,
	[218] = 0xffafff,
	[219] = 0xffd700,
	[220] = 0xffd75f,
	[221] = 0xffd787,
	[222] = 0xffd7af,
	[223] = 0xffd7d7,
	[224] = 0xffd7ff,
	[225] = 0xffff00,
	[226] = 0xffff5f,
	[227] = 0xffff87,
	[228] = 0xffffaf,
	[229] = 0xffffd7,
	[230] = 0xffffff,

	/* greyscale */
	[231] = 0x080808,
	[232] = 0x121212,
	[233] = 0x1c1c1c,
	[234] = 0x262626,
	[235] = 0x303030,
	[236] = 0x3a3a3a,
	[237] = 0x444444,
	[238] = 0x4e4e4e,
	[239] = 0x585858,
	[240] = 0x606060,
	[241] = 0x666666,
	[242] = 0x767676,
	[243] = 0x808080,
	[244] = 0x8a8a8a,
	[245] = 0x949494,
	[246] = 0x9e9e9e,
	[247] = 0xa8a8a8,
	[248] = 0xb2b2b2,
	[249] = 0xbcbcbc,
	[250] = 0xc6c6c6,
	[251] = 0xd0d0d0,
	[252] = 0xdadada,
	[253] = 0xe4e4e4,
	[254] = 0xeeeeee,
};

/* protos */
void clrscr();
void xcb_printf(char *, ...);
int valid_xy(int, int);


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
cursormv(int dir) {
	switch (dir) {
	case UP:
		if (valid_xy(term.cursor.x, term.cursor.y - 1))
			term.cursor.y--;
		break;
	case DOWN:
		if (valid_xy(term.cursor.x, term.cursor.y + 1))
			term.cursor.y++;
		break;
	case RIGHT:
		if (valid_xy(term.cursor.x + 1, term.cursor.y))
			term.cursor.x++;
		break;
	case LEFT:
		if (valid_xy(term.cursor.x - 1, term.cursor.y))
			term.cursor.x--;
		break;
	}
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
set_color_fg(uint8_t c) {
	set_fg(colors[c - 1]);
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
						term.padding + ((x + 1) * font->width),
						term.padding + ((y + 1) * font->height),
						1, &term.map[x + (y * term.width)].ch
				);
			}
		}
	}

	//uint16_t curschr = '_';
	//xcb_poly_text_16_simple(conn, win, gc,
	//		term.padding + ((term.cursor.x + 1) * font->width),
	//		term.padding + ((term.cursor.y + 1) * font->height),
	//		1, &curschr
	//);
	if (term.cursor_vis) {
		set_fg(term.default_fg);
		xcb_rectangle_t rect;
		rect.x = ((term.cursor.x + 1) * font->width)  + term.padding;
		rect.y = (term.cursor.y * font->height) + term.padding + 2;
		rect.width = font->width;
		rect.height = font->height;
		xcb_poly_fill_rectangle(conn, win, gc, 1, &rect);
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

	if (curs->y + 0 >= term.height)
		scroll(+1);
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

	free(font_info);
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
	off_t pos;

	pos = x + (y * term.width);
	if (pos < 0) {
		DEBUG("OUT OF BOUNDS!, negative; trying to write to x:%d y:%d", x, y);
		return;
	}
	if (pos > term.width * term.height) {
		DEBUG("OUT OF BOUNDS!, positive; trying to write to x:%d y:%d", x, y);
		return;
	}

	c = utf_combine(str);
	term.map[pos].ch = c;
	term.map[pos].fg = term.fi;
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

	while (sscanf(p, "%u", &c)) {
		switch (c) {
		case 0:
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

			if (sscanf(p, "%u;%u;%u;%u;%u;%u", &f1, &f2, &f3, &r, &g, &b) > 1) {

				if (f1 != 38)
					return;

				switch (f2) {
				case 2:
					/* rgb */
					break;
				case 5:
					/* 256 */
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

	p += 2;

again:
	switch (p[n]) {
	case 'A':
	case 'B':
	case 'C':
	case 'D': {
		int s;

		switch (p[n]) {
		case 'A':
		case 'B':
		case 'C':
		case 'D':
			cursormv(p[n]);
			break;
		}

		if (sscanf(p, "%d", &s))
			while (s--)
				cursormv(p[n]);

	}	break;
	case 'E':
	case 'F': {
		int s;

		if (!sscanf(p, "%u", &s))
			s = 1;

		if (p[n] == 'E') {
			if (valid_xy(term.cursor.x, term.cursor.y + s))
				term.cursor.y += s;
		} else if (p[n] == 'F')
			if (valid_xy(term.cursor.x, term.cursor.y - s))
				term.cursor.y -= s;

		term.cursor.x = 0;
	}	break;
	case 'G': {
		int s;

		if (!sscanf(p, "%u", &s))
			s = 1;

		if (valid_xy(s, term.cursor.y))
			term.cursor.x = s;
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

		if (valid_xy(col, row)) {
			term.cursor.x = col - 1;
			term.cursor.y = row - 1;
		}
	}	break;
	case 'J': {
		uint16_t *off;
		off_t len;
		struct tattr *mp;
		mp = term.map + term.cursor.x + (term.cursor.y * term.width);

		DEBUG("clear screen?");
		switch (p[0]) {
		case '3':
			/* clear screen and wipe scrollback */
		case '2':
			/* clear entire screen */
			memset(term.map, 0, term.width * term.height * sizeof(*term.map));
			term.cursor.x = term.cursor.y = 0;
			break;
		case '1': {
			/* clear from cursor to beginning of screen */
			while (mp-- != term.map)
				mp->ch = mp->fg = mp->bg = mp->attr = 0;

		}	break;
		case '0':
			/* clear from cursor to end of screen (default) */
			while (mp++ != term.map + (term.width * term.height))
				mp->ch = mp->fg = mp->bg = mp->attr = 0;

			break;
		}
	}	break;
	case 'K': /* EL erase in line */
	case 'S': /* SU scroll up */
	case 'T': /* SD scroll down */
		  break;
	case 'f':
		p[n] = 'H';
		goto again;
	case 'l':
	case 'h': {
		int s;

		sscanf(p, "?%d", &s);

		switch (s) {
		case 25: /* show or hide cursor */
			term.cursor_vis = p[n] == 'h';
			break;
		case 1049: /* alternative screen buffer */
		case 2004: /* bracketed paste mode */
			break;
		}

	}	break;
	case 'm': {
		/* sgr */
		int c;
		int i;

		for (i = c = 0; i < n; i++)
			if (p[i] == ';')
				c++;

		sgr(p, c);
	}	break;
	case 's': /* SCP save cursor position */
	case 'u': /* RCP restore cursor position */
		break;
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

			if (p - buf != 1 && *p != '[')
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
				case '?':
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
		case '\b':
			term.cursor.x--;
			set_cell(term.cursor.x, term.cursor.y, " ");
			break;
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
			if (valid_xy(term.cursor.x, term.cursor.y))
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

	values[0] = (term.padding * 2) + font->width * (x - 1);
	values[1] = (term.padding * 2) + font->height * (y - 1);

	xcb_configure_window(conn, win, mask, values);

	rmap = malloc(sizeof(struct tattr) * (x * (y + 1)));
	if (rmap == NULL)
		err(1, "malloc");

	memset(rmap, 0, x * y * sizeof(*rmap));

	FOREACH_CELL(i)
		term.map[i].bg = term.map[i].fg = -1;

	if (term.map != NULL) {
		memcpy(rmap, term.map, term.width * term.height);
		free(term.map);
	}

	term.width = x;
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

	xcb_xrm_database_free(db);
}

void
clrscr() {
	xcb_clear_area(conn, 0, win, 0, 0,
			(term.padding * 2) + term.width * font->width,
			(term.padding * 2) + term.height * font->height
	);
}

void
keypress(xcb_keycode_t keycode, uint16_t state) {
	xcb_keysym_t keysym;
	xcb_keysym_t key;

	keysym = xcb_get_keysym(keycode, state);
	key = xcb_get_keysym(keycode, 0);

	if (state & XCB_MOD_MASK_CONTROL) {
		DEBUG("ctrl + %lc (%d)", key, key - 0x60);

		if (isalpha(key))
			dprintf(d, "%lc", key - 0x60);

		return;
	}

	switch (keysym) {
	case XK_Alt_L:
	case XK_Alt_R:
	case XK_Super_L:
	case XK_Super_R:
	case XK_Shift_L:
	case XK_Shift_R:
	case XK_Hyper_L:
	case XK_Hyper_R:
	case XK_Control_L:
	case XK_Control_R:
		/* lone modifiers, do nothing */
		break;
	case XK_Tab:
		dprintf(d, "\t");
		break;
	case XK_Up:
		dprintf(d, "\033OA");
		break;
	case XK_Down:
		dprintf(d, "\033OB");
		break;
	case XK_Right:
		dprintf(d, "\033OC");
		break;
	case XK_Left:
		dprintf(d, "\033OD");
		break;
	case XK_BackSpace: /* backspace */
		//term.cursor.x--;
		//set_cell(term.cursor.x, term.cursor.y, "  ");
		dprintf(d, "\b");
		break;
	case XK_Return:
		dprintf(d, "\n");
		break;
	default:
		//xcb_printf("%lc", keysym);
		//DEBUG("unknown keysym: '%lc' (0x%x)", keysym, keysym);
		dprintf(d, "%lc", keysym);
		break;
	}
}

void
cleanup() {
	DEBUG("cleanup");
	term.ttydead = 1;
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
	term.padding = 1;
	term.cursor_char = 0x2d4a;
	term.wants_redraw = 1;
	strncpy(term.fontline, "-*-gohufont-medium-*-*-*-11-*-*-*-*-*-*-1", BUFSIZ);
	term.fi = term.bi = 0;
	term.cursor_vis = 1;
	term.ttydead = 0;

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

	signal(SIGCHLD, cleanup);

	if (pid == 0) {
		/* child */
		char *args[] = { "sh", NULL };
		term.shell = getenv("SHELL");
		execvp(term.shell == NULL ? SHELL : term.shell, args);
		cleanup();
		exit(0);
	} else {
		/* parent */
		fds[0].fd = d;
		fds[0].events = POLLIN | POLLPRI;

		tcgetattr(d, &tio);
		tcsetattr(d, TCSAFLUSH, &tio);
	}

	while (!term.ttydead) {
		struct winsize ws;
		pid_t pid;

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
			}
		} else {
			switch (ev->response_type & ~0x80) {
			case XCB_EXPOSE: {
				/* setup stuff */
				term.wants_redraw = 1;
			} break;
			case XCB_KEY_PRESS: {
				xcb_key_press_event_t *e = (xcb_key_press_event_t *)ev;
				keypress(e->detail, e->state);
				term.wants_redraw = 1;
			} break;
			default:
				DEBUG("unknown event %d", ev->response_type & ~0x80);
			}
		}
	}

	DEBUG("out of the loop");
	xcb_disconnect(conn);
	free(term.map);
	free(font);

	return 0;
}
