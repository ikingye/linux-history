/*
 * Definitions for diskquota-operations. When diskquota is configured these
 * macros expand to the right source-code.
 *
 * Author:  Marco van Wieringen <mvw@planets.elm.net>
 *
 * Version: $Id: quotaops.h,v 1.2 1998/01/15 16:22:26 ecd Exp $
 *
 */
#ifndef _LINUX_QUOTAOPS_
#define _LINUX_QUOTAOPS_

#include <linux/config.h>
#include <linux/smp_lock.h>

#include <linux/fs.h>

#if defined(CONFIG_QUOTA)

/*
 * declaration of quota_function calls in kernel.
 */
extern void dquot_initialize(struct inode *inode, short type);
extern void dquot_drop(struct inode *inode);
extern int  quota_off(struct super_block *sb, short type);
extern int  sync_dquots(struct super_block *sb, short type);

extern int  dquot_alloc_space(struct inode *inode, qsize_t number, int prealloc);
extern int  dquot_alloc_inode(const struct inode *inode, unsigned long number);

extern void dquot_free_space(struct inode *inode, qsize_t number);
extern void dquot_free_inode(const struct inode *inode, unsigned long number);

extern int  dquot_transfer(struct inode *inode, struct iattr *iattr);

/*
 * Operations supported for diskquotas.
 */
#define sb_any_quota_enabled(sb) ((sb)->s_dquot.flags & (DQUOT_USR_ENABLED | DQUOT_GRP_ENABLED))

static __inline__ void DQUOT_INIT(struct inode *inode)
{
	if (!inode->i_sb)
		BUG();
	lock_kernel();
	if (sb_any_quota_enabled(inode->i_sb) && !IS_NOQUOTA(inode))
		inode->i_sb->dq_op->initialize(inode, -1);
	unlock_kernel();
}

static __inline__ void DQUOT_DROP(struct inode *inode)
{
	lock_kernel();
	if (IS_QUOTAINIT(inode)) {
		if (!inode->i_sb)
			BUG();
		inode->i_sb->dq_op->drop(inode);	/* Ops must be set when there's any quota... */
	}
	unlock_kernel();
}

static __inline__ int DQUOT_PREALLOC_SPACE_NODIRTY(struct inode *inode, qsize_t nr)
{
	lock_kernel();
	if (sb_any_quota_enabled(inode->i_sb)) {
		/* Used space is updated in alloc_space() */
		if (inode->i_sb->dq_op->alloc_space(inode, nr, 1) == NO_QUOTA) {
			unlock_kernel();
			return 1;
		}
	}
	else
		inode_add_bytes(inode, nr);
	unlock_kernel();
	return 0;
}

static __inline__ int DQUOT_PREALLOC_SPACE(struct inode *inode, qsize_t nr)
{
	int ret;
        if (!(ret =  DQUOT_PREALLOC_SPACE_NODIRTY(inode, nr)))
		mark_inode_dirty(inode);
	return ret;
}

static __inline__ int DQUOT_ALLOC_SPACE_NODIRTY(struct inode *inode, qsize_t nr)
{
	lock_kernel();
	if (sb_any_quota_enabled(inode->i_sb)) {
		/* Used space is updated in alloc_space() */
		if (inode->i_sb->dq_op->alloc_space(inode, nr, 0) == NO_QUOTA) {
			unlock_kernel();
			return 1;
		}
	}
	else
		inode_add_bytes(inode, nr);
	unlock_kernel();
	return 0;
}

static __inline__ int DQUOT_ALLOC_SPACE(struct inode *inode, qsize_t nr)
{
	int ret;
	if (!(ret = DQUOT_ALLOC_SPACE_NODIRTY(inode, nr)))
		mark_inode_dirty(inode);
	return ret;
}

static __inline__ int DQUOT_ALLOC_INODE(struct inode *inode)
{
	lock_kernel();
	if (sb_any_quota_enabled(inode->i_sb)) {
		DQUOT_INIT(inode);
		if (inode->i_sb->dq_op->alloc_inode(inode, 1) == NO_QUOTA) {
			unlock_kernel();
			return 1;
		}
	}
	unlock_kernel();
	return 0;
}

static __inline__ void DQUOT_FREE_SPACE_NODIRTY(struct inode *inode, qsize_t nr)
{
	lock_kernel();
	if (sb_any_quota_enabled(inode->i_sb))
		inode->i_sb->dq_op->free_space(inode, nr);
	else
		inode_sub_bytes(inode, nr);
	unlock_kernel();
}

