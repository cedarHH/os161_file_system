/*
 * Declarations for file handle and file table management.
 */

#ifndef _FILE_H_
#define _FILE_H_

/*
 * Contains some file-related maximum length constants
 */
#include <limits.h>
#include <vnode.h>

/*
 * Put your function declarations and data types here ...
 */

struct file_handle {
    struct vnode *fh_vnode; /* vnode is a virtual node structure that represents a file in the file system */
    off_t fh_offset;        /* current offset of the file is updated to reflect the current read or write position */
    int fh_flags;           /* indicate flags used when opening a file, which determine how the file is accessed */
    int fh_refcount;        /* managing memory life cycles. When fh_refcount reaches 0, the kernel can free the resources. */
};

#endif /* _FILE_H_ */
