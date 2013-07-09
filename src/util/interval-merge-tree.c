#include "util/interval-merge-tree.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <assert.h>

/* rb tree sentinel */
static size_t interval_merge_tree_sentinel_key_key = 0;
static size_t interval_merge_tree_sentinel_key_value = 0;
static interval_merge_tree_key_t interval_merge_tree_sentinel_key =
    { &interval_merge_tree_sentinel_key_key, &interval_merge_tree_sentinel_key_value };
static interval_merge_tree_node_t interval_merge_tree_sentinel =
    { {&interval_merge_tree_sentinel_key_key, &interval_merge_tree_sentinel_key_value}, NULL, NULL,
    NULL, 0, 0, {0, 0}, NULL, NULL, RB_TREE_COLOR_BLACK, 0, NULL };

#define INTERVAL_TREE_LL_POOL_SIZE 4096
#define INTERVAL_TREE_NODE_POOL_SIZE 4096
static size_t interval_merge_tree_ll_pool_size = 0;
static size_t interval_merge_tree_node_pool_size = 0;
static size_t ll_pool_next = 0;
static size_t node_pool_next = 0;
static interval_merge_tree_node_t *interval_merge_tree_node_pool = NULL;
static interval_merge_tree_node_t **interval_merge_tree_node_pool_index = NULL;
static interval_merge_tree_node_t *interval_merge_tree_node_pool_min = NULL;
static interval_merge_tree_node_t *interval_merge_tree_node_pool_max = NULL;
static interval_merge_tree_interval_ll_t *interval_merge_tree_ll_pool = NULL;
static interval_merge_tree_interval_ll_t **interval_merge_tree_ll_pool_index = NULL;
static interval_merge_tree_interval_ll_t *interval_merge_tree_ll_pool_min = NULL;
static interval_merge_tree_interval_ll_t *interval_merge_tree_ll_pool_max = NULL;

static inline int interval_merge_tree_create_ll_pool(
    size_t num)
{
    size_t i = 0;
    interval_merge_tree_ll_pool =
        (interval_merge_tree_interval_ll_t *) malloc(sizeof(interval_merge_tree_interval_ll_t) *
                                                     num);
    interval_merge_tree_ll_pool_index =
        (interval_merge_tree_interval_ll_t **) malloc(sizeof(interval_merge_tree_interval_ll_t *) *
                                                      num);
    interval_merge_tree_ll_pool_size = num;
    ll_pool_next = 0;
    interval_merge_tree_ll_pool_min = &(interval_merge_tree_ll_pool[0]);
    interval_merge_tree_ll_pool_max = &(interval_merge_tree_ll_pool[num - 1]);

    for(i = 0; i < num; i++)
    {
        interval_merge_tree_ll_pool_index[i] = &(interval_merge_tree_ll_pool[i]);
    }

    return 0;
}

static inline int interval_merge_tree_destroy_ll_pool(
    )
{
    free(interval_merge_tree_ll_pool);
    interval_merge_tree_ll_pool = NULL;
    free(interval_merge_tree_ll_pool_index);
    interval_merge_tree_ll_pool_index = NULL;
    ll_pool_next = 0;
    interval_merge_tree_ll_pool_size = 0;
    interval_merge_tree_ll_pool_min = NULL;
    interval_merge_tree_ll_pool_max = NULL;

    return 0;
}

static inline interval_merge_tree_interval_ll_t *interval_merge_tree_alloc_ll(
    )
{
    if(ll_pool_next < interval_merge_tree_ll_pool_size)
    {
        return interval_merge_tree_ll_pool_index[ll_pool_next++];
    }
    else
    {
        return (interval_merge_tree_interval_ll_t *)
            malloc(sizeof(interval_merge_tree_interval_ll_t));
    }

    return NULL;
}

static inline int interval_merge_tree_dealloc_ll(
    interval_merge_tree_interval_ll_t * node)
{
    if(node >= interval_merge_tree_ll_pool_min && node <= interval_merge_tree_ll_pool_max)
    {
        interval_merge_tree_ll_pool_index[--ll_pool_next] = node;
    }
    else
    {
        free(node);
    }

    return 0;
}

static inline int interval_merge_tree_create_node_pool(
    size_t num)
{
    size_t i = 0;
    interval_merge_tree_node_pool =
        (interval_merge_tree_node_t *) malloc(sizeof(interval_merge_tree_node_t) * num);
    interval_merge_tree_node_pool_index =
        (interval_merge_tree_node_t **) malloc(sizeof(interval_merge_tree_node_t *) * num);
    interval_merge_tree_node_pool_size = num;
    node_pool_next = 0;
    interval_merge_tree_node_pool_min = &(interval_merge_tree_node_pool[0]);
    interval_merge_tree_node_pool_max = &(interval_merge_tree_node_pool[num - 1]);

    for(i = 0; i < num; i++)
    {
        interval_merge_tree_node_pool_index[i] = &(interval_merge_tree_node_pool[i]);
    }

    return 0;
}

static inline int interval_merge_tree_destroy_node_pool(
    )
{
    free(interval_merge_tree_node_pool);
    interval_merge_tree_node_pool = NULL;
    free(interval_merge_tree_node_pool_index);
    interval_merge_tree_node_pool_index = NULL;
    interval_merge_tree_node_pool_size = 0;
    node_pool_next = 0;
    interval_merge_tree_node_pool_min = NULL;
    interval_merge_tree_node_pool_max = NULL;

    return 0;
}

