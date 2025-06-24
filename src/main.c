#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#include <getopt.h>
#include <sys/syslog.h>

#include "config.h"

#include "halen.h"
#include "popup.h"
#include "hotkey.h"
#include "clipboard.h"
#include "parser.h"

Display *g_display = NULL;
Window g_root_window;
config_t config;
volatile int g_running = 1;
int g_verbose = 0;

static char *log_file = NULL;
static char *config_file = NULL;
static FILE *log_fp = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void signal_handler(int signum) {
    msg(LOG_NOTICE, "Received signal %d, exiting...", signum);
    g_running = 0;
}

void hotkey_event_callback(const char *event_type) {
    msg(LOG_NOTICE, "Hotkey callback: %s", event_type);
    
    if (strcmp(event_type, "double_paste") == 0) {
        msg(LOG_NOTICE, "Ctrl+V+V: show popup");
        
        char *latest_entry = clipboard_get_entry(-1);
        if (latest_entry) {
            clipboard_set_current_index(-1);
            if (!popup_show(latest_entry)) {
                msg(LOG_WARNING, "Failed to show popup");
            }

            free(latest_entry);
        } else {
            msg(LOG_WARNING, "Failed to show popup, no entries, or no history");
        }
        
    } else if (strcmp(event_type, "cb_clipboard_next") == 0) {
        msg(LOG_NOTICE, "Ctrl+V+V+V: Navigate NEXT (older entries)");
        
        int current_index = clipboard_get_current_index();
        int history_count = clipboard_get_history_count();
        
        if (history_count > 0) {
            int next_index;
            if (current_index <= 0) {
                // Wrap around to latest entry
                next_index = history_count - 1;
            } else {
                // previous/older
                next_index = current_index - 1;
            }
            
            char *next_entry = clipboard_get_entry(next_index);
            if (next_entry) {
                clipboard_set_current_index(next_index);
                if (popup_is_showing()) {
                    popup_update_text(next_entry);
                    msg(LOG_DEBUG, "Updated popup with NEXT entry %d/%d: %.50s", 
                        next_index + 1, history_count, next_entry);
                }
                free(next_entry);
            } else {
                msg(LOG_WARNING, "No next history entry available");
            }
        }
        
    } else if (strcmp(event_type, "cb_clipboard_prev") == 0) {
        msg(LOG_NOTICE, "Ctrl+V+V+C: PREV (newer entries)");
        
        int current_index = clipboard_get_current_index();
        int history_count = clipboard_get_history_count();
        
        if (history_count > 0) {
            int prev_index;
            if (current_index >= history_count - 1) {
                // Wrap around to oldest entry
                prev_index = 0;
            } else {
                // Go to next (newer) entry
                prev_index = current_index + 1;
            }
            
            char *prev_entry = clipboard_get_entry(prev_index);
            if (prev_entry) {
                clipboard_set_current_index(prev_index);
                if (popup_is_showing()) {
                    popup_update_text(prev_entry);
                    msg(LOG_DEBUG, "Updated popup with PREV entry %d/%d: %.50s", 
                        prev_index + 1, history_count, prev_entry);
                }
                free(prev_entry);
            } else {
                msg(LOG_WARNING, "No previous history entry available");
            }
        }
        
    } else if (strcmp(event_type, "single_paste") == 0) {
        msg(LOG_NOTICE, "Single Ctrl+V completed");
        
    } else if (strcmp(event_type, "cb_clipboard_cut") == 0) {
        msg(LOG_NOTICE, "Cut clipboard entry - selecting current entry but NOT pasting");
        
        int current_index = clipboard_get_current_index();
        if (current_index >= 0) {
            char *selected_entry = clipboard_get_entry(current_index);
            if (selected_entry) {
                // (this will trigger our clipboard listener and move entry to top of history)
                clipboard_set_content(selected_entry);
                
                msg(LOG_NOTICE, "Cut complete: selected entry %d set as clipboard content (NO PASTE)", 
                    current_index + 1);
                
                free(selected_entry);
            }
        } else {
            msg(LOG_WARNING, "No current entry to cut");
        }
        
        clipboard_reset_navigation();
        
    } else if (strcmp(event_type, "cb_clipboard_delete") == 0) {
        msg(LOG_NOTICE, "Ctrl+V+V+D: DELETE");
        
        int current_index = clipboard_get_current_index();
        nav_direction_t nav_direction = hotkey_get_nav_direction();
        
        msg(LOG_DEBUG, "DELETE: current_index=%d, direction=%s", 
            current_index, nav_direction == NAV_DIRECTION_NEXT ? "NEXT" : "PREV");
        
        if (current_index >= 0) {

            if (clipboard_delete_entry(current_index)) {
                msg(LOG_NOTICE, "Successfully deleted entry %d from history", current_index + 1);
                
                int new_history_count = clipboard_get_history_count();
                if (new_history_count > 0) {
                    int new_index;
                    
                    if (nav_direction == NAV_DIRECTION_PREV) {
                        // NEXT = toward older entries (higher indices originally)
                        // After deletion, the "next older" entry moved down to current position
                        new_index = current_index;
                        if (new_index >= new_history_count) {
                            // We were at the end, wrap to beginning
                            new_index = 0;
                        }
                        msg(LOG_DEBUG, "NEXT direction: new_index=%d", new_index);
                    } else {
                        // PREV = toward newer entries (lower indices)
                        // We want the "next newer" entry, which is at current_index - 1
                        new_index = current_index - 1;
                        if (new_index < 0) {
                            // We were at the beginning, wrap to end
                            new_index = new_history_count - 1;
                        }
                        msg(LOG_DEBUG, "PREV direction: new_index=%d", new_index);
                    }
                    
                    // Update popup with new entry
                    char *new_entry = clipboard_get_entry(new_index);
                    if (new_entry) {
                        clipboard_set_current_index(new_index);
                        if (popup_is_showing()) {
                            popup_update_text(new_entry);
                            msg(LOG_DEBUG, "Updated popup to entry %d/%d (%s): %.50s", 
                                new_index + 1, new_history_count,
                                nav_direction == NAV_DIRECTION_NEXT ? "NEXT" : "PREV",
                                new_entry);
                        }
                        free(new_entry);
                    }
                } else {
                    // No more entries
                    msg(LOG_NOTICE, "No more history entries, closing popup");
                    if (popup_is_showing()) {
                        popup_hide();
                    }
                    clipboard_reset_navigation();
                    hotkey_reset_nav_direction();
                }
            } else {
                msg(LOG_WARNING, "Failed to delete entry %d from history", current_index + 1);
            }
        } else {
            msg(LOG_WARNING, "No current entry to delete");
        }
        
    } else if (strcmp(event_type, "control_released") == 0) {
        msg(LOG_DEBUG, "Control key released");
        
        PopupAction action = hotkey_get_popup_action();
        
        if (popup_is_showing() 
            && (action == POPUP_ACTION_NEXT || action == POPUP_ACTION_PREV)) {
            int current_index = clipboard_get_current_index();
            if (current_index >= 0) {
                char *selected_entry = clipboard_get_entry(current_index);
                if (selected_entry) {
                    clipboard_set_content(selected_entry);
                    usleep(50000); // 50ms
                    hotkey_perform_paste();
                    free(selected_entry);
                }
            }
        }
        
        clipboard_reset_navigation();
    }
}

