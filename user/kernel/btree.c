/*
 * Generic btree operations
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Portions copyright (c) 2006-2008 Google Inc.
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3.h"

#ifndef trace
#define trace trace_off
#endif

struct bnode
{
	be_u32 count, unused;
	struct index_entry { be_u64 key; be_u64 block; } PACKED entries[];
} PACKED;
/*
 * Note that the first key of an index block is never accessed.  This is
 * because for a btree, there is always one more key than nodes in each
 * index node.  In other words, keys lie between node pointers.  I
 * micro-optimize by placing the node count in the first key, which allows
 * a node to contain an esthetically pleasing binary number of pointers.
 * (Not done yet.)
 */

static inline unsigned bcount(struct bnode *node)
{
	return from_be_u32(node->count);
}

static void free_block(SB, block_t block)
{
}

static struct buffer_head *new_block(struct btree *btree)
{
	block_t block = (btree->ops->balloc)(btree->sb);
	if (block == -1)
		return NULL;
	struct buffer_head *buffer = sb_getblk(vfs_sb(btree->sb), block);
	if (!buffer)
		return NULL;
	memset(bufdata(buffer), 0, bufsize(buffer));
	/* FIXME */
	set_buffer_uptodate(buffer);
	return buffer;
}

static struct buffer_head *new_leaf(struct btree *btree)
{
	struct buffer_head *buffer = new_block(btree);
	if (buffer)
		(btree->ops->leaf_init)(btree, bufdata(buffer));
	return buffer;
}

static struct buffer_head *new_node(struct btree *btree)
{
	struct buffer_head *buffer = new_block(btree);
	if (buffer)
		((struct bnode *)bufdata(buffer))->count = 0;
	return buffer;
}

/*
 * A btree cursor has n + 1 entries for a btree of depth n, with the first n
 * entries pointing at internal nodes and entry n + 1 pointing at a leaf.
 * The next field points at the next index entry that will be loaded in a left
 * to right tree traversal, not the current entry.  The next pointer is null
 * for the leaf, which has its own specialized traversal algorithms.
 */

static inline struct bnode *cursor_node(struct cursor cursor[], int level)
{
	return bufdata(cursor[level].buffer);
}

void release_cursor(struct cursor cursor[], int depth)
{
	for (int i = 0; i < depth; i++)
		brelse(cursor[i].buffer);
}

void show_cursor(struct cursor cursor[], int depth)
{
	printf(">>> cursor %p/%i:", cursor, depth);
	for (int i = 0; i < depth; i++)
		printf(" [%Lx/%i]", (L)bufindex(cursor[i].buffer), bufcount(cursor[i].buffer));
	printf("\n");
}

struct cursor *alloc_cursor(int depth)
{
	return malloc(sizeof(struct cursor) * depth);
}

void free_cursor(struct cursor cursor[])
{
	free(cursor);
}

int probe(BTREE, tuxkey_t key, struct cursor cursor[])
{
	unsigned i, depth = btree->root.depth;
	struct buffer_head *buffer = sb_bread(vfs_sb(btree->sb), btree->root.block);
	if (!buffer)
		return -EIO;
	struct bnode *node = bufdata(buffer);

	for (i = 0; i < depth; i++) {
		struct index_entry *next = node->entries, *top = next + bcount(node);
		while (++next < top) /* binary search goes here */
			if (from_be_u64(next->key) > key)
				break;
		//printf("probe level %i, %ti of %i\n", i, next - node->entries, bcount(node));
		cursor[i] = (struct cursor){ buffer, next };
		if (!(buffer = sb_bread(vfs_sb(btree->sb), from_be_u64((next - 1)->block))))
			goto eek;
		node = (struct bnode *)bufdata(buffer);
	}
	assert((btree->ops->leaf_sniff)(btree, bufdata(buffer)));
	cursor[depth] = (struct cursor){ buffer };
	return 0;
eek:
	release_cursor(cursor, i - 1);
	return -EIO; /* stupid, it might have been NOMEM */
}

static inline int level_finished(struct cursor cursor[], int level)
{
	struct bnode *node = cursor_node(cursor, level);
	return cursor[level].next == node->entries + bcount(node);
}
// also write level_beginning!!!

int advance(BTREE, struct cursor cursor[])
{
	int depth = btree->root.depth, level = depth;
	struct buffer_head *buffer = cursor[level].buffer;
	struct bnode *node;
	do {
		brelse(buffer);
		if (!level)
			return 0;
		node = bufdata(buffer = cursor[--level].buffer);
		//printf("pop to level %i, %tx of %x\n", level, cursor[level].next - node->entries, bcount(node));
	} while (level_finished(cursor, level));
	do {
		//printf("push from level %i, %tx of %x\n", level, cursor[level].next - node->entries, bcount(node));
		if (!(buffer = sb_bread(vfs_sb(btree->sb), from_be_u64(cursor[level].next++->block))))
			goto eek;
		cursor[++level] = (struct cursor){ .buffer = buffer, .next = (node = bufdata(buffer))->entries };
	} while (level < depth);
	return 1;
eek:
	release_cursor(cursor, level);
	return -EIO;
}

