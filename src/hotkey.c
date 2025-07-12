#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/record.h>
#include <X11/Xproto.h>
#include <X11/XKBlib.h>
#include <sys/syslog.h>

#include "halen.h"
#include "hotkey.h"
#include "popup.h"

static Display *record_display = NULL;
static hotkey_callback_t main_callback = NULL;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static int is_monitoring = 0;
static int monitoring_enabled = 1;
static pthread_t xrecord_thread;
static PopupAction popup_action = POPUP_ACTION_NONE;
static int ctrl_pressed = 0;
static volatile int ctrl_v_count = 0;
static volatile int replaying_paste = 0;  // Volatile flag for XRecord loop
static XRecordContext record_context = 0;
static volatile int ctrl_v_blocked = 0;
static volatile int pending_ctrl_v = 0;
static nav_direction_t current_nav_direction = NAV_DIRECTION_NEXT;

static void* xrecord_thread_func(void* arg);
static void record_callback(XPointer closure, XRecordInterceptData *data);
static int setup_key_blocking(void);
static void reset_state(void);

static int setup_key_blocking(void) {
    msg(LOG_NOTICE, "Setting up Ctrl+V key blocking");
    
    KeyCode v_keycode = XKeysymToKeycode(g_display, XK_v);
    
    if (v_keycode == 0) {
        msg(LOG_ERR, "Failed to get V keycode");
        return 0;
    }
    
    unsigned int modifier_combinations[] = {
        ControlMask,
        ControlMask | LockMask,
        ControlMask | Mod2Mask,
        ControlMask | LockMask | Mod2Mask
    };
    
    for (int i = 0; i < 4; i++) {
        XGrabKey(g_display, v_keycode, modifier_combinations[i], g_root_window,
                 True, GrabModeSync, GrabModeAsync);
    }
    
    XFlush(g_display);
    msg(LOG_NOTICE, "Ctrl+V key combinations blocked successfully");
    
    return 1;
}

static void grab_navigation_keys(void) {
    KeyCode c_keycode = XKeysymToKeycode(g_display, XK_c);
    KeyCode x_keycode = XKeysymToKeycode(g_display, XK_x);
    KeyCode z_keycode = XKeysymToKeycode(g_display, XK_z);
    KeyCode d_keycode = XKeysymToKeycode(g_display, XK_d);
    
    if (c_keycode == 0) {
        msg(LOG_WARNING, "Failed to get C keycode for grabbing");
        return;
    }
    
    if (x_keycode == 0) {
        msg(LOG_WARNING, "Failed to get X keycode for grabbing");
        return;
    }
    
    if (z_keycode == 0) {
        msg(LOG_WARNING, "Failed to get Z keycode for grabbing");
        return;
    }
    
    if (d_keycode == 0) {
        msg(LOG_WARNING, "Failed to get D keycode for grabbing");
        return;
    }
    
    unsigned int modifier_combinations[] = {
        ControlMask,
        ControlMask | LockMask,
        ControlMask | Mod2Mask,
        ControlMask | LockMask | Mod2Mask
    };
    
    for (int i = 0; i < 4; i++) {
        XGrabKey(g_display, c_keycode, modifier_combinations[i], g_root_window,
                 True, GrabModeSync, GrabModeAsync);
        XGrabKey(g_display, x_keycode, modifier_combinations[i], g_root_window,
                 True, GrabModeSync, GrabModeAsync);
        XGrabKey(g_display, z_keycode, modifier_combinations[i], g_root_window,
                 True, GrabModeSync, GrabModeAsync);
        XGrabKey(g_display, d_keycode, modifier_combinations[i], g_root_window,
                 True, GrabModeSync, GrabModeAsync);
    }
    XFlush(g_display);
    msg(LOG_DEBUG, "Ctrl+C, Ctrl+X, Ctrl+Z, and Ctrl+D grabbed for popup navigation");
}