static void print_help(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("\n");
    printf("Smart Ctrl+V clipboard manager with popup interface\n");
    printf("\n");
    printf("Options:\n");
    printf("  -V, --verbose         Enable verbose (debug) logging\n");
    printf("  -c, --config FILE     Use configuration file (default: %s)\n", DEFAULT_CONFIG_FILE);
    printf("  -h, --help            Show this help message\n");
    printf("      --version         Show version information\n");
    printf("\n");
    printf("Features:\n");
    printf("  - Single Ctrl+V: Works normally or replayed after timeout\n");
    printf("  - Ctrl+V+V: Shows popup with clipboard history\n");
    printf("  - Automatic clipboard monitoring and history saving\n");
    printf("\n");
    printf("History file: %s\n", config.history_file);
    printf("Config file: %s\n", config_file ? config_file : DEFAULT_CONFIG_FILE);
    printf("\n");
}

static void print_version(void) {
    printf("clipopup version %s\n", VERSION);
    printf("Smart Ctrl+V clipboard manager\n");
    printf("Built with X11, XFixes, XRecord, and XTest\n");
}

void msg(int priority, const char* format, ...) {
    if (!g_verbose && priority > LOG_WARNING) return;
    
    pthread_mutex_lock(&log_mutex);
    
    va_list args;
    va_start(args, format);
    time_t now;
    struct tm *tm_info;
    char timestamp[26];
    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    
    const char* priority_str;
    switch (priority) {
        case LOG_DEBUG: priority_str = "DEBUG"; break;
        case LOG_NOTICE: priority_str = "NOTICE"; break;
        case LOG_WARNING: priority_str = "WARNING"; break;
        case LOG_ERR: priority_str = "ERROR"; break;
        default: priority_str = "INFO"; break;
    }
    
    FILE *output = stdout;
    
    fprintf(output, "%s [%s] ", timestamp, priority_str);
    vfprintf(output, format, args);
    fprintf(output, "\n");
    fflush(output);
    va_end(args);
    
    pthread_mutex_unlock(&log_mutex);
}

