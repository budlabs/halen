#define _GNU_SOURCE
#include "parser.h"
#include "xdg.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "history.h"
#include <linux/limits.h>
#include <sys/stat.h>
#include "halen.h"

// Initialize config with defaults
void config_init(config_t *config) {
    config->verbose = 0;
    config->logfile = NULL;
    char *default_path = history_get_default_file_path();
    config->history_file = strdup(default_path);
    config->timeout = 2;
    config->max_lines = 10;
    config->max_line_length = 80;
    config->font = strdup("monospace");
    config->font_size = 12;
    config->background = 0xFFFFFF;
    config->foreground = 0x000000;
    config->count_color = 0xFF0000;
    config->position = POPUP_POSITION_MOUSE;
    config->position_x = 0;
    config->position_y = 0;
    config->anchor = ANCHOR_CENTER_CENTER;
    config->margin_vertical = 10;
    config->margin_horizontal = 10;
    
    char *cache_directory = xdg_get_directory(XDG_CACHE_HOME);
    if (cache_directory) {
        size_t overflow_path_length = strlen(cache_directory) + strlen("/halen/overflow") + 1;
        config->overflow_directory = malloc(overflow_path_length);
        if (config->overflow_directory) {
            snprintf(config->overflow_directory, overflow_path_length, "%s/halen/overflow", cache_directory);
            
            char halen_directory[PATH_MAX];
            snprintf(halen_directory, sizeof(halen_directory), "%s/halen", cache_directory);
            
            if (mkdir(halen_directory, 0755) == 0) {
                msg(LOG_DEBUG, "Created halen directory: %s", halen_directory);
            } else if (errno != EEXIST) {
                msg(LOG_WARNING, "Failed to create halen directory: %s", halen_directory);
            }
             
             if (mkdir(config->overflow_directory, 0755) == 0) {
                 msg(LOG_DEBUG, "Created overflow directory: %s", config->overflow_directory);
            } else if (errno != EEXIST) {
                 msg(LOG_WARNING, "Failed to create overflow directory: %s", config->overflow_directory);
            } else {
                msg(LOG_DEBUG, "Overflow directory already exists: %s", config->overflow_directory);
             }
          }
          free(cache_directory);
      } else {
         config->overflow_directory = NULL;
     }
}

