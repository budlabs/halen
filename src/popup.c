#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/syslog.h>
#include <unistd.h>
#include <time.h>

#include "popup.h"
#include "halen.h"
#include "text.h"
#include "history.h"

static Display *display = NULL;
static Window root_window = 0;
static Window popup_window = 0;
static GC popup_gc = 0;
static XftDraw *xft_draw = NULL;
static XftFont *xft_font = NULL;
static XftFont *xft_font_small = NULL;
static int screen_width = 0;
static int screen_height = 0;
static int showing_popup = 0;  
static char *popup_text_buffer = NULL;
static size_t popup_text_capacity = 0;
static int font_height = 14;
static int font_ascent = 12;
static int anchor_x = -1;
static int anchor_y = -1;
static int initial_resize_done = 0;

static int ensure_popup_text_capacity(void) {
    size_t required_capacity = (config.max_lines * config.max_line_length) + 1024;
    
    if (popup_text_buffer && popup_text_capacity >= required_capacity) {
        return 1;
    }
    
    if (!popup_text_buffer) {
        popup_text_buffer = malloc(required_capacity);
        if (!popup_text_buffer) {
            msg(LOG_ERR, "Failed to allocate popup text buffer");
            return 0;
        }
        popup_text_capacity = required_capacity;
        return 1;
    }
    
    char *new_buffer = realloc(popup_text_buffer, required_capacity);
    if (!new_buffer) {
        msg(LOG_ERR, "Failed to reallocate popup text buffer");
        return 0;
    }
    
    popup_text_buffer = new_buffer;
    popup_text_capacity = required_capacity;
    return 1;
}

static char *cached_unescaped_text = NULL;
static size_t cached_unescaped_capacity = 0;

static char* get_unescaped_text_cached(const char *escaped_text) {
    size_t required_length = strlen(escaped_text) + 1;
    
    if (!cached_unescaped_text || cached_unescaped_capacity < required_length) {
        char *new_buffer = realloc(cached_unescaped_text, required_length);
        if (!new_buffer) {
            return text_unescape_content(escaped_text);
        }
        cached_unescaped_text = new_buffer;
        cached_unescaped_capacity = required_length;
    }
    
    const char *source = escaped_text;
    char *destination = cached_unescaped_text;
    
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
    *destination = '\0';
    
    return cached_unescaped_text;
}

void popup_redraw(void);
static int calculate_text_dimensions(const char *text, int *width, int *height);
static void get_mouse_position(int *mouse_x_coordinate, int *mouse_y_coordinate);
static void calculate_position_from_anchor(int reference_x, int reference_y, int window_width, int window_height, int *final_x, int *final_y);
static void resize_window(void);
void popup_redraw(void);

void popup_redraw(void) {
    if (!xft_draw || !xft_font || !popup_text_buffer) return;

    XClearWindow(display, popup_window);
    resize_window();
    
    const char *statusbar_text = "V: Next | C: Prev | X: Cut | D: Delete | Z: Cancel";
     
    int current_y_position = font_ascent + 20;
    const int line_spacing = font_height + 2;
    int left_margin = 15;

    XWindowAttributes window_attributes;
    XGetWindowAttributes(display, popup_window, &window_attributes);

    char index_count_text[32];
    int history_count = history_get_count();
    int current_index = history_get_current_index();
    
    int display_index = (current_index == -1) ? 1 : current_index + 1;
    
    snprintf(index_count_text, sizeof(index_count_text), "%d/%d", 
              display_index, history_count);
    
    XftFont *small_font = xft_font_small ? xft_font_small : xft_font;
    XGlyphInfo index_extents;
    XftTextExtentsUtf8(display, small_font, (FcChar8*)index_count_text, 
                       strlen(index_count_text), &index_extents);
    
    int index_x_position = window_attributes.width - index_extents.width - 2;
    int index_y_position = small_font->ascent + 2;
    
    XftDrawStringUtf8(xft_draw, &config.count_color, small_font, index_x_position, index_y_position,
                      (FcChar8*)index_count_text, strlen(index_count_text));
     
    char *unescaped_text = get_unescaped_text_cached(popup_text_buffer);
    if (unescaped_text) {
        char *line_start = unescaped_text;
        char *line_end;
         
        while ((line_end = strchr(line_start, '\n')) != NULL) {
            *line_end = '\0';
            XftDrawStringUtf8(xft_draw, &config.foreground, xft_font, left_margin, current_y_position,
                              (FcChar8*)line_start, strlen(line_start));
            current_y_position += line_spacing;
            line_start = line_end + 1;
        }
         
        if (*line_start) {
            XftDrawStringUtf8(xft_draw, &config.foreground, xft_font, left_margin, current_y_position,
                              (FcChar8*)line_start, strlen(line_start));
        }
    }
     
    int statusbar_y = window_attributes.height - font_height - 2;
    int separator_y = statusbar_y - font_height / 2 + 5;
    left_margin = 5;
    
    XDrawLine(display, popup_window, popup_gc, left_margin, separator_y, 
              window_attributes.width - left_margin, separator_y);

    XftFont *statusbar_font = xft_font_small ? xft_font_small : xft_font;
    int statusbar_ascent = statusbar_font->ascent;
    
    XftDrawStringUtf8(xft_draw, &config.foreground, statusbar_font, left_margin, statusbar_y + statusbar_ascent,
                      (FcChar8*)statusbar_text, strlen(statusbar_text));

    XFlush(display);
}