static inline interval_merge_tree_node_t *interval_merge_tree_alloc_node(
    )
{
    if(node_pool_next < interval_merge_tree_node_pool_size)
    {
        return interval_merge_tree_node_pool_index[node_pool_next++];
    }
    else
    {
        return (interval_merge_tree_node_t *) malloc(sizeof(interval_merge_tree_node_t));
    }

    return NULL;
}

static inline int interval_merge_tree_dealloc_node(
    interval_merge_tree_node_t * node)
{
    if(node >= interval_merge_tree_node_pool_min && node <= interval_merge_tree_node_pool_max)
    {
        interval_merge_tree_node_pool_index[--node_pool_next] = node;
    }
    else
    {
        free(node);
    }

    return 0;
}

static inline interval_merge_tree_interval_ll_t *interval_merge_tree_interval_ll_create(
    )
{
    interval_merge_tree_interval_ll_t *ll =
        (interval_merge_tree_interval_ll_t *) malloc(sizeof(interval_merge_tree_interval_ll_t));
    //interval_merge_tree_interval_ll_t * ll = interval_merge_tree_alloc_ll();

    ll->next = NULL;
    ll->data = NULL;
    ll->key = NULL;

    return ll;
}

inline int interval_merge_tree_interval_ll_destroy(
    interval_merge_tree_interval_ll_t * ll)
{
    interval_merge_tree_interval_ll_t *cur = ll;
    interval_merge_tree_interval_ll_t *next = NULL;
    while(cur)
    {
        next = cur->next;
        cur->next = NULL;
        cur->data = NULL;
        free(cur);
        //interval_merge_tree_dealloc_ll(cur);
        cur = next;
    }

    return 0;
}

/* copy the n2 list into the n1 list */
static inline int interval_merge_tree_interval_ll_merge(
    interval_merge_tree_node_t * n1,
    interval_merge_tree_node_t ** n2)
{
    interval_merge_tree_interval_ll_t *l2_head = (*n2)->ll_head;
    interval_merge_tree_interval_ll_t *l2_tail = (*n2)->ll_tail;

    if(l2_head && l2_tail)
    {
        if(n1->ll_head && n1->ll_tail)
        {
            /* set the next value after n1 tail to n2 head */
            n1->ll_tail->next = l2_head;
            n1->ll_tail = l2_tail;
            l2_tail->next = NULL;
            (*n2)->ll_head = NULL;
            (*n2)->ll_tail = NULL;
        }
        else
        {
            /* if the current node list was empty, set it to the l2 values */
            n1->ll_head = l2_head;
            n1->ll_tail = l2_tail;
            l2_tail->next = NULL;
            (*n2)->ll_head = NULL;
            (*n2)->ll_tail = NULL;
        }
    }

    return 0;
}

inline int interval_merge_tree_interval_ll_init(
    interval_merge_tree_node_t * node,
    void *data)
{

    /* create the link list for the node and update the head with the data */
    node->ll_head = interval_merge_tree_interval_ll_create();
    node->ll_head->data = data;
    node->ll_head->key = node->key.key;
    node->ll_tail = node->ll_head;

    return 0;
}

/* determine the max value for an interval or node */
inline static size_t interval_merge_tree_interval_max(
    interval_merge_tree_node_t * node)
{
    if(node->interval.end > node->left->max)
    {
        if(node->interval.end > node->right->max)
        {
            return node->interval.end;
        }
        else    /* if(i->end <= node->right->max) */
        {
            return node->right->max;
        }
    }
    else        /* if(i->end <= node->left->max) */
    {
        if(node->left->max > node->right->max)
        {
            return node->left->max;
        }
        else    /* if(node->left->max <= node->left->max) */
        {
            return node->right->max;
        }
    }

    /* should not get here */
    return 0;
}

/* create an empty node */
inline interval_merge_tree_node_t *interval_merge_tree_create_node(
    )
{
    interval_merge_tree_node_t *node = NULL;

    /* try to get a node from the node pool first */
#if 0
    node = interval_merge_tree_node_pool_alloc_node();
    if(!node)
#endif
    {
        node = (interval_merge_tree_node_t *) malloc(sizeof(interval_merge_tree_node_t));
        //node = interval_merge_tree_alloc_node();
        node->node_pool = 0;
    }

    /* set child and parent pointers to NULL */
    node->left = &interval_merge_tree_sentinel;
    node->right = &interval_merge_tree_sentinel;
    node->parent = &interval_merge_tree_sentinel;

    /* set the key to NULL */
    node->key.key = NULL;
    node->key.value = NULL;

    /* set to a non-valid color */
    node->color = RB_TREE_COLOR_NONE;

    /* create the interval */
    node->interval.start = 0;
    node->interval.end = 0;

    /* set the max interval value */
    node->max = 0;

    /* set the order stat size to 0 */
    node->size = 0;

    /* create the link list for intervals... set the head to the tail */
    node->ll_head = NULL;
    node->ll_tail = node->ll_head;

    /* init the consumer to null */
    node->consumer = NULL;

    return node;
}

