#include "util/red-black-tree.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <assert.h>

/* rb tree sentinel */
static size_t red_black_tree_sentinel_key_key = 0;
static size_t red_black_tree_sentinel_key_value = 0;
static red_black_tree_key_t red_black_tree_sentinel_key = {&red_black_tree_sentinel_key_key, &red_black_tree_sentinel_key_value};
static red_black_tree_node_t red_black_tree_sentinel = {{&red_black_tree_sentinel_key_key, &red_black_tree_sentinel_key_value},NULL,NULL,NULL,0,0,{0,0},NULL,NULL,RB_TREE_COLOR_BLACK,0,NULL};

#define INTERVAL_TREE_LL_POOL_SIZE 4096
#define INTERVAL_TREE_NODE_POOL_SIZE 4096
static size_t red_black_tree_ll_pool_size = 0;
static size_t red_black_tree_node_pool_size = 0;
static size_t ll_pool_next = 0;
static size_t node_pool_next = 0;
static red_black_tree_node_t * red_black_tree_node_pool = NULL;
static red_black_tree_node_t ** red_black_tree_node_pool_index = NULL;
static red_black_tree_node_t * red_black_tree_node_pool_min = NULL;
static red_black_tree_node_t * red_black_tree_node_pool_max = NULL;
static red_black_tree_interval_ll_t * red_black_tree_ll_pool = NULL;
static red_black_tree_interval_ll_t ** red_black_tree_ll_pool_index = NULL;
static red_black_tree_interval_ll_t * red_black_tree_ll_pool_min = NULL;
static red_black_tree_interval_ll_t * red_black_tree_ll_pool_max = NULL;

static inline int red_black_tree_create_ll_pool(size_t num)
{
    size_t i = 0;
    red_black_tree_ll_pool = (red_black_tree_interval_ll_t *)malloc(sizeof(red_black_tree_interval_ll_t) * num);
    red_black_tree_ll_pool_index = (red_black_tree_interval_ll_t **)malloc(sizeof(red_black_tree_interval_ll_t *) * num);
    red_black_tree_ll_pool_size = num;
    ll_pool_next = 0;
    red_black_tree_ll_pool_min = &(red_black_tree_ll_pool[0]);
    red_black_tree_ll_pool_max = &(red_black_tree_ll_pool[num - 1]);

    for(i = 0 ; i < num ; i++)
    {
        red_black_tree_ll_pool_index[i] = &(red_black_tree_ll_pool[i]);
    }

    return 0;
}

static inline int red_black_tree_destroy_ll_pool()
{
    free(red_black_tree_ll_pool);
    red_black_tree_ll_pool = NULL;
    free(red_black_tree_ll_pool_index);
    red_black_tree_ll_pool_index = NULL;
    ll_pool_next = 0;
    red_black_tree_ll_pool_size = 0;
    red_black_tree_ll_pool_min = NULL;
    red_black_tree_ll_pool_max = NULL;

    return 0;
}

static inline red_black_tree_interval_ll_t * red_black_tree_alloc_ll()
{
    if(ll_pool_next < red_black_tree_ll_pool_size)
    {
        return red_black_tree_ll_pool_index[ll_pool_next++];
    }
    else
    {
        return (red_black_tree_interval_ll_t *) malloc(sizeof(red_black_tree_interval_ll_t));
    }

    return NULL;
}

static inline int red_black_tree_dealloc_ll(red_black_tree_interval_ll_t * node)
{
    if(node >= red_black_tree_ll_pool_min && node <= red_black_tree_ll_pool_max)
    {
        red_black_tree_ll_pool_index[--ll_pool_next] = node;
    }
    else
    {
        free(node);
    }

    return 0;
}

static inline int red_black_tree_create_node_pool(size_t num)
{
    size_t i = 0;
    red_black_tree_node_pool = (red_black_tree_node_t *)malloc(sizeof(red_black_tree_node_t) * num);
    red_black_tree_node_pool_index = (red_black_tree_node_t **)malloc(sizeof(red_black_tree_node_t *) * num);
    red_black_tree_node_pool_size = num;
    node_pool_next = 0;
    red_black_tree_node_pool_min = &(red_black_tree_node_pool[0]);
    red_black_tree_node_pool_max = &(red_black_tree_node_pool[num - 1]);

    for(i = 0 ; i < num ; i++)
    {
        red_black_tree_node_pool_index[i] = &(red_black_tree_node_pool[i]);
    }

    return 0;
}