/*
 * Climb up the cursor until we find the first level where we have not yet read
 * all the way to the end of the index block, there we find the key that
 * separates the subtree we are in (a leaf) from the next subtree to the right.
 */
be_u64 *next_keyp(struct cursor cursor[], int depth)
{
	for (int level = depth; level--;)
		if (!level_finished(cursor, level))
			return &cursor[level].next->key;
	return NULL;
}

tuxkey_t next_key(struct cursor cursor[], int depth)
{
	be_u64 *keyp = next_keyp(cursor, depth);
	return keyp ? from_be_u64(*keyp) : -1;
}
// also write this_key!!!

void show_tree_range(BTREE, tuxkey_t start, unsigned count)
{
	printf("%i level btree at %Li:\n", btree->root.depth, (L)btree->root.block);
	struct cursor cursor[30]; // check for overflow!!!
	if (probe(btree, start, cursor))
		error("tell me why!!!");
	struct buffer_head *buffer;
	do {
		buffer = cursor[btree->root.depth].buffer;
		assert((btree->ops->leaf_sniff)(btree, bufdata(buffer)));
		(btree->ops->leaf_dump)(btree, bufdata(buffer));
		//tuxkey_t *next = pnext_key(cursor, btree->depth);
		//printf("next key = %Lx:\n", next ? (L)*next : 0);
	} while (--count && advance(btree, cursor));
}

/* Deletion */

static void brelse_free(SB, struct buffer_head *buffer)
{
	brelse(buffer);
	if (bufcount(buffer)) {
		warn("free block %Lx still in use!", (long long)bufindex(buffer));
		return;
	}
	free_block(sb, bufindex(buffer));
	set_buffer_empty(buffer); // free it!!! (and need a buffer free state)
}

static void remove_index(struct cursor cursor[], int level)
{
	struct bnode *node = cursor_node(cursor, level);
	int count = bcount(node), i;

	/* stomps the node count (if 0th key holds count) */
	memmove(cursor[level].next - 1, cursor[level].next,
		(char *)&node->entries[count] - (char *)cursor[level].next);
	node->count = to_be_u32(count - 1);
	--(cursor[level].next);
	mark_buffer_dirty(cursor[level].buffer);

	/* no separator for last entry */
	if (level_finished(cursor, level))
		return;
	/*
	 * Climb up to common parent and set separating key to deleted key.
	 * What if index is now empty?  (no deleted key)
	 * Then some key above is going to be deleted and used to set sep
	 * Climb the cursor while at first entry, bail out at root
	 * find the node with the old sep, set it to deleted key
	 */
	if (cursor[level].next == node->entries && level) {
		be_u64 sep = (cursor[level].next)->key;
		for (i = level - 1; cursor[i].next - 1 == cursor_node(cursor, i)->entries; i--)
			if (!i)
				return;
		(cursor[i].next - 1)->key = sep;
		mark_buffer_dirty(cursor[i].buffer);
	}
}

static void merge_nodes(struct bnode *node, struct bnode *node2)
{
	memcpy(&node->entries[bcount(node)], &node2->entries[0], bcount(node2) * sizeof(struct index_entry));
	node->count = to_be_u32(bcount(node) + bcount(node2));
}

int delete_from_leaf(BTREE, vleaf *leaf, struct delete_info *info)
{
	(btree->ops->leaf_chop)(btree, info->key, leaf);
	return 0;
}

