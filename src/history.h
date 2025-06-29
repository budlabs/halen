#ifndef HISTORY_H
#define HISTORY_H

#include <stdio.h>
#include <stdint.h>



typedef struct {
    char *content;
    char *timestamp;
    char *source;
} history_entry_t;

typedef struct {
    int max_lines;
    int max_line_length;
} history_metadata_t;

// History management
int history_initialize(void);
void history_cleanup(void);

// Entry operations
int history_add_entry(const char *content, const char *source);
char* history_get_entry_truncated(int index);
char* history_get_entry_full_content(int index);
int history_delete_entry(int index);
int history_get_count(void);

// Navigation state
void history_set_current_index(int index);
int history_get_current_index(void);
void history_reset_navigation(void);

// File operations
char* history_get_default_file_path(void);

#endif // HISTORY_H
