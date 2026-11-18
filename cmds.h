#ifndef ALXA_CMDS_H
#define ALXA_CMDS_H
#include <sys/types.h>

/* A shell command. in_parent=1 means it must run inside the shell itself
 * (it changes the shell's own state, like cd or export). */
struct builtin {
    const char *name;
    int (*fn)(int argc, char **argv);
    int in_parent;
};

extern const struct builtin extra_builtins[];

const char *base_name(const char *p);
int is_dir(const char *p);
int copy_file(const char *src, const char *dst, mode_t mode);
int copy_tree(const char *src, const char *dst);
int remove_tree(const char *path);

#endif