static inline int red_black_tree_destroy_node_pool()
{
    free(red_black_tree_node_pool);
    red_black_tree_node_pool = NULL;
    free(red_black_tree_node_pool_index);
    red_black_tree_node_pool_index = NULL;
    red_black_tree_node_pool_size = 0;
    node_pool_next = 0;
    red_black_tree_node_pool_min = NULL;
    red_black_tree_node_pool_max = NULL;

    return 0;
}

static inline red_black_tree_node_t * red_black_tree_alloc_node()
{
    if(node_pool_next < red_black_tree_node_pool_size)
    {
        return red_black_tree_node_pool_index[node_pool_next++];
    }
    else
    {
        return (red_black_tree_node_t *) malloc(sizeof(red_black_tree_node_t));
    }

    return NULL;
}

static inline int red_black_tree_dealloc_node(red_black_tree_node_t * node)
{
    if(node >= red_black_tree_node_pool_min && node <= red_black_tree_node_pool_max)
    {
        red_black_tree_node_pool_index[--node_pool_next] = node;
    }
    else
    {
        free(node);
    }

    return 0;
}

static inline red_black_tree_interval_ll_t * red_black_tree_interval_ll_create()
{
    red_black_tree_interval_ll_t * ll = (red_black_tree_interval_ll_t *)malloc(sizeof(red_black_tree_interval_ll_t));
    //red_black_tree_interval_ll_t * ll = red_black_tree_alloc_ll();

    ll->next = NULL;
    ll->data = NULL;
    ll->key = NULL;

    return ll;
}

inline int red_black_tree_interval_ll_destroy(red_black_tree_interval_ll_t * ll)
{
    red_black_tree_interval_ll_t * cur = ll;
    red_black_tree_interval_ll_t * next = NULL;
    while(cur)
    {
        next = cur->next;
        cur->next = NULL;
        cur->data = NULL;
        free(cur);
        //red_black_tree_dealloc_ll(cur);
        cur = next;
    }

    return 0;
}

