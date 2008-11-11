/*
 * Inode table btree leaf operations
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include "hexdump.c"
#include "tux3.h"
#define iattr_included_from_ileaf
#include "iattr.c"

struct ileaf { u16 magic, count; u32 pad; inum_t ibase; char table[]; };

/*
 * inode leaf format
 *
 * A leaf has a small header followed by a table of attributes.  A vector of
 * offsets within the block grows down from the top of the leaf towards the
 * top of the attribute table, indexed by the difference between inum and
 * leaf->ibase, the base inum of the table block.
 */

static inline unsigned atdict(u16 *dict, unsigned at)
{
	return at ? *(dict - at) : 0;
}

static inline struct ileaf *to_ileaf(vleaf *leaf)
{
	return leaf;
}

int ileaf_init(BTREE, vleaf *leaf)
{
	printf("initialize inode leaf %p\n", leaf);
	*(struct ileaf *)leaf = (struct ileaf){ 0x90de };
	return 0;
}

struct ileaf *ileaf_create(BTREE)
{
	struct ileaf *leaf = malloc(btree->sb->blocksize);
	ileaf_init(btree, leaf);
	return leaf;
}

int ileaf_sniff(BTREE, vleaf *leaf)
{
	return ((struct ileaf *)leaf)->magic == 0x90de;
}

void ileaf_destroy(BTREE, struct ileaf *leaf)
{
	assert(ileaf_sniff(btree, leaf));
	free(leaf);
}

unsigned ileaf_need(BTREE, vleaf *vleaf)
{
	u16 *dict = vleaf + btree->sb->blocksize;
	unsigned count = to_ileaf(vleaf)->count;
	return atdict(dict, count) + count * sizeof(*dict);
}

unsigned ileaf_free(BTREE, vleaf *leaf)
{
	return btree->sb->blocksize - ileaf_need(btree, leaf) - sizeof(struct ileaf);
}

void ileaf_dump(BTREE, vleaf *vleaf)
{
	SB = btree->sb;
	struct ileaf *leaf = vleaf;
	inum_t inum = leaf->ibase;
	u16 *dict = vleaf + sb->blocksize, offset = 0;
	printf("inode table block 0x%Lx/%i (%x bytes free)\n", (L)leaf->ibase, leaf->count, ileaf_free(btree, leaf));
	//hexdump(dict - leaf->count, leaf->count * 2);
	for (int i = -1; i >= -leaf->count; i--, inum++) {
		int limit = dict[i], size = limit - offset;
		if (!size)
			continue;
		printf("  0x%Lx: ", (L)inum);
		//printf("[%x] ", offset);
		if (size < 0)
			printf("<corrupt>\n");
		else if (!size)
			printf("<empty>\n");
		else {
#ifndef main
			hexdump(leaf->table + offset, size);
#else
			struct inode inode = { .sb = btree->sb, .xcache = new_xcache(9999) };
			decode_attrs(&inode, leaf->table + offset, size);
			dump_attrs(&inode);
			xcache_dump(&inode);
			free(inode.xcache);
#endif
		}
		offset = limit;
	}
}

void *ileaf_lookup(BTREE, inum_t inum, struct ileaf *leaf, unsigned *result)
{
	assert(inum >= leaf->ibase);
	assert(inum < leaf->ibase + btree->entries_per_leaf);
	unsigned at = inum - leaf->ibase, size = 0;
	void *attrs = NULL;
	printf("lookup inode 0x%Lx, %Lx + %x\n", (L)inum, (L)leaf->ibase, at);
	if (at < leaf->count) {
		u16 *dict = (void *)leaf + btree->sb->blocksize;
		unsigned offset = atdict(dict, at);
		if ((size = *(dict - at - 1) - offset))
			attrs = leaf->table + offset;
	}
	*result = size;
	return attrs;
}

int isinorder(BTREE, struct ileaf *leaf)
{
	u16 *dict = (void *)leaf + btree->sb->blocksize;
	for (int i = 0, offset = 0, limit; --i >= -leaf->count; offset = limit)
		if ((limit = dict[i]) < offset)
			return 0;
	return 1;
}

int ileaf_check(BTREE, struct ileaf *leaf)
{
	char *why;
	why = "not an inode table leaf";
	if (leaf->magic != 0x90de)
		goto eek;
	why = "dict out of order";
	if (!isinorder(btree, leaf))
		goto eek;
	return 0;
eek:
	printf("%s!\n", why);
	return -1;
}

