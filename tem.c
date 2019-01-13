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
#include "tem.h"

static xcb_connection_t *conn;
static xcb_screen_t *scr;
static xcb_window_t win;
static struct font_s *font;
static term_t term;
static xcb_gcontext_t gc;
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
				if (term.map[i].fg && term.map[i].fg != term.fg)
					set_color_fg(term.map[i].fg);
				else
					set_fg(term.default_fg);

				xcb_poly_text_16_simple(conn, win, gc,
						term.padding + ((x + 1) * font->width),
						term.padding + ((y + 1) * font->height),
						1, &term.map[x + (y * term.width)].ch
				);
			}
		}
	}

	if (term.cursor_vis) {
		set_fg(term.default_fg);
		xcb_rectangle_t rect;
		rect.x = ((term.cursor.x + 1) * font->width) + term.padding;
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

	if (curs->y >= term.height)
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
	if (xcb_request_check(conn, cookie))
		err(1, "could not load font '%s'", name);

	r = malloc(sizeof(struct font_s));
	if (r == NULL)
		err(1, "malloc");

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
	if (x + 2 >= term.width || x < 0)
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

	while (sscanf(p, "%d", &c)) {
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
					/* XXX: rgb */
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

	if (*p == 'm')
		term.attr = term.bi = term.fi = 0;
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
		case 'X':
			if (sscanf(p, "%d", &s))
				while (s--)
					cursormv(p[n]);
			break;
		}
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

		if (!sscanf(p, "%d", &s))
			s = 1;

		if (valid_xy(s, term.cursor.y))
			term.cursor.x = s;
	}	break;
	case 'H': {
		/* CUP: n ; m H */
		int row, col;

		row = col = 0;

		if (memchr(p, ';', n)) /* n + m */
			sscanf(p, "%d;%d", &row, &col);
		else /* n only */
			sscanf(p, "%d", &row);

		if (valid_xy(col, row)) {
			term.cursor.x = col - 1;
			term.cursor.y = row - 1;
			term.wants_redraw = 1;
		}
	}	break;
	case 'J': {
		uint16_t *off;
		off_t len;
		struct tattr *mp;
		mp = term.map + term.cursor.x + (term.cursor.y * term.width);

		DEBUG("clear screen?");
		switch (p[0]) {
		case '3': /* clear screen and wipe scrollback */
			  /* we don't actually have a scrollback buffer yet */
		case '2': /* clear entire screen */
			memset(term.map, 0, term.width * term.height * sizeof(*term.map));
			term.cursor.x = term.cursor.y = 0;
			break;
		case '1': { /* clear from cursor to beginning of screen */
			while (mp-- != term.map)
				mp->ch = mp->fg = mp->bg = mp->attr = 0;

		}	break;
		case 'J': /* no arg */
		case '0': /* clear from cursor to end of screen (default) */
			while (mp++ != term.map + (term.width * term.height))
				mp->ch = mp->fg = mp->bg = mp->attr = 0;

			break;
		}
	}	break;
	case 'K': { /* EL erase in line */
		int s;
		off_t i, end;

		sscanf(p, "%d", &s);
		end = term.width - term.cursor.x;

		switch (s) {
		default:
		case 0:
			for (i = term.cursor.x; i < term.width; i++)
				set_cell(i, term.cursor.y, " ");

			break;
		case 1:
			for (i = term.cursor.x; i; i--)
				set_cell(i, term.cursor.y, " ");

			break;
		case 2:
			for (i = term.cursor.x; i < term.width; i++)
				set_cell(i, term.cursor.y, " ");

			break;
		}
	}	break;
	case 'M': {/* delete n lines DL? */
		int s, i;

		if (sscanf(p, "%dM", &s) < 1)
			s = 1;

		for (i = 0; i < term.width; i++)
			set_cell(i, s, " ");

	}	break;
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
	case 'd': {
		int s;

		if (sscanf(p, "%d", &s) < 0)
			s = 1;

		term.cursor.y = s;
	}	break;
	case 'L': {
		int s, i;

		if (sscanf(p, "%dL", &s))
			s = 1;

		for (i = 0; i < s; i++)
			xcb_printf("\n");
	}	break;
	case '@':
		xcb_printf(" ");
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
			cursormv(LEFT);
			//term.cursor.x--;
			set_cell(term.cursor.x, term.cursor.y, " ");
			break;
		case '\r':
			term.cursor.x = 0;
			break;
		case '\n':
			cursormv(DOWN);
			//if (valid_xy(term.cursor.x, term.cursor.y + 1))
			//	term.cursor.y++;
			break;
		case 0x1b:
			term.esc = 1;
			term.esc_str = p;
			n = 0;
			break;
		default:
			if (valid_xy(term.cursor.x, term.cursor.y)) {
				set_cell(term.cursor.x, term.cursor.y, p);
				cursor_next(&term.cursor);
			}
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
buttonpress(uint16_t x, uint16_t y) {
	int row, col;

	col = (term.padding + x) / font->width;
	row = (term.padding + y) / font->height;
	col = ((term.padding + x) / font->width) - 1;
	row = ((term.padding + y) / font->height) - 1;

	row--; col--;

	DEBUG("x:%d y:%d", col, row);
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
	case XK_Escape:
		dprintf(d, "\033");
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

	/* defaults */
	term.bg = term.default_bg = 0x000000;
	term.fg = term.default_fg = 0xFFFFFF;
	term.padding = 3;
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
	values[0] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS
		| XCB_EVENT_MASK_BUTTON_PRESS;

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

	ARGBEGIN {
	case 'f':
		strncpy(term.fontline, ARGF(), BUFSIZ);
		break;
	} ARGEND

	font = load_font(gc, term.fontline);
	resize(80, 24);
	resize(90, 36);
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
		(void)setenv("TERM", "tem-256color", 1);
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
			case XCB_BUTTON_PRESS: {
				xcb_button_press_event_t *e = (xcb_button_press_event_t *)ev;
				buttonpress(e->root_x, e->root_y);
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
