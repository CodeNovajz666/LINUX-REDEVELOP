#include "security.h"
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "pcb.h"

/* 全局安全系统 */
static struct security_system {
    struct list_head users;       /* 用户链表 */
    struct list_head groups;      /* 组链表 */
    security_policy_t policy;     /* 安全策略 */
    spinlock_t lock;              /* 锁 */
    bool initialized;             /* 初始化状态 */
} g_security_system;

/* 初始化安全系统 */
int security_init(void) {
    /* 初始化链表 */
    INIT_LIST_HEAD(&g_security_system.users);
    INIT_LIST_HEAD(&g_security_system.groups);
    
    /* 初始化锁 */
    spin_lock_init(&g_security_system.lock);
    
    g_security_system.policy = SEC_POLICY_DEFAULT;
    g_security_system.initialized = true;
    
    /* 添加默认用户和组 */
    security_add_group(0, "root", PERM_ALL);
    security_add_user(0, 0, "root", "root", USER_TYPE_ROOT);
    
    printk(KERN_INFO "security_init: initialized\n");
    return 0;
}

/* 清理安全系统 */
void security_cleanup(void) {
    struct user *user, *user_tmp;
    struct group *group, *group_tmp;
    struct list_head *pos, *q;
    
    if (!g_security_system.initialized) {
        return;
    }
    
    /* 清理所有用户 */
    list_for_each_safe(pos, q, &g_security_system.users) {
        user = list_entry(pos, struct user, list);
        list_del(&user->list);
        if (user->name) kfree(user->name);
        if (user->password) kfree(user->password);
        kfree(user);
    }
    
    /* 清理所有组 */
    list_for_each_safe(pos, q, &g_security_system.groups) {
        group = list_entry(pos, struct group, list);
        list_del(&group->list);
        if (group->name) kfree(group->name);
        kfree(group);
    }
    
    g_security_system.initialized = false;
    printk(KERN_INFO "security_cleanup: cleaned up\n");
}

/* 添加用户 */
int security_add_user(uid_t uid, gid_t gid, const char *name, const char *password, user_type_t type) {
    struct user *user;
    struct group *group;
    unsigned long flags;
    
    if (!g_security_system.initialized || !name || !password) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&g_security_system.lock, flags);
    
    /* 检查用户是否已存在 */
    list_for_each_entry(user, &g_security_system.users, list) {
        if (user->uid == uid) {
            spin_unlock_irqrestore(&g_security_system.lock, flags);
            return -EEXIST;
        }
    }
    
    /* 检查组是否存在 */
    group = security_get_group(gid);
    if (!group && gid != 0) {
        spin_unlock_irqrestore(&g_security_system.lock, flags);
        return -ENOENT;
    }
    
    /* 创建新用户 */
    user = kmalloc(sizeof(struct user), GFP_KERNEL);
    if (!user) {
        spin_unlock_irqrestore(&g_security_system.lock, flags);
        return -ENOMEM;
    }
    
    user->uid = uid;
    user->gid = gid;
    user->name = kmalloc(strlen(name) + 1, GFP_KERNEL);
    if (!user->name) {
        kfree(user);
        spin_unlock_irqrestore(&g_security_system.lock, flags);
        return -ENOMEM;
    }
    strcpy(user->name, name);
    
    user->password = security_encrypt_password(password);
    if (!user->password) {
        kfree(user->name);
        kfree(user);
        spin_unlock_irqrestore(&g_security_system.lock, flags);
        return -ENOMEM;
    }
    
    user->type = type;
    
    /* 根据用户类型设置权限 */
    switch (type) {
        case USER_TYPE_ROOT:
            user->permissions = PERM_ALL;
            break;
        case USER_TYPE_ADMIN:
            user->permissions = PERM_READ | PERM_WRITE | PERM_EXECUTE | PERM_CREATE | PERM_DELETE;
            break;
        case USER_TYPE_USER:
            user->permissions = PERM_READ | PERM_WRITE | PERM_EXECUTE;
            break;
        case USER_TYPE_GUEST:
            user->permissions = PERM_READ;
            break;
        default:
            user->permissions = PERM_READ;
            break;
    }
    
    INIT_LIST_HEAD(&user->list);
    list_add_tail(&user->list, &g_security_system.users);
    
    spin_unlock_irqrestore(&g_security_system.lock, flags);
    
    printk(KERN_INFO "security_add_user: added user '%s' (UID=%d)\n", name, uid);
    return 0;
}

