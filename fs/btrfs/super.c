/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <linux/module.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/smp_lock.h>
#include <linux/backing-dev.h>
#include <linux/mpage.h>
#include <linux/swap.h>
#include <linux/writeback.h>
#include <linux/statfs.h>
#include <linux/compat.h>
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "btrfs_inode.h"
#include "ioctl.h"
#include "print-tree.h"

#define BTRFS_SUPER_MAGIC 0x9123682E

static struct super_operations btrfs_super_ops;

static void btrfs_put_super (struct super_block * sb)
{
	struct btrfs_root *root = btrfs_sb(sb);
	int ret;

	ret = close_ctree(root);
	if (ret) {
		printk("close ctree returns %d\n", ret);
	}
	sb->s_fs_info = NULL;
}

static int btrfs_fill_super(struct super_block * sb, void * data, int silent)
{
	struct inode * inode;
	struct dentry * root_dentry;
	struct btrfs_super_block *disk_super;
	struct btrfs_root *tree_root;
	struct btrfs_inode *bi;
	int err;

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_magic = BTRFS_SUPER_MAGIC;
	sb->s_op = &btrfs_super_ops;
	sb->s_time_gran = 1;

	tree_root = open_ctree(sb);

	if (!tree_root || IS_ERR(tree_root)) {
		printk("btrfs: open_ctree failed\n");
		return -EIO;
	}
	sb->s_fs_info = tree_root;
	disk_super = tree_root->fs_info->disk_super;
	inode = btrfs_iget_locked(sb, btrfs_super_root_dir(disk_super),
				  tree_root);
	bi = BTRFS_I(inode);
	bi->location.objectid = inode->i_ino;
	bi->location.offset = 0;
	bi->location.flags = 0;
	bi->root = tree_root;

	btrfs_set_key_type(&bi->location, BTRFS_INODE_ITEM_KEY);

	if (!inode) {
		err = -ENOMEM;
		goto fail_close;
	}
	if (inode->i_state & I_NEW) {
		btrfs_read_locked_inode(inode);
		unlock_new_inode(inode);
	}

	root_dentry = d_alloc_root(inode);
	if (!root_dentry) {
		iput(inode);
		err = -ENOMEM;
		goto fail_close;
	}
	sb->s_root = root_dentry;
	btrfs_transaction_queue_work(tree_root, HZ * 30);
	return 0;

fail_close:
	close_ctree(tree_root);
	return err;
}

static int btrfs_sync_fs(struct super_block *sb, int wait)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root;
	int ret;
	root = btrfs_sb(sb);

	sb->s_dirt = 0;
	if (!wait) {
		filemap_flush(root->fs_info->btree_inode->i_mapping);
		return 0;
	}
	btrfs_clean_old_snapshots(root);
	mutex_lock(&root->fs_info->fs_mutex);
	btrfs_defrag_dirty_roots(root->fs_info);
	trans = btrfs_start_transaction(root, 1);
	ret = btrfs_commit_transaction(trans, root);
	sb->s_dirt = 0;
	mutex_unlock(&root->fs_info->fs_mutex);
	return ret;
}

static void btrfs_write_super(struct super_block *sb)
{
	sb->s_dirt = 0;
}

static int btrfs_get_sb(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data, struct vfsmount *mnt)
{
	return get_sb_bdev(fs_type, flags, dev_name, data,
			   btrfs_fill_super, mnt);
}

static int btrfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct btrfs_root *root = btrfs_sb(dentry->d_sb);
	struct btrfs_super_block *disk_super = &root->fs_info->super_copy;

	buf->f_namelen = BTRFS_NAME_LEN;
	buf->f_blocks = btrfs_super_total_blocks(disk_super);
	buf->f_bfree = buf->f_blocks - btrfs_super_blocks_used(disk_super);
	buf->f_bavail = buf->f_bfree;
	buf->f_bsize = dentry->d_sb->s_blocksize;
	buf->f_type = BTRFS_SUPER_MAGIC;
	return 0;
}

static struct file_system_type btrfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "btrfs",
	.get_sb		= btrfs_get_sb,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static struct super_operations btrfs_super_ops = {
	.delete_inode	= btrfs_delete_inode,
	.put_super	= btrfs_put_super,
	.read_inode	= btrfs_read_locked_inode,
	.write_super	= btrfs_write_super,
	.sync_fs	= btrfs_sync_fs,
	.write_inode	= btrfs_write_inode,
	.dirty_inode	= btrfs_dirty_inode,
	.alloc_inode	= btrfs_alloc_inode,
	.destroy_inode	= btrfs_destroy_inode,
	.statfs		= btrfs_statfs,
};

static int __init init_btrfs_fs(void)
{
	int err;
	btrfs_init_transaction_sys();
	err = btrfs_init_cachep();
	if (err)
		return err;
	extent_map_init();
	return register_filesystem(&btrfs_fs_type);
}

static void __exit exit_btrfs_fs(void)
{
	btrfs_exit_transaction_sys();
	btrfs_destroy_cachep();
	extent_map_exit();
	unregister_filesystem(&btrfs_fs_type);
}

module_init(init_btrfs_fs)
module_exit(exit_btrfs_fs)

MODULE_LICENSE("GPL");