static void get_mouse_position(int *mouse_x_coordinate, int *mouse_y_coordinate) {
    Window root_return, child_return;
    int root_x, root_y, window_x, window_y;
    unsigned int mask_return;
    
    if (XQueryPointer(display, root_window, &root_return, &child_return,
                      &root_x, &root_y, &window_x, &window_y, &mask_return)) {
        *mouse_x_coordinate = root_x;
        *mouse_y_coordinate = root_y;
    } else {
        *mouse_x_coordinate = screen_width / 2;
        *mouse_y_coordinate = screen_height / 2;
    }
}

static void calculate_position_from_anchor(int reference_x, int reference_y, int window_width, int window_height, int *final_x, int *final_y) {
    switch (config.anchor) {
        case ANCHOR_TOP_LEFT:
            *final_x = reference_x;
            *final_y = reference_y;
            break;
        case ANCHOR_TOP_CENTER:
            *final_x = reference_x - window_width / 2;
            *final_y = reference_y;
            break;
        case ANCHOR_TOP_RIGHT:
            *final_x = reference_x - window_width;
            *final_y = reference_y;
            break;
        case ANCHOR_CENTER_LEFT:
            *final_x = reference_x;
            *final_y = reference_y - window_height / 2;
            break;
        case ANCHOR_CENTER_CENTER:
            *final_x = reference_x - window_width / 2;
            *final_y = reference_y - window_height / 2;
            break;
        case ANCHOR_CENTER_RIGHT:
            *final_x = reference_x - window_width;
            *final_y = reference_y - window_height / 2;
            break;
        case ANCHOR_BOTTOM_LEFT:
            *final_x = reference_x;
            *final_y = reference_y - window_height;
            break;
        case ANCHOR_BOTTOM_CENTER:
            *final_x = reference_x - window_width / 2;
            *final_y = reference_y - window_height;
            break;
        case ANCHOR_BOTTOM_RIGHT:
            *final_x = reference_x - window_width;
            *final_y = reference_y - window_height;
            break;
        default:
            *final_x = reference_x - window_width / 2;
            *final_y = reference_y - window_height / 2;
            break;
    }
    
    if (*final_x < config.margin_horizontal) *final_x = config.margin_horizontal;
    if (*final_y < config.margin_vertical) *final_y = config.margin_vertical;
    if (*final_x + window_width > screen_width - config.margin_horizontal) {
        *final_x = screen_width - window_width - config.margin_horizontal;
    }
    if (*final_y + window_height > screen_height - config.margin_vertical) {
        *final_y = screen_height - window_height - config.margin_vertical;
    }
}

