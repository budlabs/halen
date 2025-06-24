#define _GNU_SOURCE
#include "parser.h"
#include "halen.h"  // This will provide msg() and verbose declarations
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clipboard.h"

// Initialize config with defaults
void config_init(config_t *config) {
    config->verbose = 0;
    config->logfile = NULL;
    char *default_path = clipboard_history_file_default_path();
    config->history_file = strdup(default_path);
    config->timeout = 2;
    config->max_lines = 10;  // Default max lines in popup
    config->max_line_length = 80;  // Default max line length in popup
    config->font = strdup("monospace");
    config->font_size = 12;  // Default font size in popup
    config->background = 0xFFFFFF;  // Default background color (white)
    config->foreground = 0xFF0000;  // Default foreground color (black)
    config->count_color = 0xFF0000;  // Default foreground color (black)
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
}

// Print current configuration
void config_print(const config_t *config) {
    msg(LOG_NOTICE, "Current configuration:");
    msg(LOG_NOTICE, "  verbose: %s", config->verbose ? "true" : "false");
    msg(LOG_NOTICE, "  logfile: %s", config->logfile ? config->logfile : "(stdout)");
    msg(LOG_NOTICE, "  history_file: %s", config->history_file ? config->history_file : "(default)");
    msg(LOG_NOTICE, "  timeout: %d seconds", config->timeout);
}
