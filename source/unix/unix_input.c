/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "../client/client.h"
#include "x11.h"
#include "keysym2ucs.h"

// Vic: transplanted the XInput2 code from jquake

// TODO: add in_mouse?
cvar_t *in_grabinconsole;

static qboolean focus = qfalse;
static qboolean minimized = qfalse;

static qboolean input_inited = qfalse;
static qboolean mouse_active = qfalse;
static qboolean input_active = qfalse;

static int shift_down = qfalse;

static int xi_opcode;

static int mx, my;

static qboolean go_fullscreen_on_focus = qfalse;

int Sys_XTimeToSysTime( unsigned long xtime );

//============================================

static Cursor CreateNullCursor( Display *display, Window root )
{
	Pixmap cursormask;
	XGCValues xgc;
	GC gc;
	XColor dummycolour;
	Cursor cursor;

	cursormask = XCreatePixmap( display, root, 1, 1, 1 /*depth*/ );
	xgc.function = GXclear;
	gc = XCreateGC( display, cursormask, GCFunction, &xgc );
	XFillRectangle( display, cursormask, gc, 0, 0, 1, 1 );

	dummycolour.pixel = 0;
	dummycolour.red = 0;
	dummycolour.flags = 04;

	cursor = XCreatePixmapCursor( display, cursormask, cursormask, &dummycolour, &dummycolour, 0, 0 );
	XFreePixmap( display, cursormask );
	XFreeGC( display, gc );

	return cursor;
}

static void install_grabs( void )
{
	int i;
	int num_devices;
	XIDeviceInfo *info;
	XIEventMask mask;

	assert( x11display.dpy && x11display.win );

	XDefineCursor(x11display.dpy, x11display.win, CreateNullCursor(x11display.dpy, x11display.win));

	mask.deviceid = XIAllMasterDevices;
	mask.mask_len = XIMaskLen(XI_LASTEVENT);
	mask.mask = calloc(mask.mask_len, sizeof(char));
	XISetMask(mask.mask, XI_KeyPress);
	XISetMask(mask.mask, XI_KeyRelease);
	XISetMask(mask.mask, XI_ButtonPress);
	XISetMask(mask.mask, XI_ButtonRelease);
	XISelectEvents(x11display.dpy, x11display.win, &mask, 1);
	XISetMask(mask.mask, XI_Enter);
	XISetMask(mask.mask, XI_Leave);
	XISetMask(mask.mask, XI_ButtonPress);
	XISetMask(mask.mask, XI_ButtonRelease);

	info = XIQueryDevice(x11display.dpy, XIAllDevices, &num_devices);
	for(i = 0; i < num_devices; i++) {
		int id = info[i].deviceid;
		if(info[i].use == XISlavePointer) {
			mask.deviceid = id;
			XIGrabDevice(x11display.dpy, id, x11display.win, CurrentTime, None, GrabModeSync,
				GrabModeSync, True, &mask);
		}
		else if(info[i].use == XIMasterPointer) {
			if (x11display.features.wmStateFullscreen)
				XIWarpPointer(x11display.dpy, id, None, x11display.win, 0, 0, 0, 0, 0, 0);
			else
				XIWarpPointer(x11display.dpy, id, None, x11display.win, 0, 0, 0, 0, x11display.win_width/2, x11display.win_height/2);
		}
		else if(info[i].use == XIMasterKeyboard)
		{
			XIGrabDevice(x11display.dpy, id, x11display.win, CurrentTime, None, GrabModeAsync, GrabModeAsync, False, &mask);
		}
	}
	XIFreeDeviceInfo(info);

	mask.deviceid = XIAllDevices;
	memset(mask.mask, 0, mask.mask_len);
	XISetMask(mask.mask, XI_RawMotion);

	XISelectEvents(x11display.dpy, DefaultRootWindow(x11display.dpy), &mask, 1);

	free(mask.mask);

	XSync(x11display.dpy, True);

	mx = my = 0;
	mouse_active = qtrue;
	input_active = qtrue;
}

static void uninstall_grabs( void )
{
	int i;
	int num_devices;
	XIDeviceInfo *info;

	assert( x11display.dpy && x11display.win );

	XUndefineCursor(x11display.dpy, x11display.win);
	info = XIQueryDevice(x11display.dpy, XIAllDevices, &num_devices);

	for(i = 0; i < num_devices; i++) {
		if(info[i].use == XIFloatingSlave || info[i].use == XIMasterKeyboard) {
			XIUngrabDevice(x11display.dpy, info[i].deviceid, CurrentTime);
		}
		else if(info[i].use == XIMasterPointer) {
			XIWarpPointer(x11display.dpy, info[i].deviceid, None, x11display.win, 0, 0, 0, 0,
				x11display.win_width/2, x11display.win_height/2);
		}
	}
	XIFreeDeviceInfo(info);

	mouse_active = qfalse;
	input_active = qfalse;
	mx = my = 0;

	x11display.ic = 0;
}

