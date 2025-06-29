#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "clipboard.h"
#include "halen.h"
#include "history.h"
#include "xdg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>
#include <X11/extensions/Xfixes.h>
#include <sys/syslog.h>
#include <linux/limits.h>
#include <sys/stat.h>

static pthread_t clipboard_thread;
static int clipboard_thread_running = 0;
static Display *clipboard_display = NULL;
static char last_clipboard_content[MAX_CLIPBOARD_SIZE] = {0};
static char last_primary_content[MAX_CLIPBOARD_SIZE] = {0};

static void* clipboard_monitor_thread(void* arg);
static void handle_clipboard_change_threaded(Atom selection, Atom clipboard_atom_local, Atom primary_atom_local);

static void poll_clipboard_changes(Display *display, Atom clipboard_atom_local, Atom primary_atom_local);

static int set_clipboard_content(const char* content) {
    if (!content || strlen(content) == 0) {
        msg(LOG_WARNING, "Cannot set clipboard - content is empty");
        return 0;
    }
    
    char command[] = "xclip -selection clipboard -i 2>/dev/null";
    
    FILE *fp = popen(command, "w");
    if (!fp) {
        msg(LOG_ERR, "Failed to open xclip for writing");
        return 0;
    }
    
    size_t content_len = strlen(content);
    size_t written = fwrite(content, 1, content_len, fp);
    
    if (written != content_len) {
        msg(LOG_WARNING, "Failed to write all content to xclip: %zu/%zu bytes", 
            written, content_len);
        pclose(fp);
        return 0;
    }
    
    int exit_code = pclose(fp);
    if (exit_code != 0) {
        msg(LOG_ERR, "xclip failed with exit code: %d", exit_code);
        return 0;
    }
    
    msg(LOG_DEBUG, "Successfully set clipboard content: %.50s%s", 
        content, strlen(content) > 50 ? "..." : "");
    
    return 1;
}

static void handle_clipboard_change_threaded(Atom selection, Atom clipboard_atom_local, Atom primary_atom_local) {
    (void)primary_atom_local; 

    const char *selection_name = (selection == clipboard_atom_local) ? "CLIPBOARD" : "PRIMARY";
    const char *xclip_name = (selection == clipboard_atom_local) ? "clipboard" : "primary";
    
    if (strcmp(selection_name, "CLIPBOARD") != 0) {
        msg(LOG_ERR, "ignoreing selection: %s", selection_name);
        return;
    }
    
    msg(LOG_DEBUG, "handle_clipboard_change_threaded called for %s", selection_name);
    
    char *content = clipboard_get_content(xclip_name);
    if (content) {
        
        char *last_content = (selection == clipboard_atom_local) ? 
                           last_clipboard_content : last_primary_content;
        
        if (strcmp(content, last_content) != 0) {
            msg(LOG_DEBUG, "Content changed, saving to history");
            strncpy(last_content, content, MAX_CLIPBOARD_SIZE - 1);
            last_content[MAX_CLIPBOARD_SIZE - 1] = '\0';
            history_add_entry(content, selection_name);
        } else {
            msg(LOG_DEBUG, "Content unchanged, skipping save");
        }
        
        free(content);
    } else {
        msg(LOG_DEBUG, "Failed to get clipboard content for %s", selection_name);
    }
}

static void poll_clipboard_changes(Display *display, Atom clipboard_atom_local, Atom primary_atom_local) {
    static Window last_clipboard_owner = None;
    static Window last_primary_owner = None;
    
    Window clipboard_owner = XGetSelectionOwner(display, clipboard_atom_local);
    Window primary_owner = XGetSelectionOwner(display, primary_atom_local);
    
    if (clipboard_owner != last_clipboard_owner) {
        msg(LOG_DEBUG, "CLIPBOARD owner changed: %lu -> %lu", last_clipboard_owner, clipboard_owner);
        last_clipboard_owner = clipboard_owner;
        
        if (clipboard_owner != None) {
            handle_clipboard_change_threaded(clipboard_atom_local, clipboard_atom_local, primary_atom_local);
        }
    }
    
    if (primary_owner != last_primary_owner) {
        msg(LOG_DEBUG, "PRIMARY owner changed: %lu -> %lu", last_primary_owner, primary_owner);
        last_primary_owner = primary_owner;
        
        if (primary_owner != None) {
            handle_clipboard_change_threaded(primary_atom_local, clipboard_atom_local, primary_atom_local);
        }
    }
}

