/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/* quickhash.h imported from PVFS2 repo rev 1.19 */

#ifndef SRC_COMMON_UTIL_QUICKHASH_H
#define SRC_COMMON_UTIL_QUICKHASH_H

#include <stdlib.h>
#include <stdint.h>
#include "codes/quicklist.h"

#define qhash_malloc(x)            malloc(x)
#define qhash_free(x)              free(x)
#define qhash_head                 qlist_head
#define INIT_QHASH_HEAD            INIT_QLIST_HEAD
#define qhash_entry                qlist_entry
#define qhash_add_tail             qlist_add_tail
#define qhash_del                  qlist_del
#define qhash_for_each             qlist_for_each
#define qhash_for_each_safe        qlist_for_each_safe

#define qhash_lock_init(lock_ptr)  do {} while(0)
#define qhash_lock(lock_ptr)       do {} while(0)
#define qhash_unlock(lock_ptr)     do {} while(0)

struct qhash_table
{
    struct qhash_head *array;
    int table_size;
    int (*compare) (void *key, struct qhash_head * link);
    int (*hash) (void *key, int table_size);
};

/* qhash_init()
 *
 * creates a new hash table with the specified table size.  The
 * hash function and compare function must be provided.
 * table_size should be a good prime number.
 *
 * the compare function tests equality between the input key
 * and the given qhash entry, returning 1 on equal and 0 on
 * non-equal
 *
 * returns pointer to table on success, NULL on failure
 */
static inline struct qhash_table *qhash_init(
    int (*compare) (void *key,
		    struct qhash_head * link),
    int (*hash) (void *key,
		 int table_size),
    int table_size)
{
    int i = 0;
    struct qhash_table *new_table = NULL;

    /* create struct to contain table information */
    new_table = (struct qhash_table *)
        qhash_malloc(sizeof(struct qhash_table));
    if (!new_table)
    {
	return (NULL);
    }

    /* fill in info */
    new_table->compare = compare;
    new_table->hash = hash;
    new_table->table_size = table_size;

    /* create array for actual table */
    new_table->array = (struct qhash_head *)
	qhash_malloc(sizeof(struct qhash_head) * table_size);
    if (!new_table->array)
    {
	qhash_free(new_table);
	return (NULL);
    }

    /* initialize a doubly linked at each hash table index */
    for (i = 0; i < table_size; i++)
    {
	INIT_QHASH_HEAD(&new_table->array[i]);
    }
    qhash_lock_init(&new_table->lock);

    return (new_table);
}


/* qhash_finalize()
 *
 * frees any resources created by the hash table
 *
 * no return value
 */
static inline void qhash_finalize(
    struct qhash_table *old_table)
{
    qhash_free(old_table->array);
    qhash_free(old_table);
    return;
}

/* qhash_add()
 *
 * adds a new link onto the hash table, hashes based on given key
 *
 * no return value
 */
static inline void qhash_add(
    struct qhash_table *table,
    void *key,
    struct qhash_head *link)
{
    int index = 0;

    /* hash on the key */
    index = table->hash(key, table->table_size);

    /* add to the tail of the linked list at that offset */
    qhash_lock(&table->lock);
    qhash_add_tail(link, &(table->array[index]));
    qhash_unlock(&table->lock);

    return;
}

/* qhash_search()
 *
 * searches for a link in the hash table
 * that matches the given key
 *
 * returns pointer to link on success, NULL on failure (or item
 * not found)
 */
static inline struct qhash_head *qhash_search(
    struct qhash_table *table,
    void *key)
{
    int index = 0;
    struct qhash_head *tmp_link = NULL;

    /* find the hash value */
    index = table->hash(key, table->table_size);

    /* linear search at index to find match */
    qhash_lock(&table->lock);
    qhash_for_each(tmp_link, &(table->array[index]))
    {
	if (table->compare(key, tmp_link))
	{
            qhash_unlock(&table->lock);
	    return (tmp_link);
	}
    }
    qhash_unlock(&table->lock);
    return (NULL);
}

/* qhash_search_at_index()
 *
 * searches for a link in the list matching
 * the specified index
 *
 * returns pointer to link on success, NULL on failure (or item
 * not found)
 */
static inline struct qhash_head *qhash_search_at_index(
    struct qhash_table *table,
    int index)
{
    struct qhash_head *tmp_link = NULL;

    if(index >= table->table_size)
    {
        return NULL;
    }

    qhash_lock(&table->lock);
    qhash_for_each(tmp_link, &(table->array[index]))
    {
        qhash_unlock(&table->lock);
        return (tmp_link);
    }
    qhash_unlock(&table->lock);
    return (NULL);
}

