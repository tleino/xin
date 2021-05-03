/*
 * xin - X11 forwarded input receiver
 * Copyright (c) 2020 Tommi Leino <namhas@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <X11/extensions/XTest.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <err.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>

static void xmotion(Display *, int, int);
static void xkey(Display *, char, int, int);
static void xkblayout(Display *, char *);
static void xkey_sendevent(Display *, char, int, int);
static void xbutton(Display *, char, int, int);
static void update_mapping(Display *, XEvent *);

extern int optind;

enum inject_method {
	INJECT_METHOD_XTEST,
	INJECT_METHOD_SENDEVENT
};

void
xkey(Display *dpy, char type, int state, int keycode)
{
	Bool is_press;

	is_press = (type == 'k') ? True : False;
	if (keycode == 0) {
		keycode = XKeysymToKeycode(dpy, state);
	}
	if (keycode == 0) {
		warnx("couldn't find keycode for a keysym");
		return;
	}
	XTestFakeKeyEvent(dpy, keycode, is_press, 0);
	XFlush(dpy);
}

void
xkey_sendevent(Display *dpy, char type, int state, int keycode)
{
	Window focus;
	int revert_to;
	static int modifiers;
	Bool is_press;
	XEvent e = { 0 };

	if (XGetInputFocus(dpy, &focus, &revert_to) == False) {
		warnx("no input focus; sending events to root window");
		focus = RootWindow(dpy, 0);
	}

	is_press = (type == 'k') ? True : False;
	e.type = (is_press == True) ? KeyPress : KeyRelease;
	e.xkey.keycode = XKeysymToKeycode(dpy, state);
	e.xkey.window = focus;
	e.xkey.subwindow = focus;
	if (is_press)
		modifiers |= XkbKeysymToModifiers(dpy, state);
	else
		modifiers &= ~XkbKeysymToModifiers(dpy, state);
	e.xkey.state = modifiers;
	e.xkey.type = (is_press == True) ? KeyPress : KeyRelease;
	e.xkey.time = CurrentTime;
	XSendEvent(dpy, focus, False,
	    (is_press == True) ? KeyPressMask : KeyReleaseMask, &e);
	XFlush(dpy);
}

void
xbutton(Display *dpy, char type, int state, int button)
{
	Bool is_press;

	is_press = (type == 'b') ? True : False;
	XTestFakeButtonEvent(dpy, button, is_press, 0);
	XFlush(dpy);
}

void
xmotion(Display *dpy, int x, int y)
{
	static XButtonEvent xb;
	int maxw, maxh;

	/* Query initial pointer */
	if (xb.root == None) {
		XQueryPointer(dpy, RootWindow(dpy, 0),
		    &xb.root, &xb.window, &xb.x_root, &xb.y_root,
		    &xb.x, &xb.y, &xb.state);
	}

	/* Geometry */
	maxw = DisplayWidth(dpy, DefaultScreen(dpy));
	maxh = DisplayHeight(dpy, DefaultScreen(dpy));

	/*
	 * We use don't use the RelativeMotion variant of the XTest
	 * MotionEvent because the RelativeMotion version was actually
	 * breaking up things and didn't follow its documentation.
	 * Otherwise, it would be simpler to use the relative version.
	 */
	xb.x_root -= x;
	xb.y_root -= y;
	if (xb.x_root < 0)
		xb.x_root = 0;
	if (xb.y_root < 0)
		xb.y_root = 0;
	if (xb.x_root >= maxw)
		xb.x_root = maxw;
	if (xb.y_root >= maxh)
		xb.y_root = maxh;
	XTestFakeMotionEvent(dpy, 0, xb.x_root, xb.y_root, 0);
	XFlush(dpy);
}