static void* clipboard_monitor_thread(void* arg) {
    (void)arg;
    
    msg(LOG_NOTICE, "Clipboard thread started");
    
    // Open separate X11 display for this thread
    clipboard_display = XOpenDisplay(NULL);
    if (!clipboard_display) {
        msg(LOG_ERR, "Failed to open display in clipboard thread");
        return NULL;
    }
    
    Window root = DefaultRootWindow(clipboard_display);
    
    int xfixes_event_base, xfixes_error_base;
    if (!XFixesQueryExtension(clipboard_display, &xfixes_event_base, &xfixes_error_base)) {
        msg(LOG_ERR, "XFixes not available in clipboard thread");
        XCloseDisplay(clipboard_display);
        return NULL;
    }
    
    Atom thread_clipboard_atom = XInternAtom(clipboard_display, "CLIPBOARD", False);
    Atom thread_primary_atom = XInternAtom(clipboard_display, "PRIMARY", False);
    
    msg(LOG_DEBUG, "Got atoms - CLIPBOARD: %lu, PRIMARY: %lu", 
        thread_clipboard_atom, thread_primary_atom);
    
    if (thread_clipboard_atom == None || thread_primary_atom == None) {
        msg(LOG_ERR, "Failed to get required atoms");
        XCloseDisplay(clipboard_display);
        return NULL;
    }
    
    XFixesSelectSelectionInput(clipboard_display, root, thread_clipboard_atom,
                              XFixesSetSelectionOwnerNotifyMask);
    XFixesSelectSelectionInput(clipboard_display, root, thread_primary_atom,
                              XFixesSetSelectionOwnerNotifyMask);
    
    msg(LOG_NOTICE, "Clipboard thread: XFixes initialized, event base: %d", xfixes_event_base);
    
    clipboard_thread_running = 1;
    
    // Event loop for clipboard monitoring
    while (clipboard_thread_running) {
        fd_set read_fds;
        int x11_fd = ConnectionNumber(clipboard_display);
        
        FD_ZERO(&read_fds);
        FD_SET(x11_fd, &read_fds);
        
        struct timeval timeout = {2, 0}; // 2 second timeout
        
        int select_result = select(x11_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result > 0 && FD_ISSET(x11_fd, &read_fds)) {
            while (XPending(clipboard_display)) {
                XEvent event;
                XNextEvent(clipboard_display, &event);
                
                if (event.type == xfixes_event_base + XFixesSelectionNotify) {
                    XFixesSelectionNotifyEvent *sn = (XFixesSelectionNotifyEvent *)&event;
                    
                    const char *selection_name = (sn->selection == thread_clipboard_atom) ? "CLIPBOARD" : "PRIMARY";
                    msg(LOG_DEBUG, "%s selection changed, owner: %lu", selection_name, sn->owner);
                    
                    if (sn->owner != None) {
                        handle_clipboard_change_threaded(sn->selection, 
                                                        thread_clipboard_atom, 
                                                        thread_primary_atom);
                    }
                }
            }
        } else if (select_result == 0) {
            // Timeout - poll for clipboard changes
            poll_clipboard_changes(clipboard_display, thread_clipboard_atom, thread_primary_atom);
        }
        
        pthread_testcancel();
    }
    
    msg(LOG_NOTICE, "Clipboard thread: Cleaning up...");
    XCloseDisplay(clipboard_display);
    clipboard_display = NULL;
    
    msg(LOG_NOTICE, "Clipboard thread: Exited");
    return NULL;
}

int clipboard_init(void) {
    clipboard_thread_running = 0;
    clipboard_display = NULL;
    last_clipboard_content[0] = '\0';
    last_primary_content[0] = '\0';
    
    msg(LOG_NOTICE, "Clipboard system initialized");
    return 1;
}

int clipboard_start_monitoring_async(void) {
    msg(LOG_NOTICE, "Starting clipboard monitoring in background thread...");
    
    int result = pthread_create(&clipboard_thread, NULL, clipboard_monitor_thread, NULL);
    if (result != 0) {
        msg(LOG_ERR, "Failed to create clipboard thread: %d", result);
        return 0;
    }
    
    // Give thread time to initialize
    usleep(100000); // 100ms
    
    if (clipboard_thread_running) {
        msg(LOG_NOTICE, "Clipboard monitoring started in background thread");
        return 1;
    } else {
        msg(LOG_ERR, "Clipboard thread failed to start");
        return 0;
    }
}

void clipboard_stop_monitoring(void) {
    if (clipboard_thread_running) {
        msg(LOG_NOTICE, "Stopping clipboard monitoring thread...");
        clipboard_thread_running = 0;
        pthread_cancel(clipboard_thread);
        pthread_join(clipboard_thread, NULL);
        msg(LOG_NOTICE, "Clipboard monitoring thread stopped");
    }
}

// depends on xclip command. selection_name is either "clipboard" or "primary"
char* clipboard_get_content(const char* selection_name) {
    char command[256];
    snprintf(command, sizeof(command), 
             "timeout 0.5 xclip -selection %s -o 2>/dev/null", 
             selection_name);
    
    FILE *fp = popen(command, "r");
    if (!fp) return NULL;
    
    char *content = malloc(MAX_CLIPBOARD_SIZE);
    if (!content) {
        pclose(fp);
        return NULL;
    }
    
    size_t total = 0;
    size_t bytes;
    while ((bytes = fread(content + total, 1, MAX_CLIPBOARD_SIZE - total - 1, fp)) > 0) {
        total += bytes;
    }
    content[total] = '\0';
    
    int exit_code = pclose(fp);
    if (exit_code != 0 || total == 0) {
        free(content);
        return NULL;
    }
    
    while (total > 0 && (content[total-1] == '\n' || content[total-1] == '\r')) {
        content[--total] = '\0';
    }
    
    return content;
}

int clipboard_set_content(const char* content) {
    if (!content) {
        msg(LOG_WARNING, "clipboard_set_content: content is NULL");
        return 0;
    }
    
    msg(LOG_NOTICE, "Setting clipboard content: %.50s%s", 
        content, strlen(content) > 50 ? "..." : "");
    
    return set_clipboard_content(content);
}

// Add this public function near the end of the file, before clipboard_set_content
int clipboard_delete_entry(int index) {
    return history_delete_entry(index);
}

char* clipboard_entry_get_truncated(int n) {
    return history_get_entry_truncated(n);
}

char* clipboard_entry_get_content(int n) {
    return history_get_entry_full_content(n);
}

void clipboard_set_current_index(int index) {
    history_set_current_index(index);
}

int clipboard_get_current_index(void) {
    return history_get_current_index();
}

int clipboard_get_history_count(void) {
    return history_get_count();
}

void clipboard_reset_navigation(void) {
    history_reset_navigation();
}
