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
#include <linux/fs.h>
#include <linux/sched.h>
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"

static int total_trans = 0;
extern struct kmem_cache *btrfs_trans_handle_cachep;
extern struct kmem_cache *btrfs_transaction_cachep;

static struct workqueue_struct *trans_wq;

#define BTRFS_ROOT_TRANS_TAG 0

static void put_transaction(struct btrfs_transaction *transaction)
{
	WARN_ON(transaction->use_count == 0);
	transaction->use_count--;
	if (transaction->use_count == 0) {
		WARN_ON(total_trans == 0);
		total_trans--;
		list_del_init(&transaction->list);
		memset(transaction, 0, sizeof(*transaction));
		kmem_cache_free(btrfs_transaction_cachep, transaction);
	}
}

static int join_transaction(struct btrfs_root *root)
{
	struct btrfs_transaction *cur_trans;
	cur_trans = root->fs_info->running_transaction;
	if (!cur_trans) {
		cur_trans = kmem_cache_alloc(btrfs_transaction_cachep,
					     GFP_NOFS);
		total_trans++;
		BUG_ON(!cur_trans);
		root->fs_info->generation++;
		root->fs_info->running_transaction = cur_trans;
		cur_trans->num_writers = 0;
		cur_trans->transid = root->fs_info->generation;
		init_waitqueue_head(&cur_trans->writer_wait);
		init_waitqueue_head(&cur_trans->commit_wait);
		cur_trans->in_commit = 0;
		cur_trans->use_count = 1;
		cur_trans->commit_done = 0;
		cur_trans->start_time = get_seconds();
		list_add_tail(&cur_trans->list, &root->fs_info->trans_list);
		init_bit_radix(&cur_trans->dirty_pages);
	}
	cur_trans->num_writers++;
	return 0;
}

struct btrfs_trans_handle *btrfs_start_transaction(struct btrfs_root *root,
						   int num_blocks)
{
	struct btrfs_trans_handle *h =
		kmem_cache_alloc(btrfs_trans_handle_cachep, GFP_NOFS);
	int ret;
	u64 running_trans_id;

	mutex_lock(&root->fs_info->trans_mutex);
	ret = join_transaction(root);
	BUG_ON(ret);
	running_trans_id = root->fs_info->running_transaction->transid;

	if (root != root->fs_info->tree_root && root->last_trans <
	    running_trans_id) {
		WARN_ON(root == root->fs_info->extent_root);
		WARN_ON(root->ref_cows != 1);
		if (root->root_item.refs != 0) {
			radix_tree_tag_set(&root->fs_info->fs_roots_radix,
					   (unsigned long)root->root_key.objectid,
					   BTRFS_ROOT_TRANS_TAG);
			root->commit_root = root->node;
			get_bh(root->node);
		} else {
			WARN_ON(1);
		}
	}
	root->last_trans = running_trans_id;
	h->transid = running_trans_id;
	h->transaction = root->fs_info->running_transaction;
	h->blocks_reserved = num_blocks;
	h->blocks_used = 0;
	h->block_group = NULL;
	root->fs_info->running_transaction->use_count++;
	mutex_unlock(&root->fs_info->trans_mutex);
	return h;
}

int btrfs_end_transaction(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root)
{
	struct btrfs_transaction *cur_trans;

	mutex_lock(&root->fs_info->trans_mutex);
	cur_trans = root->fs_info->running_transaction;
	WARN_ON(cur_trans != trans->transaction);
	WARN_ON(cur_trans->num_writers < 1);
	cur_trans->num_writers--;
	if (waitqueue_active(&cur_trans->writer_wait))
		wake_up(&cur_trans->writer_wait);
	put_transaction(cur_trans);
	mutex_unlock(&root->fs_info->trans_mutex);
	memset(trans, 0, sizeof(*trans));
	kmem_cache_free(btrfs_trans_handle_cachep, trans);
	return 0;
}


int btrfs_write_and_wait_transaction(struct btrfs_trans_handle *trans,
				     struct btrfs_root *root)
{
	unsigned long gang[16];
	int ret;
	int i;
	int err;
	int werr = 0;
	struct page *page;
	struct radix_tree_root *dirty_pages;
	struct inode *btree_inode = root->fs_info->btree_inode;

