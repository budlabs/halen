#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "clipboard.h"
#include "halen.h"
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

// #define MAX_CLIPBOARD_SIZE 4096
#define MAX_CLIPBOARD_ENTRIES 50
#define MAX_CLIPBOARD_SIZE (10 * 1024 * 1024)
#define METADATA_PREFIX "# HALEN_METADATA: "

typedef struct {
    int max_lines;
    int max_line_length;
} history_metadata_t;

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
static void get_timestamp(char *buffer, size_t size);
static void poll_clipboard_changes(Display *display, Atom clipboard_atom_local, Atom primary_atom_local);
static int load_history_entries(void);
static int read_history_metadata(FILE *file, history_metadata_t *metadata);
static void write_history_metadata(FILE *file, const history_metadata_t *metadata);
static int needs_regeneration(const history_metadata_t *stored_metadata);
static void regenerate_truncated_entries(void);
static history_entry_t entry_parse(const char *line);
static int create_history_file(const char *history_file);
static uint32_t calculate_fast_hash(const char *content);
static char* load_full_content_from_overflow(int index);


// Replace the existing unescape_content_from_storage function with this unified version
static char* transform_content_escaping(const char* content, int should_escape) {
    if (!content) return NULL;
    
    size_t content_length = strlen(content);
    char *result = malloc(content_length * 2 + 1);
    if (!result) return NULL;
    
    const char *source = content;
    char *destination = result;
    
    if (should_escape) {
        while (*source) {
            switch (*source) {
                case '\n': *destination++ = '\\'; *destination++ = 'n'; break;
                case '\r': *destination++ = '\\'; *destination++ = 'r'; break;
                case '\t': *destination++ = '\\'; *destination++ = 't'; break;
                default: *destination++ = *source; break;
            }
            source++;
        }
    } else {
        while (*source) {
            if (*source == '\\' && *(source + 1)) {
                switch (*(source + 1)) {
                    case 'n': *destination++ = '\n'; source += 2; break;
                    case 'r': *destination++ = '\r'; source += 2; break;
                    case 't': *destination++ = '\t'; source += 2; break;
                    default: *destination++ = *source++; break;
                }
            } else {
                *destination++ = *source++;
            }
        }
    }
    *destination = '\0';
    return result;
}

// Extract overflow hash extraction logic
static char* extract_overflow_hash_from_line(const char* line) {
    char *overflow_marker = strstr(line, "[OVERFLOW:");
    if (!overflow_marker) return NULL;
    
    char *overflow_hash = malloc(16);
    if (!overflow_hash) return NULL;
    
    if (sscanf(overflow_marker, "[OVERFLOW:%15[^]]]", overflow_hash) != 1) {
        free(overflow_hash);
        return NULL;
    }
    
    return overflow_hash;
}

// Extract file operations with temporary file pattern
static int replace_file_atomically(const char* source_filename, const char* target_filename) {
    if (rename(source_filename, target_filename) != 0) {
        unlink(source_filename);
        return 0;
    }
    return 1;
}

// Extract common file reading pattern for history entries
static int count_history_entries_in_file(FILE* file) {
    int entry_count = 0;
    char line[8192];
    
    // Skip metadata line
    fgets(line, sizeof(line), file);
    
    while (fgets(line, sizeof(line), file)) {
        if (strlen(line) > 10) {
            entry_count++;
        }
    }
    
    return entry_count;
}

static uint32_t calculate_fast_hash(const char *content) {
    uint32_t hash_value = 2166136261u;
    const uint32_t prime = 16777619u;
    
    for (const char *character = content; *character; character++) {
        hash_value ^= (uint32_t)*character;
        hash_value *= prime;
    }
    
    return hash_value;
}

