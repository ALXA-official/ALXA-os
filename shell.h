#ifndef ALXA_SHELL_H
#define ALXA_SHELL_H
/* Interactive shell session. Returns when the user types 'exit' or presses Ctrl+D. */
void shell_session(void);
/* Runs the commands in a script file. Returns the last command's status. */
int shell_run_script(const char *path);
#endif