/* 移除用户 */
int security_remove_user(uid_t uid) {
    struct user *user, *tmp;
    struct list_head *pos, *q;
    unsigned long flags;
    int found = 0;
    
    if (!g_security_system.initialized) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&g_security_system.lock, flags);
    
    list_for_each_safe(pos, q, &g_security_system.users) {
        user = list_entry(pos, struct user, list);
        if (user->uid == uid) {
            list_del(&user->list);
            if (user->name) kfree(user->name);
            if (user->password) kfree(user->password);
            kfree(user);
            found = 1;
            break;
        }
    }
    
    spin_unlock_irqrestore(&g_security_system.lock, flags);
    
    if (found) {
        printk(KERN_INFO "security_remove_user: removed user UID=%d\n", uid);
        return 0;
    } else {
        return -ENOENT;
    }
}

/* 获取用户 */
struct user *security_get_user(uid_t uid) {
    struct user *user;
    unsigned long flags;
    
    if (!g_security_system.initialized) {
        return NULL;
    }
    
    spin_lock_irqsave(&g_security_system.lock, flags);
    
    list_for_each_entry(user, &g_security_system.users, list) {
        if (user->uid == uid) {
            spin_unlock_irqrestore(&g_security_system.lock, flags);
            return user;
        }
    }
    
    spin_unlock_irqrestore(&g_security_system.lock, flags);
    return NULL;
}

/* 添加组 */
int security_add_group(gid_t gid, const char *name, unsigned int permissions) {
    struct group *group;
    unsigned long flags;
    
    if (!g_security_system.initialized || !name) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&g_security_system.lock, flags);
    
    /* 检查组是否已存在 */
    list_for_each_entry(group, &g_security_system.groups, list) {
        if (group->gid == gid) {
            spin_unlock_irqrestore(&g_security_system.lock, flags);
            return -EEXIST;
        }
    }
    
    /* 创建新组 */
    group = kmalloc(sizeof(struct group), GFP_KERNEL);
    if (!group) {
        spin_unlock_irqrestore(&g_security_system.lock, flags);
        return -ENOMEM;
    }
    
    group->gid = gid;
    group->name = kmalloc(strlen(name) + 1, GFP_KERNEL);
    if (!group->name) {
        kfree(group);
        spin_unlock_irqrestore(&g_security_system.lock, flags);
        return -ENOMEM;
    }
    strcpy(group->name, name);
    
    group->permissions = permissions;
    INIT_LIST_HEAD(&group->users);
    INIT_LIST_HEAD(&group->list);
    
    list_add_tail(&group->list, &g_security_system.groups);
    
    spin_unlock_irqrestore(&g_security_system.lock, flags);
    
    printk(KERN_INFO "security_add_group: added group '%s' (GID=%d)\n", name, gid);
    return 0;
}

/* 移除组 */
int security_remove_group(gid_t gid) {
    struct group *group, *tmp;
    struct list_head *pos, *q;
    unsigned long flags;
    int found = 0;
    
    if (!g_security_system.initialized) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&g_security_system.lock, flags);
    
    list_for_each_safe(pos, q, &g_security_system.groups) {
        group = list_entry(pos, struct group, list);
        if (group->gid == gid) {
            list_del(&group->list);
            if (group->name) kfree(group->name);
            kfree(group);
            found = 1;
            break;
        }
    }
    
    spin_unlock_irqrestore(&g_security_system.lock, flags);
    
    if (found) {
        printk(KERN_INFO "security_remove_group: removed group GID=%d\n", gid);
        return 0;
    } else {
        return -ENOENT;
    }
}

/* 获取组 */
struct group *security_get_group(gid_t gid) {
    struct group *group;
    unsigned long flags;
    
