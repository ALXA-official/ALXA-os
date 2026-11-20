#ifndef ALXA_USERS_H
#define ALXA_USERS_H
#include <stddef.h>
#include <sys/types.h>

#define MAXUSERS 64

struct user {
    char name[32];
    char hash[128];          /* empty unless loaded by root */
    uid_t uid;
    gid_t gid;
    char home[96];
};

void users_set_dir(const char *dir);          /* where passwd/shadow live */
void users_set_homebase(const char *dir);     /* where home folders live */
const char *users_homebase(void);
void users_autodetect_dir(void);              /* use /data/etc if the disk is mounted */

int users_load(void);                         /* returns number of accounts */
int users_save(void);
int users_exist(void);
const struct user *user_find(const char *name);
const struct user *user_by_uid(uid_t uid);
const struct user *user_by_gid(gid_t gid);
int user_valid_name(const char *name);
int user_verify(const struct user *u, const char *password);   /* u may be NULL */
int user_add(const char *name, const char *password, uid_t uid, gid_t gid, const char *home);
int user_set_password(const char *name, const char *password);
int user_remove(const char *name);
uid_t users_next_uid(void);
int user_make_home(const char *home, uid_t uid, gid_t gid);
int password_policy_ok(const char *pw);       /* prints why when it fails */

int read_line(const char *prompt, char *buf, size_t size, int echo);
int read_password(const char *prompt, char *buf, size_t size);
int ask_new_password(char *out, size_t size);  /* asks twice, checks policy */

int passwd_main(int argc, char **argv);
int cmd_adduser(int argc, char **argv);
int cmd_deluser(int argc, char **argv);
int cmd_users(int argc, char **argv);

#endif
