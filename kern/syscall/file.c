#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <vnode.h>
#include <file.h>
#include <syscall.h>
#include <copyinout.h>

/*
 * Add your file-related functions here ...
 */

int allocate_fd_for_current_proc(struct file_handle* fh) {
    for (int fd = 0; fd < MAX_FILES_PER_PROCESS; fd++) {
        if (curproc->file_table[fd] == NULL) {
            curproc->file_table[fd] = fh; // 分配文件句柄到文件表
            return fd; // 返回文件描述符
        }
    }
    return -1; // 没有空闲的文件描述符
}

int sys_open(const char *filename, int flags, int mode, int *retval) {
    struct vnode *vn;
    struct file_handle *fh;
    int fd, result;

    // 第一步：将用户空间的文件名字符串复制到内核空间
    char kfilename[PATH_MAX];
    result = copyinstr((const_userptr_t)filename, kfilename, sizeof(kfilename), NULL);
    if (result) {
        return result; // 如果复制失败，返回错误
    }

    // 第二步：打开文件
    result = vfs_open(kfilename, flags, mode, &vn);
    if (result) {
        return result;
    }

    // 第三步：创建文件句柄
    fh = kmalloc(sizeof(*fh));
    if (fh == NULL) {
        vfs_close(vn);
        return ENOMEM;
    }

    fh->fh_vnode = vn;
    fh->fh_offset = 0; // 默认从文件开头开始
    fh->fh_flags = flags;
    fh->fh_refcount = 1;
    fh->fh_lock = lock_create(kfilename);
    if (fh->fh_lock == NULL) {
        vfs_close(vn);
        kfree(fh);
        return ENOMEM;
    }

    // 第四步：分配文件描述符
    // 这里假设有一个为当前进程分配文件描述符的函数
    fd = allocate_fd_for_current_proc(fh);
    if (fd < 0) {
        vfs_close(vn);
        lock_destroy(fh->fh_lock);
        kfree(fh);
        return EMFILE; // 太多打开的文件
    }

    *retval = fd; // 设置返回值为文件描述符
    return 0; // 成功
}

ssize_t sys_write(int fd, const void *buf, size_t nbytes) {
    if (fd < 0 || fd >= OPEN_MAX) {
        return -EBADF; // fd 不合法
    }
    if (buf == NULL) {
        return -EFAULT; // buf 指针无效
    }

    // 更进一步的检查可以包括验证 buf 指针指向的内存区域是否完全位于用户空间

    struct file_handle *fh = curproc->file_table[fd];
    if (fh == NULL) {
        return -EBADF; // 指定的文件描述符没有关联的文件句柄
    }

    if ((fh->fh_flags & O_ACCMODE) == O_RDONLY) {
        return -EBADF; // 文件不是以写模式打开
    }

    struct iovec iov;
    struct uio u;
    iov.iov_ubase = (userptr_t)buf;
    iov.iov_len = nbytes; // 要写入的字节数
    u.uio_iov = &iov;
    u.uio_iovcnt = 1;
    u.uio_resid = nbytes;  // 剩余未写入的字节数
    u.uio_offset = fh->fh_offset;
    u.uio_segflg = UIO_USERSPACE;
    u.uio_rw = UIO_WRITE;
    u.uio_space = curproc->p_addrspace;

    lock_acquire(fh->fh_lock); // 确保写操作的原子性
    int result = VOP_WRITE(fh->fh_vnode, &u);
    lock_release(fh->fh_lock);

    if (result) {
        return -EIO; // 写入过程中出错
    }

    ssize_t bytes_written = nbytes - u.uio_resid;
    fh->fh_offset += bytes_written; // 更新文件偏移量

    return bytes_written;
}