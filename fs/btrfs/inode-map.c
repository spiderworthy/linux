#include <linux/module.h>
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"

int btrfs_find_highest_inode(struct btrfs_root *fs_root, u64 *objectid)
{
	struct btrfs_path *path;
	int ret;
	struct btrfs_leaf *l;
	struct btrfs_root *root = fs_root->fs_info->inode_root;
	struct btrfs_key search_key;
	int slot;

	path = btrfs_alloc_path();
	BUG_ON(!path);

	search_key.objectid = (u64)-1;
	search_key.offset = (u64)-1;
	ret = btrfs_search_slot(NULL, root, &search_key, path, 0, 0);
	if (ret < 0)
		goto error;
	BUG_ON(ret == 0);
	if (path->slots[0] > 0) {
		slot = path->slots[0] - 1;
		l = btrfs_buffer_leaf(path->nodes[0]);
		*objectid = btrfs_disk_key_objectid(&l->items[slot].key);
	} else {
		*objectid = BTRFS_FIRST_FREE_OBJECTID;
	}
	ret = 0;
error:
	btrfs_free_path(path);
	return ret;
}

/*
 * walks the btree of allocated inodes and find a hole.
 */
int btrfs_find_free_objectid(struct btrfs_trans_handle *trans,
			     struct btrfs_root *fs_root,
			     u64 dirid, u64 *objectid)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	int ret;
	u64 hole_size = 0;
	int slot = 0;
	u64 last_ino = 0;
	int start_found;
	struct btrfs_leaf *l;
	struct btrfs_root *root = fs_root->fs_info->inode_root;
	struct btrfs_key search_key;
	u64 search_start = dirid;

	path = btrfs_alloc_path();
	BUG_ON(!path);
	search_key.flags = 0;
	btrfs_set_key_type(&search_key, BTRFS_INODE_MAP_ITEM_KEY);

	search_start = fs_root->fs_info->last_inode_alloc;
	search_start = max(search_start, BTRFS_FIRST_FREE_OBJECTID);
	search_key.objectid = search_start;
	search_key.offset = 0;

	btrfs_init_path(path);
	start_found = 0;
	ret = btrfs_search_slot(trans, root, &search_key, path, 0, 0);
	if (ret < 0)
		goto error;

	if (path->slots[0] > 0)
		path->slots[0]--;

	while (1) {
		l = btrfs_buffer_leaf(path->nodes[0]);
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(&l->header)) {
			ret = btrfs_next_leaf(root, path);
			if (ret == 0)
				continue;
			if (ret < 0)
				goto error;
			if (!start_found) {
				*objectid = search_start;
				start_found = 1;
				goto found;
			}
			*objectid = last_ino > search_start ?
				last_ino : search_start;
			goto found;
		}
		btrfs_disk_key_to_cpu(&key, &l->items[slot].key);
		if (key.objectid >= search_start) {
			if (start_found) {
				if (last_ino < search_start)
					last_ino = search_start;
				hole_size = key.objectid - last_ino;
				if (hole_size > 0) {
					*objectid = last_ino;
					goto found;
				}
			}
		}
		start_found = 1;
		last_ino = key.objectid + 1;
		path->slots[0]++;
	}
	// FIXME -ENOSPC
found:
	root->fs_info->last_inode_alloc = *objectid;
	btrfs_release_path(root, path);
	btrfs_free_path(path);
	BUG_ON(*objectid < search_start);
	return 0;
error:
	btrfs_release_path(root, path);
	btrfs_free_path(path);
	return ret;
}

int btrfs_insert_inode_map(struct btrfs_trans_handle *trans,
			   struct btrfs_root *fs_root,
			   u64 objectid, struct btrfs_key *location)
{
	int ret = 0;
	struct btrfs_path *path;
	struct btrfs_inode_map_item *inode_item;
	struct btrfs_key key;
	struct btrfs_root *inode_root = fs_root->fs_info->inode_root;

	key.objectid = objectid;
	key.flags = 0;
	btrfs_set_key_type(&key, BTRFS_INODE_MAP_ITEM_KEY);
	key.offset = 0;
	path = btrfs_alloc_path();
	BUG_ON(!path);
	btrfs_init_path(path);
	ret = btrfs_insert_empty_item(trans, inode_root, path, &key,
				      sizeof(struct btrfs_inode_map_item));
	if (ret)
		goto out;

	inode_item = btrfs_item_ptr(btrfs_buffer_leaf(path->nodes[0]),
				    path->slots[0], struct btrfs_inode_map_item);
	btrfs_cpu_key_to_disk(&inode_item->key, location);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	if (objectid > fs_root->fs_info->highest_inode)
		fs_root->fs_info->highest_inode = objectid;
out:
	btrfs_release_path(inode_root, path);
	btrfs_free_path(path);
	return ret;
}

int btrfs_lookup_inode_map(struct btrfs_trans_handle *trans,
			   struct btrfs_root *fs_root, struct btrfs_path *path,
			   u64 objectid, int mod)
{
	int ret;
	struct btrfs_key key;
	int ins_len = mod < 0 ? -1 : 0;
	int cow = mod != 0;
	struct btrfs_root *inode_root = fs_root->fs_info->inode_root;

	key.objectid = objectid;
	key.flags = 0;
	key.offset = 0;
	btrfs_set_key_type(&key, BTRFS_INODE_MAP_ITEM_KEY);
	ret = btrfs_search_slot(trans, inode_root, &key, path, ins_len, cow);
	return ret;
}