static void resize_window(void) {
    if (!showing_popup || !popup_window || !xft_font || !popup_text_buffer) return;
    
    int calculated_width = 600;
    int calculated_height = 200;
    
    calculate_text_dimensions(popup_text_buffer, &calculated_width, &calculated_height);
    
    if (calculated_width < 400) calculated_width = 400;
    if (calculated_width > screen_width - (config.margin_horizontal * 2)) {
        calculated_width = screen_width - (config.margin_horizontal * 2);
    }
    if (calculated_height < 100) calculated_height = 100;
    if (calculated_height > screen_height - (config.margin_vertical * 2)) {
        calculated_height = screen_height - (config.margin_vertical * 2);
    }
    
    int window_x_coordinate, window_y_coordinate;
    int reference_x, reference_y;
    
    if (!initial_resize_done) {
        switch (config.position) {
            case POPUP_POSITION_MOUSE: {
                int mouse_x_coordinate, mouse_y_coordinate;
                get_mouse_position(&mouse_x_coordinate, &mouse_y_coordinate);
                reference_x = mouse_x_coordinate;
                reference_y = mouse_y_coordinate;
                break;
            }
            case POPUP_POSITION_SCREEN:
                switch (config.anchor) {
                    case ANCHOR_TOP_LEFT:
                    case ANCHOR_CENTER_LEFT:
                    case ANCHOR_BOTTOM_LEFT:
                        reference_x = 0;
                        break;
                    case ANCHOR_TOP_CENTER:
                    case ANCHOR_CENTER_CENTER:
                    case ANCHOR_BOTTOM_CENTER:
                        reference_x = screen_width / 2;
                        break;
                    case ANCHOR_TOP_RIGHT:
                    case ANCHOR_CENTER_RIGHT:
                    case ANCHOR_BOTTOM_RIGHT:
                        reference_x = screen_width;
                        break;
                }
                
                switch (config.anchor) {
                    case ANCHOR_TOP_LEFT:
                    case ANCHOR_TOP_CENTER:
                    case ANCHOR_TOP_RIGHT:
                        reference_y = 0;
                        break;
                    case ANCHOR_CENTER_LEFT:
                    case ANCHOR_CENTER_CENTER:
                    case ANCHOR_CENTER_RIGHT:
                        reference_y = screen_height / 2;
                        break;
                    case ANCHOR_BOTTOM_LEFT:
                    case ANCHOR_BOTTOM_CENTER:
                    case ANCHOR_BOTTOM_RIGHT:
                        reference_y = screen_height;
                        break;
                }
                break;
            case POPUP_POSITION_ABSOLUTE:
                reference_x = config.position_x;
                reference_y = config.position_y;
                break;
            default:
                reference_x = screen_width / 2;
                reference_y = screen_height / 2;
                break;
        }
        
        calculate_position_from_anchor(reference_x, reference_y, calculated_width, calculated_height, 
                                     &window_x_coordinate, &window_y_coordinate);

        switch (config.anchor) {
            case ANCHOR_TOP_LEFT:
                anchor_x = window_x_coordinate;
                anchor_y = window_y_coordinate;
                break;
            case ANCHOR_TOP_CENTER:
                anchor_x = window_x_coordinate + calculated_width / 2;
                anchor_y = window_y_coordinate;
                break;
            case ANCHOR_TOP_RIGHT:
                anchor_x = window_x_coordinate + calculated_width;
                anchor_y = window_y_coordinate;
                break;
            case ANCHOR_CENTER_LEFT:
                anchor_x = window_x_coordinate;
                anchor_y = window_y_coordinate + calculated_height / 2;
                break;
            case ANCHOR_CENTER_CENTER:
                anchor_x = window_x_coordinate + calculated_width / 2;
                anchor_y = window_y_coordinate + calculated_height / 2;
                break;
            case ANCHOR_CENTER_RIGHT:
                anchor_x = window_x_coordinate + calculated_width;
                anchor_y = window_y_coordinate + calculated_height / 2;
                break;
            case ANCHOR_BOTTOM_LEFT:
                anchor_x = window_x_coordinate;
                anchor_y = window_y_coordinate + calculated_height;
                break;
            case ANCHOR_BOTTOM_CENTER:
                anchor_x = window_x_coordinate + calculated_width / 2;
                anchor_y = window_y_coordinate + calculated_height;
                break;
            case ANCHOR_BOTTOM_RIGHT:
                anchor_x = window_x_coordinate + calculated_width;
                anchor_y = window_y_coordinate + calculated_height;
                break;
        }
        initial_resize_done = 1;
    } else {
        switch (config.anchor) {
            case ANCHOR_TOP_LEFT:
                window_x_coordinate = anchor_x;
                window_y_coordinate = anchor_y;
                break;
            case ANCHOR_TOP_CENTER:
                window_x_coordinate = anchor_x - calculated_width / 2;
                window_y_coordinate = anchor_y;
                break;
            case ANCHOR_TOP_RIGHT:
                window_x_coordinate = anchor_x - calculated_width;
                window_y_coordinate = anchor_y;
                break;
            case ANCHOR_CENTER_LEFT:
                window_x_coordinate = anchor_x;
                window_y_coordinate = anchor_y - calculated_height / 2;
                break;
            case ANCHOR_CENTER_CENTER:
                window_x_coordinate = anchor_x - calculated_width / 2;
                window_y_coordinate = anchor_y - calculated_height / 2;
                break;
            case ANCHOR_CENTER_RIGHT:
                window_x_coordinate = anchor_x - calculated_width;
                window_y_coordinate = anchor_y - calculated_height / 2;
                break;
            case ANCHOR_BOTTOM_LEFT:
                window_x_coordinate = anchor_x;
                window_y_coordinate = anchor_y - calculated_height;
                break;
            case ANCHOR_BOTTOM_CENTER:
                window_x_coordinate = anchor_x - calculated_width / 2;
                window_y_coordinate = anchor_y - calculated_height;
                break;
            case ANCHOR_BOTTOM_RIGHT:
                window_x_coordinate = anchor_x - calculated_width;
                window_y_coordinate = anchor_y - calculated_height;
                break;
        }
        
        if (config.position != POPUP_POSITION_SCREEN) {
            if (window_x_coordinate < config.margin_horizontal) window_x_coordinate = config.margin_horizontal;
            if (window_y_coordinate < config.margin_vertical) window_y_coordinate = config.margin_vertical;
            if (window_x_coordinate + calculated_width > screen_width) {
                window_x_coordinate = screen_width - calculated_width - config.margin_horizontal;
            }
            if (window_y_coordinate + calculated_height > screen_height) {
                window_y_coordinate = screen_height - calculated_height - config.margin_vertical;
            }
        }
    }
    
    XMoveResizeWindow(display, popup_window, window_x_coordinate, window_y_coordinate, 
                      calculated_width, calculated_height);
}