    if (!g_security_system.initialized) {
        return NULL;
    }
    
    spin_lock_irqsave(&g_security_system.lock, flags);
    
    list_for_each_entry(group, &g_security_system.groups, list) {
        if (group->gid == gid) {
            spin_unlock_irqrestore(&g_security_system.lock, flags);
            return group;
        }
    }
    
    spin_unlock_irqrestore(&g_security_system.lock, flags);
    return NULL;
}

/* 添加用户到组 */
int security_add_user_to_group(uid_t uid, gid_t gid) {
    struct user *user;
    struct group *group;
    unsigned long flags;
    
    if (!g_security_system.initialized) {
        return -EINVAL;
    }
    
    user = security_get_user(uid);
    if (!user) {
        return -ENOENT;
    }
    
    group = security_get_group(gid);
    if (!group) {
        return -ENOENT;
    }
    
    spin_lock_irqsave(&group->lock, flags);
    
    /* 将用户添加到组 */
    list_add_tail(&user->list, &group->users);
    
    spin_unlock_irqrestore(&group->lock, flags);
    
    printk(KERN_INFO "security_add_user_to_group: added user %d to group %d\n", uid, gid);
    return 0;
}

/* 从组中移除用户 */
int security_remove_user_from_group(uid_t uid, gid_t gid) {
    struct user *user;
    struct group *group;
    struct list_head *pos, *q;
    unsigned long flags;
    int found = 0;
    
    if (!g_security_system.initialized) {
        return -EINVAL;
    }
    
    group = security_get_group(gid);
    if (!group) {
        return -ENOENT;
    }
    
    spin_lock_irqsave(&group->lock, flags);
    
    list_for_each_safe(pos, q, &group->users) {
        user = list_entry(pos, struct user, list);
        if (user->uid == uid) {
            list_del(&user->list);
            found = 1;
            break;
        }
    }
    
    spin_unlock_irqrestore(&group->lock, flags);
    
    if (found) {
        printk(KERN_INFO "security_remove_user_from_group: removed user %d from group %d\n", uid, gid);
        return 0;
    } else {
        return -ENOENT;
    }
}

/* 检查权限 */
bool security_check_permission(struct security_context *context, perm_type_t perm) {
    if (!context) {
        return false;
    }
    
    /* 检查有效权限 */
    if (context->effective_perms & perm) {
        return true;
    }
    
    /* 检查用户权限 */
    if (context->user && (context->user->permissions & perm)) {
        return true;
    }
    
    /* 检查组权限 */
    if (context->group && (context->group->permissions & perm)) {
        return true;
    }
    
    return false;
}

/* 检查进程权限 */
bool security_check_process_permission(pid_t pid, perm_type_t perm) {
    struct pcb *pcb;
    struct process_security *psec;
    
    if (!g_security_system.initialized) {
        return false;
    }
    
    /* 查找进程 */
    pcb = pcb_find_by_pid(pid);
    if (!pcb) {
        return false;
    }
    
    /* 获取进程安全结构 */
    psec = pcb->security;
    if (!psec) {
        return false;
    }
    
    return process_security_check_permission(psec, perm);
}

/* 设置进程安全上下文 */
int security_set_process_context(pid_t pid, uid_t uid, gid_t gid) {
    struct pcb *pcb;
    struct process_security *psec;
    
    if (!g_security_system.initialized) {
        return -EINVAL;
    }
    
    /* 查找进程 */
    pcb = pcb_find_by_pid(pid);
    if (!pcb) {
        return -ESRCH;
    }
    
    /* 获取进程安全结构 */
    psec = pcb->security;
    if (!psec) {
        return -EINVAL;
    }
    
    /* 设置用户和组 */
    if (process_security_set_user(psec, uid) != 0) {
        return -EINVAL;
    }
    
    if (process_security_set_group(psec, gid) != 0) {
        return -EINVAL;
    }
    
    printk(KERN_INFO "security_set_process_context: PID %d set to UID=%d, GID=%d\n", pid, uid, gid);
    return 0;
}