/* destroy a node */
inline int interval_merge_tree_destroy_node(
    interval_merge_tree_node_t * node)
{
    if(node && node != &interval_merge_tree_sentinel)
    {
        node->left = NULL;
        node->right = NULL;

        /* destroy the key */
        node->key.key = NULL;
        node->key.value = NULL;

        /* do not attempt to destroy the parent node ... just set to NULL */
        node->parent = NULL;

        /* set the color to 0 */
        node->color = 0;

        /* set the max interval value */
        node->max = 0;

        /* destroy the interval */
        node->interval.start = 0;
        node->interval.end = 0;

        /* erase the pointers to, but do not destroy, the link list */
        node->ll_head = NULL;
        node->ll_tail = NULL;

        /* destroy the node */
#if 0
        if(node->node_pool)
        {
            interval_merge_tree_node_pool_free_node(node);
        }
        else
#endif
        {
            free(node);
            //interval_merge_tree_dealloc_node(node);
        }
    }
    return 0;
}

/* recycle a node */
inline int interval_merge_tree_recycle_node(
    interval_merge_tree_node_t * node)
{
    if(node && node != &interval_merge_tree_sentinel)
    {
        node->left = NULL;
        node->right = NULL;

        /* destroy the key */
        node->key.key = NULL;
        node->key.value = NULL;

        /* do not attempt to destroy the parent node ... just set to NULL */
        node->parent = NULL;

        /* set the color to 0 */
        node->color = 0;

        /* set the max interval value */
        node->max = 0;

        /* destroy the interval */
        //interval_merge_tree_destroy_interval(node->interval);
        node->interval.start = 0;
        node->interval.end = 0;

        /* erase the pointers to, but do not destroy, the link list */
        node->ll_head = NULL;
        node->ll_tail = NULL;
    }
    return 0;
}

/* destroy a node and it's children... but not the node parent */
int interval_merge_tree_destroy_tree(
    interval_merge_tree_node_t * node)
{
    if(node && node != &interval_merge_tree_sentinel)
    {
        /* destroy the left node */
        if(node->left && node->left != &interval_merge_tree_sentinel)
        {
            interval_merge_tree_destroy_tree(node->left);
            node->left = NULL;
        }

        /* destory the right node */
        if(node->right && node->right != &interval_merge_tree_sentinel)
        {
            interval_merge_tree_destroy_tree(node->right);
            node->right = NULL;
        }

        /* destroy the key */
        node->key.key = NULL;
        node->key.value = NULL;

        /* do not attempt to destroy the parent node ... just set to NULL */
        node->parent = NULL;

        /* set the color to 0 */
        node->color = 0;

        /* set the max interval value */
        node->max = 0;

        /* destroy the interval */
        node->interval.start = 0;
        node->interval.end = 0;

        /* cleanup the interval link list */
        interval_merge_tree_interval_ll_destroy(node->ll_head);
        node->ll_head = NULL;
        node->ll_tail = NULL;

        /* destroy the node */
#if 0
        if(node->node_pool)
        {
            interval_merge_tree_node_pool_free_node(node);
        }
        else
#endif
        {
            free(node);
            //interval_merge_tree_dealloc_node(node);
        }
    }

    /* cleanup the mem pools */
    //interval_merge_tree_destroy_ll_pool();
    //interval_merge_tree_destroy_node_pool();

    return 0;
}

