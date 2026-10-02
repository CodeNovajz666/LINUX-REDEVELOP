#ifndef _SECURITY_H
#define _SECURITY_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>

/* 用户类型 */
typedef enum {
    USER_TYPE_ROOT = 0,        /* 根用户 */
    USER_TYPE_ADMIN = 1,       /* 管理员 */
    USER_TYPE_USER = 2,        /* 普通用户 */
    USER_TYPE_GUEST = 3        /* 访客 */
} user_type_t;

/* 权限类型 */
typedef enum {
    PERM_READ = 0x01,          /* 读权限 */
    PERM_WRITE = 0x02,         /* 写权限 */
    PERM_EXECUTE = 0x04,       /* 执行权限 */
    PERM_CREATE = 0x08,        /* 创建权限 */
    PERM_DELETE = 0x10,        /* 删除权限 */
    PERM_ADMIN = 0x20,         /* 管理权限 */
    PERM_ALL = 0x3F            /* 所有权限 */
} perm_type_t;

/* 用户结构 */
struct user {
    uid_t uid;                 /* 用户ID */
    gid_t gid;                 /* 组ID */
    char *name;                /* 用户名 */
    char *password;            /* 密码（加密） */
    user_type_t type;          /* 用户类型 */
    unsigned int permissions;  /* 权限位 */
    struct list_head list;     /* 链表节点 */
};

/* 组结构 */
struct group {
    gid_t gid;                 /* 组ID */
    char *name;                /* 组名 */
    unsigned int permissions;  /* 组权限 */
    struct list_head users;     /* 组成员链表 */
    struct list_head list;     /* 链表节点 */
};

/* 权限上下文 */
struct security_context {
    struct user *user;         /* 当前用户 */
    struct group *group;       /* 当前组 */
    unsigned int effective_perms; /* 有效权限 */
};

/* 进程安全结构 */
struct process_security {
    pid_t pid;                 /* 进程PID */
    struct security_context context; /* 安全上下文 */
    spinlock_t lock;           /* 锁 */
};

/* 安全策略 */
typedef enum {
    SEC_POLICY_DEFAULT = 0,    /* 默认策略 */
    SEC_POLICY_STRICT = 1,     /* 严格策略 */
    SEC_POLICY_PERMISSIVE = 2  /* 宽松策略 */
} security_policy_t;

/* 安全系统API */
int security_init(void);
void security_cleanup(void);
int security_add_user(uid_t uid, gid_t gid, const char *name, const char *password, user_type_t type);
int security_remove_user(uid_t uid);
struct user *security_get_user(uid_t uid);
int security_add_group(gid_t gid, const char *name, unsigned int permissions);
int security_remove_group(gid_t gid);
struct group *security_get_group(gid_t gid);
int security_add_user_to_group(uid_t uid, gid_t gid);
int security_remove_user_from_group(uid_t uid, gid_t gid);

/* 权限检查API */
bool security_check_permission(struct security_context *context, perm_type_t perm);
bool security_check_process_permission(pid_t pid, perm_type_t perm);
int security_set_process_context(pid_t pid, uid_t uid, gid_t gid);
int security_get_process_context(pid_t pid, struct security_context *context);

/* 进程安全API */
int process_security_init(struct process_security *psec, pid_t pid);
void process_security_cleanup(struct process_security *psec);
int process_security_set_user(struct process_security *psec, uid_t uid);
int process_security_set_group(struct process_security *psec, gid_t gid);
bool process_security_check_permission(struct process_security *psec, perm_type_t perm);

/* 安全策略API */
int security_set_policy(security_policy_t policy);
security_policy_t security_get_policy(void);

/* 密码加密API */
char *security_encrypt_password(const char *password);
bool security_verify_password(const char *password, const char *encrypted);

#endif /* _SECURITY_H */