static void ungrab_navigation_keys(void) {
    KeyCode c_keycode = XKeysymToKeycode(g_display, XK_c);
    KeyCode x_keycode = XKeysymToKeycode(g_display, XK_x);
    KeyCode z_keycode = XKeysymToKeycode(g_display, XK_z);
    KeyCode d_keycode = XKeysymToKeycode(g_display, XK_d);
    
    if (c_keycode == 0 || x_keycode == 0 || z_keycode == 0 || d_keycode == 0) {
        msg(LOG_WARNING, "Failed to get keycode for ungrabbing");
        return;
    }
    
    unsigned int modifier_combinations[] = {
        ControlMask,
        ControlMask | LockMask,
        ControlMask | Mod2Mask,
        ControlMask | LockMask | Mod2Mask
    };
    
    for (int i = 0; i < 4; i++) {
        XUngrabKey(g_display, c_keycode, modifier_combinations[i], g_root_window);
        XUngrabKey(g_display, x_keycode, modifier_combinations[i], g_root_window);
        XUngrabKey(g_display, z_keycode, modifier_combinations[i], g_root_window);
        XUngrabKey(g_display, d_keycode, modifier_combinations[i], g_root_window);
    }
    XFlush(g_display);
    msg(LOG_DEBUG, "Ctrl+C, Ctrl+X, Ctrl+Z, and Ctrl+D ungrabbed - normal keys restored");
}

void hotkey_toggle_monitoring(void) {
    pthread_mutex_lock(&state_mutex);
    
    monitoring_enabled = !monitoring_enabled;
    
    if (monitoring_enabled) {
        msg(LOG_NOTICE, "Enabling hotkey monitoring");
        
        // Re-grab keys
        if (!setup_key_blocking()) {
            msg(LOG_ERR, "Failed to re-enable key blocking");
        }
        
        // Reset state when re-enabling
        reset_state();
        
    } else {
        msg(LOG_NOTICE, "Disabling hotkey monitoring");
        
        // Ungrab all keys
        KeyCode v_keycode = XKeysymToKeycode(g_display, XK_v);
        if (v_keycode != 0) {
            unsigned int modifier_combinations[] = {
                ControlMask,
                ControlMask | LockMask,
                ControlMask | Mod2Mask,
                ControlMask | LockMask | Mod2Mask
            };
            
            for (int i = 0; i < 4; i++) {
                XUngrabKey(g_display, v_keycode, modifier_combinations[i], g_root_window);
            }
        }
        
        // Ungrab navigation keys if they were grabbed
        if (ctrl_v_count >= 2) {
            ungrab_navigation_keys();
        }
        
        // Reset state and hide popup
        reset_state();
        XFlush(g_display);
    }
    
    pthread_mutex_unlock(&state_mutex);
}

