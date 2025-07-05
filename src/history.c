#define _GNU_SOURCE
#include "history.h"
#include "halen.h"
#include "clipboard.h"
#include "xdg.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include <time.h>
#include <unistd.h>

#define METADATA_PREFIX "# HALEN_METADATA: "
#define MAX_CLIPBOARD_ENTRIES 50


static history_entry_t *entries = NULL;
static int history_count = 0;
static int current_index = -1;
static size_t max_clipboard_memory_size = 0;

static char* load_overflow_content_by_hash(const char* overflow_hash);
static int replace_file_atomically(const char* source_filename, const char* target_filename);
static int create_history_file(const char *history_file);
static char* extract_overflow_hash_from_line(const char* line);
static void regenerate_truncated_entries(void);
static int needs_regeneration(const history_metadata_t *stored_metadata);
static int read_history_metadata(FILE *file, history_metadata_t *metadata);
static void write_history_metadata(FILE *file, const history_metadata_t *metadata);
static int count_history_entries_in_file(FILE* file);
static int load_history_entries(void);
static uint32_t calculate_content_hash(const char *content);
static void get_timestamp(char *buffer, size_t size);
static char* create_display_formatted_content(const char *content);
static char* truncate_content_for_storage(const char *content, char **overflow_hash);
static char* transform_content_escaping(const char* content, int should_escape);
static char* extract_display_content(const char *raw_content);
static history_entry_t entry_parse(const char *line);

static char* load_overflow_content_by_hash(const char* overflow_hash) {
    if (!config.overflow_directory || !overflow_hash) return NULL;
    
    char overflow_file_path[PATH_MAX];
    snprintf(overflow_file_path, sizeof(overflow_file_path), "%s/%s", 
            config.overflow_directory, overflow_hash);
    
    FILE *overflow_file = fopen(overflow_file_path, "r");
    if (!overflow_file) return NULL;
    
    char *full_content = malloc(MAX_OVERFLOW_FILE_SIZE);
    if (full_content) {
        size_t content_size = fread(full_content, 1, MAX_OVERFLOW_FILE_SIZE - 1, overflow_file);
        full_content[content_size] = '\0';
    }
    fclose(overflow_file);
    
    return full_content;
}

