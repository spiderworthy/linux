#ifndef __CTREE__
#define __CTREE__

#include "list.h"

#define CTREE_BLOCKSIZE 1024

/*
 * the key defines the order in the tree, and so it also defines (optimal)
 * block layout.  objectid corresonds to the inode number.  The flags
 * tells us things about the object, and is a kind of stream selector.
 * so for a given inode, keys with flags of 1 might refer to the inode
 * data, flags of 2 may point to file data in the btree and flags == 3
 * may point to extents.
 *
 * offset is the starting byte offset for this key in the stream.
 */
struct key {
	u64 objectid;
	u32 flags;
	u64 offset;
} __attribute__ ((__packed__));

/*
 * every tree block (leaf or node) starts with this header.
 */
struct btrfs_header {
	__le64 fsid[2]; /* FS specific uuid */
	__le64 blocknr; /* which block this node is supposed to live in */
	__le64 parentid; /* objectid of the tree root */
	__le32 csum;
	__le32 ham;
	__le16 nritems;
	__le16 flags;
	/* generation flags to be added */
} __attribute__ ((__packed__));

#define MAX_LEVEL 8
#define NODEPTRS_PER_BLOCK ((CTREE_BLOCKSIZE - sizeof(struct btrfs_header)) / \
			    (sizeof(struct key) + sizeof(u64)))

struct tree_buffer;

/*
 * in ram representation of the tree.  extent_root is used for all allocations
 * and for the extent tree extent_root root.  current_insert is used
 * only for the extent tree.
 */
struct ctree_root {
	struct tree_buffer *node;
	struct tree_buffer *commit_root;
	struct ctree_root *extent_root;
	struct key current_insert;
	struct key last_insert;
	int fp;
	struct radix_tree_root cache_radix;
	struct radix_tree_root pinned_radix;
	struct list_head trans;
	struct list_head cache;
	int cache_size;
};

/*
 * describes a tree on disk
 */
struct ctree_root_info {
	u64 fsid[2]; /* FS specific uuid */
	u64 blocknr; /* blocknr of this block */
	u64 objectid; /* inode number of this root */
	u64 tree_root; /* the tree root block */
	u32 csum;
	u32 ham;
	u64 snapuuid[2]; /* root specific uuid */
} __attribute__ ((__packed__));

/*
 * the super block basically lists the main trees of the FS
 * it currently lacks any block count etc etc
 */
struct ctree_super_block {
	struct ctree_root_info root_info;
	struct ctree_root_info extent_info;
} __attribute__ ((__packed__));

/*
 * A leaf is full of items.  The exact type of item is defined by
 * the key flags parameter.  offset and size tell us where to find
 * the item in the leaf (relative to the start of the data area)
 */
struct item {
	struct key key;
	u16 offset;
	u16 size;
} __attribute__ ((__packed__));

/*
 * leaves have an item area and a data area:
 * [item0, item1....itemN] [free space] [dataN...data1, data0]
 *
 * The data is separate from the items to get the keys closer together
 * during searches.
 */
#define LEAF_DATA_SIZE (CTREE_BLOCKSIZE - sizeof(struct btrfs_header))
struct leaf {
	struct btrfs_header header;
	union {
		struct item items[LEAF_DATA_SIZE/sizeof(struct item)];
		u8 data[CTREE_BLOCKSIZE-sizeof(struct btrfs_header)];
	};
} __attribute__ ((__packed__));

/*
 * all non-leaf blocks are nodes, they hold only keys and pointers to
 * other blocks
 */
struct node {
	struct btrfs_header header;
	struct key keys[NODEPTRS_PER_BLOCK];
	u64 blockptrs[NODEPTRS_PER_BLOCK];
} __attribute__ ((__packed__));

/*
 * items in the extent btree are used to record the objectid of the
 * owner of the block and the number of references
 */
struct extent_item {
	u32 refs;
	u64 owner;
} __attribute__ ((__packed__));

/*
 * ctree_paths remember the path taken from the root down to the leaf.
 * level 0 is always the leaf, and nodes[1...MAX_LEVEL] will point
 * to any other levels that are present.
 *
 * The slots array records the index of the item or block pointer
 * used while walking the tree.
 */
struct ctree_path {
	struct tree_buffer *nodes[MAX_LEVEL];
	int slots[MAX_LEVEL];
};

static inline u64 btrfs_header_blocknr(struct btrfs_header *h)
{
	return le64_to_cpu(h->blocknr);
}

static inline void btrfs_set_header_blocknr(struct btrfs_header *h, u64 blocknr)
{
	h->blocknr = cpu_to_le64(blocknr);
}

static inline u64 btrfs_header_parentid(struct btrfs_header *h)
{
	return le64_to_cpu(h->parentid);
}

static inline void btrfs_set_header_parentid(struct btrfs_header *h,
					     u64 parentid)
{
	h->parentid = cpu_to_le64(parentid);
}

static inline u16 btrfs_header_nritems(struct btrfs_header *h)
{
	return le16_to_cpu(h->nritems);
}

static inline void btrfs_set_header_nritems(struct btrfs_header *h, u16 val)
{
	h->nritems = cpu_to_le16(val);
}

static inline u16 btrfs_header_flags(struct btrfs_header *h)
{
	return le16_to_cpu(h->flags);
}

static inline void btrfs_set_header_flags(struct btrfs_header *h, u16 val)
{
	h->flags = cpu_to_le16(val);
}

static inline int btrfs_header_level(struct btrfs_header *h)
{
	return btrfs_header_flags(h) & (MAX_LEVEL - 1);
}

static inline void btrfs_set_header_level(struct btrfs_header *h, int level)
{
	u16 flags;
	BUG_ON(level > MAX_LEVEL);
	flags = btrfs_header_flags(h) & ~(MAX_LEVEL - 1);
	btrfs_set_header_flags(h, flags | level);
}

static inline int btrfs_is_leaf(struct node *n)
{
	return (btrfs_header_level(&n->header) == 0);
}

struct tree_buffer *alloc_free_block(struct ctree_root *root);
int btrfs_inc_ref(struct ctree_root *root, struct tree_buffer *buf);
int free_extent(struct ctree_root *root, u64 blocknr, u64 num_blocks);
int search_slot(struct ctree_root *root, struct key *key, struct ctree_path *p, int ins_len, int cow);
void release_path(struct ctree_root *root, struct ctree_path *p);
void init_path(struct ctree_path *p);
int del_item(struct ctree_root *root, struct ctree_path *path);
int insert_item(struct ctree_root *root, struct key *key, void *data, int data_size);
int next_leaf(struct ctree_root *root, struct ctree_path *path);
int leaf_free_space(struct leaf *leaf);
int btrfs_drop_snapshot(struct ctree_root *root, struct tree_buffer *snap);
int btrfs_finish_extent_commit(struct ctree_root *root);
#endif
