#include <linux/module.h>
#include "ctree.h"
#include "disk-io.h"

void btrfs_print_leaf(struct btrfs_root *root, struct btrfs_leaf *l)
{
	int i;
	u32 nr = btrfs_header_nritems(&l->header);
	struct btrfs_item *item;
	struct btrfs_extent_item *ei;
	struct btrfs_root_item *ri;
	struct btrfs_dir_item *di;
	struct btrfs_inode_item *ii;
	u32 type;

	printk("leaf %Lu total ptrs %d free space %d\n",
		btrfs_header_blocknr(&l->header), nr,
		btrfs_leaf_free_space(root, l));
	for (i = 0 ; i < nr ; i++) {
		item = l->items + i;
		type = btrfs_disk_key_type(&item->key);
		printk("\titem %d key (%Lu %u %Lu) itemoff %d itemsize %d\n",
			i,
			btrfs_disk_key_objectid(&item->key),
			btrfs_disk_key_flags(&item->key),
			btrfs_disk_key_offset(&item->key),
			btrfs_item_offset(item),
			btrfs_item_size(item));
		switch (type) {
		case BTRFS_INODE_ITEM_KEY:
			ii = btrfs_item_ptr(l, i, struct btrfs_inode_item);
			printk("\t\tinode generation %Lu size %Lu mode %o\n",
			       btrfs_inode_generation(ii),
			       btrfs_inode_size(ii),
			       btrfs_inode_mode(ii));
			break;
		case BTRFS_DIR_ITEM_KEY:
			di = btrfs_item_ptr(l, i, struct btrfs_dir_item);
			printk("\t\tdir oid %Lu flags %u type %u\n",
				btrfs_disk_key_objectid(&di->location),
				btrfs_dir_flags(di),
				btrfs_dir_type(di));
			printk("\t\tname %.*s\n",
			       btrfs_dir_name_len(di),(char *)(di + 1));
			break;
		case BTRFS_ROOT_ITEM_KEY:
			ri = btrfs_item_ptr(l, i, struct btrfs_root_item);
			printk("\t\troot data blocknr %Lu refs %u\n",
				btrfs_root_blocknr(ri), btrfs_root_refs(ri));
			break;
		case BTRFS_EXTENT_ITEM_KEY:
			ei = btrfs_item_ptr(l, i, struct btrfs_extent_item);
			printk("\t\textent data refs %u\n",
				btrfs_extent_refs(ei));
			break;
		case BTRFS_STRING_ITEM_KEY:
			printk("\t\titem data %.*s\n", btrfs_item_size(item),
				btrfs_leaf_data(l) + btrfs_item_offset(item));
			break;
		};
	}
}

void btrfs_print_tree(struct btrfs_root *root, struct buffer_head *t)
{
	int i;
	u32 nr;
	struct btrfs_node *c;

	if (!t)
		return;
	c = btrfs_buffer_node(t);
	nr = btrfs_header_nritems(&c->header);
	if (btrfs_is_leaf(c)) {
		btrfs_print_leaf(root, (struct btrfs_leaf *)c);
		return;
	}
	printk("node %Lu level %d total ptrs %d free spc %u\n",
	       btrfs_header_blocknr(&c->header),
	       btrfs_header_level(&c->header), nr,
	       (u32)BTRFS_NODEPTRS_PER_BLOCK(root) - nr);
	for (i = 0; i < nr; i++) {
		printk("\tkey %d (%Lu %u %Lu) block %Lu\n",
		       i,
		       c->ptrs[i].key.objectid,
		       c->ptrs[i].key.flags,
		       c->ptrs[i].key.offset,
		       btrfs_node_blockptr(c, i));
	}
	for (i = 0; i < nr; i++) {
		struct buffer_head *next_buf = read_tree_block(root,
						btrfs_node_blockptr(c, i));
		struct btrfs_node *next = btrfs_buffer_node(next_buf);
		if (btrfs_is_leaf(next) &&
		    btrfs_header_level(&c->header) != 1)
			BUG();
		if (btrfs_header_level(&next->header) !=
			btrfs_header_level(&c->header) - 1)
			BUG();
		btrfs_print_tree(root, next_buf);
		btrfs_block_release(root, next_buf);
	}
}