static int replace_file_atomically(const char* source_filename, const char* target_filename) {
    if (rename(source_filename, target_filename) != 0) {
        unlink(source_filename);
        return 0;
    }
    return 1;
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
            char *full_content = load_overflow_content_by_hash(overflow_hash);
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

static int needs_regeneration(const history_metadata_t *stored_metadata) {
    if (stored_metadata->max_lines != config.max_lines || 
        stored_metadata->max_line_length != config.max_line_length) {
        return 1;
    }
    
    return 0;
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

static int count_history_entries_in_file(FILE* file) {
    int count = 0;
    char line[8192];
    fgets(line, sizeof(line), file); // Skip metadata
    while (fgets(line, sizeof(line), file)) {
        if (strlen(line) > 10) count++;
    }
    return count;
}

static int load_history_entries(void) {
    if (entries) {
        for (int i = 0; i < history_count; i++) {
            free(entries[i].content);
            free(entries[i].timestamp);
            free(entries[i].source);
            free(entries[i].hash);
        }
        free(entries);
        entries = NULL;
    }
    
    if (access(config.history_file, F_OK) != 0) {
        msg(LOG_DEBUG, "History file doesn't exist, creating with initial entry");
        
        char *current_clipboard = clipboard_get_content("clipboard");
        const char *initial_content = current_clipboard ? current_clipboard : "Clipboard Empty";

        create_history_file(config.history_file);
        
        FILE *history_file = fopen(config.history_file, "a");
        if (history_file) {
            char timestamp[32];
            get_timestamp(timestamp, sizeof(timestamp));
            
            char *escaped_content = transform_content_escaping(initial_content, 1);
            if (escaped_content) {
                fprintf(history_file, "[%s] [%s] %s\n", timestamp, "CLIPBOARD", escaped_content);
                free(escaped_content);
            }
            fclose(history_file);
        }
        
        if (current_clipboard) {
            free(current_clipboard);
        }
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
        msg(LOG_ERR, "Failed to open history file after creating it");
        history_count = 0;
        return 0;
    }
    
    history_count = count_history_entries_in_file(history_file);
    if (history_count == 0) {
        fclose(history_file);
        return 0;
    }
    
    entries = malloc(history_count * sizeof(history_entry_t));
    if (!entries) {
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
            if (entry.hash) free(entry.hash);
            continue;
        }

        entries[index] = entry;
        index++;
    }
    
    history_count = index;
    fclose(history_file);
    
    msg(LOG_DEBUG, "Loaded %d history entries", history_count);
    return history_count;
}


static uint32_t calculate_content_hash(const char *content) {
    uint32_t hash_value = 2166136261u;
    const uint32_t prime = 16777619u;
    
    for (const char *character = content; *character; character++) {
        hash_value ^= (uint32_t)*character;
        hash_value *= prime;
    }
    
    return hash_value;
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
    uint32_t content_hash = calculate_content_hash(content);
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

static void write_history_metadata(FILE *file, const history_metadata_t *metadata) {
    fprintf(file, "%smax_lines=%d max_line_length=%d\n", 
            METADATA_PREFIX, metadata->max_lines, metadata->max_line_length);
}

static char* transform_content_escaping(const char* content, int should_escape) {
    if (!content) return NULL;
    
    size_t content_length = strlen(content);
    size_t max_result_size = should_escape ? (content_length * 2 + 1) : (content_length + 1);
    
    if (max_result_size > max_clipboard_memory_size) {
        max_result_size = max_clipboard_memory_size;
        msg(LOG_WARNING, "Content too large for escaping, truncating");
    }
    
    char *result = malloc(max_result_size);
    if (!result) return NULL;
    
    const char *source = content;
    char *destination = result;
    char *result_end = result + max_result_size - 1;
    
    if (should_escape) {
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
    } else {
        while (*source && destination < result_end) {
            if (*source == '\\' && *(source + 1) && destination < result_end - 1) {
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
    
    char *trimmed_result = realloc(result, strlen(result) + 1);
    return trimmed_result ? trimmed_result : result;
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

int history_add_entry(const char *content, const char *source) {
    if (!content || strlen(content) == 0) return 0;
    if (!config.history_file) return 0;

    char *overflow_hash = NULL;
    char *storage_content = truncate_content_for_storage(content, &overflow_hash);
    if (!storage_content) return 0;
    
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
        return 0;
    }
    
    char temporary_filename[] = ".history.tmp";
    FILE *temporary_file = fopen(temporary_filename, "w");
    if (!temporary_file) {
        free(storage_content);
        if (overflow_hash) free(overflow_hash);
        return 0;
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
        return 0;
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

    return 1;
}

char* history_get_entry_truncated(int index) {
    if (history_count < 1) {
        load_history_entries();
    }
    
    if (index < 0) {
        index = 0;  // Default to newest entry
    }
    
    if (index < 0 || index >= history_count) {
        return NULL;
    }
    
    // Reverse the index to get newest-first ordering
    int actual_index = history_count - 1 - index;
    
    return strdup(entries[actual_index].content);
}

char* history_get_entry_full_content(int index) {
    if (history_count < 1) {
        load_history_entries();
    }
    
    if (index < 0) {
        index = 0;
    }
    
    if (index < 0 || index >= history_count) {
        return NULL;
    }
    
    int actual_index = history_count - 1 - index;
    
    if (entries[actual_index].hash) {
        char* content = load_overflow_content_by_hash(entries[actual_index].hash);
        if (content) {
            return content;
        }
    }
    
    return strdup(entries[actual_index].content);
}

int history_delete_entry(int index) {
    if (history_count < 1) load_history_entries();
    if (history_count < 1 || index < 0 || index >= history_count) {
        return 0;
    }
    
    int actual_index = history_count - 1 - index;
    
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
    int current_line_index = 0;
    int deleted = 0;
    char *overflow_hash_to_delete = entries[actual_index].hash ? strdup(entries[actual_index].hash) : NULL;
    
    if (fgets(line, sizeof(line), history_file)) {
        fputs(line, temp_file);
    }
    
    while (fgets(line, sizeof(line), history_file)) {
        if (strlen(line) < 10) {
            fputs(line, temp_file);
            continue;
        }
        
        if (current_line_index == actual_index) {
            deleted = 1;
            msg(LOG_NOTICE, "Deleted history entry %d: %.50s", 
                index + 1, entries[actual_index].content);
        } else {
            fputs(line, temp_file);
        }
        current_line_index++;
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
        }
        
        if (overflow_hash_to_delete) {
            free(overflow_hash_to_delete);
        }
        
        if (!replace_file_atomically(temp_filename, config.history_file)) {
            msg(LOG_ERR, "Failed to replace history file after deletion");
            return 0;
        }
        
        load_history_entries();
        
        if (current_index >= history_count) {
            current_index = -1;
        }
        
        return 1;
    } else {
        if (overflow_hash_to_delete) {
            free(overflow_hash_to_delete);
        }
        unlink(temp_filename);
        return 0;
    }
}

static history_entry_t entry_parse(const char *line) {
    history_entry_t entry = {NULL, NULL, NULL, NULL};
    
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
    
    entry.hash = extract_overflow_hash_from_line(content_start);
    
    char *display_content = extract_display_content(content_start);
    entry.content = transform_content_escaping(display_content, 0);
    free(display_content);
    free(line_copy);

    return entry;
}

void history_cleanup(void) {
    if (entries) {
        for (int i = 0; i < history_count; i++) {
            free(entries[i].content);
            free(entries[i].timestamp);
            free(entries[i].source);
            free(entries[i].hash);
        }
        free(entries);
        entries = NULL;
    }
    history_count = 0;
}

char* history_get_default_file_path(void) {
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

int history_get_count(void) {
    if (history_count < 1) {
        load_history_entries();
    }
    return history_count;
}


void history_set_current_index(int index) {
    if (index >= 0 && index < history_count) {
        current_index = index;
        msg(LOG_DEBUG, "Set current history index to %d (entry %d/%d)", 
            index, index + 1, history_count);
    } else if (index == -1) {
        current_index = -1;
        msg(LOG_DEBUG, "Reset current history index to -1");
    } else {
        msg(LOG_WARNING, "Attempted to set invalid history index %d (count: %d)", 
            index, history_count);
    }
}

int history_get_current_index(void) {
    return current_index;
}

void history_reset_navigation(void) {
    current_index = -1;
}

int history_initialize(void) {
    entries = NULL;
    history_count = 0;
    current_index = -1;
    
    max_clipboard_memory_size = (config.max_lines * config.max_line_length * 2) + 1024;
    
    return 1;
}