// Q3 version
static char *XLateKey( int keycode, int *key )
{
	KeySym keysym;

	keysym = XkbKeycodeToKeysym(x11display.dpy, keycode, 0, 0); /* Don't care about shift state for in game keycode, but... */
	int kp = 1;

	switch(keysym) {
		case XK_Scroll_Lock: *key = K_SCROLLLOCK; break;
		case XK_Caps_Lock: *key = K_CAPSLOCK; break;
		case XK_Num_Lock: *key = K_NUMLOCK; break;
		case XK_KP_Page_Up: *key = kp ? KP_PGUP : K_PGUP; break;
		case XK_Page_Up: *key = K_PGUP; break;
		case XK_KP_Page_Down: *key = kp ? KP_PGDN : K_PGDN; break;
		case XK_Page_Down: *key = K_PGDN; break;
		case XK_KP_Home: *key = kp ? KP_HOME : K_HOME; break;
		case XK_Home: *key = K_HOME; break;
		case XK_KP_End: *key = kp ? KP_END : K_END; break;
		case XK_End: *key = K_END; break;
		case XK_KP_Left: *key = kp ? KP_LEFTARROW : K_LEFTARROW; break;
		case XK_Left: *key = K_LEFTARROW; break;
		case XK_KP_Right: *key = kp ? KP_RIGHTARROW : K_RIGHTARROW; break;
		case XK_Right: *key = K_RIGHTARROW; break;
		case XK_KP_Down: *key = kp ? KP_DOWNARROW : K_DOWNARROW; break;
		case XK_Down: *key = K_DOWNARROW; break;
		case XK_KP_Up: *key = kp ? KP_UPARROW : K_UPARROW; break;
		case XK_Up: *key = K_UPARROW; break;
		case XK_Escape: *key = K_ESCAPE; break;
		case XK_KP_Enter: *key = kp ? KP_ENTER : K_ENTER; break;
		case XK_Return: *key = K_ENTER; break;
		case XK_Tab: *key = K_TAB; break;
		case XK_F1: *key = K_F1; break;
		case XK_F2: *key = K_F2; break;
		case XK_F3: *key = K_F3; break;
		case XK_F4: *key = K_F4; break;
		case XK_F5: *key = K_F5; break;
		case XK_F6: *key = K_F6; break;
		case XK_F7: *key = K_F7; break;
		case XK_F8: *key = K_F8; break;
		case XK_F9: *key = K_F9; break;
		case XK_F10: *key = K_F10; break;
		case XK_F11: *key = K_F11; break;
		case XK_F12: *key = K_F12; break;
		case XK_BackSpace: *key = K_BACKSPACE; break;
		case XK_KP_Delete: *key = kp ? KP_DEL : K_DEL; break;
		case XK_Delete: *key = K_DEL; break;
		case XK_Pause: *key = K_PAUSE; break;
		case XK_Shift_L: *key = K_SHIFT; break;
		case XK_Shift_R: *key = K_SHIFT; break;
		case XK_Execute:
		case XK_Control_L: *key = K_CTRL; break;
		case XK_Control_R: *key = K_CTRL; break;
		case XK_Alt_L:
		case XK_Meta_L: *key = K_ALT; break;
		case XK_Alt_R:
		case XK_ISO_Level3_Shift:
		case XK_Meta_R: *key = K_ALT; break;
		case XK_Super_L: *key = K_WIN; break;
		case XK_Super_R: *key = K_WIN; break;
		case XK_Multi_key: *key = K_WIN; break;
		case XK_Menu: *key = K_MENU; break;
		case XK_KP_Begin: *key = kp ? KP_5 : '5'; break;
		case XK_KP_Insert: *key = kp ? KP_INS : K_INS; break;
		case XK_Insert: *key = K_INS; break;
		case XK_KP_Multiply: *key = kp ? KP_STAR : '*'; break;
		case XK_KP_Add: *key = kp ? KP_PLUS : '+'; break;
		case XK_KP_Subtract: *key = kp ? KP_MINUS : '-'; break;
		case XK_KP_Divide: *key = kp ? KP_SLASH : '/'; break;
		default:
			if (keysym >= 32 && keysym <= 126) {
				*key = keysym;
			}
			break;
	}

	/* ... if we're in console or chatting, please activate SHIFT */
	keysym = XkbKeycodeToKeysym(x11display.dpy, keycode, 0, shift_down);

	/* this is stupid, there must exist a better way */
	switch(keysym) {
		case XK_bracketleft: return "[";
		case XK_bracketright: return "]";
		case XK_parenleft: return "(";
		case XK_parenright: return ")";
		case XK_braceleft: return "{";
		case XK_braceright: return "}";
		case XK_space: return " ";
		case XK_asciitilde: return "~";
		case XK_grave: return "`";
		case XK_exclam: return "!";
		case XK_at: return "@";
		case XK_numbersign: return "#";
		case XK_dollar: return "$";
		case XK_percent: return "%";
		case XK_asciicircum: return "^";
		case XK_ampersand: return "&";
		case XK_asterisk: return "*";
		case XK_minus: return "-";
		case XK_underscore: return "_";
		case XK_equal: return "=";
		case XK_plus: return "+";
		case XK_semicolon: return ";";
		case XK_colon: return ":";
		case XK_apostrophe: return "'";
		case XK_quotedbl: return "\"";
		case XK_backslash: return "\\";
		case XK_bar: return "|";
		case XK_comma: return ",";
		case XK_period: return ".";
		case XK_less: return "<";
		case XK_greater: return ">";
		case XK_slash: return "/";
		case XK_question: return "?";
		default: if (XKeysymToString(keysym)) return XKeysymToString(keysym);
	}
	return "";	
}