static __inline__ void DQUOT_FREE_SPACE(struct inode *inode, qsize_t nr)
{
	DQUOT_FREE_SPACE_NODIRTY(inode, nr);
	mark_inode_dirty(inode);
}

static __inline__ void DQUOT_FREE_INODE(struct inode *inode)
{
	lock_kernel();
	if (sb_any_quota_enabled(inode->i_sb))
		inode->i_sb->dq_op->free_inode(inode, 1);
	unlock_kernel();
}

static __inline__ int DQUOT_TRANSFER(struct inode *inode, struct iattr *iattr)
{
	lock_kernel();
	if (sb_any_quota_enabled(inode->i_sb) && !IS_NOQUOTA(inode)) {
		DQUOT_INIT(inode);
		if (inode->i_sb->dq_op->transfer(inode, iattr) == NO_QUOTA) {
			unlock_kernel();
			return 1;
		}
	}
	unlock_kernel();
	return 0;
}

#define DQUOT_SYNC(sb)	sync_dquots(sb, -1)
#define DQUOT_OFF(sb)	quota_off(sb, -1)

#else

/*
 * NO-OP when quota not configured.
 */
#define DQUOT_INIT(inode)			do { } while(0)
#define DQUOT_DROP(inode)			do { } while(0)
#define DQUOT_ALLOC_INODE(inode)		(0)
#define DQUOT_FREE_INODE(inode)			do { } while(0)
#define DQUOT_SYNC(sb)				do { } while(0)
#define DQUOT_OFF(sb)				do { } while(0)
#define DQUOT_TRANSFER(inode, iattr)		(0)
extern __inline__ int DQUOT_PREALLOC_SPACE_NODIRTY(struct inode *inode, qsize_t nr)
{
	lock_kernel();
	inode_add_bytes(inode, nr);
	unlock_kernel();
	return 0;
}

extern __inline__ int DQUOT_PREALLOC_SPACE(struct inode *inode, qsize_t nr)
{
	DQUOT_PREALLOC_SPACE_NODIRTY(inode, nr);
	mark_inode_dirty(inode);
	return 0;
}

extern __inline__ int DQUOT_ALLOC_SPACE_NODIRTY(struct inode *inode, qsize_t nr)
{
	lock_kernel();
	inode_add_bytes(inode, nr);
	unlock_kernel();
	return 0;
}

extern __inline__ int DQUOT_ALLOC_SPACE(struct inode *inode, qsize_t nr)
{
	DQUOT_ALLOC_SPACE_NODIRTY(inode, nr);
	mark_inode_dirty(inode);
	return 0;
}

extern __inline__ void DQUOT_FREE_SPACE_NODIRTY(struct inode *inode, qsize_t nr)
{
	lock_kernel();
	inode_sub_bytes(inode, nr);
	unlock_kernel();
}

extern __inline__ void DQUOT_FREE_SPACE(struct inode *inode, qsize_t nr)
{
	DQUOT_FREE_SPACE_NODIRTY(inode, nr);
	mark_inode_dirty(inode);
}	

#endif /* CONFIG_QUOTA */

#define DQUOT_PREALLOC_BLOCK_NODIRTY(inode, nr)	DQUOT_PREALLOC_SPACE_NODIRTY(inode, ((qsize_t)(nr)) << (inode)->i_sb->s_blocksize_bits)
#define DQUOT_PREALLOC_BLOCK(inode, nr)	DQUOT_PREALLOC_SPACE(inode, ((qsize_t)(nr)) << (inode)->i_sb->s_blocksize_bits)
#define DQUOT_ALLOC_BLOCK_NODIRTY(inode, nr) DQUOT_ALLOC_SPACE_NODIRTY(inode, ((qsize_t)(nr)) << (inode)->i_sb->s_blocksize_bits)
#define DQUOT_ALLOC_BLOCK(inode, nr) DQUOT_ALLOC_SPACE(inode, ((qsize_t)(nr)) << (inode)->i_sb->s_blocksize_bits)
#define DQUOT_FREE_BLOCK_NODIRTY(inode, nr) DQUOT_FREE_SPACE_NODIRTY(inode, ((qsize_t)(nr)) << (inode)->i_sb->s_blocksize_bits)
#define DQUOT_FREE_BLOCK(inode, nr) DQUOT_FREE_SPACE(inode, ((qsize_t)(nr)) << (inode)->i_sb->s_blocksize_bits)

#endif /* _LINUX_QUOTAOPS_ */