static char* create_display_formatted_content(const char *content) {
    if (!content) return NULL;
    
    size_t buffer_size = (config.max_lines * (config.max_line_length + 1)) + 100;
    char *result = malloc(buffer_size);
    if (!result) return strdup(content);
    
    char *write_position = result;
    const char *line_start = content;
    int displayed_lines = 0;
    int total_lines = 0;
    
    for (const char *p = content; *p; p++) {
        if (*p == '\n') total_lines++;
    }
    if (*content && content[strlen(content) - 1] != '\n') {
        total_lines++;
    }
    
    while (*line_start && displayed_lines < config.max_lines) {
        const char *line_end = strchr(line_start, '\n');
        int line_length;
        
        if (line_end) {
            line_length = line_end - line_start;
        } else {
            line_length = strlen(line_start);
        }
        
        if (displayed_lines > 0) {
            *write_position++ = '\n';
        }
        
        if (line_length > config.max_line_length) {
            strncpy(write_position, line_start, config.max_line_length - 3);
            write_position += config.max_line_length - 3;
            strcpy(write_position, "...");
            write_position += 3;
        } else {
            strncpy(write_position, line_start, line_length);
            write_position += line_length;
        }
        
        displayed_lines++;
        
        if (line_end) {
            line_start = line_end + 1;
        } else {
            break;
        }
    }
    
    int remaining_lines = total_lines - displayed_lines;
    if (remaining_lines > 0) {
        sprintf(write_position, "\n(+%d lines)", remaining_lines);
    } else {
        *write_position = '\0';
    }
    
    return result;
}

static char* truncate_content_for_storage(const char *content, char **overflow_hash) {
    int content_length = strlen(content);
    int line_count = 0;
    int current_line_length = 0;
    int needs_truncation = 0;
    
    // Check if content exceeds limits
    for (int i = 0; i < content_length; i++) {
        if (content[i] == '\n') {
            line_count++;
            current_line_length = 0;
            if (line_count >= config.max_lines) {
                needs_truncation = 1;
                break;
            }
        } else {
            current_line_length++;
            if (current_line_length > config.max_line_length) {
                needs_truncation = 1;
                break;
            }
        }
    }
    
    if (!needs_truncation) {
        *overflow_hash = NULL;
        return strdup(content);
    }
    
    if (!config.overflow_directory) {
        *overflow_hash = NULL;
        return strdup(content);
    }
    
    // Create overflow file with full content
    uint32_t content_hash = calculate_fast_hash(content);
    *overflow_hash = malloc(16);
    if (!*overflow_hash) {
        return strdup(content);
    }
    snprintf(*overflow_hash, 16, "%08x", content_hash);
    
    char overflow_file_path[PATH_MAX];
    snprintf(overflow_file_path, sizeof(overflow_file_path), "%s/%s", 
             config.overflow_directory, *overflow_hash);
    
    FILE *overflow_file = fopen(overflow_file_path, "w");
    if (overflow_file) {
        fprintf(overflow_file, "%s", content);
        fclose(overflow_file);
    } else {
        free(*overflow_hash);
        *overflow_hash = NULL;
        return strdup(content);
    }
    
    // Create truncated version with "..." marker
    char *truncated_content = create_display_formatted_content(content);
    if (!truncated_content) {
        truncated_content = strdup(content);
    }
    
    return truncated_content;
}

static char* extract_display_content(const char *raw_content) {
    if (!raw_content) return NULL;
    
    const char *overflow_marker = "[OVERFLOW:";
    const char *overflow_start = strstr(raw_content, overflow_marker);
    
    if (overflow_start) {
        const char *content_start = strchr(overflow_start, ']');
        if (content_start && content_start[1] == ' ') {
            return strdup(content_start + 2);
        }
    }
    
    return strdup(raw_content);
}