/*
*_X11_CheckWMSTATE
*/
static void _X11_CheckWMSTATE( void )
{
#define WM_STATE_ELEMENTS 1
	unsigned long *property = NULL;
	unsigned long nitems;
	unsigned long leftover;
	Atom xa_WM_STATE, actual_type;
	int actual_format;
	int status;

	minimized = qfalse;
	xa_WM_STATE = x11display.wmState;

	status = XGetWindowProperty ( x11display.dpy, x11display.win,
		xa_WM_STATE, 0L, WM_STATE_ELEMENTS,
		False, xa_WM_STATE, &actual_type, &actual_format,
		&nitems, &leftover, (unsigned char **)&property );

	if ( ! ( ( status == Success ) &&
		( actual_type == xa_WM_STATE ) &&
		( nitems == WM_STATE_ELEMENTS ) ) )
	{
		if ( property )
		{
			XFree ( (char *)property );
			property = NULL;
			return;
		}
	}

	if( ( *property == IconicState ) || ( *property == WithdrawnState ) )
		minimized = qtrue;

	XFree( (char *)property );
}

static void handle_button(XGenericEventCookie *cookie)
{
	XIDeviceEvent *ev = (XIDeviceEvent *)cookie->data;
	qboolean down = cookie->evtype == XI_ButtonPress;
	int button = ev->detail;
	unsigned time = Sys_XTimeToSysTime(ev->time);
	int k_button;

	switch(button) {
		case 1: k_button = K_MOUSE1; break;
		case 2: k_button = K_MOUSE3; break;
		case 3: k_button = K_MOUSE2; break;
		case 4: k_button = K_MWHEELUP; break;
		case 5: k_button = K_MWHEELDOWN; break;
			/* Switch place of MOUSE4-5 with MOUSE6-7 */
		case 6: k_button = K_MOUSE6; break;
		case 7: k_button = K_MOUSE7; break;
		case 8: k_button = K_MOUSE4; break;
		case 9: k_button = K_MOUSE5; break;
			/* End switch */
		case 10: k_button = K_MOUSE8; break;
		default: return;
	}

	Key_Event(k_button, down, time);
}

static void handle_key(XGenericEventCookie *cookie)
{
	XIDeviceEvent *ev = (XIDeviceEvent *)cookie->data;
	qboolean down = cookie->evtype == XI_KeyPress;
	int keycode = ev->detail;
	unsigned time = Sys_XTimeToSysTime(ev->time);
	int key = 0;
	const char *name = XLateKey(keycode, &key);

	if (ev->flags & XIKeyRepeat)
		return;

	if (key == K_SHIFT)
		shift_down = down;

	Key_Event(key, down, time);

	if(name && name[0] && down) {
		qwchar wc = keysym2ucs(XkbKeycodeToKeysym(x11display.dpy, keycode, 0, shift_down));
		Key_CharEvent( key, wc );
	}
}

static void handle_raw_motion(XIRawEvent *ev)
{
	double *raw_valuator = ev->raw_values;

	if(!mouse_active)
		return;

	if(XIMaskIsSet(ev->valuators.mask, 0)) {
		mx += *raw_valuator++;
	}

	if(XIMaskIsSet(ev->valuators.mask, 1)) {
		my += *raw_valuator++;
	}

}

static void handle_cookie(XGenericEventCookie *cookie)
{
	switch(cookie->evtype) {
	case XI_RawMotion:
		handle_raw_motion(cookie->data);
		break;
	case XI_Enter:
	case XI_Leave:
		break;
	case XI_ButtonPress:
	case XI_ButtonRelease:
		handle_button(cookie);
		break;
	case XI_KeyPress:
	case XI_KeyRelease:
		handle_key(cookie);
		break;
	default:
		break;
	}
}

