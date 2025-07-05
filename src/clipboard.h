#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include <X11/Xlib.h>
#include <X11/Xatom.h>

int   clipboard_init(void);
int   clipboard_start_monitoring_async(void);
void  clipboard_stop_monitoring(void);
char* clipboard_get_content(const char* selection_name);
int   clipboard_set_content(const char* content);
char* clipboard_history_file_default_path(void);


#endif // CLIPBOARD_H
