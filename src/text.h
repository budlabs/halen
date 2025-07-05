#ifndef TEXT_H
#define TEXT_H

#include <stddef.h>
#include <stdint.h>

char* text_escape_content(const char* content);
char* text_unescape_content(const char* content);
char* text_format_for_display(const char* content);
char* text_truncate_for_storage(const char* content, char** overflow_hash);
uint32_t text_calculate_hash(const char* content);
int text_contains_non_whitespace(const char* content);
char* text_trim_trailing_whitespace(char* content);
void text_set_memory_limit(void);

#endif
