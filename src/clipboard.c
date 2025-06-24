#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "clipboard.h"
#include "halen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>
#include <X11/extensions/Xfixes.h>
#include <sys/syslog.h>
#include <linux/limits.h>

#define MAX_CLIPBOARD_SIZE 4096

static pthread_t clipboard_thread;
static int clipboard_thread_running = 0;
static Display *clipboard_display = NULL;
static char last_clipboard_content[MAX_CLIPBOARD_SIZE] = {0};
static char last_primary_content[MAX_CLIPBOARD_SIZE] = {0};

typedef struct {
    char *content;
    char *timestamp;
    char *source;
} history_entry_t;

static history_entry_t *history_entries = NULL;
static int history_count = 0;
static int current_history_index = -1;

static void* clipboard_monitor_thread(void* arg);
static void handle_clipboard_change_threaded(Atom selection, Atom clipboard_atom_local, Atom primary_atom_local);
static char* get_clipboard_content(const char* selection_name);
static void save_to_history(const char *content, const char *source);
static void get_timestamp(char *buffer, size_t size);
static void poll_clipboard_changes(Display *display, Atom clipboard_atom_local, Atom primary_atom_local);
static int load_history_entries(void);
static history_entry_t entry_parse(char* line);
static int create_history_file(const char *history_file);