/* rotate the nodes to the left */
static inline int interval_merge_tree_rotate_left(
    interval_merge_tree_node_t ** root,
    interval_merge_tree_node_t * x,
    int op)
{
    interval_merge_tree_node_t *y = x->right;

    x->right = y->left;
    if(y->left && y->left != &interval_merge_tree_sentinel)
    {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if(!x->parent || x->parent == &interval_merge_tree_sentinel)
    {
        *root = y;
    }
    else
    {
        if(x == x->parent->left)
        {
            x->parent->left = y;
        }
        else
        {
            x->parent->right = y;
        }
    }
    y->left = x;
    x->parent = y;

    /* update the size based on the operation */
    if(op == RB_TREE_INSERT)
    {
        if(x != &interval_merge_tree_sentinel && y != &interval_merge_tree_sentinel)
        {
            y->size = x->size;
            assert(x->right);
            assert(x->left);
            x->size = x->left->size + x->right->size + 1;
        }
    }
    else
    {
        if(x != &interval_merge_tree_sentinel && y != &interval_merge_tree_sentinel)
        {
            y->size = x->size;
            assert(x->right);
            assert(x->left);
            x->size = x->left->size - x->right->size - 1;
        }
    }

    /* update the max interval */
    y->max = interval_merge_tree_interval_max(y);
    x->max = interval_merge_tree_interval_max(x);

    return 0;
}

/* rotate the nodes to the right */
static inline int interval_merge_tree_rotate_right(
    interval_merge_tree_node_t ** root,
    interval_merge_tree_node_t * y,
    int op)
{
    interval_merge_tree_node_t *x = y->left;

    y->left = x->right;
    if(x->right && x->right != &interval_merge_tree_sentinel)
    {
        x->right->parent = y;
    }
    x->parent = y->parent;
    if(!y->parent || y->parent == &interval_merge_tree_sentinel)
    {
        *root = x;
    }
    else
    {
        if(y == y->parent->right)
        {
            y->parent->right = x;
        }
        else
        {
            y->parent->left = x;
        }
    }
    x->right = y;
    y->parent = x;

    /* update the size based on the operation */
    if(op == RB_TREE_INSERT)
    {
        if(x != &interval_merge_tree_sentinel && y != &interval_merge_tree_sentinel)
        {
            x->size = y->size;
            assert(y->left);
            assert(y->right);
            y->size = y->left->size + y->right->size + 1;
        }
    }
    else
    {
        if(x != &interval_merge_tree_sentinel && y != &interval_merge_tree_sentinel)
        {
            x->size = y->size;
            assert(y->left);
            assert(y->right);
            y->size = y->left->size - y->right->size - 1;
        }
    }

    /* update the max interval */
    x->max = interval_merge_tree_interval_max(x);
    y->max = interval_merge_tree_interval_max(y);

    return 0;
}

/* function to make sure the red-black tree properties are followed after inserts */
static int interval_merge_tree_insert_rules(
    interval_merge_tree_node_t ** root,
    interval_merge_tree_node_t * node)
{
    interval_merge_tree_node_t *y = NULL;
    while(node->parent->color == RB_TREE_COLOR_RED)
    {
        if(node->parent == node->parent->parent->left)
        {
            y = node->parent->parent->right;

            if(y->color == RB_TREE_COLOR_RED)
            {
                node->parent->color = RB_TREE_COLOR_BLACK;
                y->color = RB_TREE_COLOR_BLACK;
                node->parent->parent->color = RB_TREE_COLOR_RED;
                node = node->parent->parent;
            }
            else
            {
                if(node == node->parent->right)
                {
                    node = node->parent;
                    interval_merge_tree_rotate_left(root, node, RB_TREE_INSERT);
                }
                node->parent->color = RB_TREE_COLOR_BLACK;
                node->parent->parent->color = RB_TREE_COLOR_RED;
                interval_merge_tree_rotate_right(root, node->parent->parent, RB_TREE_INSERT);
            }
        }
        else
        {
            y = node->parent->parent->left;

            if(y->color == RB_TREE_COLOR_RED)
            {
                node->parent->color = RB_TREE_COLOR_BLACK;
                y->color = RB_TREE_COLOR_BLACK;
                node->parent->parent->color = RB_TREE_COLOR_RED;
                node = node->parent->parent;
            }
            else
            {
                if(node == node->parent->left)
                {
                    node = node->parent;
                    interval_merge_tree_rotate_right(root, node, RB_TREE_INSERT);
                }
                node->parent->color = RB_TREE_COLOR_BLACK;
                node->parent->parent->color = RB_TREE_COLOR_RED;
                interval_merge_tree_rotate_left(root, node->parent->parent, RB_TREE_INSERT);
            }
        }
    }
    (*root)->color = RB_TREE_COLOR_BLACK;

    return 0;
}

/* insert a node into the rb tree */
int interval_merge_tree_insert(
    interval_merge_tree_node_t ** node,
    interval_merge_tree_key_t key,
    interval_merge_tree_node_t ** root)
{
    interval_merge_tree_node_t *y = &interval_merge_tree_sentinel;
    interval_merge_tree_node_t *x = *root;

    /* if x is null, make it a sentinel */
    if(!x)
    {
        x = &interval_merge_tree_sentinel;
    }

    /* if this is a new tree */
    if(!*root)
    {
        /* create new memory pools */
        //interval_merge_tree_create_ll_pool(INTERVAL_TREE_LL_POOL_SIZE);
        //interval_merge_tree_create_node_pool(INTERVAL_TREE_NODE_POOL_SIZE);
    }

    if(!*node)
    {
        *node = interval_merge_tree_create_node();

        /* update the size of the new node */
        (*node)->size = 1;

        /* set the node max to the end of the interval */
        (*node)->max = (*node)->interval.end;
    }

    /* set the key */
    (*node)->key = key;

    while(x != &interval_merge_tree_sentinel)
    {
        y = x;

        /* this is a duplicate... don't insert it */
        if(*(size_t *) key.key == *(size_t *) x->key.key)
        {
            return 1;
        }
        else
        {
            /* update the size of the path */
            x->size += 1;

            /* update the max variable for all nodes along the path */
            if((*node)->max > x->max)
            {
                x->max = (*node)->max;
            }

            if(*(size_t *) key.key < *(size_t *) x->key.key)
            {
                x = x->left;
            }
            else
            {
                x = x->right;
            }
        }
    }

    /* update the parent field in the node */
    (*node)->parent = y;

    /* check if the new node is the root node */
    if(y == &interval_merge_tree_sentinel)
    {
        (*root) = (*node);
    }
    else
    {
        if(*(size_t *) key.key < *(size_t *) y->key.key)
        {
            y->left = (*node);
        }
        else
        {
            y->right = (*node);
        }
    }

    /* set the node child pointers */
    (*node)->left = &interval_merge_tree_sentinel;
    (*node)->right = &interval_merge_tree_sentinel;

    /* set the color to RED */
    (*node)->color = RB_TREE_COLOR_RED;

    /* run the red-black tree rules */
    interval_merge_tree_insert_rules(root, *node);

    return 0;
}

/* enforces red-black tree rules after deletes */
static int interval_merge_tree_delete_rules(
    interval_merge_tree_node_t ** root,
    interval_merge_tree_node_t * node)
{
    interval_merge_tree_node_t *w = NULL;

    /* make sure node is not null */
    if(!node)
    {
        return 1;
    }

    while(node != *root && node->color == RB_TREE_COLOR_BLACK)
    {
        if(node == node->parent->left)
        {
            w = node->parent->right;

            if(w->color == RB_TREE_COLOR_RED)
            {
                w->color = RB_TREE_COLOR_BLACK;
                node->parent->color = RB_TREE_COLOR_RED;
                interval_merge_tree_rotate_left(root, node->parent, RB_TREE_DELETE);
                w = node->parent->right;
            }

            if(w->left->color == RB_TREE_COLOR_BLACK && w->right->color == RB_TREE_COLOR_BLACK)
            {
                w->color = RB_TREE_COLOR_RED;
                node = node->parent;
            }
            else
            {
                if(w->right->color == RB_TREE_COLOR_BLACK)
                {
                    w->left->color = RB_TREE_COLOR_BLACK;
                    w->color = RB_TREE_COLOR_RED;
                    interval_merge_tree_rotate_right(root, w, RB_TREE_DELETE);
                    w = node->parent->right;
                }
                w->color = node->parent->color;
                node->parent->color = RB_TREE_COLOR_BLACK;
                w->right->color = RB_TREE_COLOR_BLACK;
                interval_merge_tree_rotate_left(root, node->parent, RB_TREE_DELETE);
                node = *root;
            }
        }
        else
        {
            w = node->parent->left;

            if(w->color == RB_TREE_COLOR_RED)
            {
                w->color = RB_TREE_COLOR_BLACK;
                node->parent->color = RB_TREE_COLOR_RED;
                interval_merge_tree_rotate_right(root, node->parent, RB_TREE_DELETE);
                w = node->parent->left;
            }

            if(w->right->color == RB_TREE_COLOR_BLACK && w->left->color == RB_TREE_COLOR_BLACK)
            {
                w->color = RB_TREE_COLOR_RED;
                node = node->parent;
            }
            else
            {
                if(w->left->color == RB_TREE_COLOR_BLACK)
                {
                    w->right->color = RB_TREE_COLOR_BLACK;
                    w->color = RB_TREE_COLOR_RED;
                    interval_merge_tree_rotate_left(root, w, RB_TREE_DELETE);
                    w = node->parent->left;
                }
                w->color = node->parent->color;
                node->parent->color = RB_TREE_COLOR_BLACK;
                w->left->color = RB_TREE_COLOR_BLACK;
                interval_merge_tree_rotate_right(root, node->parent, RB_TREE_DELETE);
                node = *root;
            }
        }
    }

    node->color = RB_TREE_COLOR_BLACK;

    return 0;
}

/* find the min value from a given node in the tree */
static inline interval_merge_tree_node_t *interval_merge_tree_min(
    interval_merge_tree_node_t * node)
{
    if(!node)
        return NULL;

    while(node->left != &interval_merge_tree_sentinel)
    {
        node = node->left;
    }
    return node;
}

/* find the node in the tree that is the next greatest value */
static inline interval_merge_tree_node_t *interval_merge_tree_successor(
    interval_merge_tree_node_t * node)
{
    interval_merge_tree_node_t *y = NULL;

    if(!node)
        return NULL;

    if(node->right != &interval_merge_tree_sentinel)
    {
        return interval_merge_tree_min(node->right);
    }

    y = node->parent;

    while(y != &interval_merge_tree_sentinel && node == y->right)
    {
        node = y;
        y = y->parent;
    }

    return y;
}

static int interval_merge_tree_delete_update_rank(
    interval_merge_tree_node_t * node,
    size_t k)
{
    if(node && node != &interval_merge_tree_sentinel)
    {
        node->size -= 1;

        if(*(size_t *) node->key.key == k)
        {
            return 0;
        }
        else if(k < *(size_t *) node->key.key)
        {
            return interval_merge_tree_delete_update_rank(node->left, k);
        }
        else if(k > *(size_t *) node->key.key)
        {
            return interval_merge_tree_delete_update_rank(node->right, k);
        }
    }

    return 0;
}

/* delete a node from the tree */
interval_merge_tree_node_t *interval_merge_tree_find_delete(
    interval_merge_tree_node_t ** root,
    size_t k)
{
    interval_merge_tree_node_t *x = NULL;
    interval_merge_tree_node_t *y = NULL;
    interval_merge_tree_node_t *node = NULL;

    node = interval_merge_tree_node_find_rank(*root, k, RB_TREE_RANK_DEC);

    /* make sure node is not null */
    if(!node)
    {
        return NULL;
    }

    if((!node->left || node->left == &interval_merge_tree_sentinel) ||
       (!node->right || node->right == &interval_merge_tree_sentinel))
    {
        y = node;
    }
    else
    {
        y = interval_merge_tree_successor(node);
    }

    if(y->left && y->left != &interval_merge_tree_sentinel)
    {
        x = y->left;
    }
    else
    {
        x = y->right;
    }

    assert(x);
    assert(y);
    x->parent = y->parent;

    if(y->parent == &interval_merge_tree_sentinel)
    {
        *root = x;
    }
    else
    {
        if(y == y->parent->left)
        {
            y->parent->left = x;
        }
        else
        {
            y->parent->right = x;
        }
    }

    /* update the key */
    if(y != node)
    {
        node->key = y->key;
        node->ll_head = y->ll_head;
        node->ll_tail = y->ll_tail;
    }

    if(y->color == RB_TREE_COLOR_BLACK)
    {
        interval_merge_tree_delete_rules(root, x);
    }

    return y;
}

/* delete a node from the tree */
interval_merge_tree_node_t *interval_merge_tree_delete(
    interval_merge_tree_node_t ** root,
    interval_merge_tree_node_t * node)
{
    interval_merge_tree_node_t *x = NULL;
    interval_merge_tree_node_t *y = NULL;

    /* make sure node is not null */
    if(!node)
    {
        return NULL;
    }

    /* update rank */
    interval_merge_tree_delete_update_rank(*root, *(size_t *) node->key.key);

    if((!node->left || node->left == &interval_merge_tree_sentinel) ||
       (!node->right || node->right == &interval_merge_tree_sentinel))
    {
        y = node;
    }
    else
    {
        y = interval_merge_tree_successor(node);
    }

    if(y->left && y->left != &interval_merge_tree_sentinel)
    {
        x = y->left;
    }
    else
    {
        x = y->right;
    }

    assert(x);
    assert(y);
    x->parent = y->parent;

    if(y->parent == &interval_merge_tree_sentinel)
    {
        *root = x;
    }
    else
    {
        if(y == y->parent->left)
        {
            y->parent->left = x;
        }
        else
        {
            y->parent->right = x;
        }
    }

    /* update the node */
    if(y != node)
    {
        node->key = y->key;
        node->interval.start = y->interval.start;
        node->interval.end = y->interval.end;
        node->size = node->left->size + node->right->size + 1;
        node->max = interval_merge_tree_interval_max(node);
        node->ll_head = y->ll_head;
        node->ll_tail = y->ll_tail;
    }

    if(y->color == RB_TREE_COLOR_BLACK)
    {
        interval_merge_tree_delete_rules(root, x);
    }

    return y;
}

/* print the node */
int interval_merge_tree_print(
    interval_merge_tree_node_t * node,
    int level,
    char dir)
{
    if(!node || node == &interval_merge_tree_sentinel)
    {
        return 0;
    }

    /* print the node value */
    fprintf(stderr,
            "level=%i, node=internal, color=%c, dir=%c, key=%lu, size=%i, max=%i, start=%lu, end=%lu\n",
            level, node->color, dir, *(size_t *) node->key.key, node->size, node->max,
            node->interval.start, node->interval.end);

    /* descend the right nodes */
    interval_merge_tree_print(node->right, level + 1, 'R');

    /* descend the left nodes */
    interval_merge_tree_print(node->left, level + 1, 'L');

    return 0;
}

/* print the tree */
int interval_merge_tree_print_tree(
    interval_merge_tree_node_t * node)
{
    fprintf(stderr, "print tree:\n");
    return interval_merge_tree_print(node, 0, 'C');
}

/* find the key in the tree */
interval_merge_tree_key_t interval_merge_tree_key_find(
    interval_merge_tree_node_t * node,
    size_t k)
{
    if(node && node != &interval_merge_tree_sentinel)
    {
        /* check for valid key */
        if(!node->key.key)
            return interval_merge_tree_sentinel_key;

        if(*(size_t *) node->key.key == k)
        {
            return node->key;
        }
        else if(k < *(size_t *) node->key.key)
        {
            return interval_merge_tree_key_find(node->left, k);
        }
        else if(k > *(size_t *) node->key.key)
        {
            return interval_merge_tree_key_find(node->right, k);
        }
    }
    return interval_merge_tree_sentinel_key;
}

/* find the node in the tree given a key */
interval_merge_tree_node_t *interval_merge_tree_node_find(
    interval_merge_tree_node_t * node,
    size_t k)
{
    if(node && node != &interval_merge_tree_sentinel)
    {
        /* verify that pointer is valid */
        if(node->key.key == NULL)
            return NULL;

        if(*(size_t *) node->key.key == k)
        {
            return node;
        }
        else if(k < *(size_t *) node->key.key)
        {
            return interval_merge_tree_node_find(node->left, k);
        }
        else if(k > *(size_t *) node->key.key)
        {
            return interval_merge_tree_node_find(node->right, k);
        }
    }
    return NULL;
}

/* find the node in the tree given a key */
interval_merge_tree_node_t *interval_merge_tree_node_find_rank(
    interval_merge_tree_node_t * node,
    size_t k,
    int op)
{
    interval_merge_tree_node_t *fn = NULL;

    /* recursively search for the node */
    if(node && node != &interval_merge_tree_sentinel)
    {
        if(*(size_t *) node->key.key == k)
        {
            fn = node;
        }
        else if(k < *(size_t *) node->key.key)
        {
            fn = interval_merge_tree_node_find(node->left, k);
        }
        else if(k > *(size_t *) node->key.key)
        {
            fn = interval_merge_tree_node_find(node->right, k);
        }
    }

    /* if we found the node, update the rank */
    if(fn)
    {
        if(op == RB_TREE_RANK_INC)
        {
            node->size += 1;
        }
        else if(op == RB_TREE_RANK_DEC)
        {
            node->size -= 1;
        }
    }
    return fn;
}

/* compute the rank of a node in the rb-tree */
inline int interval_merge_tree_rank(
    interval_merge_tree_node_t * root,
    interval_merge_tree_node_t * node)
{
    interval_merge_tree_node_t *y = node;
    int rank = node->left->size + 1;

    while(y != root)
    {
        if(y == y->parent->right)
        {
            rank = rank + y->parent->left->size + 1;
        }
        y = y->parent;
    }

    return rank;
}

/* identify overlapping or adjacent intervals */
static inline int interval_merge_tree_interval_overlap(
    interval_merge_tree_node_t * node,
    interval_merge_tree_interval_t i)
{
    interval_merge_tree_interval_t n = node->interval;

    /* Exact same interval...
     * n: [s........e]
     * i: [e........e]
     */
    if(n.start == i.start && n.end == i.end)
    {
        return RB_TREE_INTERVAL_SAME;
    }
    /*
     * n: [s........e]
     * i:   [s....e]
     */
    else if(n.start <= i.start && n.end >= i.end)
    {
        return RB_TREE_INTERVAL_C3;
    }
    /*
     * n:   [s....e]
     * i: [s........e]
     */
    else if(n.start >= i.start && n.end <= i.end)
    {
        return RB_TREE_INTERVAL_C4;
    }
    /*
     * n: [s........e]
     * i:    [e........e]
     */
    else if(n.start <= i.start && i.start <= n.end && n.end <= i.end)
    {
        return RB_TREE_INTERVAL_C1;
    }
    /*
     * n:     [s........e]
     * i: [s........e]
     */
    else if(n.start >= i.start && n.start <= i.end && n.end >= i.end)
    {
        return RB_TREE_INTERVAL_C2;
    }

    return RB_TREE_INTERVAL_NO_OVERLAP;
}

/* search for an interval in the rb tree and see if there is overlap with another interval */
interval_merge_tree_node_t *interval_merge_tree_interval_search(
    interval_merge_tree_node_t * root,
    interval_merge_tree_interval_t interval,
    unsigned int *oc)
{
    interval_merge_tree_node_t *node = root;
    *oc = RB_TREE_INTERVAL_NO_OVERLAP;

    /* while the node is not the sentinel and the intervals do not overlap */
    while(node != &interval_merge_tree_sentinel &&
          ((*oc =
            interval_merge_tree_interval_overlap(node, interval)) == RB_TREE_INTERVAL_NO_OVERLAP))
    {
        /* go left */
        if(node->left != &interval_merge_tree_sentinel && node->left->max >= interval.start)
        {
            node = node->left;
        }
        /* go right */
        else
        {
            node = node->right;
        }
    }

    return node;
}

/* update the max end interval from node to root */
static int interval_merge_tree_interval_update_max(
    interval_merge_tree_node_t * root,
    interval_merge_tree_node_t * node)
{
    /* while node is not null or the sentinel */
    while(node && node != &interval_merge_tree_sentinel)
    {
        /* update the max interval value for the node */
        size_t max = interval_merge_tree_interval_max(node);

        /* if there was no change in max intervals for this node, break the loop */
        if(max == node->max)
        {
            break;
        }

        /* update the max value for this node */
        node->max = max;

        /* if this was the root, break the loop */
        if(node == root)
        {
            break;
        }

        /* look at the next parent */
        node = node->parent;
    }

    return 0;
}

/* insert new node only when there is no overlap, else merge node with existing node */
int interval_merge_tree_merge_intervals(
    interval_merge_tree_node_t ** root,
    interval_merge_tree_node_t ** n_node,
    interval_merge_tree_key_t key)
{
    unsigned int oc = RB_TREE_INTERVAL_NO_OVERLAP;
    interval_merge_tree_interval_t interval = (*n_node)->interval;
    interval_merge_tree_node_t *e_node = NULL;

    /* if this is an empty tree, just insert the node */
    if(!*root || *root == &interval_merge_tree_sentinel)
    {
        return interval_merge_tree_insert(n_node, key, root);
    }

    /* find an interval that overlaps */
    e_node = interval_merge_tree_interval_search(*root, interval, &oc);

    /* if no overlap detected, insert a new node with the interval i */
    if(oc == RB_TREE_INTERVAL_NO_OVERLAP)
    {
        return interval_merge_tree_insert(n_node, key, root);
    }
    /* interval already exists... ignore and return */
    else if(oc == RB_TREE_INTERVAL_SAME)
    {
        interval_merge_tree_interval_ll_destroy((*n_node)->ll_head);
        (*n_node)->ll_head = NULL;
        (*n_node)->ll_tail = NULL;
        interval_merge_tree_destroy_node(*n_node);
        return 0;
    }
    /* if overlap, what kind */
    else
    {
        /* extend the end of the existing node / interval and update the max values in affected nodes */
        if(oc == RB_TREE_INTERVAL_C2)
        {
            size_t e_end = e_node->interval.end;
            interval_merge_tree_interval_ll_t *t_ll_head = e_node->ll_head;
            interval_merge_tree_interval_ll_t *t_ll_tail = e_node->ll_tail;
            interval_merge_tree_node_t *c_node = NULL;

            c_node = interval_merge_tree_delete(root, e_node);
            interval_merge_tree_recycle_node(c_node);

            c_node->interval.start = (*n_node)->interval.start;
            c_node->interval.end = e_end;
            c_node->max = e_end;
            c_node->ll_head = t_ll_head;
            c_node->ll_tail = t_ll_tail;

            c_node->key = key;
            c_node->size = 1;

            /* sanity... update the child and parent links to point the sentinel */
            c_node->left = &interval_merge_tree_sentinel;
            c_node->right = &interval_merge_tree_sentinel;
            c_node->parent = &interval_merge_tree_sentinel;

            /* merge the data linked lists */
            interval_merge_tree_interval_ll_merge(c_node, n_node);

            /* destroy the old copy of n_node */
            interval_merge_tree_destroy_node(*n_node);

            /* re-insert the mereged node... try and merge some more nodes */
            return interval_merge_tree_merge_intervals(root, &c_node, c_node->key);
        }
        /* delete the node and add a new node with interval i at the front of the old interval */
        /* this case will not update max values */
        else if(oc == RB_TREE_INTERVAL_C1)
        {
            size_t e_start = e_node->interval.start;
            interval_merge_tree_key_t e_key = e_node->key;
            interval_merge_tree_node_t *c_node = NULL;
            interval_merge_tree_interval_ll_t *t_ll_head = e_node->ll_head;
            interval_merge_tree_interval_ll_t *t_ll_tail = e_node->ll_tail;

            /* delete the node from the tree */
            c_node = interval_merge_tree_delete(root, e_node);
            interval_merge_tree_recycle_node(c_node);

            /* update the interval and key with the new start value */
            c_node->interval.start = e_start;
            c_node->interval.end = (*n_node)->interval.end;
            c_node->max = (*n_node)->interval.end;
            c_node->key = e_key;
            c_node->size = 1;
            c_node->ll_head = t_ll_head;
            c_node->ll_tail = t_ll_tail;

            /* sanity... update the child and parent links to point the sentinel */
            c_node->left = &interval_merge_tree_sentinel;
            c_node->right = &interval_merge_tree_sentinel;
            c_node->parent = &interval_merge_tree_sentinel;

            /* merge the data linked lists */
            interval_merge_tree_interval_ll_merge(c_node, n_node);

            /* destroy the old copy of n_node */
            interval_merge_tree_destroy_node(*n_node);

            /* re-insert the mereged node... try and merge some more nodes */
            return interval_merge_tree_merge_intervals(root, &c_node, c_node->key);
        }
        /* new interval is consumed by the existing interval, do not modify the tree */
        else if(oc == RB_TREE_INTERVAL_C3)
        {
            (*n_node)->consumer = e_node;
            /*interval_merge_tree_interval_ll_destroy((*n_node)->ll_head);
             * (*n_node)->ll_head = NULL;
             * (*n_node)->ll_tail = NULL;
             * interval_merge_tree_destroy_node(*n_node); */
            fprintf(stderr, "consume\n");
            return RB_TREE_CONSUME;
        }
        /* delete the existing node and add a new node for interval i */
        /* this case will update the max values along the path */
        else if(oc == RB_TREE_INTERVAL_C4)
        {
            interval_merge_tree_node_t *c_node = NULL;
            interval_merge_tree_interval_ll_t *t_ll_head = e_node->ll_head;
            interval_merge_tree_interval_ll_t *t_ll_tail = e_node->ll_tail;

            interval_merge_tree_node_t *e_p = e_node->parent;

            /* delete the node from the tree */
            c_node = interval_merge_tree_delete(root, e_node);
            interval_merge_tree_recycle_node(c_node);

            /* update the max values from the e_p to the root */
            interval_merge_tree_interval_update_max(*root, e_p);

            /* update the interval and key with the new start value */
            c_node->interval.start = interval.start;
            c_node->interval.end = interval.end;
            c_node->max = interval.end;
            c_node->key = key;
            c_node->size = 1;
            c_node->ll_head = t_ll_head;
            c_node->ll_tail = t_ll_tail;

            /* sanity... update the child and parent links to point the sentinel */
            c_node->left = &interval_merge_tree_sentinel;
            c_node->right = &interval_merge_tree_sentinel;
            c_node->parent = &interval_merge_tree_sentinel;

            /* merge the data linked lists */
            interval_merge_tree_interval_ll_merge(c_node, n_node);

            /* destroy the old copy of n_node */
            interval_merge_tree_destroy_node(*n_node);

            /* re-insert the mereged node... try and merge some more nodes */
            return interval_merge_tree_merge_intervals(root, &c_node, c_node->key);
        }
    }

    /* should not get here */
    return 1;
}

/* get the current size of the node if it is valid, else return 0 */
inline size_t interval_merge_tree_size(
    interval_merge_tree_node_t * node)
{
    if(node && node != &interval_merge_tree_sentinel)
    {
        return node->size;
    }
    return 0;
}

interval_merge_tree_node_t *interval_merge_tree_delete_min_node(
    interval_merge_tree_node_t ** root)
{
    interval_merge_tree_node_t *n = interval_merge_tree_min(*root);

    return interval_merge_tree_delete(root, n);
}