/* copy the n2 list into the n1 list */
static inline int red_black_tree_interval_ll_merge(red_black_tree_node_t * n1, red_black_tree_node_t ** n2)
{
    red_black_tree_interval_ll_t * l2_head = (*n2)->ll_head;
    red_black_tree_interval_ll_t * l2_tail = (*n2)->ll_tail;

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

inline int red_black_tree_interval_ll_init(red_black_tree_node_t * node, void * data)
{

    /* create the link list for the node and update the head with the data */
    node->ll_head = red_black_tree_interval_ll_create();
    node->ll_head->data = data;
    node->ll_head->key = node->key.key;
    node->ll_tail = node->ll_head;

    return 0;
}

/* determine the max value for an interval or node */
inline static size_t red_black_tree_interval_max(red_black_tree_node_t * node)
{
    if(node->interval.end > node->left->max)
    {
        if(node->interval.end > node->right->max)
        {
            return node->interval.end;
        }
        else /* if(i->end <= node->right->max) */
        {
            return node->right->max;
        }
    }
    else /* if(i->end <= node->left->max) */
    {
        if(node->left->max > node->right->max)
        {
            return node->left->max;
        }
        else /* if(node->left->max <= node->left->max) */
        {
            return node->right->max;
        }
    }

    /* should not get here */
    return 0;
}

/* create an empty node */
inline red_black_tree_node_t * red_black_tree_create_node()
{
    red_black_tree_node_t * node = NULL;

    /* try to get a node from the node pool first */
#if 0
    node = red_black_tree_node_pool_alloc_node();
    if(!node)
#endif
    {
        node = (red_black_tree_node_t *)malloc(sizeof(red_black_tree_node_t));
        //node = red_black_tree_alloc_node();
        node->node_pool = 0;
    }

    /* set child and parent pointers to NULL */
    node->left = &red_black_tree_sentinel;
    node->right = &red_black_tree_sentinel;
    node->parent = &red_black_tree_sentinel;

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
inline int red_black_tree_destroy_node(red_black_tree_node_t * node)
{
    if(node && node != &red_black_tree_sentinel)
    {
        node->left = NULL;
        node->right = NULL;

        /* destroy the key */
        node->key.key = NULL;
        node->key.value = NULL;

        /* do not attempt to destroy the parent node ... just set to NULL*/
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
            red_black_tree_node_pool_free_node(node);
        }
        else
#endif
        {
            free(node);
            //red_black_tree_dealloc_node(node);
        }
    }
    return 0;
}

/* recycle a node */
inline int red_black_tree_recycle_node(red_black_tree_node_t * node)
{
    if(node && node != &red_black_tree_sentinel)
    {
        node->left = NULL;
        node->right = NULL;

        /* destroy the key */
        node->key.key = NULL;
        node->key.value = NULL;

        /* do not attempt to destroy the parent node ... just set to NULL*/
        node->parent = NULL;

        /* set the color to 0 */
        node->color = 0;

        /* set the max interval value */
        node->max = 0;

        /* destroy the interval */
        //red_black_tree_destroy_interval(node->interval);
        node->interval.start = 0;
        node->interval.end = 0;

        /* erase the pointers to, but do not destroy, the link list */
        node->ll_head = NULL;
        node->ll_tail = NULL;
    }
    return 0;
}

/* destroy a node and it's children... but not the node parent */
int red_black_tree_destroy_tree(red_black_tree_node_t * node)
{
    if(node && node != &red_black_tree_sentinel)
    {
        /* destroy the left node */
        if(node->left && node->left != &red_black_tree_sentinel)
        {
            red_black_tree_destroy_tree(node->left);
            node->left = NULL;
        }

        /* destory the right node */
        if(node->right && node->right != &red_black_tree_sentinel)
        {
            red_black_tree_destroy_tree(node->right);
            node->right = NULL;
        }

        /* destroy the key */
        node->key.key = NULL;
        node->key.value = NULL;

        /* do not attempt to destroy the parent node ... just set to NULL*/
        node->parent = NULL;

        /* set the color to 0 */
        node->color = 0;

        /* set the max interval value */
        node->max = 0;

        /* destroy the interval */
        node->interval.start = 0;
        node->interval.end = 0;

        /* cleanup the interval link list */
        red_black_tree_interval_ll_destroy(node->ll_head);
        node->ll_head = NULL;
        node->ll_tail = NULL;

        /* destroy the node */
#if 0
        if(node->node_pool)
        {
            red_black_tree_node_pool_free_node(node);
        }
        else
#endif
        {
            free(node);
            //red_black_tree_dealloc_node(node);
        }
    }

    /* cleanup the mem pools */
    //red_black_tree_destroy_ll_pool();
    //red_black_tree_destroy_node_pool();

    return 0;
}

/* rotate the nodes to the left */
static inline int red_black_tree_rotate_left(red_black_tree_node_t ** root, red_black_tree_node_t * x, int op)
{
    red_black_tree_node_t * y = x->right;

    x->right = y->left;
    if(y->left && y->left != &red_black_tree_sentinel)
    {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if(!x->parent || x->parent == &red_black_tree_sentinel)
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
        if(x != &red_black_tree_sentinel && y != &red_black_tree_sentinel)
        {
            y->size = x->size;
            assert(x->right);
            assert(x->left);
            x->size = x->left->size + x->right->size + 1;
        }
    }
    else
    {
        if(x != &red_black_tree_sentinel && y != &red_black_tree_sentinel)
        {
            y->size = x->size;
            assert(x->right);
            assert(x->left);
            x->size = x->left->size - x->right->size - 1;
        }
    }

    /* update the max interval */
    y->max = red_black_tree_interval_max(y);
    x->max = red_black_tree_interval_max(x);

    return 0;
}

/* rotate the nodes to the right */
static inline int red_black_tree_rotate_right(red_black_tree_node_t ** root, red_black_tree_node_t * y, int op)
{
    red_black_tree_node_t * x = y->left;

    y->left = x->right;
    if(x->right && x->right != &red_black_tree_sentinel)
    {
        x->right->parent = y;
    }
    x->parent = y->parent;
    if(!y->parent || y->parent == &red_black_tree_sentinel)
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
        if(x != &red_black_tree_sentinel && y != &red_black_tree_sentinel)
        {
            x->size = y->size;
            assert(y->left);
            assert(y->right);
            y->size = y->left->size + y->right->size + 1;
        }
    }
    else
    {
        if(x != &red_black_tree_sentinel && y != &red_black_tree_sentinel)
        {
            x->size = y->size;
            assert(y->left);
            assert(y->right);
            y->size = y->left->size - y->right->size - 1;
        }
    }

    /* update the max interval */
    x->max = red_black_tree_interval_max(x);
    y->max = red_black_tree_interval_max(y);

    return 0;
}

