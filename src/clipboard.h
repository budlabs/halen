#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include <X11/Xlib.h>
#include <X11/Xatom.h>

// Initialize clipboard monitoring system
int clipboard_init(void);

// Start clipboard monitoring in background thread
int clipboard_start_monitoring_async(void);

// Stop clipboard monitoring thread
void clipboard_stop_monitoring(void);

// Get clipboard history entry by index (0 = oldest, -1 = latest)
char* clipboard_get_entry(int n);

char* clipboard_history_file_default_path(void);

// Navigation functions
void clipboard_reset_navigation(void);
int clipboard_get_history_count(void);
int clipboard_get_current_index(void);
void clipboard_set_current_index(int index);
int clipboard_set_content(const char* content);

// Add this function declaration
int clipboard_delete_entry(int index);

#endif // CLIPBOARD_H