// Parse configuration file
int config_parse_file(config_t *config, const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        // Config file is optional, so don't error if it doesn't exist
        if (g_verbose) {
            msg(LOG_DEBUG, "Config file '%s' not found, using defaults", filename);
        }
        return 1;
    }
    
    msg(LOG_NOTICE, "Reading config file: %s", filename);
    
    char line[512];
    int line_num = 0;
    
    while (fgets(line, sizeof(line), f)) {
        line_num++;
        
        // Remove trailing newline and carriage return
        line[strcspn(line, "\r\n")] = 0;
        
        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        
        // Find the equals sign
        char *equals = strchr(line, '=');
        if (!equals) {
            msg(LOG_WARNING, "Invalid config line %d: missing '=' in '%s'", line_num, line);
            continue;
        }
        
        // Split into key and value
        *equals = '\0';
        char *key = line;
        char *value = equals + 1;
        
        // Trim whitespace from key
        while (*key == ' ' || *key == '\t') key++;
        char *key_end = key + strlen(key) - 1;
        while (key_end > key && (*key_end == ' ' || *key_end == '\t')) {
            *key_end = '\0';
            key_end--;
        }
        
        // Trim whitespace from value
        while (*value == ' ' || *value == '\t') value++;
        char *value_end = value + strlen(value) - 1;
        while (value_end > value && (*value_end == ' ' || *value_end == '\t')) {
            *value_end = '\0';
            value_end--;
        }
        
        // Process config options
        if (strcmp(key, "verbose") == 0) {
            config->verbose = (strcmp(value, "true") == 0 || 
                              strcmp(value, "1") == 0 || 
                              strcmp(value, "yes") == 0 ||
                              strcmp(value, "on") == 0);
            msg(LOG_DEBUG, "Config: verbose = %s", config->verbose ? "true" : "false");
            
        } else if (strcmp(key, "logfile") == 0) {
            if (config->logfile) {
                free(config->logfile);
            }
            config->logfile = strdup(value);
            msg(LOG_DEBUG, "Config: logfile = %s", config->logfile);
        } else if (strcmp(key, "font") == 0) {
            if (config->font) {
                free(config->font);
            }
            config->font = strdup(value);
            msg(LOG_DEBUG, "Config: font = %s", config->font);

        } else if (strcmp(key, "font_size") == 0) {
            // Validate font size
            char *endptr;
            long font_size_val = strtol(value, &endptr, 10);
            if (*endptr == '\0' && font_size_val > 0 && font_size_val <= 72) {
                config->font_size = (int)font_size_val;
                msg(LOG_DEBUG, "Config: font_size = %d", config->font_size);
            } else {
                msg(LOG_WARNING, "Invalid font_size value '%s' on line %d (must be 1-72)", value, line_num);
            }
            
        } else if (strcmp(key, "font_size") == 0) {
            char *endptr;
            long font_size_val = strtol(value, &endptr, 10);
            if (*endptr == '\0' && font_size_val > 0 && font_size_val <= 72) {
                config->font_size = (int)font_size_val;
                msg(LOG_DEBUG, "Config: font_size = %d", config->font_size);
            } else {
                msg(LOG_WARNING, "Invalid font_size value '%s' on line %d (must be 1-72)", value, line_num);
            }
            
        } else if (strcmp(key, "max_lines") == 0) {
            char *endptr;
            long max_lines_val = strtol(value, &endptr, 10);
            if (*endptr == '\0' && max_lines_val > 0 && max_lines_val <= 100) {
                config->max_lines = (int)max_lines_val;
                msg(LOG_DEBUG, "Config: max_lines = %d", config->max_lines);
            } else {
                msg(LOG_WARNING, "Invalid max_lines value '%s' on line %d (must be 1-100)", value, line_num);
            }
            
        } else if (strcmp(key, "max_line_length") == 0) {
            char *endptr;
            long max_line_length_val = strtol(value, &endptr, 10);
            if (*endptr == '\0' && max_line_length_val > 0 && max_line_length_val <= 500) {
                config->max_line_length = (int)max_line_length_val;
                msg(LOG_DEBUG, "Config: max_line_length = %d", config->max_line_length);
            } else {
                msg(LOG_WARNING, "Invalid max_line_length value '%s' on line %d (must be 1-500)", value, line_num);
            }
            
        } else if (strcmp(key, "history_file") == 0) {
            if (config->history_file) {
                free(config->history_file);
            }
            config->history_file = strdup(value);
            msg(LOG_DEBUG, "Config: history_file = %s", config->history_file);
            
        } else if (strcmp(key, "timeout") == 0) {
            char *endptr;
            long timeout_val = strtol(value, &endptr, 10);
            if (*endptr == '\0' && timeout_val > 0 && timeout_val <= 60) {
                config->timeout = (int)timeout_val;
                msg(LOG_DEBUG, "Config: timeout = %d seconds", config->timeout);
            } else {
                msg(LOG_WARNING, "Invalid timeout value '%s' on line %d (must be 1-60)", value, line_num);
            }
            
        } else if (strcmp(key, "background") == 0) {
            if (value[0] == '#' && strlen(value) == 7) {
                char *endptr;
                unsigned long color_value = strtoul(value + 1, &endptr, 16);
                if (*endptr == '\0' && color_value <= 0xFFFFFF) {
                    config->background = (unsigned int)color_value;
                    msg(LOG_DEBUG, "Config: background = 0x%06X", config->background);
                } else {
                    msg(LOG_WARNING, "Invalid background color '%s' on line %d (must be #RRGGBB)", value, line_num);
                }
            } else {
                msg(LOG_WARNING, "Invalid background color format '%s' on line %d (must be #RRGGBB)", value, line_num);
            }

        } else if (strcmp(key, "count_color") == 0) {
            if (value[0] == '#' && strlen(value) == 7) {
                char *endptr;
                unsigned long color_value = strtoul(value + 1, &endptr, 16);
                if (*endptr == '\0' && color_value <= 0xFFFFFF) {
                    config->count_color = (unsigned int)color_value;
                    msg(LOG_DEBUG, "Config: count_color = 0x%06X", config->count_color);
                } else {
                    msg(LOG_WARNING, "Invalid count_color color '%s' on line %d (must be #RRGGBB)", value, line_num);
                }
            } else {
                msg(LOG_WARNING, "Invalid count_color color format '%s' on line %d (must be #RRGGBB)", value, line_num);
            }
            
        } else if (strcmp(key, "foreground") == 0) {
            if (value[0] == '#' && strlen(value) == 7) {
                char *endptr;
                unsigned long color_value = strtoul(value + 1, &endptr, 16);
                if (*endptr == '\0' && color_value <= 0xFFFFFF) {
                    config->foreground = (unsigned int)color_value;
                    msg(LOG_DEBUG, "Config: foreground = 0x%06X", config->foreground);
                } else {
                    msg(LOG_WARNING, "Invalid foreground color '%s' on line %d (must be #RRGGBB)", value, line_num);
                }
            } else {
                msg(LOG_WARNING, "Invalid foreground color format '%s' on line %d (must be #RRGGBB)", value, line_num);
            }
            
        } else if (strcmp(key, "position") == 0) {
            if (strcasecmp(value, "mouse") == 0) {
                config->position = POPUP_POSITION_MOUSE;
                config->position_x = 0;
                config->position_y = 0;
                msg(LOG_DEBUG, "Config: position = MOUSE");
            } else if (strcasecmp(value, "screen") == 0) {
                config->position = POPUP_POSITION_SCREEN;
                config->position_x = 0;
                config->position_y = 0;
                msg(LOG_DEBUG, "Config: position = SCREEN");
            } else {
                char *colon = strchr(value, ':');
                if (colon) {
                    *colon = '\0';
                    char *x_str = value;
                    char *y_str = colon + 1;
                    
                    char *x_endptr, *y_endptr;
                    long x_value = strtol(x_str, &x_endptr, 10);
                    long y_value = strtol(y_str, &y_endptr, 10);
                    
                    if (*x_endptr == '\0' && *y_endptr == '\0' && 
                        x_value >= 0 && x_value <= 9999 &&
                        y_value >= 0 && y_value <= 9999) {
                        config->position = POPUP_POSITION_ABSOLUTE;
                        config->position_x = (int)x_value;
                        config->position_y = (int)y_value;
                        msg(LOG_DEBUG, "Config: position = ABSOLUTE (%d:%d)", config->position_x, config->position_y);
                    } else {
                        msg(LOG_WARNING, "Invalid absolute position '%s' on line %d (must be X:Y with valid coordinates)", value, line_num);
                    }
                } else {
                    msg(LOG_WARNING, "Invalid position value '%s' on line %d (must be 'mouse', 'screen' or 'X:Y')", value, line_num);
                }
            }

        } else if (strcmp(key, "anchor") == 0) {
            char *endptr;
            long anchor_value = strtol(value, &endptr, 10);
            if (*endptr == '\0' && anchor_value >= 1 && anchor_value <= 9) {
                config->anchor = (PopupAnchor)anchor_value;
                msg(LOG_DEBUG, "Config: anchor = %d", config->anchor);
            } else {
                msg(LOG_WARNING, "Invalid anchor value '%s' on line %d (must be 1-9)", value, line_num);
            }
            
        } else if (strcmp(key, "margin") == 0) {
            char *space = strchr(value, ' ');
            if (space) {
                *space = '\0';
                char *vertical_str = value;
                char *horizontal_str = space + 1;
                
                while (*horizontal_str == ' ' || *horizontal_str == '\t') horizontal_str++;
                
                char *vertical_endptr, *horizontal_endptr;
                long vertical_value = strtol(vertical_str, &vertical_endptr, 10);
                long horizontal_value = strtol(horizontal_str, &horizontal_endptr, 10);
                
                if (*vertical_endptr == '\0' && *horizontal_endptr == '\0' && 
                    vertical_value >= 0 && vertical_value <= 100 &&
                    horizontal_value >= 0 && horizontal_value <= 100) {
                    config->margin_vertical = (int)vertical_value;
                    config->margin_horizontal = (int)horizontal_value;
                    msg(LOG_DEBUG, "Config: margin = %d %d", config->margin_vertical, config->margin_horizontal);
                } else {
                    msg(LOG_WARNING, "Invalid margin values '%s' on line %d (must be two values 0-100)", value, line_num);
                }
            } else {
                char *endptr;
                long margin_value = strtol(value, &endptr, 10);
                if (*endptr == '\0' && margin_value >= 0 && margin_value <= 100) {
                    config->margin_vertical = (int)margin_value;
                    config->margin_horizontal = (int)margin_value;
                    msg(LOG_DEBUG, "Config: margin = %d", config->margin_vertical);
                } else {
                    msg(LOG_WARNING, "Invalid margin value '%s' on line %d (must be 0-100)", value, line_num);
                }
            }

        } else {
            msg(LOG_WARNING, "Unknown config option '%s' on line %d", key, line_num);
        }
    }
    
    fclose(f);
    msg(LOG_NOTICE, "Config file parsed successfully");
    return 1;
}

