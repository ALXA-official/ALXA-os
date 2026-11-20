#ifndef ALXA_LINE_H
#define ALXA_LINE_H
#include <stddef.h>

/* Completion callback: fills *out with a malloc'd array of malloc'd strings
 * (full replacements for WORD) and returns how many. */
typedef int (*line_completer)(const char *word, int first_word, char ***out);

void line_set_completer(line_completer fn);
/* Reads one edited line. Returns 1 = got a line, 0 = end of input (Ctrl+D), -1 = not a terminal. */
int line_edit(const char *prompt, char *buf, size_t size);
void line_history_add(const char *line);
int line_history_count(void);
const char *line_history_get(int i);

#endif