void hotkey_handle_xevent(XEvent *event) {
    if (!monitoring_enabled) {
        XAllowEvents(g_display, SyncKeyboard, event->xkey.time);
        XFlush(g_display);
        return;
    }
    
    if (replaying_paste) {
        XAllowEvents(g_display, SyncKeyboard, event->xkey.time);
        XFlush(g_display);
        return;
    }
    
    if (event->type == KeyPress || event->type == KeyRelease) {
        int is_press = (event->type == KeyPress);
        KeySym keysym = XkbKeycodeToKeysym(g_display, event->xkey.keycode, 0, 0);
        
        if (is_press && keysym == XK_v) {
            pthread_mutex_lock(&state_mutex);
            
            ctrl_v_count++;
            msg(LOG_NOTICE, "Blocked Ctrl+V (count: %d) - waiting for Control release", ctrl_v_count);
            
            if (ctrl_v_count == 1) {
                pending_ctrl_v = 1;
                ctrl_v_blocked = 1;
                popup_action = POPUP_ACTION_NONE;
                msg(LOG_DEBUG, "First Ctrl+V - will replay on Control release");
            } else if (ctrl_v_count == 2) {
                pending_ctrl_v = 0;
                ctrl_v_blocked = 0;
                popup_action = POPUP_ACTION_NEXT;
                msg(LOG_DEBUG, "Second Ctrl+V - showing popup, action=NEXT");
                
                grab_navigation_keys();
                
                if (main_callback) {
                    main_callback("double_paste");
                }
            } else if (ctrl_v_count >= 3) {
                // v pressed while popup is showing
                popup_action = POPUP_ACTION_NEXT;
                msg(LOG_DEBUG, "Additional Ctrl+V (count: %d) - action=NEXT", ctrl_v_count);
                current_nav_direction = NAV_DIRECTION_NEXT;  // Explicitly set to NEXT
                if (main_callback) {
                    main_callback("cb_clipboard_next");
                }
            }
            
            pthread_mutex_unlock(&state_mutex);
            
        } else if (is_press && keysym == XK_c && ctrl_v_count >= 2) {
            // This is a blocked Ctrl+C press (only happens when popup is showing)
            pthread_mutex_lock(&state_mutex);
            
            popup_action = POPUP_ACTION_PREV;
            current_nav_direction = NAV_DIRECTION_PREV;
            msg(LOG_DEBUG, "Blocked Ctrl+C - action=PREV");
            
            if (main_callback) {
                main_callback("cb_clipboard_prev");
            }
            
            pthread_mutex_unlock(&state_mutex);
            
        } else if (is_press && keysym == XK_x && ctrl_v_count >= 2) {

            pthread_mutex_lock(&state_mutex);
            popup_action = POPUP_ACTION_CUT;
            msg(LOG_DEBUG, "Blocked Ctrl+X - action=CUT");
            
            if (main_callback) {
                main_callback("cb_clipboard_cut");
            }
            
            ungrab_navigation_keys();
            reset_state();
            pthread_mutex_unlock(&state_mutex);
        } else if (is_press && keysym == XK_z && ctrl_v_count >= 2) {
            // This is a blocked Ctrl+Z press (only happens when popup is showing)
            pthread_mutex_lock(&state_mutex);

            msg(LOG_DEBUG, "Blocked Ctrl+Z - action=CANCEL (close popup and reset counter)");
            popup_action = POPUP_ACTION_CANCEL;
            if (main_callback) {
                main_callback("cb_clipboard_cancel");
            }
            
            // Immediately close popup and reset counter as per specification  
            msg(LOG_DEBUG, "Closing popup and resetting counter for CANCEL action");
            
            ungrab_navigation_keys();
            reset_state();
            pthread_mutex_unlock(&state_mutex);

        } else if (is_press && (keysym == XK_d || keysym == XK_D) && ctrl_v_count >= 2) {

            pthread_mutex_lock(&state_mutex);
            
            msg(LOG_DEBUG, "Blocked Ctrl+D - action=DELETE (delete current entry)");
            popup_action = POPUP_ACTION_DELETE;

            if (main_callback) {
                main_callback("cb_clipboard_delete");
            }
            
            pthread_mutex_unlock(&state_mutex);
        }
        
        XAllowEvents(g_display, SyncKeyboard, event->xkey.time);
        XFlush(g_display);
    }
}

// this is only used to track the Control key state
static void record_callback(XPointer closure, XRecordInterceptData *data) {
    (void)closure;
    
    if (!monitoring_enabled) {
        goto end;
    }
    
    if (replaying_paste) {
        goto end;
    }
    
    if (!data || data->category != XRecordFromServer || data->data_len < 8) {
        goto end;
    }
    
    unsigned char *event_data = (unsigned char *)data->data;
    if (!event_data) {
        goto end;
    }
    
    int event_type = event_data[0] & 0x7F;
    
    if (event_type == KeyPress || event_type == KeyRelease) {
        int is_press = (event_type == KeyPress);
        KeyCode keycode = event_data[1];
        
        if (!g_display) {
            goto end;
        }
        
        KeySym keysym = XkbKeycodeToKeysym(g_display, keycode, 0, 0);
        
        // ONLY track Control key state
        if (keysym == XK_Control_L || keysym == XK_Control_R) {
            pthread_mutex_lock(&state_mutex);
            
            if (is_press) {
                ctrl_pressed = 1;
                msg(LOG_DEBUG, "Control pressed");
            } else {
                ctrl_pressed = 0;
                msg(LOG_DEBUG, "Control released");
                
                if (ctrl_v_count == 0) {
                    msg(LOG_DEBUG, "State already cleaned up - ignoring Control release");
                    
                    if (popup_action == POPUP_ACTION_CUT && main_callback) {
                        main_callback("control_released");
                        popup_action = POPUP_ACTION_NONE;
                    }
                    
                    pthread_mutex_unlock(&state_mutex);
                    goto end;
                }
                
                if (ctrl_v_count == 1 && pending_ctrl_v && ctrl_v_blocked) {
                    msg(LOG_NOTICE, "Control released after single Ctrl+V - replay paste");
                    hotkey_perform_paste();
                    
                    if (main_callback) {
                        main_callback("single_paste");
                    }

                } else if (ctrl_v_count >= 2) {
                    msg(LOG_NOTICE, "Control released on popup");
                    ungrab_navigation_keys();
                }
                
                // Call the control_released callback BEFORE resetting state
                // This allows the callback to access popup_action before it's reset
                if (main_callback) {
                    main_callback("control_released");
                }
                
                reset_state();
            }
            
            pthread_mutex_unlock(&state_mutex);
        }
    }
    
end:
    if (data) {
        XRecordFreeData(data);
    }
}