int main(int argc, char *argv[]) {

    signal(SIGINT, signal_handler);
    config_init(&config);
    
    static struct option long_options[] = {
        {"verbose",  no_argument,       0, 'V'},
        {"config",   required_argument, 0, 'c'},
        {"help",     no_argument,       0, 'h'},
        {"version",  no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "Vc:hv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'V':
                config.verbose = 1;
                g_verbose = 1;
                break;
            case 'c':
                if (config_file) free(config_file);
                config_file = strdup(optarg);
                break;
            case 'h':
                print_help(argv[0]);
                config_free(&config);
                return EXIT_SUCCESS;
            case 'v':
                print_version();
                config_free(&config);
                return EXIT_SUCCESS;
            case '?':
                fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
                config_free(&config);
                return EXIT_FAILURE;
            default:
                print_help(argv[0]);
                config_free(&config);
                return EXIT_FAILURE;
        }
    }
    
    if (!config_file) {
        config_file = strdup(DEFAULT_CONFIG_FILE);
    }
    
    if (!config_parse_file(&config, config_file)) {
        msg(LOG_ERR, "Failed to parse config file");
        config_free(&config);
        return EXIT_FAILURE;
    }
    
    config_apply(&config);
    
    msg(LOG_NOTICE, "clipopup Ctrl+V interception (plain C event loop) v%s", VERSION);

    if (g_verbose) {
        msg(LOG_DEBUG, "Verbose logging enabled");
        config_print(&config);
    }
    
    g_display = XOpenDisplay(NULL);
    if (!g_display) {
        msg(LOG_ERR, "Cannot open X display");
        config_free(&config);
        return EXIT_FAILURE;
    }
    
    msg(LOG_NOTICE, "X Display opened successfully");
    
    g_root_window = DefaultRootWindow(g_display);
    
    Screen *screen = DefaultScreenOfDisplay(g_display);
    int screen_width = WidthOfScreen(screen);
    int screen_height = HeightOfScreen(screen);
    
    popup_init(g_display, g_root_window, screen_width, screen_height);
    hotkey_init(hotkey_event_callback);
    
    if (!clipboard_init()) {
        msg(LOG_WARNING, "Clipboard system initialization failed");
    } else if (!clipboard_start_monitoring_async()) {
        msg(LOG_WARNING, "Clipboard monitoring disabled");
    }
    
    int x11_fd = ConnectionNumber(g_display);
    
    while (g_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(x11_fd, &read_fds);
        
        struct timeval timeout = {0, 100000}; // 100ms
        
        int select_result = select(x11_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result < 0) {
            if (errno == EINTR) {
                // Interrupted by signal, check g_running
                continue;
            }
            msg(LOG_ERR, "select() failed: %s", strerror(errno));
            break;
        }
        
        if (select_result > 0 && FD_ISSET(x11_fd, &read_fds)) {
            while (XPending(g_display)) {
                XEvent event;
                XNextEvent(g_display, &event);
                
                switch (event.type) {
                    case KeyPress:
                    case KeyRelease: {
                        hotkey_handle_xevent(&event);
                        break;
                    }
                    case Expose: {
                        popup_handle_expose(&event.xexpose);
                        break;
                    }
                    default: {
                        break;
                    }
                }
            }
        }
    }
    
    msg(LOG_NOTICE, "Event loop finished");

    clipboard_stop_monitoring();
    msg(LOG_NOTICE, "Cleaning up...");
    hotkey_cleanup();
    
    config_free(&config);

    if (log_fp) fclose(log_fp);
    if (log_file) free(log_file);
    if (config_file) free(config_file);

    XCloseDisplay(g_display);

    return EXIT_SUCCESS;
}