/* qhash_search_and_remove()
 *
 * searches for and removes a link in the hash table
 * that matches the given key
 *
 * returns pointer to link on success, NULL on failure (or item
 * not found).  On success, link is removed from hashtable.
 */
static inline struct qhash_head *qhash_search_and_remove(
    struct qhash_table *table,
    void *key)
{
    int index = 0;
    struct qhash_head *tmp_link = NULL;

    /* find the hash value */
    index = table->hash(key, table->table_size);

    /* linear search at index to find match */
    qhash_lock(&table->lock);
    qhash_for_each(tmp_link, &(table->array[index]))
    {
	if (table->compare(key, tmp_link))
	{
	    qhash_del(tmp_link);
	    qhash_unlock(&table->lock);
	    return (tmp_link);
	}
    }
    qhash_unlock(&table->lock);
    return (NULL);
}

/* qhash_search_and_remove_at_index()
 *
 * searches for and removes the first link in the list
 * matching the specified index
 *
 * returns pointer to link on success, NULL on failure (or item
 * not found).  On success, link is removed from hashtable.
 */
static inline struct qhash_head *qhash_search_and_remove_at_index(
    struct qhash_table *table,
    int index)
{
    struct qhash_head *tmp_link = NULL;

    if(index >= table->table_size)
    {
        return NULL;
    }

    qhash_lock(&table->lock);
    qhash_for_each(tmp_link, &(table->array[index]))
    {
        qhash_del(tmp_link);
        qhash_unlock(&table->lock);
        return (tmp_link);
    }
    qhash_unlock(&table->lock);
    return (NULL);
}

#define qhash_destroy_and_finalize(_oldtable, _entry_type, _link, _destructor) \
    do                                                                         \
    {                                                                          \
        int i = 0;                                                             \
        struct qhash_head *entry;                                              \
        struct qhash_head *tmpe;                                               \
        qhash_lock(&_oldtable->lock);                                          \
        for(i = 0; i < _oldtable->table_size; ++i)                             \
        {                                                                      \
            qhash_for_each_safe(entry, tmpe, &(_oldtable->array[i]))           \
            {                                                                  \
                qhash_del(entry);                                              \
                _destructor(qhash_entry(entry, _entry_type, _link));           \
            }                                                                  \
        }                                                                      \
        qhash_unlock(&_oldtable->lock);                                        \
                                                                               \
        qhash_finalize(_oldtable);                                             \
                                                                               \
    } while(0)

/* http://www.cris.com/~Ttwang/tech/inthash.htm */
static inline int quickhash_32bit_hash(void *k, int table_size)
{
    int32_t key = *(int32_t *)k;
    key = ~key + (key << 15); /* key = (key << 15) - key - 1; */
    key = key ^ (key >> 12);
    key = key + (key << 2);
    key = key ^ (key >> 4);
    key = key * 2057; /* key = (key + (key << 3)) + (key << 11); */
    key = key ^ (key >> 16);

    return (int) (key & (table_size - 1));
}

static inline int quickhash_64bit_hash(void *k, int table_size)
{
    uint64_t key  = *(uint64_t *)k;

    key = (~key) + (key << 18); /* key = (key << 18) - key - 1; */
    key = key ^ (key >> 31);
    key = key * 21; /* key = (key + (key << 2)) + (key << 4); */
    key = key ^ (key >> 11);
    key = key + (key << 6);
    key = key ^ (key >> 22);

    return (int) (key & ((uint64_t)(table_size - 1)));
}

/**
 * derived from an algorithm found in Aho, Sethi and Ullman's
 * {Compilers: Principles, Techniques and Tools}, published by Addison-Wesley. 
 * This algorithm comes from P.J. Weinberger's C compiler. 
 */
static inline int quickhash_string_hash(void *k, int table_size)
{
    const char *str = (char *)k;
    uint32_t g, h = 0;

    while(*str)
    {
        h = (h << 4) + *str++;
        if((g = (h & 0xF0UL)))
        {
            h ^= g >> 24;
            h ^= g;
        }
    }

    return (int)(h & ((uint64_t)(table_size - 1)));
}

/* used for cases where we the key is already in good shape for hashing */ 
static inline int quickhash_null32_hash(void *k, int table_size)
{
    uint32_t *tmp = (uint32_t*)k;
    return(int)(*tmp & (table_size - 1));
}

#endif /* SRC_COMMON_UTIL_QUICKHASH_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
