/*
 *  Libstruct
 *  Copyright (C) 2012  Rafael Ubal (ubal@ece.neu.edu)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef LIB_UTIL_LIST_H
#define LIB_UTIL_LIST_H

/* Error codes generated by functions */
enum list_err_t
{
	LIST_ERR_OK = 0,
	LIST_ERR_BOUNDS,
	LIST_ERR_NOT_FOUND,
	LIST_ERR_EMPTY
};


struct list_t
{
	/* Public */
	char *name; /* star added, Name of the queue */
	int max_size; /* star added, queue's max size*/
	int count;  /* Number of elements in the list */
	int error_code;  /* Error code updated by functions */

	int locked;

	/* Private */
	int size;  /* Size of allocated vector */
	int head;  /* Head element in vector */
	int tail;  /* Tail element in vector */
	void **elem;  /* Vector of elements */
};


/** Iterate through all element of linked list.
 *
 * @param list
 * @param iterator
 * 	Integer variable used to iterate.
 */
#define LIST_FOR_EACH(list, iter) \
	for ((iter) = 0; (iter) < list_count((list)); (iter)++)


/** Create a list based on a vector. Insertion/deletion operations at the head
 * or tail of the list are done in constant time. Insertion/deletion cost in
 * intermediate positions depends on the distance between head/tail to the
 * inserted/deleted index.
 *
 * @return
 *	Pointer to the list object created.
 */
struct list_t *list_create(void);


/** Create a list with an initial size for the containing vector.
 * This function should be used instead of 'list_create' when the size of the
 * list is known in beforehand. This will prevent insertion functions to
 * dynamically resize the vector.
 *
 * @return
 *	Pointer to the list object created.
 */
struct list_t *list_create_with_size(int size);


/** Free list.
 *
 * @param list
 *	List object.
 */
void list_free(struct list_t *list);


/** Get number of elements.
 *
 * @param list
 *	List object.
 *
 * @return
 *	Number of elements in the list. Calling this function is equivalent to
 *	consulting the value of 'list->count'. This function does not update the
 *	error code.
 */
int list_count(struct list_t *list);


/** Add an element at the end of the list.
 *
 * @param list
 *	List object.
 * @return
 *	No value is return. The function sets the error code to LIST_ERR_OK.
 */
void list_add(struct list_t *list, void *elem);


/** Get an element from the list.
 *
 * @param list
 *	List object.
 * @param index
 *	Position in the list. This must be a value between 0 and the number of
 *	elements in the list minus one.
 *
 * @return
 *	The element at position 'index' is returned, or NULL if index is out of
 *	bounds. The error code is set to one of the following values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_BOUNDS
 *		Index out of bounds.
 */
void *list_get(struct list_t *list, int index);


/** Set the value of an element in the list.
 *
 * @param list
 *	List object.
 * @param index
 *	Position of the list to change value. Must be a value between 0 and the
 *	number of elements in the list minus 1.
 *
 * @return
 *	No value is returned. The error code is set to one of the following
 *	values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_BOUNDS
 *		Index out of bounds.
 */
void list_set(struct list_t *list, int index, void *elem);


/** Insert an element in the list.
 *
 * @param list
 *	List object.
 * @param index
 *	Position before which the element should be inserted. Must be a value
 *	between 0 and N (included). If it is N, the element will be inserted at
 *	the end of the list (as in 'list_add').
 *
 * @return
 *	No value is returned. The error code is set to one of the following
 *	values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_BOUNDS
 *		Index out of bounds.
 */
void list_insert(struct list_t *list, int index, void *elem);


/** Return the first occurrence of an element in the list.
 *
 * @param list
 *	List object.
 * @param elem
 *	Element to look for.
 *
 * @return
 *	The index of the first occurrence of the element in the list is
 *	returned, or -1 if the element is not present in the list. The error
 *	code is set to one of the following values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_NOT_FOUND
 *		Element not found in the list.
 */
int list_index_of(struct list_t *list, void *elem);


/** Remove an element from the list.
 *
 * @param list
 *	List object.
 * @param index
 *	Position of the element to remove. Must be a value between 0 and the
 *	number of elements in the list minus 1.
 *
 * @return
 *	The removed element is returned, or NULL if an invalid position was
 *	given in 'index'. The error code is set to one of the following values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_BOUNDS
 *		Index out of bounds.
 */
