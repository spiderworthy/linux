#include <stdio.h>
#include <stdlib.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"

void btrfs_print_leaf(struct btrfs_leaf *l)
{
	int i;
	u32 nr = btrfs_header_nritems(&l->header);
	struct btrfs_item *item;
	struct btrfs_extent_item *ei;
	printf("leaf %Lu total ptrs %d free space %d\n",
		btrfs_header_blocknr(&l->header), nr, btrfs_leaf_free_space(l));
	fflush(stdout);
	for (i = 0 ; i < nr ; i++) {
		item = l->items + i;
		printf("\titem %d key (%Lu %u %Lu) itemoff %d itemsize %d\n",
			i,
			btrfs_key_objectid(&item->key),
			btrfs_key_flags(&item->key),
			btrfs_key_offset(&item->key),
			btrfs_item_offset(item),
			btrfs_item_size(item));
		fflush(stdout);
		printf("\t\titem data %.*s\n", btrfs_item_size(item),
			l->data + btrfs_item_offset(item));
		ei = (struct btrfs_extent_item *)(l->data +
						  btrfs_item_offset(item));
		printf("\t\textent data refs %u owner %Lu\n", ei->refs,
			ei->owner);
		fflush(stdout);
	}
}
void btrfs_print_tree(struct btrfs_root *root, struct btrfs_buffer *t)
{
	int i;
	u32 nr;
	struct btrfs_node *c;

	if (!t)
		return;
	c = &t->node;
	nr = btrfs_header_nritems(&c->header);
	if (btrfs_is_leaf(c)) {
		btrfs_print_leaf((struct btrfs_leaf *)c);
		return;
	}
	printf("node %Lu level %d total ptrs %d free spc %u\n", t->blocknr,
	        btrfs_header_level(&c->header), nr,
		(u32)NODEPTRS_PER_BLOCK - nr);
	fflush(stdout);
	for (i = 0; i < nr; i++) {
		printf("\tkey %d (%Lu %u %Lu) block %Lu\n",
		       i,
		       c->keys[i].objectid, c->keys[i].flags, c->keys[i].offset,
		       btrfs_node_blockptr(c, i));
		fflush(stdout);
	}
	for (i = 0; i < nr; i++) {
		struct btrfs_buffer *next_buf = read_tree_block(root,
						btrfs_node_blockptr(c, i));
		struct btrfs_node *next = &next_buf->node;
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