static void reset_state(void) {
    msg(LOG_DEBUG, "Resetting all state: count=%d -> 0", ctrl_v_count);
    ctrl_v_count = 0;
    pending_ctrl_v = 0;
    ctrl_v_blocked = 0;
    popup_action = POPUP_ACTION_NONE;
    
    if (popup_is_showing()) {
        popup_hide();
    }
}

void hotkey_perform_paste(void) {
    if (!g_display) {
        msg(LOG_ERR, "hotkey_perform_paste: g_display is NULL");
        return;
    }

    replaying_paste = 1;
    msg(LOG_NOTICE, "Replaying Ctrl+V for selected clipboard entry - ALL monitoring paused");
    
    KeyCode v_keycode = XKeysymToKeycode(g_display, XK_v);
    KeyCode ctrl_keycode = XKeysymToKeycode(g_display, XK_Control_L);
    
    if (v_keycode == 0) {
        msg(LOG_WARNING, "Failed to get V keycode");
        replaying_paste = 0;
        return;
    }
    
    // Temporarily ungrab Ctrl+V to allow our fake event through
    unsigned int modifier_combinations[] = {
        ControlMask,
        ControlMask | LockMask,
        ControlMask | Mod2Mask,
        ControlMask | LockMask | Mod2Mask
    };
    
    msg(LOG_DEBUG, "Ungrabbing Ctrl+V for replay");
    for (int i = 0; i < 4; i++) {
        XUngrabKey(g_display, v_keycode, modifier_combinations[i], g_root_window);
    }
    XFlush(g_display);
    
    Window focus_window;
    int revert_to;
    XGetInputFocus(g_display, &focus_window, &revert_to);
    
    if (focus_window == None || focus_window == DefaultRootWindow(g_display)) {
        focus_window = g_root_window;
    }
    
    msg(LOG_DEBUG, "Sending fake Ctrl+V to window: %lu", focus_window);
    
    XTestFakeKeyEvent(g_display, ctrl_keycode, True, CurrentTime);
    XSync(g_display, False);
    
    XTestFakeKeyEvent(g_display, v_keycode, True, CurrentTime);
    XSync(g_display, False);
    
    XTestFakeKeyEvent(g_display, v_keycode, False, CurrentTime);
    XSync(g_display, False);
    
    XTestFakeKeyEvent(g_display, ctrl_keycode, False, CurrentTime);
    XSync(g_display, False);
    
    usleep(100000); // 100ms
    
    msg(LOG_DEBUG, "Re-grabbing Ctrl+V");
    for (int i = 0; i < 4; i++) {
        XGrabKey(g_display, v_keycode, modifier_combinations[i], g_root_window,
                 True, GrabModeSync, GrabModeAsync);
    }
    XFlush(g_display);
    
    replaying_paste = 0;
    msg(LOG_NOTICE, "Ctrl+V replay completed for selected entry - monitoring resumed");
}

int hotkey_init(hotkey_callback_t callback) {
    main_callback = callback;
    
    pthread_mutex_init(&state_mutex, NULL);
    
    ctrl_v_count = 0;
    replaying_paste = 0;
    ctrl_pressed = 0;
    pending_ctrl_v = 0;
    ctrl_v_blocked = 0;
    
    if (!setup_key_blocking()) {
        msg(LOG_ERR, "Failed to set up key blocking");
        return 0;
    }

    if (pthread_create(&xrecord_thread, NULL, xrecord_thread_func, NULL) != 0) {
        msg(LOG_ERR, "Failed to create XRecord thread");
        return 0;
    }
    
    msg(LOG_NOTICE, "Hotkey system initialized with blocking + XRecord");
    return 1;
}


