#define _GNU_SOURCE
#include "text.h"
#include "halen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <linux/limits.h>

static size_t display_content_length = 0;

void text_set_memory_limit(void) {
    display_content_length = (config.max_lines * (config.max_line_length + 1)) + 100;
}

char* text_escape_content(const char* content) {
    if (!content) return NULL;
    
    size_t content_length = strlen(content);
    size_t max_result_size = content_length * 2 + 1;
    
    if (display_content_length > 0 && max_result_size > display_content_length) {
        max_result_size = display_content_length;
        msg(LOG_WARNING, "Content too large for escaping, truncating");
    }
    
    char *result = malloc(max_result_size);
    if (!result) return NULL;
    
    const char *source = content;
    char *destination = result;
    char *result_end = result + max_result_size - 1;
    
    while (*source && destination < result_end - 1) {
        switch (*source) {
            case '\n': 
                if (destination < result_end - 2) {
                    *destination++ = '\\'; 
                    *destination++ = 'n'; 
                }
                break;
            case '\r': 
                if (destination < result_end - 2) {
                    *destination++ = '\\'; 
                    *destination++ = 'r'; 
                }
                break;
            case '\t': 
                if (destination < result_end - 2) {
                    *destination++ = '\\'; 
                    *destination++ = 't'; 
                }
                break;
            default: 
                *destination++ = *source; 
                break;
        }
        source++;
    }
    *destination = '\0';
    
    char *trimmed_result = realloc(result, strlen(result) + 1);
    return trimmed_result ? trimmed_result : result;
}

char* text_unescape_content(const char* content) {
    if (!content) return NULL;
    
    size_t content_length = strlen(content);
    char *result = malloc(content_length + 1);
    if (!result) return NULL;
    
    const char *source = content;
    char *destination = result;
    
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
    
    char *trimmed_result = realloc(result, strlen(result) + 1);
    return trimmed_result ? trimmed_result : result;
}

char* text_format_for_display(const char* content) {
    if (!content) return NULL;
    
    // size_t buffer_size = (max_lines * (max_line_length + 1)) + 100;
    char *result = malloc(display_content_length);
    if (!result) return strdup(content);
    
    char *write_position = result;
    const char *line_start = content;
    int displayed_lines = 0;
    int total_lines = 0;
    
    for (const char *character = content; *character; character++) {
        if (*character == '\n') total_lines++;
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
    
    char *trimmed_result = realloc(result, strlen(result) + 1);
    return trimmed_result ? trimmed_result : result;
}

char* text_truncate_for_storage(const char* content, char** overflow_hash) {
    int content_length = strlen(content);
    int line_count = 0;
    int current_line_length = 0;
    int needs_truncation = 0;
    
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
        return text_format_for_display(content);
    }
    
    uint32_t content_hash = text_calculate_hash(content);
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
        msg(LOG_DEBUG, "Created overflow file: %s (content size: %d bytes)", 
            *overflow_hash, content_length);
    } else {
        msg(LOG_WARNING, "Failed to create overflow file, falling back to truncation");
        free(*overflow_hash);
        *overflow_hash = NULL;
        return text_format_for_display(content);
    }
    
    char *truncated_content = text_format_for_display(content);
    if (!truncated_content) {
        truncated_content = strdup(content);
    }
    
    return truncated_content;
}

uint32_t text_calculate_hash(const char* content) {
    uint32_t hash_value = 2166136261u;
    const uint32_t prime = 16777619u;
    
    for (const char *character = content; *character; character++) {
        hash_value ^= (uint32_t)*character;
        hash_value *= prime;
    }
    
    return hash_value;
}

int text_contains_non_whitespace(const char* content) {
    if (!content) return 0;
    
    for (const char *character = content; *character; character++) {
        if (*character != ' ' && *character != '\t' && 
            *character != '\n' && *character != '\r') {
            return 1;
        }
    }
    return 0;
}

char* text_trim_trailing_whitespace(char* content) {
    if (!content) return NULL;
    
    size_t length = strlen(content);
    while (length > 0 && (content[length-1] == '\n' || content[length-1] == '\r')) {
        content[--length] = '\0';
    }
    
    return content;
}