int tree_chop(BTREE, struct delete_info *info, millisecond_t deadline)
{
	int depth = btree->root.depth, level = depth - 1, suspend = 0;
	struct cursor *cursor, *prev;
	struct buffer_head *leafbuf, *leafprev = NULL;
	struct btree_ops *ops = btree->ops;
	struct sb *sb = btree->sb;

	cursor = alloc_cursor(depth + 1);
	prev = alloc_cursor(depth + 1);
	memset(prev, 0, sizeof(struct cursor) * (depth + 1));

	probe(btree, info->resume, cursor);
	leafbuf = cursor[depth].buffer;

	/* leaf walk */
	while (1) {
		if (delete_from_leaf(btree, bufdata(leafbuf), info))
			mark_buffer_dirty(leafbuf);

		/* try to merge this leaf with prev */
		if (leafprev) {
			struct vleaf *this = bufdata(leafbuf);
			struct vleaf *that = bufdata(leafprev);
			trace_off("check leaf %p against %p", leafbuf, leafprev);
			trace_off("need = %i, free = %i", (ops->dleaf_need)(btree, this), dleaf_free(sb, that));
			/* try to merge leaf with prev */
			if ((ops->leaf_need)(btree, this) <= (ops->leaf_free)(btree, that)) {
				trace(">>> can merge leaf %p into leaf %p", leafbuf, leafprev);
				(ops->leaf_merge)(btree, that, this);
				remove_index(cursor, level);
				mark_buffer_dirty(leafprev);
				brelse_free(sb, leafbuf);
				//dirty_buffer_count_check(sb);
				goto keep_prev_leaf;
			}
			brelse(leafprev);
		}
		leafprev = leafbuf;
keep_prev_leaf:

		//nanosleep(&(struct timespec){ 0, 50 * 1000000 }, NULL);
		//printf("time remaining: %Lx\n", deadline - gettime());
//		if (deadline && gettime() > deadline)
//			suspend = -1;
		if (info->blocks && info->freed >= info->blocks)
			suspend = -1;

		/* pop and try to merge finished nodes */
		while (suspend || level_finished(cursor, level)) {
			/* try to merge node with prev */
			if (prev[level].buffer) {
				assert(level); /* node has no prev */
				struct bnode *this = cursor_node(cursor, level);
				struct bnode *that = cursor_node(prev, level);
				trace_off("check node %p against %p", this, that);
				trace_off("this count = %i prev count = %i", bcount(this), bcount(that));
				/* try to merge with node to left */
				if (bcount(this) <= sb->entries_per_node - bcount(that)) {
					trace(">>> can merge node %p into node %p", this, that);
					merge_nodes(that, this);
					remove_index(cursor, level - 1);
					mark_buffer_dirty(prev[level].buffer);
					brelse_free(sb, cursor[level].buffer);
					//dirty_buffer_count_check(sb);
					goto keep_prev_node;
				}
				brelse(prev[level].buffer);
			}
			prev[level].buffer = cursor[level].buffer;
keep_prev_node:

			/* deepest key in the cursor is the resume address */
			if (suspend == -1 && !level_finished(cursor, level)) {
				suspend = 1; /* only set resume once */
				info->resume = from_be_u64((cursor[level].next)->key);
			}
			if (!level) { /* remove depth if possible */
				while (depth > 1 && bcount(cursor_node(prev, 0)) == 1) {
					trace("drop btree level");
					btree->root.block = bufindex(prev[1].buffer);
					brelse_free(sb, prev[0].buffer);
					//dirty_buffer_count_check(sb);
					depth = --btree->root.depth;
					memcpy(prev, prev + 1, depth * sizeof(prev[0]));
					//set_sb_dirty(sb);
				}
				brelse(leafprev);
				release_cursor(prev, depth);
				free_cursor(cursor);
				free_cursor(prev);
				//sb->snapmask &= ~snapmask; delete_snapshot_from_disk();
				//set_sb_dirty(sb);
				//save_sb(sb);
				return suspend;
			}
			level--;
			trace_off(printf("pop to level %i, block %Lx, %i of %i nodes\n", level, cursor[level].buffer->index, cursor[level].next - cursor_node(cursor, level)->entries, bcount(cursor_node(cursor, level))););
		}

		/* push back down to leaf level */
		while (level < depth - 1) {
			struct buffer_head *buffer = sb_bread(vfs_sb(sb), from_be_u64(cursor[level++].next++->block));
			if (!buffer) {
				brelse(leafprev);
				release_cursor(cursor, level - 1);
				free_cursor(cursor);
				free_cursor(prev);
				return -ENOMEM;
			}
			cursor[level].buffer = buffer;
			cursor[level].next = ((struct bnode *)bufdata(buffer))->entries;
			trace_off(printf("push to level %i, block %Lx, %i nodes\n", level, bufindex(buffer), bcount(cursor_node(cursor, level))););
		};
		//dirty_buffer_count_check(sb);
		/* go to next leaf */
		if (!(leafbuf = sb_bread(vfs_sb(sb), from_be_u64(cursor[level].next++->block)))) {
			release_cursor(cursor, level);
			free_cursor(cursor);
			free_cursor(prev);
			return -ENOMEM;
		}
	}
}

/* Insertion */

static void add_child(struct bnode *node, struct index_entry *p, block_t child, u64 childkey)
{
	vecmove(p + 1, p, node->entries + bcount(node) - p);
	p->block = to_be_u64(child);
	p->key = to_be_u64(childkey);
	node->count = to_be_u32(bcount(node) + 1);
}

