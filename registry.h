#ifndef MG_REGISTRY_H
#define MG_REGISTRY_H
#include <stddef.h>

/* The registry holds named values (settings and real system variables).
 *   System hive: <etc>/registry   everyone can read, only root can change
 *   User hive:   ~/.mg-registry   private to each user
 * Keys look like  Taskbar/Position  or  Environment/EDITOR. */
enum { HIVE_SYSTEM = 0, HIVE_USER = 1 };

/* NULL keeps the current path of that hive. */
void reg_set_paths(const char *system_path, const char *user_path);
void reg_refresh(void);                         /* reload a hive if its file changed */
const char *reg_get(int hive, const char *key, const char *def);
const char *reg_eff(const char *key, const char *def);   /* user value, else system, else def */
int reg_eff_int(const char *key, int def);
int reg_set(int hive, const char *key, const char *val); /* 0 = ok, -1 = failed (errno set) */
int reg_del(int hive, const char *key);
int reg_del_prefix(int hive, const char *prefix);        /* delete a whole key */
int reg_count(int hive);
const char *reg_entry(int hive, int i, const char **val);
const char *reg_hive_name(int hive);
int reg_apply_env(void);                        /* export the Environment values */
int cmd_reg(int argc, char **argv);

#endif
