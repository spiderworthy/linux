#ifndef __CTREE__
#define __CTREE__

#define CTREE_BLOCKSIZE 4096

struct key {
	u64 objectid;
	u32 flags;
	u64 offset;
} __attribute__ ((__packed__));

struct header {
	u64 fsid[2]; /* FS specific uuid */
	u64 blocknr;
	u64 parentid;
	u32 csum;
	u32 ham;
	u16 nritems;
	u16 flags;
} __attribute__ ((__packed__));

#define NODEPTRS_PER_BLOCK ((CTREE_BLOCKSIZE - sizeof(struct header)) / \
			    (sizeof(struct key) + sizeof(u64)))

#define MAX_LEVEL 8
#define node_level(f) ((f) & (MAX_LEVEL-1))
#define is_leaf(f) (node_level(f) == 0)

struct tree_buffer;

struct ctree_root {
	struct tree_buffer *node;
	struct ctree_root *extent_root;
	struct key current_insert;
	int fp;
	struct radix_tree_root cache_radix;
};

struct ctree_root_info {
	u64 fsid[2]; /* FS specific uuid */
	u64 blocknr; /* blocknr of this block */
	u64 objectid; /* inode number of this root */
	u64 tree_root; /* the tree root */
	u32 csum;
	u32 ham;
	u64 snapuuid[2]; /* root specific uuid */
} __attribute__ ((__packed__));

struct ctree_super_block {
	struct ctree_root_info root_info;
	struct ctree_root_info extent_info;
} __attribute__ ((__packed__));

struct item {
	struct key key;
	u16 offset;
	u16 size;
} __attribute__ ((__packed__));

#define LEAF_DATA_SIZE (CTREE_BLOCKSIZE - sizeof(struct header))
struct leaf {
	struct header header;
	union {
		struct item items[LEAF_DATA_SIZE/sizeof(struct item)];
		u8 data[CTREE_BLOCKSIZE-sizeof(struct header)];
	};
} __attribute__ ((__packed__));

struct node {
	struct header header;
	struct key keys[NODEPTRS_PER_BLOCK];
	u64 blockptrs[NODEPTRS_PER_BLOCK];
} __attribute__ ((__packed__));

struct extent_item {
	u32 refs;
	u64 owner;
} __attribute__ ((__packed__));

struct ctree_path {
	struct tree_buffer *nodes[MAX_LEVEL];
	int slots[MAX_LEVEL];
};

struct tree_buffer *alloc_free_block(struct ctree_root *root);
int free_extent(struct ctree_root *root, u64 blocknr, u64 num_blocks);
int search_slot(struct ctree_root *root, struct key *key, struct ctree_path *p, int ins_len);
void release_path(struct ctree_root *root, struct ctree_path *p);
void init_path(struct ctree_path *p);
int del_item(struct ctree_root *root, struct ctree_path *path);
int insert_item(struct ctree_root *root, struct key *key, void *data, int data_size);
int next_leaf(struct ctree_root *root, struct ctree_path *path);
int leaf_free_space(struct leaf *leaf);
#endif
