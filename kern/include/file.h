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
    struct vnode *fh_vnode; // 指向文件的vnode结构
    off_t fh_offset;        // 文件当前的偏移量
    int fh_flags;           // 打开文件时使用的标志，如O_RDONLY
    int fh_refcount;        // 引用计数，用于dup2等操作
    // struct lock *fh_lock;   // 文件操作的互斥锁
};

#endif /* _FILE_H_ */