static history_entry_t entry_parse(char *line) {
    history_entry_t entry = {NULL, NULL, NULL};
    line[strcspn(line, "\n")] = 0;
    
    if (strlen(line) < 10) {
        msg(LOG_WARNING, "Invalid history entry: '%s'", line);
        return entry;
    }
    
    // Parse: [timestamp] [source] content
    char *timestamp_start = strchr(line, '[');
    char *timestamp_end = strchr(line, ']');
    char *source_start = strchr(timestamp_end + 1, '[');
    char *source_end = strchr(source_start + 1, ']');
    char *content_start = source_end + 2; // Skip "] "
    
    if (!timestamp_start || !timestamp_end || !source_start || !source_end) {
        msg(LOG_WARNING, "Invalid history entry format: '%s'", line);
        return entry;
    }
    
    // Extract timestamp
    int timestamp_len = timestamp_end - timestamp_start - 1;
    entry.timestamp = malloc(timestamp_len + 1);
    strncpy(entry.timestamp, timestamp_start + 1, timestamp_len);
    entry.timestamp[timestamp_len] = '\0';
    
    // Extract source (CLIPBOARD or PRIMARY)
    int source_len = source_end - source_start - 1;
    entry.source = malloc(source_len + 1);
    strncpy(entry.source, source_start + 1, source_len);
    entry.source[source_len] = '\0';
    
    // Extract and unescape content
    int content_len = strlen(content_start);
    entry.content = malloc(content_len + 1);
    char *src = content_start;
    char *dst = entry.content;
    while (*src) {
        if (*src == '\\' && *(src + 1)) {
            switch (*(src + 1)) {
                case 'n': *dst++ = '\n'; src += 2; break;
                case 'r': *dst++ = '\r'; src += 2; break;
                case 't': *dst++ = '\t'; src += 2; break;
                default: *dst++ = *src++; break;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    return entry;
}

static void get_timestamp(char *buffer, size_t size) {
    struct timeval tv;
    struct tm *tm_info;
    
    gettimeofday(&tv, NULL);
    tm_info = localtime(&tv.tv_sec);
    
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
             (int)(tv.tv_usec / 1000));
}

// depends on xclip command. selection_name is either "clipboard" or "primary"
static char* get_clipboard_content(const char* selection_name) {
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

static void save_to_history(const char *content, const char *source) {
    if (!content || strlen(content) == 0) return;
    
    int has_content_flag = 0;
    for (const char *p = content; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            has_content_flag = 1;
            break;
        }
    }
    if (!has_content_flag) return;
    
    char temporary_filename[] = ".history.tmp";
    FILE *temporary_file = fopen(temporary_filename, "w");
    if (!temporary_file) {
        msg(LOG_ERR, "Failed to create temporary file");
        return;
    }
     
    FILE *existing_history_file = fopen(config.history_file, "r");
    int duplicate_found = 0;
    if (existing_history_file) {
        char line[8192];
        while (fgets(line, sizeof(line), existing_history_file)) {
            history_entry_t entry = entry_parse(line);
            if (entry.content != NULL) {
                if (strcmp(entry.content, content) == 0) {
                    duplicate_found = 1;
                    msg(LOG_DEBUG, "Found duplicate entry, removing old one");
                } else {
                    if (line[strlen(line) - 1] != '\n') {
                        fprintf(temporary_file, "%s\n", line);
                    } else {
                        fputs(line, temporary_file);
                    }
                }
                
                free(entry.content);
                free(entry.timestamp);
                free(entry.source);
            } else {
                if (line[strlen(line) - 1] != '\n') {
                    fprintf(temporary_file, "%s\n", line);
                } else {
                    fputs(line, temporary_file);
                }
            }
        }
        fclose(existing_history_file);
    }
    
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));
    
    char *escaped_content = malloc(strlen(content) * 2 + 1);
    if (escaped_content) {
        const char *src = content;
        char *dst = escaped_content;
        while (*src) {
            if (*src == '\n') {
                *dst++ = '\\';
                *dst++ = 'n';
            } else if (*src == '\r') {
                *dst++ = '\\';
                *dst++ = 'r';
            } else if (*src == '\t') {
                *dst++ = '\\';
                *dst++ = 't';
            } else {
                *dst++ = *src;
            }
            src++;
        }
        *dst = '\0';
        
        fprintf(temporary_file, "[%s] [%s] %s\n", timestamp, source, escaped_content);
        free(escaped_content);
    }
    
    fclose(temporary_file);
    
    if (rename(temporary_filename, config.history_file) != 0) {
        msg(LOG_ERR, "Failed to replace history file");
        unlink(temporary_filename);
        return;
    }
     
    if (duplicate_found) {
        msg(LOG_NOTICE, "Updated %s in history (removed duplicate): %.50s%s", source, content,
            strlen(content) > 50 ? "..." : "");
    } else {
        msg(LOG_NOTICE, "Saved %s to history: %.50s%s", source, content,
            strlen(content) > 50 ? "..." : "");
    }

    load_history_entries(); 
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
    
    char *content = get_clipboard_content(xclip_name);
    if (content) {
        msg(LOG_DEBUG, "Got clipboard content: '%.50s%s'", content, 
            strlen(content) > 50 ? "..." : "");
        
        char timestamp[32];
        get_timestamp(timestamp, sizeof(timestamp));
        
        // Display content (truncated for console)
        char display[101];
        strncpy(display, content, 100);
        display[100] = '\0';
        
        // Replace newlines with spaces for display
        for (int i = 0; i < 100 && display[i]; i++) {
            if (display[i] == '\n' || display[i] == '\r' || display[i] == '\t') {
                display[i] = ' ';
            }
        }
        
        msg(LOG_NOTICE, "[%s] %s: %s%s", timestamp, selection_name, display,
            strlen(content) > 100 ? "..." : "");
        
        char *last_content = (selection == clipboard_atom_local) ? 
                           last_clipboard_content : last_primary_content;
        
        if (strcmp(content, last_content) != 0) {
            msg(LOG_DEBUG, "Content changed, saving to history");
            strncpy(last_content, content, MAX_CLIPBOARD_SIZE - 1);
            last_content[MAX_CLIPBOARD_SIZE - 1] = '\0';
            save_to_history(content, selection_name);
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

char* clipboard_history_file_default_path(void) {
    static char history_path[PATH_MAX];
    const char *xdg_cache_home = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char cache_dir[PATH_MAX];
    
    // Determine cache directory according to XDG spec
    if (xdg_cache_home && strlen(xdg_cache_home) > 0) {
        snprintf(cache_dir, sizeof(cache_dir), "%s", xdg_cache_home);
    } else if (home && strlen(home) > 0) {
        snprintf(cache_dir, sizeof(cache_dir), "%s/.cache", home);
    } else {
        snprintf(cache_dir, sizeof(cache_dir), "/tmp");
    }
    
    // Create full path: cache_dir/clipopup/history
    int written = snprintf(history_path, sizeof(history_path), "%s/clipopup/history", cache_dir);
    if (written < 0 || (size_t)written >= sizeof(history_path)) {
        msg(LOG_ERR, "Path too long for history file: %s/clipopup/history", cache_dir);
        return NULL;
    }
    return history_path;
}

static int create_history_file(const char *history_file) {
    // Create the history file if it doesn't exist
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", history_file);
    char *directory = dirname(dir);

    // Create directory with proper permissions (755)
    if (mkdir(directory, 0755) == -1 && errno != EEXIST) {
        msg(LOG_WARNING, "Warning: Could not create directory %s: %s", directory, strerror(errno));
    }

    FILE *file = fopen(history_file, "a");
    if (!file) {
        msg(LOG_ERR, "Failed to create history file: %s", history_file);
        return 0;
    }
    fclose(file);
    msg(LOG_DEBUG, "Created history file: %s", history_file);
    return 1;
}

static int load_history_entries(void) {
    
    if (history_entries) {
        for (int i = 0; i < history_count; i++) {
            free(history_entries[i].content);
            free(history_entries[i].timestamp);
            free(history_entries[i].source);
        }
        free(history_entries);
        history_entries = NULL;
    }
    
    FILE *history_file = fopen(config.history_file, "r");
    if (!history_file) {
        msg(LOG_DEBUG, "History file doesn't exist, creating with initial entry");

        // Try to get current clipboard content
        char *current_clipboard = get_clipboard_content("clipboard");
        const char *initial_content = current_clipboard ? current_clipboard : "Clipboard Empty";

        create_history_file(config.history_file);
        save_to_history(initial_content, "CLIPBOARD");
        
        // Try opening again after creating
        history_file = fopen(config.history_file, "r");
        if (!history_file) {
            msg(LOG_ERR, "Failed to open history file after creating it");
            history_count = 0;
            return 0;
        }
    }
    
    // Count lines first
    history_count = 0;
    char line[8192];
    while (fgets(line, sizeof(line), history_file)) {
        if (strlen(line) > 10) { // Skip empty/short lines
            history_count++;
        }
    }
    
    if (history_count == 0) {
        fclose(history_file);
        return 0;
    }
    
    // Allocate memory for entries
    history_entries = malloc(history_count * sizeof(history_entry_t));
    if (!history_entries) {
        fclose(history_file);
        history_count = 0;
        return 0;
    }
    
    // Read entries
    rewind(history_file);
    int index = 0;
    while (fgets(line, sizeof(line), history_file) && index < history_count) {
        history_entry_t entry = entry_parse(line);
        if (entry.content == NULL) {
            msg(LOG_WARNING, "Invalid history entry format: '%s'", line);
            if (entry.timestamp) free(entry.timestamp);
            if (entry.source) free(entry.source);
            continue;
        }

        history_entries[index] = entry;
        index++;
    }
    
    history_count = index;
    fclose(history_file);
    
    msg(LOG_DEBUG, "Loaded %d history entries", history_count);
    return history_count;
}


int clipboard_get_history_count(void) {
    if (history_count < 1) load_history_entries();
    return history_count;
}

int clipboard_get_current_index(void) {
    return current_history_index;
}

void clipboard_set_current_index(int index) {
    if (history_count < 1) load_history_entries();
    if (history_count > 0) {
        if (index >= 0 && index < history_count) {
            current_history_index = index;
        } else if (index == -1) {
            current_history_index = history_count - 1; // Latest entry
        }
    }
}

// Get clipboard history entry by index
// n = 0: oldest entry, n = history_count-1: latest entry, n = -1: latest entry
char* clipboard_get_entry(int n) {
    if (history_count < 1) load_history_entries();
    if (history_count < 1) {
        msg(LOG_WARNING, "No history entries available to get");
        return NULL;
    }
    
    int index;
    if (n == -1) {
        index = history_count - 1; // Latest entry
    } else if (n >= 0 && n < history_count) {
        index = n;
    } else {
        return NULL; // Invalid index
    }
    
    if (index >= 0 && index < history_count) {
        msg(LOG_DEBUG, "Getting history entry %d/%d: %.50s", 
            index + 1, history_count, 
            history_entries[index].content);
        return strdup(history_entries[index].content);
    }
    
    return NULL;
}

void clipboard_reset_navigation(void) {
    current_history_index = -1;
}

static int delete_history_entry(int index) {
    if (history_count < 1) load_history_entries();
    if (history_count < 1 || index < 0 || index >= history_count) {
        return 0;
    }
    
    // Create temporary file
    char temp_filename[] = ".history.tmp";
    FILE *temp_file = fopen(temp_filename, "w");
    if (!temp_file) {
        msg(LOG_ERR, "Failed to create temporary file for deletion");
        return 0;
    }
    
    FILE *history_file = fopen(config.history_file, "r");
    if (!history_file) {
        fclose(temp_file);
        unlink(temp_filename);
        return 0;
    }
    
    char line[8192];
    int current_index = 0;
    int deleted = 0;
    
    while (fgets(line, sizeof(line), history_file)) {
        if (strlen(line) < 10) {
            fputs(line, temp_file);
            continue;
        }
        
        if (current_index == index) {
            deleted = 1;
            msg(LOG_NOTICE, "Deleted history entry %d: %.50s", 
                index + 1, history_entries[index].content);
        } else {
            fputs(line, temp_file);
        }
        current_index++;
    }
    
    fclose(history_file);
    fclose(temp_file);
    
    if (deleted) {
        if (rename(temp_filename, config.history_file) != 0) {
            msg(LOG_ERR, "Failed to replace history file after deletion");
            unlink(temp_filename);
            return 0;
        }
        
        load_history_entries();
        
        return 1;
    } else {
        unlink(temp_filename);
        return 0;
    }
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
    return delete_history_entry(index);
}