int insert_node(struct btree *btree, u64 childkey, block_t childblock, struct cursor cursor[])
{
	trace("insert node 0x%Lx key 0x%Lx into node 0x%Lx", (L)childblock, (L)childkey, (L)btree->root.block);
	int depth = btree->root.depth;
	while (depth--) {
		struct index_entry *next = cursor[depth].next;
		struct buffer_head *parentbuf = cursor[depth].buffer;
		struct bnode *parent = bufdata(parentbuf);

		/* insert and exit if not full */
		if (bcount(parent) < btree->sb->entries_per_node) {
			add_child(parent, next, childblock, childkey);
			mark_buffer_dirty(parentbuf);
			return 0;
		}

		/* split a full index node */
		struct buffer_head *newbuf = new_node(btree);
		if (!newbuf)
			goto eek;
		struct bnode *newnode = bufdata(newbuf);
		unsigned half = bcount(parent) / 2;
		u64 newkey = from_be_u64(parent->entries[half].key);
		newnode->count = to_be_u32(bcount(parent) - half);
		memcpy(&newnode->entries[0], &parent->entries[half], bcount(newnode) * sizeof(struct index_entry));
		parent->count = to_be_u32(half);

		/* if the cursor is in the new node, use that as the parent */
		if (next > parent->entries + half) {
			next = next - &parent->entries[half] + newnode->entries;
			mark_buffer_dirty(parentbuf);
			parentbuf = newbuf;
			parent = newnode;
		} else
			mark_buffer_dirty(newbuf);
		add_child(parent, next, childblock, childkey);
		mark_buffer_dirty(parentbuf);
		childkey = newkey;
		childblock = bufindex(newbuf);
		brelse(newbuf);
	}
	trace("add tree level");
	struct buffer_head *newbuf = new_node(btree);
	if (!newbuf)
		goto eek;
	struct bnode *newroot = bufdata(newbuf);
	newroot->count = to_be_u32(2);
	newroot->entries[0].block = to_be_u64(btree->root.block);
	newroot->entries[1].key = to_be_u64(childkey);
	newroot->entries[1].block = to_be_u64(childblock);
	btree->root.block = bufindex(newbuf);
	vecmove(cursor + 1, cursor, btree->root.depth++ + 1);
	cursor[0] = (struct cursor){ .buffer = newbuf }; // .next = ???
	//set_sb_dirty(sb);
	mark_buffer_dirty(newbuf);
	return 0;
eek:
	release_cursor(cursor, depth + 1);
	return -ENOMEM;
}

int btree_leaf_split(struct btree *btree, struct cursor cursor[], tuxkey_t key)
{
	trace("split leaf");
	struct buffer_head *leafbuf = cursor[btree->root.depth].buffer;
	struct buffer_head *newbuf = new_leaf(btree);
	if (!newbuf) {
		/* the rule: release cursor at point of error */
		release_cursor(cursor, btree->root.depth + 1);
		return -ENOMEM;
	}
	u64 newkey = (btree->ops->leaf_split)(btree, key, bufdata(leafbuf), bufdata(newbuf));
	block_t childblock = bufindex(newbuf);
	trace_off("use upper? %Li %Li", key, newkey);
	if (key >= newkey) {
		struct buffer_head *swap = leafbuf;
		leafbuf = cursor[btree->root.depth].buffer = newbuf;
		newbuf = swap;
	}
	brelse_dirty(newbuf);
	return insert_node(btree, newkey, childblock, cursor);
}

void *tree_expand(struct btree *btree, tuxkey_t key, unsigned newsize, struct cursor cursor[])
{
	for (int i = 0; i < 2; i++) {
		struct buffer_head *leafbuf = cursor[btree->root.depth].buffer;
		void *space = (btree->ops->leaf_resize)(btree, key, bufdata(leafbuf), newsize);
		if (space)
			return space;
		assert(!i);
		int err = btree_leaf_split(btree, cursor, key);
		if (err) {
			warn("insert_node failed (%d)", err);
			break;
		}
	}
	return NULL;
}

struct btree new_btree(SB, struct btree_ops *ops)
{
	struct btree btree = { .sb = sb, .ops = ops };
	struct buffer_head *rootbuf = new_node(&btree);
	struct buffer_head *leafbuf = new_leaf(&btree);
	if (!rootbuf || !leafbuf)
		goto eek;
	struct bnode *root = bufdata(rootbuf);
	root->entries[0].block = to_be_u64(bufindex(leafbuf));
	root->count = to_be_u32(1);
	btree.root = (struct root){ .block = bufindex(rootbuf), .depth = 1 };
	printf("root at %Lx\n", (L)bufindex(rootbuf));
	printf("leaf at %Lx\n", (L)bufindex(leafbuf));
	brelse_dirty(rootbuf);
	brelse_dirty(leafbuf);
	return btree;
eek:
	if (rootbuf)
		brelse(rootbuf);
	if (leafbuf)
		brelse(leafbuf);
	return (struct btree){ };
}

void free_btree(struct btree *btree)
{
	// write me
}