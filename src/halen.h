#ifndef CLIPOPUP_H
#define CLIPOPUP_H

#include <sys/syslog.h>
#include <X11/Xlib.h>

#define DEFAULT_CONFIG_FILE "clipopup.conf"

typedef enum {
    POPUP_ACTION_NONE = 0,
    POPUP_ACTION_NEXT,    // V key - navigate to next (older) entry
    POPUP_ACTION_PREV,    // C key - navigate to previous (newer) entry
    POPUP_ACTION_CUT,     // X key - close popup and cut current entry
    POPUP_ACTION_DELETE,  // D key - delete current entry from history
    POPUP_ACTION_CANCEL   // Z key - cancel and exit without selecting
} PopupAction;

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
    int count_color;       // Color for popup text
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