static history_entry_t entry_parse(const char *line) {
    history_entry_t entry = {NULL, NULL, NULL};
    
    char *line_copy = strdup(line);
    if (!line_copy) return entry;
    
    line_copy[strcspn(line_copy, "\n")] = 0;
    
    if (strlen(line_copy) < 10) {
        msg(LOG_WARNING, "Invalid history entry: '%s'", line_copy);
        free(line_copy);
        return entry;
    }
    
    char *timestamp_start = strchr(line_copy, '[');
    char *timestamp_end = strchr(line_copy, ']');
    char *source_start = strchr(timestamp_end + 1, '[');
    char *source_end = strchr(source_start + 1, ']');
    char *content_start = source_end + 2;
    
    if (!timestamp_start || !timestamp_end || !source_start || !source_end) {
        msg(LOG_WARNING, "Invalid history entry format: '%s'", line_copy);
        free(line_copy);
        return entry;
    }
    
    int timestamp_length = timestamp_end - timestamp_start - 1;
    entry.timestamp = malloc(timestamp_length + 1);
    strncpy(entry.timestamp, timestamp_start + 1, timestamp_length);
    entry.timestamp[timestamp_length] = '\0';
    
    int source_length = source_end - source_start - 1;
    entry.source = malloc(source_length + 1);
    strncpy(entry.source, source_start + 1, source_length);
    entry.source[source_length] = '\0';
    
    char *display_content = extract_display_content(content_start);
    entry.content = transform_content_escaping(display_content, 0);
    free(display_content);
    free(line_copy);

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
    
    char *overflow_hash = NULL;
    char *storage_content = truncate_content_for_storage(content, &overflow_hash);
    if (!storage_content) return;
    
    int has_content_flag = 0;
    for (const char *character = storage_content; *character; character++) {
        if (*character != ' ' && *character != '\t' && *character != '\n' && *character != '\r') {
            has_content_flag = 1;
            break;
        }
    }
    if (!has_content_flag) {
        free(storage_content);
        if (overflow_hash) free(overflow_hash);
        return;
    }
    
    if (!config.history_file) {
        free(storage_content);
        if (overflow_hash) free(overflow_hash);
        return;
    }
    
    char temporary_filename[] = ".history.tmp";
    FILE *temporary_file = fopen(temporary_filename, "w");
    if (!temporary_file) {
        free(storage_content);
        if (overflow_hash) free(overflow_hash);
        return;
    }
    
    history_metadata_t current_metadata = { config.max_lines, config.max_line_length };
    write_history_metadata(temporary_file, &current_metadata);
 
    int duplicate_found = 0;
    
    FILE *existing_history_file = fopen(config.history_file, "r");
    if (existing_history_file) {
        char line[8192];
        
        fgets(line, sizeof(line), existing_history_file);
        
        while (fgets(line, sizeof(line), existing_history_file)) {
            if (strlen(line) == 0) continue;
            
            history_entry_t entry = entry_parse(line);
            if (entry.content != NULL) {
                int is_duplicate = 0;
                
                char *entry_overflow_hash = extract_overflow_hash_from_line(line);
                if (entry_overflow_hash) {
                    if (overflow_hash && strcmp(entry_overflow_hash, overflow_hash) == 0) {
                        is_duplicate = 1;
                    }
                    free(entry_overflow_hash);
                } else {
                    is_duplicate = (strcmp(entry.content, content) == 0);
                }
                
                if (is_duplicate) {
                    duplicate_found = 1;
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
    
    size_t content_length = strlen(storage_content);
    if (content_length == 0) {
        goto cleanup;
    }
    
    // Add missing helper function
    char *escaped_content = transform_content_escaping(storage_content, 1);
    if (escaped_content) {
        if (overflow_hash) {
            fprintf(temporary_file, "[%s] [%s] [OVERFLOW:%s] %s\n", 
                    timestamp, source, overflow_hash, escaped_content);
        } else {
            fprintf(temporary_file, "[%s] [%s] %s\n", timestamp, source, escaped_content);
        }
        free(escaped_content);
    }
    
cleanup:
    fclose(temporary_file);
    
    if (!replace_file_atomically(temporary_filename, config.history_file)) {
        free(storage_content);
        if (overflow_hash) free(overflow_hash);
        return;
    }
      
    if (duplicate_found) {
        msg(LOG_NOTICE, "Updated %s in history%s (removed duplicate): %.50s%s", 
            source, overflow_hash ? " (truncated)" : "", storage_content,
            strlen(storage_content) > 50 ? "..." : "");
    } else {
        msg(LOG_NOTICE, "Saved %s to history%s: %.50s%s", 
            source, overflow_hash ? " (truncated)" : "", storage_content,
            strlen(storage_content) > 50 ? "..." : "");
    }

    free(storage_content);
    if (overflow_hash) free(overflow_hash);
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

static int read_history_metadata(FILE *file, history_metadata_t *metadata) {
    char line[256];
    rewind(file);
    
    if (fgets(line, sizeof(line), file)) {
        if (strncmp(line, METADATA_PREFIX, strlen(METADATA_PREFIX)) == 0) {
            char *metadata_content = line + strlen(METADATA_PREFIX);
            if (sscanf(metadata_content, "max_lines=%d max_line_length=%d", 
                      &metadata->max_lines, &metadata->max_line_length) == 2) {
                return 1;
            }
        }
    }
    
    metadata->max_lines = -1;
    metadata->max_line_length = -1;
    return 0;
}

static void write_history_metadata(FILE *file, const history_metadata_t *metadata) {
    fprintf(file, "%smax_lines=%d max_line_length=%d\n", 
            METADATA_PREFIX, metadata->max_lines, metadata->max_line_length);
}

static int needs_regeneration(const history_metadata_t *stored_metadata) {
    // Always regenerate if metadata doesn't match current settings
    if (stored_metadata->max_lines != config.max_lines || 
        stored_metadata->max_line_length != config.max_line_length) {
        return 1;
    }
    
    return 0;
}

static void regenerate_truncated_entries(void) {
    if (!config.history_file || !config.overflow_directory) {
        if (!config.overflow_directory) {
            msg(LOG_WARNING, "No overflow directory configured, skipping regeneration");
        }
        return;
    }
    
    msg(LOG_NOTICE, "Regenerating truncated entries with new settings: max_lines=%d, max_line_length=%d", 
        config.max_lines, config.max_line_length);
    
    FILE *history_file = fopen(config.history_file, "r");
    if (!history_file) return;
    
    char temp_filename[] = ".history_regen.tmp";
    FILE *temp_file = fopen(temp_filename, "w");
    if (!temp_file) {
        fclose(history_file);
        return;
    }
    
    history_metadata_t current_metadata = {
        .max_lines = config.max_lines,
        .max_line_length = config.max_line_length
    };
    write_history_metadata(temp_file, &current_metadata);
    
    char line[8192];
    fgets(line, sizeof(line), history_file);
    
    int entries_regenerated = 0;
    int line_index = 0;
    
    while (fgets(line, sizeof(line), history_file)) {
        if (strlen(line) < 10) {
            fputs(line, temp_file);
            continue;
        }
        
        char *overflow_hash = extract_overflow_hash_from_line(line);
        if (overflow_hash) {
            char *full_content = load_full_content_from_overflow(line_index);
            if (full_content) {
                char *regenerated_content = create_display_formatted_content(full_content);
                if (regenerated_content) {
                    char *escaped_regenerated = transform_content_escaping(regenerated_content, 1);
                    if (escaped_regenerated) {
                        char timestamp[32], source[16];
                        if (sscanf(line, "[%31[^]]] [%15[^]]]", timestamp, source) == 2) {
                            fprintf(temp_file, "[%s] [%s] [OVERFLOW:%s] %s\n", 
                                    timestamp, source, overflow_hash, escaped_regenerated);
                            entries_regenerated++;
                        } else {
                            msg(LOG_WARNING, "Failed to parse timestamp/source from line: %.50s", line);
                            fputs(line, temp_file);
                        }
                        free(escaped_regenerated);
                    } else {
                        msg(LOG_ERR, "Failed to allocate memory for escaped content");
                        fputs(line, temp_file);
                    }
                    free(regenerated_content);
                } else {
                    msg(LOG_ERR, "Failed to regenerate display content");
                    fputs(line, temp_file);
                }
                free(full_content);
            } else {
                msg(LOG_WARNING, "Could not load full content for regeneration");
                fputs(line, temp_file);
            }
            free(overflow_hash);
        } else {
            fputs(line, temp_file);
        }
        
        line_index++;
    }
    
    fclose(history_file);
    fclose(temp_file);
    
    if (entries_regenerated > 0) {
        msg(LOG_NOTICE, "Regenerated %d entries, replacing history file", entries_regenerated);
        replace_file_atomically(temp_filename, config.history_file);
        msg(LOG_NOTICE, "History entries regenerated successfully");
    } else {
        msg(LOG_DEBUG, "No entries needed regeneration, removing temp file");
        unlink(temp_filename);
    }
}

char* clipboard_history_file_default_path(void) {
    static char history_path[PATH_MAX];
    char *cache_dir = xdg_get_directory(XDG_CACHE_HOME);
    if (!cache_dir) {
        msg(LOG_ERR, "Failed to determine cache directory");
        return NULL;
    }
    
    int written = snprintf(history_path, sizeof(history_path), "%s/halen/history", cache_dir);
    free(cache_dir);
    
    if (written < 0 || (size_t)written >= sizeof(history_path)) {
        msg(LOG_ERR, "Path too long for history file");
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
    
    history_metadata_t metadata = { config.max_lines, config.max_line_length };
    write_history_metadata(file, &metadata);
    
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
    
    FILE *metadata_check_file = fopen(config.history_file, "r");
    if (metadata_check_file) {
        history_metadata_t stored_metadata;
        if (read_history_metadata(metadata_check_file, &stored_metadata)) {
            msg(LOG_DEBUG, "Found metadata: max_lines=%d, max_line_length=%d", 
                stored_metadata.max_lines, stored_metadata.max_line_length);
            msg(LOG_DEBUG, "Current config: max_lines=%d, max_line_length=%d", 
                config.max_lines, config.max_line_length);
            
            if (needs_regeneration(&stored_metadata)) {
                msg(LOG_NOTICE, "Settings changed, regenerating truncated entries");
                fclose(metadata_check_file);
                regenerate_truncated_entries();
                metadata_check_file = fopen(config.history_file, "r");
            } else {
                msg(LOG_DEBUG, "Settings unchanged, no regeneration needed");
            }
        } else {
            msg(LOG_NOTICE, "No metadata found, assuming regeneration needed");
            fclose(metadata_check_file);
            regenerate_truncated_entries();
            metadata_check_file = fopen(config.history_file, "r");
        }
        if (metadata_check_file) fclose(metadata_check_file);
    }
    
    FILE *history_file = fopen(config.history_file, "r");
    if (!history_file) {
        msg(LOG_DEBUG, "History file doesn't exist, creating with initial entry");

        char *current_clipboard = get_clipboard_content("clipboard");
        const char *initial_content = current_clipboard ? current_clipboard : "Clipboard Empty";

        create_history_file(config.history_file);
        save_to_history(initial_content, "CLIPBOARD");
        
        history_file = fopen(config.history_file, "r");
        if (!history_file) {
            msg(LOG_ERR, "Failed to open history file after creating it");
            history_count = 0;
            return 0;
        }
    }
    
    history_count = count_history_entries_in_file(history_file);
    if (history_count == 0) {
        fclose(history_file);
        return 0;
    }
    
    history_entries = malloc(history_count * sizeof(history_entry_t));
    if (!history_entries) {
        fclose(history_file);
        history_count = 0;
        return 0;
    }
    
    rewind(history_file);
    char line[8192];
    
    fgets(line, sizeof(line), history_file);
    
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

// Refactor delete_history_entry to use extracted functions
static int delete_history_entry(int index) {
    if (history_count < 1) load_history_entries();
    if (history_count < 1 || index < 0 || index >= history_count) {
        return 0;
    }
    
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
    char *overflow_hash_to_delete = NULL;
    
    if (fgets(line, sizeof(line), history_file)) {
        fputs(line, temp_file);
    }
    
    while (fgets(line, sizeof(line), history_file)) {
        if (strlen(line) < 10) {
            fputs(line, temp_file);
            continue;
        }
        
        if (current_index == index) {
            overflow_hash_to_delete = extract_overflow_hash_from_line(line);
            if (overflow_hash_to_delete) {
                msg(LOG_DEBUG, "Will delete overflow file for hash: %s", overflow_hash_to_delete);
            }
            
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
        if (overflow_hash_to_delete && config.overflow_directory) {
            char overflow_file_path[PATH_MAX];
            snprintf(overflow_file_path, sizeof(overflow_file_path), "%s/%s", 
                    config.overflow_directory, overflow_hash_to_delete);
            
            if (unlink(overflow_file_path) == 0) {
                msg(LOG_DEBUG, "Deleted overflow file: %s", overflow_file_path);
            } else {
                msg(LOG_WARNING, "Failed to delete overflow file: %s", overflow_file_path);
            }
            free(overflow_hash_to_delete);
        }
        
        if (!replace_file_atomically(temp_filename, config.history_file)) {
            msg(LOG_ERR, "Failed to replace history file after deletion");
            return 0;
        }
        
        load_history_entries();
        
        if (current_history_index >= history_count) {
            current_history_index = -1;
        }
        
        return 1;
    } else {
        unlink(temp_filename);
        return 0;
    }
}

char* clipboard_entry_get_truncated(int n) {
    if (history_count < 1) load_history_entries();
    if (history_count < 1) {
        msg(LOG_WARNING, "No history entries available to get");
        return NULL;
    }
    
    int index;
    if (n == -1) {
        index = history_count - 1;
    } else if (n >= 0 && n < history_count) {
        index = n;
    } else {
        return NULL;
    }
    
    if (index >= 0 && index < history_count) {
        msg(LOG_DEBUG, "Getting truncated history entry %d/%d: %.50s", 
            index + 1, history_count, 
            history_entries[index].content);
        return strdup(history_entries[index].content);
    }
    
    return NULL;
}

char* clipboard_entry_get_content(int n) {
    if (history_count < 1) load_history_entries();
    if (history_count < 1) {
        msg(LOG_WARNING, "No history entries available to get");
        return NULL;
    }
    
    int index;
    if (n == -1) {
        index = history_count - 1;
    } else if (n >= 0 && n < history_count) {
        index = n;
    } else {
        return NULL;
    }
    
    if (index >= 0 && index < history_count) {
        char *full_content = load_full_content_from_overflow(index);
        if (full_content) {
            msg(LOG_DEBUG, "Retrieved full content from overflow for entry %d/%d: %.50s", 
                index + 1, history_count, full_content);
            return full_content;
        }
        
        msg(LOG_DEBUG, "Getting regular history entry %d/%d: %.50s", 
            index + 1, history_count, 
            history_entries[index].content);
        return strdup(history_entries[index].content);
    }
    
    return NULL;
}

void clipboard_set_current_index(int index) {
    current_history_index = index;
    msg(LOG_DEBUG, "Set current clipboard index to %d", index);
}

int clipboard_get_current_index(void) {
    return current_history_index;
}

int clipboard_get_history_count(void) {
    return history_count;
}

void clipboard_reset_navigation(void) {
    current_history_index = -1;
    msg(LOG_DEBUG, "Reset clipboard navigation index");
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

static char* load_full_content_from_overflow(int index) {
    if (!config.overflow_directory || index < 0) {
        return NULL;
    }
    
    FILE *history_file = fopen(config.history_file, "r");
    if (!history_file) return NULL;
    
    char line[8192];
    int current_index = 0;
    
    // Skip metadata line
    fgets(line, sizeof(line), history_file);
    
    while (fgets(line, sizeof(line), history_file)) {
        if (strlen(line) < 10) continue;
        
        if (current_index == index) {
            char *overflow_marker = strstr(line, "[OVERFLOW:");
            if (overflow_marker) {
                char overflow_hash[16];
                if (sscanf(overflow_marker, "[OVERFLOW:%15[^]]]", overflow_hash) == 1) {
                    fclose(history_file);
                    
                    char overflow_file_path[PATH_MAX];
                    snprintf(overflow_file_path, sizeof(overflow_file_path), "%s/%s", 
                            config.overflow_directory, overflow_hash);
                    
                    FILE *overflow_file = fopen(overflow_file_path, "r");
                    if (overflow_file) {
                        char *full_content = malloc(MAX_CLIPBOARD_SIZE);
                        if (full_content) {
                            size_t content_size = fread(full_content, 1, MAX_CLIPBOARD_SIZE - 1, overflow_file);
                            full_content[content_size] = '\0';
                            fclose(overflow_file);
                            
                            msg(LOG_DEBUG, "Loaded full content from overflow file: %s", overflow_file_path);
                            return full_content;
                        }
                        fclose(overflow_file);
                    } else {
                        msg(LOG_WARNING, "Could not open overflow file: %s", overflow_file_path);
                    }
                }
            }
            break;
        }
        current_index++;
    }
    
    fclose(history_file);
    return NULL;
}