/* 获取进程安全上下文 */
int security_get_process_context(pid_t pid, struct security_context *context) {
    struct pcb *pcb;
    struct process_security *psec;
    
    if (!g_security_system.initialized || !context) {
        return -EINVAL;
    }
    
    /* 查找进程 */
    pcb = pcb_find_by_pid(pid);
    if (!pcb) {
        return -ESRCH;
    }
    
    /* 获取进程安全结构 */
    psec = pcb->security;
    if (!psec) {
        return -EINVAL;
    }
    
    memcpy(context, &psec->context, sizeof(struct security_context));
    return 0;
}

/* 初始化进程安全结构 */
int process_security_init(struct process_security *psec, pid_t pid) {
    if (!psec) {
        return -EINVAL;
    }
    
    psec->pid = pid;
    psec->context.user = NULL;
    psec->context.group = NULL;
    psec->context.effective_perms = 0;
    spin_lock_init(&psec->lock);
    
    printk(KERN_INFO "process_security_init: initialized for PID %d\n", pid);
    return 0;
}

/* 清理进程安全结构 */
void process_security_cleanup(struct process_security *psec) {
    if (!psec) {
        return;
    }
    
    printk(KERN_INFO "process_security_cleanup: cleaned up for PID %d\n", psec->pid);
}

/* 设置进程用户 */
int process_security_set_user(struct process_security *psec, uid_t uid) {
    struct user *user;
    unsigned long flags;
    
    if (!psec) {
        return -EINVAL;
    }
    
    user = security_get_user(uid);
    if (!user) {
        return -ENOENT;
    }
    
    spin_lock_irqsave(&psec->lock, flags);
    
    psec->context.user = user;
    
    /* 更新有效权限 */
    psec->context.effective_perms = user->permissions;
    if (psec->context.group) {
        psec->context.effective_perms |= psec->context.group->permissions;
    }
    
    spin_unlock_irqrestore(&psec->lock, flags);
    
    return 0;
}

/* 设置进程组 */
int process_security_set_group(struct process_security *psec, gid_t gid) {
    struct group *group;
    unsigned long flags;
    
    if (!psec) {
        return -EINVAL;
    }
    
    group = security_get_group(gid);
    if (!group) {
        return -ENOENT;
    }
    
    spin_lock_irqsave(&psec->lock, flags);
    
    psec->context.group = group;
    
    /* 更新有效权限 */
    psec->context.effective_perms = group->permissions;
    if (psec->context.user) {
        psec->context.effective_perms |= psec->context.user->permissions;
    }
    
    spin_unlock_irqrestore(&psec->lock, flags);
    
    return 0;
}

/* 检查进程权限 */
bool process_security_check_permission(struct process_security *psec, perm_type_t perm) {
    unsigned long flags;
    bool result = false;
    
    if (!psec) {
        return false;
    }
    
    spin_lock_irqsave(&psec->lock, flags);
    
    result = security_check_permission(&psec->context, perm);
    
    spin_unlock_irqrestore(&psec->lock, flags);
    
    return result;
}

/* 设置安全策略 */
int security_set_policy(security_policy_t policy) {
    if (!g_security_system.initialized) {
        return -EINVAL;
    }
    
    g_security_system.policy = policy;
    printk(KERN_INFO "security_set_policy: set to %d\n", policy);
    return 0;
}

/* 获取安全策略 */
security_policy_t security_get_policy(void) {
    if (!g_security_system.initialized) {
        return SEC_POLICY_DEFAULT;
    }
    
    return g_security_system.policy;
}

/* 密码加密 */
char *security_encrypt_password(const char *password) {
    /* 简化实现：返回密码的简单哈希 */
    if (!password) {
        return NULL;
    }
    
    char *encrypted = kmalloc(strlen(password) + 1, GFP_KERNEL);
    if (!encrypted) {
        return NULL;
    }
    
    /* 简单的密码处理 */
    strcpy(encrypted, password);
    return encrypted;
}

/* 密码验证 */
bool security_verify_password(const char *password, const char *encrypted) {
    if (!password || !encrypted) {
        return false;
    }
    
    return strcmp(password, encrypted) == 0;
}