/* function to make sure the red-black tree properties are followed after inserts */
static int red_black_tree_insert_rules(red_black_tree_node_t ** root, red_black_tree_node_t * node)
{
    red_black_tree_node_t * y = NULL;
    while(node->parent->color == RB_TREE_COLOR_RED)
    {
        if(node->parent == node->parent->parent->left)
        {
            y = node->parent->parent->right;

            if(y->color == RB_TREE_COLOR_RED)
            {
                node->parent->color= RB_TREE_COLOR_BLACK;
                y->color = RB_TREE_COLOR_BLACK;
                node->parent->parent->color = RB_TREE_COLOR_RED;
                node = node->parent->parent;
            }
            else
            {
                if(node == node->parent->right)
                {
                    node = node->parent;
                    red_black_tree_rotate_left(root, node, RB_TREE_INSERT);
                }
                node->parent->color = RB_TREE_COLOR_BLACK;
                node->parent->parent->color = RB_TREE_COLOR_RED;
                red_black_tree_rotate_right(root, node->parent->parent, RB_TREE_INSERT);
            }
        }
        else
        {
            y = node->parent->parent->left;

            if(y->color == RB_TREE_COLOR_RED)
            {
                node->parent->color= RB_TREE_COLOR_BLACK;
                y->color = RB_TREE_COLOR_BLACK;
                node->parent->parent->color = RB_TREE_COLOR_RED;
                node = node->parent->parent;
            }
            else
            {
                if(node == node->parent->left)
                {
                    node = node->parent;
                    red_black_tree_rotate_right(root, node, RB_TREE_INSERT);
                }
                node->parent->color = RB_TREE_COLOR_BLACK;
                node->parent->parent->color = RB_TREE_COLOR_RED;
                red_black_tree_rotate_left(root, node->parent->parent, RB_TREE_INSERT);
            }
        }
    }
    (*root)->color = RB_TREE_COLOR_BLACK;

    return 0;
}

/* insert a node into the rb tree */
int red_black_tree_insert(red_black_tree_node_t ** node, red_black_tree_key_t key, red_black_tree_node_t ** root)
{
    red_black_tree_node_t * y = &red_black_tree_sentinel;
    red_black_tree_node_t * x = *root;

    /* if x is null, make it a sentinel */
    if(!x)
    {
        x = &red_black_tree_sentinel;
    }

    /* if this is a new tree */
    if(!*root)
    {
        /* create new memory pools */
        //red_black_tree_create_ll_pool(INTERVAL_TREE_LL_POOL_SIZE);
        //red_black_tree_create_node_pool(INTERVAL_TREE_NODE_POOL_SIZE);
    }

    if(!*node)
    {
        *node = red_black_tree_create_node();

        /* update the size of the new node */
        (*node)->size = 1;

        /* set the node max to the end of the interval */
        (*node)->max = (*node)->interval.end;
    }

    /* set the key */
    (*node)->key = key;

    while(x != &red_black_tree_sentinel)
    {
        y = x;

        /* this is a duplicate... don't insert it */
        if(*(size_t *)key.key == *(size_t *)x->key.key)
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

            if(*(size_t *)key.key < *(size_t *)x->key.key)
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
    if(y == &red_black_tree_sentinel)
    {
        (*root) = (*node);
    }
    else
    {
        if(*(size_t *)key.key < *(size_t *)y->key.key)
        {
            y->left = (*node);
        }
        else
        {
            y->right = (*node);
        }
    }

    /* set the node child pointers */
    (*node)->left = &red_black_tree_sentinel;
    (*node)->right = &red_black_tree_sentinel;

    /* set the color to RED */
    (*node)->color = RB_TREE_COLOR_RED;

    /* run the red-black tree rules */
    red_black_tree_insert_rules(root, *node);

    return 0;
}

/* enforces red-black tree rules after deletes */
static int red_black_tree_delete_rules(red_black_tree_node_t ** root, red_black_tree_node_t * node)
{
    red_black_tree_node_t * w = NULL;

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
                red_black_tree_rotate_left(root, node->parent, RB_TREE_DELETE);
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
                    red_black_tree_rotate_right(root, w, RB_TREE_DELETE);
                    w = node->parent->right;
                }
                w->color = node->parent->color;
                node->parent->color = RB_TREE_COLOR_BLACK;
                w->right->color = RB_TREE_COLOR_BLACK;
                red_black_tree_rotate_left(root, node->parent, RB_TREE_DELETE);
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
                red_black_tree_rotate_right(root, node->parent, RB_TREE_DELETE);
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
                    red_black_tree_rotate_left(root, w, RB_TREE_DELETE);
                    w = node->parent->left;
                }
                w->color = node->parent->color;
                node->parent->color = RB_TREE_COLOR_BLACK;
                w->left->color = RB_TREE_COLOR_BLACK;
                red_black_tree_rotate_right(root, node->parent, RB_TREE_DELETE);
                node = *root;
            }
        }
    }

    node->color = RB_TREE_COLOR_BLACK;

    return 0;
}