	if (!trans || !trans->transaction) {
		return filemap_write_and_wait(btree_inode->i_mapping);
	}
	dirty_pages = &trans->transaction->dirty_pages;
	while(1) {
		ret = find_first_radix_bit(dirty_pages, gang,
					   0, ARRAY_SIZE(gang));
		if (!ret)
			break;
		for (i = 0; i < ret; i++) {
			/* FIXME EIO */
			clear_radix_bit(dirty_pages, gang[i]);
			page = find_lock_page(btree_inode->i_mapping,
					      gang[i]);
			if (!page)
				continue;
			err = write_one_page(page, 0);
			if (err)
				werr = err;
			page_cache_release(page);
		}
	}
	err = filemap_fdatawait(btree_inode->i_mapping);
	if (err)
		werr = err;
	return werr;
}

int btrfs_commit_tree_roots(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root)
{
	int ret;
	u64 old_extent_block;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *extent_root = fs_info->extent_root;

	btrfs_write_dirty_block_groups(trans, extent_root);
	while(1) {
		old_extent_block = btrfs_root_blocknr(&extent_root->root_item);
		if (old_extent_block == bh_blocknr(extent_root->node))
			break;
		btrfs_set_root_blocknr(&extent_root->root_item,
				       bh_blocknr(extent_root->node));
		ret = btrfs_update_root(trans, tree_root,
					&extent_root->root_key,
					&extent_root->root_item);
		BUG_ON(ret);
		btrfs_write_dirty_block_groups(trans, extent_root);
	}
	return 0;
}

static int wait_for_commit(struct btrfs_root *root,
			   struct btrfs_transaction *commit)
{
	DEFINE_WAIT(wait);
	mutex_lock(&root->fs_info->trans_mutex);
	while(!commit->commit_done) {
		prepare_to_wait(&commit->commit_wait, &wait,
				TASK_UNINTERRUPTIBLE);
		if (commit->commit_done)
			break;
		mutex_unlock(&root->fs_info->trans_mutex);
		schedule();
		mutex_lock(&root->fs_info->trans_mutex);
	}
	mutex_unlock(&root->fs_info->trans_mutex);
	finish_wait(&commit->commit_wait, &wait);
	return 0;
}

struct dirty_root {
	struct list_head list;
	struct btrfs_key snap_key;
	struct buffer_head *commit_root;
	struct btrfs_root *root;
	int free_on_drop;
};

int btrfs_add_dead_root(struct btrfs_root *root, struct list_head *dead_list)
{
	struct dirty_root *dirty;

	dirty = kmalloc(sizeof(*dirty), GFP_NOFS);
	if (!dirty)
		return -ENOMEM;
	memcpy(&dirty->snap_key, &root->root_key, sizeof(root->root_key));
	dirty->commit_root = root->node;
	dirty->root = root;
	dirty->free_on_drop = 1;
	list_add(&dirty->list, dead_list);
	return 0;
}

static int add_dirty_roots(struct btrfs_trans_handle *trans,
			   struct radix_tree_root *radix,
			   struct list_head *list)
{
	struct dirty_root *dirty;
	struct btrfs_root *gang[8];
	struct btrfs_root *root;
	struct btrfs_root_item tmp_item;
	int i;
	int ret;
	int err = 0;
	u32 refs;

	while(1) {
		ret = radix_tree_gang_lookup_tag(radix, (void **)gang, 0,
						 ARRAY_SIZE(gang),
						 BTRFS_ROOT_TRANS_TAG);
		if (ret == 0)
			break;
		for (i = 0; i < ret; i++) {
			root = gang[i];
			radix_tree_tag_clear(radix,
				     (unsigned long)root->root_key.objectid,
				     BTRFS_ROOT_TRANS_TAG);
			if (root->commit_root == root->node) {
				WARN_ON(bh_blocknr(root->node) !=
					btrfs_root_blocknr(&root->root_item));
				brelse(root->commit_root);
				root->commit_root = NULL;
				continue;
			}
			dirty = kmalloc(sizeof(*dirty), GFP_NOFS);
			BUG_ON(!dirty);
			memcpy(&dirty->snap_key, &root->root_key,
			       sizeof(root->root_key));
			dirty->commit_root = root->commit_root;
			root->commit_root = NULL;
			dirty->root = root;
			dirty->free_on_drop = 0;
			memcpy(&tmp_item, &root->root_item, sizeof(tmp_item));

			root->root_key.offset = root->fs_info->generation;
			btrfs_set_root_blocknr(&root->root_item,
					       bh_blocknr(root->node));
			err = btrfs_insert_root(trans, root->fs_info->tree_root,
						&root->root_key,
						&root->root_item);
			if (err)
				break;
			refs = btrfs_root_refs(&tmp_item);
			btrfs_set_root_refs(&tmp_item, refs - 1);
			err = btrfs_update_root(trans, root->fs_info->tree_root,
						&dirty->snap_key,
						&tmp_item);

			BUG_ON(err);
			if (refs == 1)
				list_add(&dirty->list, list);
			else
				kfree(dirty);
		}
	}
	return err;
}

