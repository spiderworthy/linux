#include <linux/module.h>
#include "ctree.h"
#include "disk-io.h"
#include "hash.h"
#include "transaction.h"

int btrfs_insert_dir_item(struct btrfs_trans_handle *trans, struct btrfs_root
			  *root, const char *name, int name_len, u64 dir, u64
			  objectid, u8 type)
{
	int ret = 0;
	struct btrfs_path *path;
	struct btrfs_dir_item *dir_item;
	char *name_ptr;
	struct btrfs_key key;
	u32 data_size;

	key.objectid = dir;
	key.flags = 0;
	btrfs_set_key_type(&key, BTRFS_DIR_ITEM_KEY);
	ret = btrfs_name_hash(name, name_len, &key.offset);
	BUG_ON(ret);
	path = btrfs_alloc_path();
	btrfs_init_path(path);
	data_size = sizeof(*dir_item) + name_len;
	ret = btrfs_insert_empty_item(trans, root, path, &key, data_size);
	if (ret)
		goto out;

	dir_item = btrfs_item_ptr(btrfs_buffer_leaf(path->nodes[0]),
				  path->slots[0],
				  struct btrfs_dir_item);
	btrfs_set_dir_objectid(dir_item, objectid);
	btrfs_set_dir_type(dir_item, type);
	btrfs_set_dir_flags(dir_item, 0);
	btrfs_set_dir_name_len(dir_item, name_len);
	name_ptr = (char *)(dir_item + 1);
	btrfs_memcpy(root, path->nodes[0]->b_data, name_ptr, name, name_len);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	btrfs_release_path(root, path);

	btrfs_set_key_type(&key, BTRFS_DIR_INDEX_KEY);
	key.offset = objectid;
	ret = btrfs_insert_empty_item(trans, root, path, &key, data_size);
	// FIXME clear the dirindex bit
	if (ret)
		goto out;

	dir_item = btrfs_item_ptr(btrfs_buffer_leaf(path->nodes[0]),
				  path->slots[0],
				  struct btrfs_dir_item);
	btrfs_set_dir_objectid(dir_item, objectid);
	btrfs_set_dir_type(dir_item, type);
	btrfs_set_dir_flags(dir_item, 0);
	btrfs_set_dir_name_len(dir_item, name_len);
	name_ptr = (char *)(dir_item + 1);
	btrfs_memcpy(root, path->nodes[0]->b_data, name_ptr, name, name_len);
	btrfs_mark_buffer_dirty(path->nodes[0]);
out:
	btrfs_release_path(root, path);
	btrfs_free_path(path);
	return ret;
}

int btrfs_lookup_dir_item(struct btrfs_trans_handle *trans, struct btrfs_root
			  *root, struct btrfs_path *path, u64 dir,
			  const char *name, int name_len, int mod)
{
	int ret;
	struct btrfs_key key;
	int ins_len = mod < 0 ? -1 : 0;
	int cow = mod != 0;

	key.objectid = dir;
	key.flags = 0;
	btrfs_set_key_type(&key, BTRFS_DIR_ITEM_KEY);
	ret = btrfs_name_hash(name, name_len, &key.offset);
	BUG_ON(ret);
	ret = btrfs_search_slot(trans, root, &key, path, ins_len, cow);
	return ret;
}

int btrfs_match_dir_item_name(struct btrfs_root *root,
			      struct btrfs_path *path,
			      const char *name, int name_len)
{
	struct btrfs_dir_item *dir_item;
	char *name_ptr;

	dir_item = btrfs_item_ptr(btrfs_buffer_leaf(path->nodes[0]),
				  path->slots[0],
				  struct btrfs_dir_item);
	if (btrfs_dir_name_len(dir_item) != name_len)
		return 0;
	name_ptr = (char *)(dir_item + 1);
	if (memcmp(name_ptr, name, name_len))
		return 0;
	return 1;
}