/* find the min value from a given node in the tree */
static inline red_black_tree_node_t * red_black_tree_min(red_black_tree_node_t * node)
{
    if(!node)
        return NULL;

    while(node->left != &red_black_tree_sentinel)
    {
        node = node->left;
    }
    return node;
}

/* find the node in the tree that is the next greatest value */
static inline red_black_tree_node_t * red_black_tree_successor(red_black_tree_node_t * node)
{
    red_black_tree_node_t * y = NULL;

    if(!node)
        return NULL;

    if(node->right != &red_black_tree_sentinel)
    {
        return red_black_tree_min(node->right);
    }

    y = node->parent;

    while(y != &red_black_tree_sentinel && node == y->right)
    {
        node = y;
        y = y->parent;
    }

    return y;
}

/* delete a node from the tree */
red_black_tree_node_t * red_black_tree_find_delete(red_black_tree_node_t ** root, size_t k)
{
    red_black_tree_node_t * x = NULL;
    red_black_tree_node_t * y = NULL;
    red_black_tree_node_t * node = NULL;

    /* make sure node is not null */
    if(!node)
    {
        return NULL;
    }

    if((!node->left || node->left == &red_black_tree_sentinel) || (!node->right || node->right == &red_black_tree_sentinel))
    {
        y = node;
    }
    else
    {
        y = red_black_tree_successor(node);
    }

    if(y->left && y->left != &red_black_tree_sentinel)
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

    if(y->parent == &red_black_tree_sentinel)
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
        red_black_tree_delete_rules(root, x);
    }

    return y;
}

/* delete a node from the tree */
red_black_tree_node_t * red_black_tree_delete(red_black_tree_node_t ** root, red_black_tree_node_t * node)
{
    red_black_tree_node_t * x = NULL;
    red_black_tree_node_t * y = NULL;

    /* make sure node is not null */
    if(!node)
    {
        return NULL;
    }

    if((!node->left || node->left == &red_black_tree_sentinel) || (!node->right || node->right == &red_black_tree_sentinel))
    {
        y = node;
    }
    else
    {
        y = red_black_tree_successor(node);
    }

    if(y->left && y->left != &red_black_tree_sentinel)
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

    if(y->parent == &red_black_tree_sentinel)
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
        node->max = red_black_tree_interval_max(node);
        node->ll_head = y->ll_head;
        node->ll_tail = y->ll_tail;
    }

    if(y->color == RB_TREE_COLOR_BLACK)
    {
        red_black_tree_delete_rules(root, x);
    }

    return y;
}

/* print the node */
int red_black_tree_print(red_black_tree_node_t * node, int level, char dir)
{
    if(!node || node == &red_black_tree_sentinel)
    {
        return 0;
    }

    /* print the node value */
    fprintf(stderr, "level=%i, node=internal, color=%c, dir=%c, key=%lu, size=%i, max=%i, start=%lu, end=%lu\n",
        level, node->color, dir, *(size_t *)node->key.key, node->size, node->max, node->interval.start, node->interval.end);

    /* descend the right nodes */
    red_black_tree_print(node->right, level + 1, 'R');

    /* descend the left nodes */
    red_black_tree_print(node->left, level + 1, 'L');

    return 0;
}

/* print the tree */
int red_black_tree_print_tree(red_black_tree_node_t * node)
{
    fprintf(stderr, "print tree:\n");
    return red_black_tree_print(node, 0, 'C');
}

/* find the key in the tree */
red_black_tree_key_t red_black_tree_key_find(red_black_tree_node_t * node, size_t k)
{
    if(node && node != &red_black_tree_sentinel)
    {
        /* check for valid key */
        if(!node->key.key)
            return red_black_tree_sentinel_key;

        if(*(size_t *)node->key.key == k)
        {
            return node->key;
        }
        else if(k < *(size_t *)node->key.key)
        {
            return red_black_tree_key_find(node->left, k);
        }
        else if(k > *(size_t *)node->key.key)
        {
            return red_black_tree_key_find(node->right, k);
        }
    }
    return red_black_tree_sentinel_key;
}

/* find the node in the tree given a key */
red_black_tree_node_t * red_black_tree_node_find(red_black_tree_node_t * node, size_t k)
{
    if(node && node != &red_black_tree_sentinel)
    {
        /* verify that pointer is valid */ 
        if(node->key.key == NULL)
            return NULL;

        if(*(size_t *)node->key.key == k)
        {
            return node;
        }
        else if(k < *(size_t *)node->key.key)
        {
            return red_black_tree_node_find(node->left, k);
        }
        else if(k > *(size_t *)node->key.key)
        {
            return red_black_tree_node_find(node->right, k);
        }
    }
    return NULL;
}