void ileaf_trim(BTREE, struct ileaf *leaf) {
	u16 *dict = (void *)leaf + btree->sb->blocksize;
	while (leaf->count > 1 && *(dict - leaf->count) == *(dict - leaf->count + 1))
		leaf->count--;
	if (leaf->count == 1 && !*(dict - 1))
		leaf->count = 0;
}

#define SPLIT_AT_INUM

tuxkey_t ileaf_split(BTREE, tuxkey_t inum, vleaf *from, vleaf *into)
{
	assert(ileaf_sniff(btree, from));
	struct ileaf *leaf = from, *dest = into;
	u16 *dict = from + btree->sb->blocksize, *destdict = into + btree->sb->blocksize;

#ifdef SPLIT_AT_INUM
	printf("split at inum 0x%Lx\n", (L)inum);
	assert(inum >= leaf->ibase);
	unsigned at = inum - leaf->ibase < leaf->count ? inum - leaf->ibase : leaf->count;
#else
	/* binsearch inum starting nearest middle of block */
	unsigned at = 1, hi = leaf->count;
	while (at < hi) {
		int mid = (at + hi) / 2;
		if (*(dict - mid) < (btree->sb->blocksize / 2))
			at = mid + 1;
		else
			hi = mid;
	}
#endif
	/* should trim leading empty inodes on copy */
	unsigned split = atdict(dict, at), free = *(dict - leaf->count);
	printf("split at %x of %x\n", at, leaf->count);
	printf("copy out %x bytes at %x\n", free - split, split);
	assert(free >= split);
	memcpy(dest->table, leaf->table + split, free - split);
	dest->count = leaf->count - at;
	veccopy(destdict - dest->count, dict - leaf->count, dest->count);
	for (int i = 1; i <= dest->count; i++)
		*(destdict - i) -= split;
#ifdef SPLIT_AT_INUM
	/* round down to multiple of 64 above ibase */
	inum_t round = inum & ~(inum_t)(btree->entries_per_leaf - 1);
	dest->ibase = round > leaf->ibase + leaf->count ? round : inum;
#else
	dest->ibase = leaf->ibase + at;
#endif
	leaf->count = at;
	memset(leaf->table + split, 0, (char *)(dict - leaf->count) - (leaf->table + split));
	ileaf_trim(btree, leaf);
	return dest->ibase;
}

void ileaf_merge(BTREE, struct ileaf *leaf, struct ileaf *from)
{
	if (!from->count)
		return;
	u16 *dict = (void *)leaf + btree->sb->blocksize;
	u16 *fromdict = (void *)from + btree->sb->blocksize;
	unsigned at = leaf->count, free = atdict(dict, at), size = atdict(fromdict, from->count);
	printf("copy in %i bytes\n", size);
	memcpy(leaf->table + free, from->table, size);
	veccopy(dict - (leaf->count += from->count), fromdict - from->count, from->count);
	for (int i = at + 1; at && i <= at + from->count; i++)
		*(dict - i) += *(dict - at);
}

void *ileaf_resize(BTREE, tuxkey_t inum, vleaf *base, unsigned newsize)
{
	assert(ileaf_sniff(btree, base));
	struct ileaf *leaf = base;
	assert(inum >= leaf->ibase);
	u16 *dict = base + btree->sb->blocksize;

	unsigned at = inum - leaf->ibase;
	if (at >= btree->entries_per_leaf)
		return NULL;

	unsigned extend_empty = at < leaf->count ? 0 : at - leaf->count + 1;
	unsigned offset = at && leaf->count ? *(dict - (at < leaf->count ? at : leaf->count)) : 0;
	unsigned size = at < leaf->count ? *(dict - at - 1) - offset : 0;
	int more = newsize - size;
	if (more > 0 && sizeof(*dict) * extend_empty + more > ileaf_free(btree, leaf))
		return NULL;
	for (; extend_empty--; leaf->count++)
		*(dict - leaf->count - 1) = atdict(dict, leaf->count);
	assert(leaf->count);
	unsigned itop = *(dict - leaf->count);
	void *attrs = leaf->table + offset;
	printf("resize inum 0x%Lx at 0x%x from %x to %x\n", (L)inum, offset, size, newsize);

	assert(itop >= offset + size);
	memmove(attrs + newsize, attrs + size, itop - offset - size);
	for (int i = at + 1; i <= leaf->count; i++)
		*(dict - i) += more;
	return attrs;
}

