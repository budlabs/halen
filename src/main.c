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
#include <fcntl.h>
#include <sys/stat.h>

#include "config.h"

#include "halen.h"
#include "popup.h"
#include "hotkey.h"
#include "clipboard.h"
#include "parser.h"
#include "xdg.h"

Display *g_display = NULL;
Window g_root_window;
config_t config;
volatile int g_running = 1;
int g_verbose = 0;
static int signal_pipe_write_fd = -1;
static int signal_pipe_read_fd = -1;

static char *log_file = NULL;
static char *config_file = NULL;
static char *pid_file_path = NULL;
static FILE *log_fp = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void setup_signal_handling(void) {
    int pipe_fds[2];
    if (pipe(pipe_fds) == -1) {
        msg(LOG_ERR, "Failed to create signal pipe: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    signal_pipe_read_fd = pipe_fds[0];
    signal_pipe_write_fd = pipe_fds[1];
    
    int flags = fcntl(signal_pipe_write_fd, F_GETFL);
    fcntl(signal_pipe_write_fd, F_SETFL, flags | O_NONBLOCK);
}

static char* construct_pid_file_path(void) {
    char *runtime_dir = xdg_get_directory(XDG_RUNTIME_DIR);
    if (!runtime_dir) {
        return NULL;
    }
    
    size_t path_length = strlen(runtime_dir) + strlen("/halen.pid") + 1;
    char *path = malloc(path_length);
    if (!path) {
        free(runtime_dir);
        return NULL;
    }
    
    snprintf(path, path_length, "%s/halen.pid", runtime_dir);
    free(runtime_dir);
    return path;
}

static int create_pid_file(void) {
    pid_file_path = construct_pid_file_path();
    if (!pid_file_path) {
        msg(LOG_ERR, "Failed to determine runtime directory");
        return 0;
    }
    
    // Check if PID file already exists and contains a running process
    FILE *existing_pid_file = fopen(pid_file_path, "r");
    if (existing_pid_file) {
        pid_t existing_pid;
        if (fscanf(existing_pid_file, "%d", &existing_pid) == 1) {
            fclose(existing_pid_file);
            
            // Check if process is still running
            if (kill(existing_pid, 0) == 0) {
                msg(LOG_ERR, "Another instance is already running with PID %d", existing_pid);
                return 0;
            } else if (errno == ESRCH) {
                msg(LOG_NOTICE, "Stale PID file found, removing it");
                unlink(pid_file_path);
            }
        } else {
            fclose(existing_pid_file);
        }
    }
    
    // Create new PID file
    FILE *pid_file = fopen(pid_file_path, "w");
    if (!pid_file) {
        msg(LOG_ERR, "Failed to create PID file %s: %s", pid_file_path, strerror(errno));
        return 0;
    }
    
    fprintf(pid_file, "%d\n", getpid());
    fclose(pid_file);
    
    msg(LOG_NOTICE, "Created PID file: %s", pid_file_path);
    return 1;
}

static void remove_pid_file(void) {
    if (pid_file_path) {
        if (unlink(pid_file_path) == 0) {
            msg(LOG_NOTICE, "Removed PID file: %s", pid_file_path);
        } else {
            msg(LOG_WARNING, "Failed to remove PID file %s: %s", pid_file_path, strerror(errno));
        }
        free(pid_file_path);
        pid_file_path = NULL;
    }
}

static void cleanup_resources(void) {
    msg(LOG_NOTICE, "Cleaning up resources...");
    
    clipboard_stop_monitoring();
    hotkey_cleanup();
    
    if (signal_pipe_read_fd != -1) {
        close(signal_pipe_read_fd);
        signal_pipe_read_fd = -1;
    }
    if (signal_pipe_write_fd != -1) {
        close(signal_pipe_write_fd);
        signal_pipe_write_fd = -1;
    }
    
    remove_pid_file();
    
    config_free(&config);
    
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
    if (log_file) {
        free(log_file);
        log_file = NULL;
    }
    if (config_file) {
        free(config_file);
        config_file = NULL;
    }
    
    if (g_display) {
        XCloseDisplay(g_display);
        g_display = NULL;
    }
    
    msg(LOG_NOTICE, "Cleanup completed");
}

void signal_handler(int signal_number) {
    char signal_byte = (char)signal_number;
    ssize_t result = write(signal_pipe_write_fd, &signal_byte, 1);
    (void)result; // Suppress unused variable warning
}

static void process_received_signal(int signal_number) {
    msg(LOG_NOTICE, "Processing signal %d", signal_number);
    
    switch (signal_number) {
        case SIGINT:
        case SIGTERM:
            msg(LOG_NOTICE, "Termination signal received, shutting down");
            g_running = 0;
            break;
        case SIGUSR1:
            msg(LOG_NOTICE, "USR1 signal received, toggling hotkey monitoring");
            hotkey_toggle_monitoring();
            break;
        default:
            msg(LOG_NOTICE, "Unknown signal %d received", signal_number);
            break;
    }
}

void hotkey_event_callback(const char *event_type) {
    msg(LOG_NOTICE, "Hotkey callback: %s", event_type);
    
    if (strcmp(event_type, "double_paste") == 0) {
        msg(LOG_NOTICE, "Ctrl+V+V: show popup");
        
        char *latest_entry = clipboard_entry_get_truncated(-1);
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
                next_index = history_count - 1;
            } else {
                next_index = current_index - 1;
            }
            
            char *next_entry = clipboard_entry_get_truncated(next_index);
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
                prev_index = 0;
            } else {
                prev_index = current_index + 1;
            }
            
            char *prev_entry = clipboard_entry_get_truncated(prev_index);
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
            char *selected_entry = clipboard_entry_get_content(current_index);
            if (selected_entry) {
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
                msg(LOG_DEBUG, "DELETE: new_history_count=%d", new_history_count);
                
                if (new_history_count > 0) {
                    int new_index;
                    
                    if (nav_direction == NAV_DIRECTION_PREV) {
                        new_index = current_index;
                        if (new_index >= new_history_count) {
                            new_index = 0;
                        }
                        msg(LOG_DEBUG, "PREV direction: new_index=%d", new_index);
                    } else {
                        new_index = current_index - 1;
                        if (new_index < 0) {
                            new_index = new_history_count - 1;
                        }
                        msg(LOG_DEBUG, "NEXT direction: new_index=%d", new_index);
                    }
                    
                    char *new_entry = clipboard_entry_get_truncated(new_index);
                    if (new_entry) {
                        clipboard_set_current_index(new_index);
                        msg(LOG_DEBUG, "DELETE: popup_is_showing=%d", popup_is_showing());
                        if (popup_is_showing()) {
                            popup_update_text(new_entry);
                            msg(LOG_DEBUG, "Updated popup to entry %d/%d (%s): %.50s", 
                                new_index + 1, new_history_count,
                                nav_direction == NAV_DIRECTION_NEXT ? "NEXT" : "PREV",
                                new_entry);
                        }
                        free(new_entry);
                    } else {
                        msg(LOG_WARNING, "Failed to get entry after deletion, closing popup");
                        if (popup_is_showing()) {
                            msg(LOG_DEBUG, "DELETE: force hiding popup due to failed entry retrieval");
                            popup_hide();
                        }
                        clipboard_reset_navigation();
                        hotkey_reset_nav_direction();
                    }
                } else {
                    msg(LOG_NOTICE, "No more history entries, closing popup");
                    if (popup_is_showing()) {
                        msg(LOG_DEBUG, "DELETE: force hiding popup - no more entries");
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
        
        
        PopupAction action = hotkey_get_popup_action();
        msg(LOG_DEBUG, "Control key released, action: %d", action);

        if (popup_is_showing() 
            && (action == POPUP_ACTION_NEXT || action == POPUP_ACTION_PREV)) {
            int current_index = clipboard_get_current_index();
            if (current_index >= 0 && current_index < clipboard_get_history_count()) {
                char *selected_entry = clipboard_entry_get_content(current_index);
                if (selected_entry) {
                    clipboard_set_content(selected_entry);
                    usleep(50000);
                    hotkey_perform_paste();
                    free(selected_entry);
                } else {
                    msg(LOG_WARNING, "Failed to get selected entry content for paste");
                }
            } else {
                msg(LOG_WARNING, "Invalid current index %d for paste operation", current_index);
            }
        } else if (popup_is_showing() && action == POPUP_ACTION_CUT) {
            int current_index = clipboard_get_current_index();
            if (current_index >= 0 && current_index < clipboard_get_history_count()) {
                char *selected_entry = clipboard_entry_get_content(current_index);
                if (selected_entry) {
                    clipboard_set_content(selected_entry);
                    msg(LOG_NOTICE, "Entry %d set to clipboard after deletion (no paste)", current_index + 1);
                    free(selected_entry);
                } else {
                    msg(LOG_WARNING, "Failed to get selected entry content for cut");
                }
            }
        }
        
        if (popup_is_showing()) {
            popup_hide();
            msg(LOG_DEBUG, "Popup hidden on control release");
        }
        clipboard_reset_navigation();
    }
}

static void print_help(const char *program_name) {
    config_file = config_file ? config_file : xdg_get_user_config_path(PROGRAM_NAME);
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("\n");
    printf("Smart Ctrl+V clipboard manager with popup interface\n");
    printf("\n");
    printf("Options:\n");
    printf("  -V, --verbose         Enable verbose (debug) logging\n");
    printf("  -c, --config FILE     Use configuration file (default: %s)\n", config_file);
    printf("  -t, --toggle          Toggle monitoring in running instance\n");
    printf("  -h, --help            Show this help message\n");
    printf("      --version         Show version information\n");
    printf("\n");
    printf("Features:\n");
    printf("  - Single Ctrl+V: Works normally or replayed after timeout\n");
    printf("  - Ctrl+V+V: Shows popup with clipboard history\n");
    printf("  - Automatic clipboard monitoring and history saving\n");
    printf("\n");
    printf("History file: %s\n", config.history_file);
    printf("Config file: %s\n", config_file);
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

static int send_toggle_signal(void) {
    char *temp_pid_path = construct_pid_file_path();
    if (!temp_pid_path) {
        msg(LOG_ERR, "Failed to determine runtime directory");
        return 0;
    }
    
    FILE *existing_pid_file = fopen(temp_pid_path, "r");
    if (!existing_pid_file) {
        msg(LOG_ERR, "No running instance found (PID file not found)");
        free(temp_pid_path);
        return 0;
    }
    
    pid_t target_process_id;
    if (fscanf(existing_pid_file, "%d", &target_process_id) != 1) {
        msg(LOG_ERR, "Invalid PID file format");
        fclose(existing_pid_file);
        free(temp_pid_path);
        return 0;
    }
    fclose(existing_pid_file);
    free(temp_pid_path);
    
    if (kill(target_process_id, SIGUSR1) == 0) {
        msg(LOG_NOTICE, "Toggle signal sent to process %d", target_process_id);
        return 1;
    } else {
        msg(LOG_ERR, "Failed to send signal to process %d: %s", target_process_id, strerror(errno));
        return 0;
    }
}

int main(int argc, char *argv[]) {
    setup_signal_handling();

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, signal_handler);
    config_init(&config);
    
    static struct option long_options[] = {
        {"verbose",  no_argument,       0, 'V'},
        {"config",   required_argument, 0, 'c'},
        {"toggle",   no_argument,       0, 't'},
        {"help",     no_argument,       0, 'h'},
        {"version",  no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "Vc:thv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'V':
                config.verbose = 1;
                g_verbose = 1;
                break;
            case 'c':
                if (config_file) free(config_file);
                config_file = strdup(optarg);
                break;
            case 't':
                if (send_toggle_signal()) {
                    return EXIT_SUCCESS;
                } else {
                    return EXIT_FAILURE;
                }
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
    
    // Create PID file after parsing CLI arguments
    if (!create_pid_file()) {
        config_free(&config);
        return EXIT_FAILURE;
    }
    
    if (!config_file) {
        config_file = xdg_get_user_config_path(PROGRAM_NAME);
        if (!config_file) {
            msg(LOG_ERR, "Failed to determine config file path: %s", strerror(errno));
            remove_pid_file();
            config_free(&config);
            return EXIT_FAILURE;
        }
    }
    
    if (!config_parse_file(&config, config_file)) {
        msg(LOG_ERR, "Failed to parse config file");
        remove_pid_file();
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
        remove_pid_file();
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
    int max_fd = (x11_fd > signal_pipe_read_fd) ? x11_fd : signal_pipe_read_fd;
    
    while (g_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(x11_fd, &read_fds);
        FD_SET(signal_pipe_read_fd, &read_fds);
        
        struct timeval timeout = {0, 100000};
        
        int select_result = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            msg(LOG_ERR, "select() failed: %s", strerror(errno));
            break;
        }
        
        if (select_result > 0 && FD_ISSET(signal_pipe_read_fd, &read_fds)) {
            char signal_byte;
            ssize_t bytes_read = read(signal_pipe_read_fd, &signal_byte, 1);
            if (bytes_read > 0) {
                process_received_signal((int)signal_byte);
            }
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
    
    msg(LOG_NOTICE, "Main event loop finished");
    cleanup_resources();

    return EXIT_SUCCESS;
}
