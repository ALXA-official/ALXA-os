#ifndef MG_GUI_H
#define MG_GUI_H
#define GUI_TO_SHELL 0
#define GUI_LOGOUT   1
/* Runs the text-mode desktop. Returns GUI_TO_SHELL or GUI_LOGOUT. */
int gui_run(void);
#endif
