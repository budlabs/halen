#ifndef HOTKEY_H
#define HOTKEY_H

#include <X11/Xlib.h>
#include <time.h>

typedef enum {
    NAV_DIRECTION_NEXT = 0,  // Navigate to older entries (V key)
    NAV_DIRECTION_PREV = 1   // Navigate to newer entries (C key)
} nav_direction_t;

typedef void (*hotkey_callback_t)(const char *event_type);

int hotkey_init(hotkey_callback_t callback);
void hotkey_cleanup(void);
void hotkey_handle_xevent(XEvent *event);
PopupAction hotkey_get_popup_action(void);
void hotkey_perform_paste(void);
nav_direction_t hotkey_get_nav_direction(void);
void hotkey_reset_nav_direction(void);

#endif // HOTKEY_H