static void HandleEvents( void )
{
	XEvent event;
	qboolean was_focused = focus;

	assert( x11display.dpy && x11display.win );

	while( XPending( x11display.dpy ) )
	{
		XGenericEventCookie *cookie = &event.xcookie;
		XNextEvent( x11display.dpy, &event );

		if( cookie->type == GenericEvent && cookie->extension == xi_opcode 
			&& XGetEventData( x11display.dpy, cookie ) ) {
				handle_cookie( cookie );
				XFreeEventData( x11display.dpy, cookie );
				continue;
		}

		switch( event.type )
		{
		case FocusIn:
			if( x11display.ic )
				XSetICFocus(x11display.ic);
			if( !focus )
			{
				focus = qtrue;
			}
			break;

		case FocusOut:
			if( x11display.ic )
				XUnsetICFocus(x11display.ic);
			if( focus )
			{
				Key_ClearStates();
				focus = qfalse;
				shift_down = 0;
			}
			break;

		case ClientMessage:
			if( event.xclient.data.l[0] == x11display.wmDeleteWindow )
				Cbuf_ExecuteText( EXEC_NOW, "quit" );
			break;

		case ConfigureNotify:
			VID_AppActivate( qtrue, qfalse );
			break;

		case PropertyNotify:
			if( event.xproperty.window == x11display.win )
			{
				if ( event.xproperty.atom == x11display.wmState )
				{
					qboolean was_minimized = minimized;

					_X11_CheckWMSTATE();

					if( minimized != was_minimized )
					{
						// FIXME: find a better place for this?..
						CL_SoundModule_Activate( !minimized );
					}
				}
			}
			break;
		}
	}

	// set fullscreen or windowed mode upon focus in/out events if:
	//  a) lost focus in fullscreen -> windowed
	//  b) received focus -> fullscreen if a)
	if( ( focus != was_focused ) )
	{
		if( x11display.features.wmStateFullscreen )
		{
			if( !focus && Cvar_Value( "vid_fullscreen" ) )
			{
				go_fullscreen_on_focus = qtrue;
				Cbuf_ExecuteText( EXEC_APPEND, "vid_fullscreen 0\n" );
			}
			else if( focus && go_fullscreen_on_focus )
			{
				go_fullscreen_on_focus = qfalse;
				Cbuf_ExecuteText( EXEC_APPEND, "vid_fullscreen 1\n" );
			}
		}
	}
}

/*****************************************************************************/

void IN_Commands( void )
{
}

void IN_MouseMove( usercmd_t *cmd )
{
	if( mouse_active )
	{
		CL_MouseMove( cmd, mx, my );
		mx = my = 0;
	}
}

void IN_JoyMove( usercmd_t *cmd )
{
}

void IN_Activate( qboolean active )
{
	if( !input_inited )
		return;

	assert( x11display.dpy && x11display.win );

	if( active )
	{
		install_grabs();
	}
	else
	{
		uninstall_grabs();
	}
}


void IN_Init( void )
{
	int event, error;
	int xi2_major = 2;
	int xi2_minor = 0;

	if( input_inited )
		return;

	in_grabinconsole = Cvar_Get( "in_grabinconsole", "0", CVAR_ARCHIVE );

	input_inited = qtrue;
	input_active = qfalse; // will be activated by IN_Frame if necessary

	if( !XQueryExtension( x11display.dpy, "XInputExtension", &xi_opcode, &event, &error ) ) {
		Com_Printf( "ERROR: XInput Extension not available.\n" );
		return;
	}

	if( XIQueryVersion (x11display.dpy, &xi2_major, &xi2_minor ) == BadRequest ) {
		Com_Printf( "ERROR: Can't initialize XInput2. Server supports %d.%d\n", xi2_major, xi2_minor );
	} else {
		Com_Printf( "Successfully initialized XInput2 %d.%d\n", xi2_major, xi2_minor );
		install_grabs();
	}
}

void IN_Shutdown( void )
{
	if( !input_inited )
		return;

	uninstall_grabs();

	input_inited = qfalse;
}

void IN_Restart( void )
{
	IN_Shutdown();
	IN_Init();
}

void IN_Frame( void )
{
	if( !input_inited )
		return;

	if( ( ( x11display.features.wmStateFullscreen || !Cvar_Value( "vid_fullscreen" ) ) && ( !focus || ( ( cls.key_dest == key_console ) && !in_grabinconsole->integer ) ) ) )
	{
		if( input_active )
			IN_Activate( qfalse );
	}
	else
	{
		if( !input_active )
			IN_Activate( qtrue );
	}

	HandleEvents();
}