static int calculate_text_dimensions(const char *text, int *width, int *height) {
    if (!xft_font || !text) return 0;
    
    int max_width = 0;
    int total_height = font_height + 20;
    
    char *unescaped_text = get_unescaped_text_cached(text);
    if (!unescaped_text) return 0;
    
    char *line_start = unescaped_text;
    char *line_end;
    
    while ((line_end = strchr(line_start, '\n')) != NULL) {
        *line_end = '\0';
        XGlyphInfo extents;
        XftTextExtentsUtf8(display, xft_font, (FcChar8*)line_start, strlen(line_start), &extents);
        if (extents.width > max_width) max_width = extents.width;
        total_height += font_height + 2;
        line_start = line_end + 1;
    }
    
    if (*line_start) {
        XGlyphInfo extents;
        XftTextExtentsUtf8(display, xft_font, (FcChar8*)line_start, strlen(line_start), &extents);
        if (extents.width > max_width) max_width = extents.width;
        total_height += font_height + 2;
    }
    
    total_height += font_height + 30;
    
    *width = max_width + 40;
    *height = total_height;
    
    return 1;
}

int popup_init(Display *display_connection, Window root_window_param, int screen_width_pixels, int screen_height_pixels) {
    display = display_connection;
    root_window = root_window_param;
    screen_width = screen_width_pixels;
    screen_height = screen_height_pixels;
    
    popup_text_buffer = NULL;
    popup_text_capacity = 0;
    
    if (!FcInit()) {
        msg(LOG_ERR, "Failed to initialize fontconfig");
        return 0;
    }
    
    FcResult font_result;

    FcPattern *small_font_pattern = FcNameParse((FcChar8*)config.font);
    FcPatternAddDouble(small_font_pattern, FC_SIZE, (double)config.font_size * 0.85);
    
    FcConfigSubstitute(NULL, small_font_pattern, FcMatchPattern);
    FcDefaultSubstitute(small_font_pattern);
    
    FcPattern *matched_small_font = FcFontMatch(NULL, small_font_pattern, &font_result);
    FcPatternDestroy(small_font_pattern);
    
    if (matched_small_font) {
        xft_font_small = XftFontOpenPattern(display, matched_small_font);
    }
    
    FcPattern *font_pattern = FcNameParse((FcChar8*)config.font);
    FcPatternAddDouble(font_pattern, FC_SIZE, (double)config.font_size);
    
    FcConfigSubstitute(NULL, font_pattern, FcMatchPattern);
    FcDefaultSubstitute(font_pattern);
    
    FcPattern *matched_font = FcFontMatch(NULL, font_pattern, &font_result);
    FcPatternDestroy(font_pattern);
    
    if (!matched_font) {
        msg(LOG_ERR, "Failed to match font pattern");
        return 0;
    }
    
    xft_font = XftFontOpenPattern(display, matched_font);
    if (!xft_font) {
        msg(LOG_ERR, "Failed to load font");
        FcPatternDestroy(matched_font);
        return 0;
    }
    
    font_height = xft_font->height;
    font_ascent = xft_font->ascent;
    
    msg(LOG_NOTICE, "Popup system initialized: %dx%d", 
        screen_width_pixels, screen_height_pixels);
    return 1;
}