static int drop_dirty_roots(struct btrfs_root *tree_root,
			    struct list_head *list)
{
	struct dirty_root *dirty;
	struct btrfs_trans_handle *trans;
	int ret = 0;
	while(!list_empty(list)) {
		mutex_lock(&tree_root->fs_info->fs_mutex);
		dirty = list_entry(list->next, struct dirty_root, list);
		list_del_init(&dirty->list);

		trans = btrfs_start_transaction(tree_root, 1);
		ret = btrfs_drop_snapshot(trans, dirty->root,
					  dirty->commit_root);
		BUG_ON(ret);
		ret = btrfs_del_root(trans, tree_root, &dirty->snap_key);
		if (ret)
			break;
		ret = btrfs_end_transaction(trans, tree_root);
		BUG_ON(ret);

		if (dirty->free_on_drop)
			kfree(dirty->root);
		kfree(dirty);
		mutex_unlock(&tree_root->fs_info->fs_mutex);
		btrfs_btree_balance_dirty(tree_root);
	}
	return ret;
}

int btrfs_commit_transaction(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root)
{
	int ret = 0;
	struct btrfs_transaction *cur_trans;
	struct btrfs_transaction *prev_trans = NULL;
	struct list_head dirty_fs_roots;
	struct radix_tree_root pinned_copy;
	DEFINE_WAIT(wait);

	init_bit_radix(&pinned_copy);
	INIT_LIST_HEAD(&dirty_fs_roots);

	mutex_lock(&root->fs_info->trans_mutex);
	if (trans->transaction->in_commit) {
		cur_trans = trans->transaction;
		trans->transaction->use_count++;
		mutex_unlock(&root->fs_info->trans_mutex);
		btrfs_end_transaction(trans, root);

		mutex_unlock(&root->fs_info->fs_mutex);
		ret = wait_for_commit(root, cur_trans);
		BUG_ON(ret);
		put_transaction(cur_trans);
		mutex_lock(&root->fs_info->fs_mutex);
		return 0;
	}
	trans->transaction->in_commit = 1;
	cur_trans = trans->transaction;
	if (cur_trans->list.prev != &root->fs_info->trans_list) {
		prev_trans = list_entry(cur_trans->list.prev,
					struct btrfs_transaction, list);
		if (!prev_trans->commit_done) {
			prev_trans->use_count++;
			mutex_unlock(&root->fs_info->fs_mutex);
			mutex_unlock(&root->fs_info->trans_mutex);

			wait_for_commit(root, prev_trans);
			put_transaction(prev_trans);

			mutex_lock(&root->fs_info->fs_mutex);
			mutex_lock(&root->fs_info->trans_mutex);
		}
	}
	while (trans->transaction->num_writers > 1) {
		WARN_ON(cur_trans != trans->transaction);
		prepare_to_wait(&trans->transaction->writer_wait, &wait,
				TASK_UNINTERRUPTIBLE);
		if (trans->transaction->num_writers <= 1)
			break;
		mutex_unlock(&root->fs_info->fs_mutex);
		mutex_unlock(&root->fs_info->trans_mutex);
		schedule();
		mutex_lock(&root->fs_info->fs_mutex);
		mutex_lock(&root->fs_info->trans_mutex);
		finish_wait(&trans->transaction->writer_wait, &wait);
	}
	finish_wait(&trans->transaction->writer_wait, &wait);
	WARN_ON(cur_trans != trans->transaction);
	ret = add_dirty_roots(trans, &root->fs_info->fs_roots_radix,
			      &dirty_fs_roots);
	BUG_ON(ret);

	ret = btrfs_commit_tree_roots(trans, root);
	BUG_ON(ret);

	cur_trans = root->fs_info->running_transaction;
	root->fs_info->running_transaction = NULL;
	btrfs_set_super_generation(&root->fs_info->super_copy,
				   cur_trans->transid);
	btrfs_set_super_root(&root->fs_info->super_copy,
			     bh_blocknr(root->fs_info->tree_root->node));
	memcpy(root->fs_info->disk_super, &root->fs_info->super_copy,
	       sizeof(root->fs_info->super_copy));

	btrfs_copy_pinned(root, &pinned_copy);

	mutex_unlock(&root->fs_info->trans_mutex);
	mutex_unlock(&root->fs_info->fs_mutex);
	ret = btrfs_write_and_wait_transaction(trans, root);
	BUG_ON(ret);
	write_ctree_super(trans, root);
	mutex_lock(&root->fs_info->fs_mutex);
	btrfs_finish_extent_commit(trans, root, &pinned_copy);
	mutex_lock(&root->fs_info->trans_mutex);
	cur_trans->commit_done = 1;
	wake_up(&cur_trans->commit_wait);
	put_transaction(cur_trans);
	put_transaction(cur_trans);
	if (root->fs_info->closing)
		list_splice_init(&root->fs_info->dead_roots, &dirty_fs_roots);
	else
		list_splice_init(&dirty_fs_roots, &root->fs_info->dead_roots);
	mutex_unlock(&root->fs_info->trans_mutex);
	kmem_cache_free(btrfs_trans_handle_cachep, trans);

	if (root->fs_info->closing) {
		mutex_unlock(&root->fs_info->fs_mutex);
		drop_dirty_roots(root->fs_info->tree_root, &dirty_fs_roots);
		mutex_lock(&root->fs_info->fs_mutex);
	}
	return ret;
}

