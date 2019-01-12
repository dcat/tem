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

#define POLLTIMEOUT 60
#define DEBUG(...)	warnx(__VA_ARGS__)

struct xt_cursor {
	int x, y;
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
	uint16_t *map, map_curs;
	int padding;
	uint16_t cursor_char;
	char fontline[BUFSIZ];
	char wants_redraw, esc;
	char *esc_str;
} term_t;

uint32_t colors[255] = {
	/* http://www.calmar.ws/vim/256-xterm-24bit-rgb-color-chart.html */
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

	set_fg(term.default_fg);
	clrscr();

	for (x = 0; x < term.width; x++)
		for (y = 0; y < term.height; y++)
			if (term.map[x + (y * term.width)])
				xcb_poly_text_16_simple(conn, win, gc,
						(term.padding * 2) + ((x + 1) * font->width),
						(term.padding * 2) + ((y + 1) * font->height),
						1, &term.map[x + (y * term.width)]
				);

	xcb_flush(conn);
	return 0;
}
void
scroll(int dir) {
	/* add first line to history queue */
	memmove(term.map, term.map + term.width, term.width * term.height * sizeof(uint16_t));
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

	if (curs->y + 1> term.height)
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
	int rx, ry;

	c = utf_combine(str);

	rx = (x + 1) * font->width;
	ry = (y + 1) * font->height;

	term.map[x + (y * term.width)] = c;
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

void
csiseq(char *esc, size_t n) {
	char *p;

	p = esc;

	if (p[1] != '[')
		return;


	DEBUG("csi length %ld", n);

	*p++;
	*p++;
	//*p += 2;

	switch (p[n]) {
	case 'm':
		/* sgr */
		break;
	case 'J': {
		uint16_t *off;
		off_t len;

		switch (p[0]) {
		case 'J':
		case '0':
			off = term.map + term.cursor.x + (term.cursor.y * term.cursor.x);
			len = (term.cursor.x * term.cursor.y) - (term.map - off);
			memset(off, 0, len);
			/* clear from cursor to end of screen (default) */
			break;
		case '1':
			/* clear from cursor to beginning of screen */
			break;
		case '2':
			/* clear entire screen */
			break;
		case '3':
			/* clear screen and wipe scrollback */
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
	default:
		DEBUG("unknown escape type: '%lc'", p[n]);
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
				case '[':
					n++;
					/* accepted characters */
					break;
				default:
					csiseq(term.esc_str, n);
					term.esc = !term.esc;
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
	uint16_t *rmap;

	values[0] = font->width * (x - 1);
	values[1] = font->height * (y - 1);

	xcb_configure_window(conn, win, mask, values);

	rmap = malloc(sizeof(int) * (x * y));
	if (rmap == NULL)
		err(1, "malloc");

	memset(rmap, 0, x * y * sizeof(uint16_t));

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
	xcb_rectangle_t rect;
	int rx, ry;
	uint32_t tmpc;

	rect.x = 0;
	rect.y = 0;
	rect.width = term.width * font->width;
	rect.height = term.height * font->height;

	tmpc = term.fg;
	set_fg(term.bg);
	xcb_poly_fill_rectangle(conn, win, gc, 1, &rect);
	set_fg(tmpc);
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
		set_fg(term.bg);
		set_cell(term.cursor.x, term.cursor.y, "  ");
		set_fg(term.fg);
		break;
	default:
		xcb_printf("%lc", keysym);
		DEBUG("unknown keysym: '%lc' (0x%x)", keysym, keysym);
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
	term.padding = 0;
	term.cursor_char = 0x2d4a;
	term.wants_redraw = 1;
	strncpy(term.fontline, "-*-gohufont-medium-*-*-*-11-*-*-*-*-*-*-1", BUFSIZ);

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