inum_t find_empty_inode(BTREE, struct ileaf *leaf, inum_t goal)
{
	assert(goal >= leaf->ibase);
	goal -= leaf->ibase;
	//printf("find empty inode starting at %Lx, base %Lx\n", (L)goal, (L)leaf->ibase);
	u16 *dict = (void *)leaf + btree->sb->blocksize;
	unsigned i, offset = goal && goal < leaf->count ? *(dict - goal) : 0;
	for (i = goal; i < leaf->count; i++) {
		unsigned limit = *(dict - i - 1);
		if (offset == limit)
			break;
		offset = limit;
	}
	return i + leaf->ibase;
}

int ileaf_purge(BTREE, inum_t inum, struct ileaf *leaf)
{
	if (inum < leaf->ibase || inum - leaf->ibase >= btree->entries_per_leaf)
		return -EINVAL;
	u16 *dict = (void *)leaf + btree->sb->blocksize;
	unsigned at = inum - leaf->ibase;
	unsigned offset = atdict(dict, at);
	unsigned size = *(dict - at - 1) - offset;
	printf("delete inode %Lx from %p[%x/%x]\n", (L)inum, leaf, at, size);
	if (!size)
		return -ENOENT;
	unsigned free = *(dict - leaf->count), tail = free - offset - size;
	assert(offset + size + tail <= free);
	memmove(leaf->table + offset, leaf->table + offset + size, tail);
	for (int i = at + 1; i <= leaf->count; i++)
		*(dict - i) -= size;
	ileaf_trim(btree, leaf);
	return 0;
}

struct btree_ops itable_ops = {
	.leaf_dump = ileaf_dump,
	.leaf_sniff = ileaf_sniff,
	.leaf_init = ileaf_init,
	.leaf_split = ileaf_split,
	.leaf_resize = ileaf_resize,
	.balloc = balloc,
};

#ifndef main
void test_append(BTREE, struct ileaf *leaf, inum_t inum, int more, char fill)
{
	unsigned size = 0;
	char *attrs = ileaf_lookup(btree, inum, leaf, &size);
	printf("attrs size = %i\n", size);
	attrs = ileaf_resize(btree, inum, leaf, size + more);
	memset(attrs + size, fill, more);
}

void test_remove(BTREE, struct ileaf *leaf, inum_t inum, int less)
{
	unsigned size = 0;
	char *attrs = ileaf_lookup(btree, inum, leaf, &size);
	printf("attrs size = %i\n", size);
	attrs = ileaf_resize(btree, inum, leaf, size - less);
}

block_t balloc(SB)
{
	return sb->nextalloc++;
}

int main(int argc, char *argv[])
{
	printf("--- test inode table leaf methods ---\n");
	SB = &(struct sb){ .blocksize = 4096 };
	struct btree *btree = &(struct btree){ .sb = sb, .ops = &itable_ops };
	btree->entries_per_leaf = 64; // !!! should depend on blocksize
	struct ileaf *leaf = ileaf_create(btree);
	struct ileaf *dest = ileaf_create(btree);
	leaf->ibase = 0x10;
	ileaf_dump(btree, leaf);
	test_append(btree, leaf, 0x13, 2, 'a');
	test_append(btree, leaf, 0x14, 4, 'b');
	test_append(btree, leaf, 0x16, 6, 'c');
	ileaf_dump(btree, leaf);
	ileaf_split(btree, 0x10, leaf, dest);
	ileaf_dump(btree, leaf);
	ileaf_dump(btree, dest);
	ileaf_merge(btree, leaf, dest);
	ileaf_dump(btree, leaf);
	test_append(btree, leaf, 0x13, 3, 'x');
	ileaf_dump(btree, leaf);
	test_append(btree, leaf, 0x18, 3, 'y');
	ileaf_dump(btree, leaf);
	test_remove(btree, leaf, 0x16, 5);
	ileaf_dump(btree, leaf);
	unsigned size = 0;
	char *inode = ileaf_lookup(btree, 0x13, leaf, &size);
	hexdump(inode, size);
	for (int i = 0x11; i <= 0x20; i++)
		printf("goal 0x%x => 0x%Lx\n", i, (L)find_empty_inode(btree, leaf, i));
	ileaf_purge(btree, 0x14, leaf);
	ileaf_purge(btree, 0x18, leaf);
hexdump(leaf, 16);
	ileaf_check(btree, leaf);
	ileaf_dump(btree, leaf);
	ileaf_destroy(btree, leaf);
	ileaf_destroy(btree, dest);
	return 0;
}
#endif