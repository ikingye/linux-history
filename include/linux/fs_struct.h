#ifndef _LINUX_FS_STRUCT_H
#define _LINUX_FS_STRUCT_H
#ifdef __KERNEL__

struct fs_struct {
	atomic_t count;
	int umask;
	struct dentry * root, * pwd, * altroot;
	struct vfsmount * rootmnt, * pwdmnt, * altrootmnt;
};

#define INIT_FS { \
	ATOMIC_INIT(1), \
	0022, \
	NULL, NULL, NULL, NULL, NULL, NULL \
}

extern void exit_fs(struct task_struct *);
extern void set_fs_altroot(void);

/*
 * Replace the fs->{rootmnt,root} with {mnt,dentry}. Put the old values.
 * It can block. Requires the big lock held.
 */

static inline void set_fs_root(struct fs_struct *fs,
	struct vfsmount *mnt,
	struct dentry *dentry)
{
	struct dentry *old_root = fs->root;
	struct vfsmount *old_rootmnt = fs->rootmnt;
	fs->rootmnt = mntget(mnt);
	fs->root = dget(dentry);
	dput(old_root);
	mntput(old_rootmnt);
}

/*
 * Replace the fs->{pwdmnt,pwd} with {mnt,dentry}. Put the old values.
 * It can block. Requires the big lock held.
 */

static inline void set_fs_pwd(struct fs_struct *fs,
	struct vfsmount *mnt,
	struct dentry *dentry)
{
	struct dentry *old_pwd = fs->pwd;
	struct vfsmount *old_pwdmnt = fs->pwdmnt;
	fs->pwdmnt = mntget(mnt);
	fs->pwd = dget(dentry);
	dput(old_pwd);
	mntput(old_pwdmnt);
}

struct fs_struct *copy_fs_struct(struct fs_struct *old);
void put_fs_struct(struct fs_struct *fs);

#endif
#endif
