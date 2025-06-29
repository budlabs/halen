#ifndef CLIPOPUP_H
#define CLIPOPUP_H

#include "config.h"
#include <sys/syslog.h>
#include <X11/Xlib.h>

typedef enum {
    POPUP_ACTION_NONE = 0,
    POPUP_ACTION_NEXT,
    POPUP_ACTION_PREV,
    POPUP_ACTION_CUT,
    POPUP_ACTION_DELETE,
    POPUP_ACTION_CANCEL
} PopupAction;

typedef enum {
    ANCHOR_TOP_LEFT = 1,
    ANCHOR_TOP_CENTER = 2,
    ANCHOR_TOP_RIGHT = 3,
    ANCHOR_CENTER_LEFT = 4,
    ANCHOR_CENTER_CENTER = 5,
    ANCHOR_CENTER_RIGHT = 6,
    ANCHOR_BOTTOM_LEFT = 7,
    ANCHOR_BOTTOM_CENTER = 8,
    ANCHOR_BOTTOM_RIGHT = 9
} PopupAnchor;

typedef enum {
    POPUP_POSITION_MOUSE,
    POPUP_POSITION_SCREEN,
    POPUP_POSITION_ABSOLUTE
} PopupPosition;

typedef struct {
    int verbose;
    char *logfile;
    char *history_file;
    int timeout;
    int max_lines;
    int max_line_length;
    char *font;
    int font_size;
    int background; // Color for popup background
    int foreground;       // Color for popup text
    int count_color;       // Color for count text
    PopupPosition position;
    int position_x;
    int position_y;
    PopupAnchor anchor;
    int margin_vertical;
    int margin_horizontal;
} config_t;

// Global verbose flag (defined in main.c)
extern int g_verbose;
extern config_t config;

// Global X11 variables (defined in main.c)
extern Display *g_display;
extern Window g_root_window;
extern volatile int g_running;

// Global logging function (defined in main.c)
void msg(int priority, const char* format, ...);

#endif // CLIPOPUP_H