int
main(int argc, char **argv)
{
	Display *dpy;
	char buf[64], c, *denv;
	int v1, v2, n, skip_truncated;
	int xtst_event, xtst_error, xtst_majv, xtst_minv;
	int xkbmaj, xkbmin, xkb_op, xkb_event, xkb_error;
	int method, want_xtst;
	char *layout;

#ifdef __OpenBSD__
	if (pledge("stdio rpath dns unix inet proc exec", NULL) != 0)
		err(1, "pledge");
#endif

	if ((denv = getenv("DISPLAY")) == NULL && errno != 0)
		err(1, "getenv");
	if ((dpy = XOpenDisplay(denv)) == NULL) {
		if (denv == NULL)
			errx(1, "X11 connection failed; "
			    "DISPLAY environment variable not set?");
		else
			errx(1, "failed X11 connection to '%s'", denv);
	}

#ifdef __OpenBSD__
	if (pledge("stdio rpath proc exec", NULL) != 0)
		err(1, "pledge");
#endif
	/*
	 * We use XKB extension because we wish to extract the current
	 * set of modifiers from a KeySym for SendEvent event injection
	 * type. If the SendEvent type is not needed, use of XKB extension
	 * could be removed.
	 */
	xkbmaj = XkbMajorVersion;
	xkbmin = XkbMinorVersion;
	if (XkbLibraryVersion(&xkbmaj, &xkbmin) == False)
		errx(1, "trouble with XKB extension; needed %d.%d got %d.%d",
		    XkbMajorVersion, XkbMinorVersion, xkbmaj, xkbmin);
	if (XkbQueryExtension(dpy, &xkb_op, &xkb_event, &xkb_error,
	    &xkbmaj, &xkbmin) == False)
		errx(1, "trouble with XKB extension");

	/*
	 * By default we really wish to use XTEST because XTEST sends
	 * the events more like they really go, honoring grabs and such,
	 * while the SendEvent method ignores grabs and may be filtered
	 * by applications. However, sometimes using the SendEvent might
	 * be handy, for example if XTEST extension is disabled.
	 *
	 * TODO: The SendEvent implementation is not fully complete yet.
	 */
	want_xtst = 1;
	if (argc >= 2) {
		while ((c = getopt(argc, argv, "s")) != -1) {
			switch (c) {
			case 's':
				want_xtst = 0;
				break;
			default:
				fprintf(stderr, "Usage: %s [-s]\n",
				    argv[0]);
				return 1;
			}
		}
		argc -= optind;
		argv += optind;
	}

	if (want_xtst == 1) {
		if (XTestQueryExtension(dpy, &xtst_event, &xtst_error,
		    &xtst_majv, &xtst_minv) == False)
			errx(1, "XTEST not available; try %s -s",
			    argv[0]);
		method = INJECT_METHOD_XTEST;
	} else
		method = INJECT_METHOD_SENDEVENT;

	skip_truncated = 0;
	while (fgets(buf, sizeof(buf), stdin) != NULL) {
		n = strcspn(buf, "\r\n");
		if (buf[n] == '\0') {
			if (skip_truncated == 0)
				warnx("parse error; truncated input");
			skip_truncated = 1;
			continue;
		} else if (skip_truncated) {
			skip_truncated = 0;
			continue;
		}
		buf[n] = '\0';
		if (buf[0] == 'l' && n > 2) {
			layout = &buf[2];
			xkblayout(dpy, layout);
		} else if (sscanf(buf, "%c %d", &c, &v1) == 2 &&
		    (c == 'k' || c == 'K')) {
				if (method == INJECT_METHOD_SENDEVENT)
					xkey_sendevent(dpy, c, v1, 0);
				else
					xkey(dpy, c, v1, 0);
		} else if (sscanf(buf, "%c %d %d", &c, &v1, &v2) != 3) {
			warnx("parse error; invalid or incomplete format");
		} else {
			switch(c) {
			case 'm':
				xmotion(dpy, v1, v2);
				break;
			case 'b':
			case 'B':
				xbutton(dpy, c, v1, v2);
				break;
			case 'k':
			case 'K':
				xkey(dpy, c, v1, v2);
				break;
			default:
				warnx("parse error; unknown control");
				break;
			}
		}
	}
	if (ferror(stdin))
		err(1, "reading stdin");
	else if (!feof(stdin) && errno != 0)
		err(1, "fgets");

	return EXIT_SUCCESS;
}

static void
xkblayout(Display *dpy, char *layout)
{
	char *p, s[128];
	XEvent e;

	for (p = layout; *p != '\0'; p++)
		if (isalpha(*p) == 0) {
			warnx("layout name cannot contain special "
			    "characters");
			return;
		}

	if (snprintf(s, sizeof(s), "setxkbmap %s", layout) >= sizeof(s)) {
		warnx("layout name too long");
		return;
	}

	XGrabKey(dpy, XKeysymToKeycode(dpy, XStringToKeysym("Super_L")), 0,
	    RootWindow(dpy, 0), 0, 0, 1);
	XSync(dpy, False);

	while (XCheckMaskEvent(dpy, MappingNotify, &e) == True)
		update_mapping(dpy, &e);

	if (system(s) == -1)
		err(1, "system");

	do {
		XNextEvent(dpy, &e);
		switch (e.type) {
		case MappingNotify:
			update_mapping(dpy, &e);
			break;
		default:
			break;
		}
	} while (e.type != MappingNotify);

	/*
	 * We need to wait for an event from the keymap change
	 * before we can continue because we need to refresh
	 * internal keysym to keycode mapping.
	 */

	while (XCheckMaskEvent(dpy, MappingNotify, &e) == True)
		update_mapping(dpy, &e);
}

static void
update_mapping(Display *dpy, XEvent *e)
{
	if (e->xmapping.request == MappingKeyboard)
		XRefreshKeyboardMapping(&e->xmapping);
}