void *list_remove_at(struct list_t *list, int index);


/** Remove an element from the list.
 *
 * @param list
 *	List object.
 * @param elem
 *	The first occurrence of this element will be removed from the list.
 *
 * @return
 *	The removed element is returned, or NULL if the element is not present
 *	in the list. The error code is set to one of the following values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_NOT_FOUND
 *		Element not found in the list.
 */
void *list_remove(struct list_t *list, void *elem);


/** Remove all elements from the list.
 *
 * @param list
 *	List object
 *
 * @return
 *	No value is returned. The error code is set to LIST_ERR_OK.
 */
void list_clear(struct list_t *list);


/** Treating the list as a stack, push an element. This is equivalent to adding
 * an element at the end of the list. This operation is done in constant cost.
 *
 * @param list
 *	List object.
 * @param elem
 *	Element to push.
 *
 * @return
 *	No value is return. The function sets the error code to LIST_ERR_OK.
 */
void list_push(struct list_t *list, void *elem);


/** Treating the list as a stack, pop the element at the top of the stack. This
 * is equivalent to removing the element in the last position of the list. This
 * operation is done in constant cost.
 *
 * @param list
 *	List object.
 *
 * @return
 *	The value at the top of the stack is returned, or NULL if the stack was
 *	empty. The error code is set to one of the following values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_EMPTY
 *		Stack is empty.
 */
void *list_pop(struct list_t *list);


/** Treating the list as a stack, get the element at the top of the stack. This
 * is equivalent to consulting the element in the last position of the list.
 *
 * @param list
 *	List object.
 *
 * @return
 *	The value at the top of the stack is returned, or NULL if the stack was
 *	empty. The error code is set to one of the following values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_EMPTY
 *		Stack is empty.
 */
void *list_top(struct list_t *list);


/** Treating the list as a stack, get the element at the bottom of the stack. This
 * is equivalent to consulting the element in the first position of the list.
 *
 * @param list
 *	List object.
 *
 * @return
 *	The value at the bottom of the stack is returned, or NULL if the stack was
 *	empty. The error code is set to one of the following values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_EMPTY
 *		Stack is empty.
 */
void *list_bottom(struct list_t *list);

/** Treating the list as a FIFO queue, enqueue an element. This is equivalent to
 * adding an element at the end of the list. The operation is done in constant
 * cost.
 *
 * @param list
 *	List object.
 * @param elem
 *	Element to push.
 *
 * @return
 *	No value is return. The function sets the error code to LIST_ERR_OK.
 */
void list_enqueue(struct list_t *list, void *elem);


/** Treating the list as a queue, dequeue the element at the head of the queue.
 * This is equivalent to removing the first element of the list. The operation
 * is done in constant cost.
 *
 * @param list
 *	List object.
 *
 * @return
 *	The value that was dequeued is returned, or NULL if the queue was empty.
 *	The error code is set to one of the following values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_EMPTY
 *		Queue is empty.
 */
void *list_dequeue(struct list_t *list);


/** Treating the list as a queue, get the element at the head of the queue.
 * This is equivalent to consulting the first element of the list.
 *
 * @param list
 *	List object.
 *
 * @return
 *	The value at the head of the queue is returned, or NULL if the queue is
 *	empty. The error code is set to one of the following values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_EMPTY
 *		Queue is empty.
 */
void *list_head(struct list_t *list);


/** Treating the list as a queue, get the element at the tail of the queue.
 * This is equivalent to consulting the last element of the list.
 *
 * @param list
 *	List object.
 *
 * @return
 *	The value at the tail of the queue is returned, or NULL if the queue is
 *	empty. The error code is set to one of the following values:
 *
 *	LIST_ERR_OK
 *		No error.
 *	LIST_ERR_EMPTY
 *		Queue is empty.
 */
void *list_tail(struct list_t *list);


/** Sort list.
 *
 * @param list
 *	List object.
 * @param comp
 *	Comparison call-back function. The function takes two pointers A and B,
 *	and should return -1 if A < B, 0 if A = B, and 1 if A > B.
 */
void list_sort(struct list_t *list,
	int (*comp)(const void *, const void *));


#endif

