#ifndef MG_GUI_H
#define MG_GUI_H
#include <stddef.h>

#define GUI_TO_SHELL 0
#define GUI_LOGOUT   1

/* Runs the pixel-graphics desktop. Returns GUI_TO_SHELL or GUI_LOGOUT. */
int gui_run(void);

/* Graphical first-boot account setup wizard (returns 0 when complete). */
int gui_first_start(void);

/* Graphical login screen. Writes authenticated username to out_user and returns 0. */
int gui_login(char *out_user, size_t n);

#endif