// Apply configuration (sets global variables)
int config_apply(config_t *config) {
    // Apply verbose setting
    g_verbose = config->verbose;
    
    // Note: logfile opening is handled in main.c since it needs to be done early
    // history_file and timeout would be applied to their respective subsystems
    
    msg(LOG_DEBUG, "Configuration applied");
    return 1;
}

// Free configuration resources
void config_free(config_t *config) {
    if (config->logfile) {
        free(config->logfile);
        config->logfile = NULL;
    }
    if (config->history_file) {
        free(config->history_file);
        config->history_file = NULL;
    }
    if (config->overflow_directory) {
        free(config->overflow_directory);
        config->overflow_directory = NULL;
    }
}

// Print current configuration
void config_print(const config_t *config) {
    msg(LOG_NOTICE, "Current configuration:");
    msg(LOG_NOTICE, "  verbose: %s", config->verbose ? "true" : "false");
    msg(LOG_NOTICE, "  logfile: %s", config->logfile ? config->logfile : "(stdout)");
    msg(LOG_NOTICE, "  history_file: %s", config->history_file ? config->history_file : "(default)");
    msg(LOG_NOTICE, "  timeout: %d seconds", config->timeout);
    
    const char *position_description;
    switch (config->position) {
        case POPUP_POSITION_MOUSE:
            position_description = "mouse";
            break;
        case POPUP_POSITION_SCREEN:
            position_description = "screen";
            break;
        case POPUP_POSITION_ABSOLUTE:
            position_description = "absolute";
            break;
        default:
            position_description = "unknown";
            break;
    }
    
    if (config->position == POPUP_POSITION_ABSOLUTE) {
        msg(LOG_NOTICE, "  position: %s (%d:%d)", position_description, config->position_x, config->position_y);
    } else {
        msg(LOG_NOTICE, "  position: %s", position_description);
    }
    
    msg(LOG_NOTICE, "  anchor: %d", config->anchor);
    if (config->margin_vertical == config->margin_horizontal) {
        msg(LOG_NOTICE, "  margin: %d pixels", config->margin_vertical);
    } else {
        msg(LOG_NOTICE, "  margin: %d %d pixels", config->margin_vertical, config->margin_horizontal);
    }
}