static void* xrecord_thread_func(void* arg) {
    (void)arg;
    
    msg(LOG_NOTICE, "XRecord thread started");
    
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    
    usleep(200000); // 200ms delay
    
    record_display = XOpenDisplay(NULL);
    if (!record_display) {
        msg(LOG_ERR, "Cannot open display for XRecord");
        return NULL;
    }
    
    int major, minor;
    if (!XRecordQueryVersion(record_display, &major, &minor)) {
        msg(LOG_ERR, "XRecord extension not available");
        XCloseDisplay(record_display);
        record_display = NULL;
        return NULL;
    }
    
    XRecordRange *range = XRecordAllocRange();
    if (!range) {
        msg(LOG_ERR, "Failed to allocate XRecord range");
        XCloseDisplay(record_display);
        record_display = NULL;
        return NULL;
    }
    
    range->device_events.first = KeyPress;
    range->device_events.last = KeyRelease;
    
    XRecordClientSpec client_spec = XRecordAllClients;
    
    record_context = XRecordCreateContext(record_display, 0, &client_spec, 1, &range, 1);
    
    if (!record_context) {
        msg(LOG_ERR, "Failed to create XRecord context");
        XFree(range);
        XCloseDisplay(record_display);
        record_display = NULL;
        return NULL;
    }
    
    XFree(range);
    
    msg(LOG_NOTICE, "Starting XRecord monitoring...");
    is_monitoring = 1;
    
    pthread_testcancel();
    
    if (!XRecordEnableContext(record_display, record_context, record_callback, NULL)) {
        msg(LOG_ERR, "XRecordEnableContext failed");
    }
    
    is_monitoring = 0;
    if (record_context) {
        XRecordFreeContext(record_display, record_context);
        record_context = 0;
    }
    XCloseDisplay(record_display);
    record_display = NULL;
    
    msg(LOG_NOTICE, "XRecord monitoring ended");
    return NULL;
}

static void hotkey_stop_monitoring(void) {
    msg(LOG_NOTICE, "Stopping hotkey monitoring...");
    
    if (is_monitoring) {
        msg(LOG_DEBUG, "Canceling XRecord thread...");
        
        pthread_cancel(xrecord_thread);
        usleep(100000); // 100ms
        
        // Force join with timeout
        struct timespec timeout_spec;
        clock_gettime(CLOCK_REALTIME, &timeout_spec);
        timeout_spec.tv_sec += 1; // 1 second timeout
        
        void *result;
        int join_result = pthread_timedjoin_np(xrecord_thread, &result, &timeout_spec);
        
        if (join_result == 0) {
            if (result == PTHREAD_CANCELED) {
                msg(LOG_DEBUG, "XRecord thread was cancelled");
            } else {
                msg(LOG_DEBUG, "XRecord thread exited normally");
            }
        } else if (join_result == ETIMEDOUT) {
            msg(LOG_WARNING, "XRecord thread join timed out - detaching");
            pthread_detach(xrecord_thread);
        } else {
            msg(LOG_ERR, "Failed to join XRecord thread: %d", join_result);
        }
    }
    
    is_monitoring = 0;
    
    if (record_display) {
        record_display = NULL;
    }
}

void hotkey_cleanup(void) {
    msg(LOG_DEBUG, "Cleaning up hotkey system...");
    
    if (is_monitoring) {
        hotkey_stop_monitoring();
    }
    
    pthread_mutex_destroy(&state_mutex);
    msg(LOG_DEBUG, "Hotkey cleanup completed");
}

PopupAction hotkey_get_popup_action(void) {
    return popup_action;
}

nav_direction_t hotkey_get_nav_direction(void) {
    return current_nav_direction;
}

void hotkey_reset_nav_direction(void) {
    current_nav_direction = NAV_DIRECTION_NEXT;
}
