#ifndef POPUP_H
#define POPUP_H

#include <X11/Xlib.h>

int popup_init(Display *dpy, Window root, int width, int height);
int popup_show(const char *text);
void popup_hide(void);
void popup_cleanup(void);
void popup_update_text(const char *new_text);
int popup_is_showing(void);
void popup_handle_expose(XExposeEvent *expose_event);

#endif // POPUP_H