int popup_show(const char *text) {
    if (showing_popup) return 1;
    
    if (!display || !root_window) return 0;
    
    if (!ensure_popup_text_capacity()) {
        return 0;
    }
    
    strncpy(popup_text_buffer, text, popup_text_capacity - 1);
    popup_text_buffer[popup_text_capacity - 1] = '\0';
    
    XSetWindowAttributes window_attributes;
    window_attributes.background_pixel = config.background.pixel;
    window_attributes.border_pixel = config.foreground.pixel;
    window_attributes.override_redirect = True;
    
    int popup_width = 600;
    int popup_height = 200;
    int popup_x = (screen_width - popup_width) / 2;
    int popup_y = (screen_height - popup_height) / 2;
    
    popup_window = XCreateWindow(display, root_window,
                                popup_x, popup_y, popup_width, popup_height,
                                2, CopyFromParent, InputOutput, CopyFromParent,
                                CWBackPixel | CWBorderPixel | CWOverrideRedirect,
                                &window_attributes);
    
    if (!popup_window) return 0;
    
    xft_draw = XftDrawCreate(display, popup_window,
                            DefaultVisual(display, DefaultScreen(display)),
                            DefaultColormap(display, DefaultScreen(display)));
    if (!xft_draw) {
        XDestroyWindow(display, popup_window);
        popup_window = 0;
        return 0;
    }
    
    popup_gc = XCreateGC(display, popup_window, 0, NULL);
    
    XSelectInput(display, popup_window, ExposureMask);
    XMapWindow(display, popup_window);
    XRaiseWindow(display, popup_window);
    
    showing_popup = 1;
    
    popup_redraw();
    
    return 1;
}

void popup_hide(void) {
    if (!showing_popup) {
        msg(LOG_DEBUG, "popup_hide: popup was not showing");
        return;
    }
    
    msg(LOG_DEBUG, "popup_hide: starting cleanup, window=%lu", popup_window);
    
    if (popup_window) {
        if (xft_draw) {
            XftDrawDestroy(xft_draw);
            xft_draw = NULL;
            msg(LOG_DEBUG, "popup_hide: destroyed xft_draw");
        }
        if (popup_gc) {
            XFreeGC(display, popup_gc);
            popup_gc = 0;
            msg(LOG_DEBUG, "popup_hide: freed popup_gc");
        }
        
        // Force unmapping before destroying
        XUnmapWindow(display, popup_window);
        XSync(display, False);
        msg(LOG_DEBUG, "popup_hide: unmapped window");
        
        XDestroyWindow(display, popup_window);
        XSync(display, False);
        msg(LOG_DEBUG, "popup_hide: destroyed window");
        
        popup_window = 0;
    }
    
    showing_popup = 0;
    initial_resize_done = 0;
    anchor_x = -1;
    anchor_y = -1;
    
    // Force a final flush to ensure all X11 commands are processed
    XFlush(display);
    msg(LOG_DEBUG, "popup_hide: cleanup completed");
}

void popup_cleanup(void) {
    popup_hide();
    
    if (xft_font) {
        XftFontClose(display, xft_font);
        xft_font = NULL;
    }

    if (xft_font_small) {
        XftFontClose(display, xft_font_small);
        xft_font_small = NULL;
    }
    
    if (popup_text_buffer) {
        free(popup_text_buffer);
        popup_text_buffer = NULL;
        popup_text_capacity = 0;
    }
    
    if (cached_unescaped_text) {
        free(cached_unescaped_text);
        cached_unescaped_text = NULL;
        cached_unescaped_capacity = 0;
    }
    
    FcFini();
}

int popup_is_showing(void) {
    return showing_popup;
}

void popup_handle_expose(XExposeEvent *expose_event) {
    (void)expose_event;
    if (showing_popup && popup_window) {
        popup_redraw();
    }
}

void popup_update_text(const char *new_text) {
    if (!new_text) return;
    
    if (!ensure_popup_text_capacity()) {
        return;
    }
    
    strncpy(popup_text_buffer, new_text, popup_text_capacity - 1);
    popup_text_buffer[popup_text_capacity - 1] = '\0';
    
    if (showing_popup) {
        popup_redraw();
    }
}
