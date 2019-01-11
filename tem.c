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
#include <stdio.h>
#include <poll.h>
#include <err.h>

#include <pty.h> /* XXX */

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
	struct xt_cursor cursor;
	uint16_t *map, map_curs;
	int padding;
} term_t;


static xcb_connection_t *conn;
static xcb_screen_t *scr;
static xcb_window_t win;
static struct font_s *font;
static term_t term;
static xcb_gcontext_t gc;
/* static char fontline[BUFSIZ] = "-*-gohufont-medium-*-*-*-11-*-*-*-*-*-*-1"; */
static char fontline[BUFSIZ] = "-*-fixed-*-r-normal-*-10-*-*-*-*-*-*-1";
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
scroll(int dir) {
	puts("scroll");
	memmove(term.map, term.map + term.width, term.width + (term.width * term.height));
}

void
cursor_next(struct xt_cursor *curs) {
	if (curs->x + 1 >= term.width) {
		if (curs->y + 1 >= term.height)
			scroll(+1);
		else
			curs->y++;

		curs->x = 0;
	} else
		curs->x++;

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
redraw() {
	int x, y;

	for (x = 0; x < term.width; x++)
		for (y = 0; y < term.height; y++)
			if (term.map[x + (y * term.width)])
				xcb_poly_text_16_simple(conn, win, gc,
						(term.padding * 2) + ((x + 1) * font->width),
						(term.padding * 2) + ((y + 1) * font->height),
						1, &term.map[x + (y * term.width)]
				);

	xcb_flush(conn);
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

void
xt_printf(char *fmt, ...) {
	va_list args;
	char buf[BUFSIZ];
	char *p;

	va_start(args, fmt);
	vsprintf(buf, fmt, args);

	p = buf;

	while (*p) {
		fflush(stdout);
		switch (*p) {
		case '\t': {
			int i;
			int x = term.cursor.x;
			int ts = 8 - (x % 8);

			for (i = 0; i < ts; i++)
				xt_printf(" ");
		 }	break;
		case '\r':
			term.cursor.x = 0;
			break;
		case '\n':
			term.cursor.y++;
			break;
		case '\e':
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
xt_printfxy(int x, int y, char *fmt, ...) {
	va_list args;
	char buf[BUFSIZ];
	int rx, ry;
	char *p;

	va_start(args, fmt);
	vsprintf(buf, fmt, args);

	p = buf;

	rx = x;
	ry = y;

	while (*p) {
		set_cell(rx, ry, p);
		p += utf_len(p);
		rx++;

		if (rx <= term.width - 2)
			continue;

		rx = 0;
		ry++;

		if (ry > term.height)
			return;
	}
}

void
resize(int x, int y) {
	uint32_t values[3];
	uint32_t mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	uint16_t *rmap;

	values[0] = font->width * x;
	values[1] = font->height * y;

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

	xcb_flush(conn);
}

void
set_fg(int fg) {
	uint32_t mask;
	uint32_t values[3];

	mask = XCB_GC_FOREGROUND;
	values[0] = fg;
	xcb_change_gc(conn, gc, mask, values);

	return;
}

void
load_config() {
	xcb_xrm_database_t *db;
	char *xrm_buf;

	db = xcb_xrm_database_from_default(conn);
	if (db != NULL) {
		xcb_xrm_resource_get_string(db, "xt.font", NULL, &xrm_buf);
		if (xrm_buf != NULL) {
			strncpy(fontline, xrm_buf, BUFSIZ);
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
set_bg(int bg) {
	uint32_t mask;
	uint32_t values[2];

	mask = XCB_CW_BACK_PIXEL;
	values[0] = bg;
	xcb_change_window_attributes(conn, win, mask, values);
	xcb_flush(conn);
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
	case 0x1b3: /* ^L */
		redraw();
		break;
	case (0xff0d): /* enter */
		*wdatap++ = '\n';
		write(d, &wdata, wdatap - wdata);
		memset(wdata, 0, wdatap - wdata);
		wdatap = wdata;
		term.cursor.y++;
		term.cursor.x = 0;
		redraw();
		//strcpy(wdata, "1+1\n");
		//write(d, &wdata, 4);
		break;
	case XK_BackSpace: /* backspace */
		puts("backspace");
		*wdatap-- = 0;
		term.cursor.x--;
		//xt_rectfill(term.cursor.x, term.cursor.y);
		set_fg(term.bg);
		set_cell(term.cursor.x, term.cursor.y, "#");
		xt_rectfill(term.cursor.x, term.cursor.y);
		set_fg(term.fg);
		break;
	default:
		xt_printfxy(term.cursor.x, term.cursor.y, "%lc", keysym);
		cursor_next(&term.cursor);
		xcb_flush(conn);
		printf("unknown keysym: '%lc' (0x%x)\n", keysym, keysym);
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

	/* defaults */
	term.bg = 0x000000;
	term.fg = 0xFFFFFF;

	(void)setlocale(LC_ALL, "");

	conn = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(conn))
		err(1, "xcb_connection_has_error");

	scr = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;

	load_config();
	mask = XCB_CW_EVENT_MASK;
	values[0] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS;

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

	font = load_font(gc, fontline);
	resize(80, 24);
	set_bg(term.bg);
	set_fg(term.fg);

	xcb_flush(conn);
	atexit(cleanup);
	signal(SIGCHLD, cleanup);

	xcb_generic_event_t *ev;
	int s;
	ssize_t n;
	char buf[BUFSIZ];

	/* poll */
	struct pollfd fds[1];

	pid_t pid;
	struct winsize ws;
	struct termios tio;

	ws.ws_col = term.width;
	ws.ws_row = term.height;

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
				s = poll(fds, 1, 50);
				if (s < 0)
					err(1, "poll");

				if (s) {
					if (fds[0].revents & POLLOUT)
						puts("timeout reached");

					if (fds[0].revents & POLLIN) {
						memset(buf, 0, BUFSIZ);
						n = read(d, &buf, BUFSIZ);
						//xt_printfxy(term.cursor.x, term.cursor.y, "%*s", n, buf);
						xt_printf("%*s", n, buf);
						redraw();
						xcb_flush(conn);
					}
				}

				continue;
			}
		}

		switch (ev->response_type & ~0x80) {
		case XCB_EXPOSE: {
			/* setup stuff */
		} break;
		case XCB_KEY_PRESS: {
			xcb_key_press_event_t *e = (xcb_key_press_event_t *)ev;
			keypress(e->detail, xcb_get_keysym(e->detail, e->state));
			redraw();
		} break;
		}
	}

	return 0;
}