void btrfs_transaction_cleaner(struct work_struct *work)
{
	struct btrfs_fs_info *fs_info = container_of(work,
						     struct btrfs_fs_info,
						     trans_work.work);

	struct btrfs_root *root = fs_info->tree_root;
	struct btrfs_transaction *cur;
	struct btrfs_trans_handle *trans;
	struct list_head dirty_roots;
	unsigned long now;
	unsigned long delay = HZ * 30;
	int ret;

	INIT_LIST_HEAD(&dirty_roots);
	mutex_lock(&root->fs_info->fs_mutex);
	mutex_lock(&root->fs_info->trans_mutex);
	cur = root->fs_info->running_transaction;
	if (!cur) {
		mutex_unlock(&root->fs_info->trans_mutex);
		goto out;
	}
	now = get_seconds();
	if (now < cur->start_time || now - cur->start_time < 30) {
		mutex_unlock(&root->fs_info->trans_mutex);
		delay = HZ * 5;
		goto out;
	}
	mutex_unlock(&root->fs_info->trans_mutex);
	trans = btrfs_start_transaction(root, 1);
	ret = btrfs_commit_transaction(trans, root);
out:
	mutex_unlock(&root->fs_info->fs_mutex);

	mutex_lock(&root->fs_info->trans_mutex);
	list_splice_init(&root->fs_info->dead_roots, &dirty_roots);
	mutex_unlock(&root->fs_info->trans_mutex);

	if (!list_empty(&dirty_roots)) {
		drop_dirty_roots(root, &dirty_roots);
	}
	btrfs_transaction_queue_work(root, delay);
}

void btrfs_transaction_queue_work(struct btrfs_root *root, int delay)
{
	queue_delayed_work(trans_wq, &root->fs_info->trans_work, delay);
}

void btrfs_transaction_flush_work(struct btrfs_root *root)
{
	cancel_rearming_delayed_workqueue(trans_wq, &root->fs_info->trans_work);
	flush_workqueue(trans_wq);
}

void __init btrfs_init_transaction_sys(void)
{
	trans_wq = create_workqueue("btrfs");
}

void __exit btrfs_exit_transaction_sys(void)
{
	destroy_workqueue(trans_wq);
}

