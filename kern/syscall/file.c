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
        return -result; // 如果复制失败，返回错误
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
        return -ENOMEM;
    }

    fh->fh_vnode = vn;
    fh->fh_offset = 0; // 默认从文件开头开始
    fh->fh_flags = flags;
    fh->fh_refcount = 1;
    // fh->fh_lock = lock_create(kfilename);
    // if (fh->fh_lock == NULL) {
    //     vfs_close(vn);
    //     kfree(fh);
    //     return ENOMEM;
    // }

    // 第四步：分配文件描述符
    // 这里假设有一个为当前进程分配文件描述符的函数
    fd = allocate_fd_for_current_proc(fh);
    if (fd < 0) {
        vfs_close(vn);
        // lock_destroy(fh->fh_lock);
        kfree(fh);
        return -EMFILE; // 太多打开的文件
    }

    *retval = fd; // 设置返回值为文件描述符
    return 0; // 成功
}

ssize_t sys_read(int fd, void *buf, size_t buflen) {
    // 检查文件描述符的有效性
    if (fd < 0 || fd >= OPEN_MAX) {
        return -EBADF;
    }

    // 获取当前进程的文件表
    struct file_handle *fh = curproc->file_table[fd];
    if (fh == NULL) {
        return -EBADF;
    }

    // 检查文件是否以读方式打开
    if ((fh->fh_flags & O_ACCMODE) == O_WRONLY) {
        return -EBADF;
    }

    // 准备 uio 结构体，以用户空间模式从当前文件偏移量开始读取
    struct iovec iov;
    struct uio u;
    iov.iov_ubase = (userptr_t)buf;
    iov.iov_len = buflen;
    u.uio_iov = &iov;
    u.uio_iovcnt = 1;
    u.uio_offset = fh->fh_offset;
    u.uio_resid = buflen;
    u.uio_segflg = UIO_USERSPACE;
    u.uio_rw = UIO_READ;
    u.uio_space = curproc->p_addrspace;

    // 获取锁以保证读操作的原子性
    // lock_acquire(fh->fh_lock);
    int result = VOP_READ(fh->fh_vnode, &u);
    // lock_release(fh->fh_lock);

    if (result) {
        return -EIO;
    }

    // 计算实际读取的字节数，并更新文件偏移量
    ssize_t bytes_read = buflen - u.uio_resid;
    fh->fh_offset += bytes_read;

    return bytes_read;
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

    // lock_acquire(fh->fh_lock); // 确保写操作的原子性
    int result = VOP_WRITE(fh->fh_vnode, &u);
    // lock_release(fh->fh_lock);

    if (result) {
        return -EIO; // 写入过程中出错
    }

    ssize_t bytes_written = nbytes - u.uio_resid;
    fh->fh_offset += bytes_written; // 更新文件偏移量

    return bytes_written;
}

int sys_close(int fd) {
    // 检查文件描述符的有效性
    if (fd < 0 || fd >= OPEN_MAX) {
        return -EBADF;
    }

    // 获取当前进程的文件表
    struct file_handle *fh = curproc->file_table[fd];
    if (fh == NULL) {
        return -EBADF;
    }

    // 加锁以同步对文件句柄的操作
    // lock_acquire(fh->fh_lock);

    // 减少文件句柄的引用计数
    fh->fh_refcount--;

    // 如果没有更多的引用，释放文件句柄和相关资源
    if (fh->fh_refcount == 0) {
        vfs_close(fh->fh_vnode);
        // lock_release(fh->fh_lock);
        // lock_destroy(fh->fh_lock);
        kfree(fh);
    } else {
        // lock_release(fh->fh_lock);
    }

    // 清除文件描述符表中的条目
    curproc->file_table[fd] = NULL;

    return 0; // 成功
}


off_t sys_lseek(int fd, off_t pos, int whence){   
    struct vnode *vnode;
    struct file_handle *fileHandle;
    off_t offset;
    struct stat file_stat;

    if (fd < 0 || fd >= OPEN_MAX) {
        return -EBADF;
    }
    fileHandle = curproc->file_table[fd];
    if (fileHandle == NULL){
        return -EBADF;
    }

    vnode = fileHandle->fh_vnode;

    if (VOP_STAT(vnode, &file_stat)) {
        return -EBADF;
    }
    if (!VOP_ISSEEKABLE(vnode)) {
        return -ESPIPE;
    }

    switch (whence) {
        case SEEK_SET:
            offset = pos;
            break;
        case SEEK_CUR:
            offset = fileHandle->fh_offset + pos;
            break;
        case SEEK_END:
            offset = file_stat.st_size + pos;
            break;
        default:
            return -EINVAL;
    }

    if (offset < 0) {
        return -EINVAL;
    }

    fileHandle->fh_offset = offset;
    return offset;
}

int sys_dup2(int old_fd, int new_fd){
    struct file_handle *oldFileHandle;

    oldFileHandle = curproc->file_table[old_fd];
    if (old_fd < 0 || old_fd >= OPEN_MAX || oldFileHandle == NULL) {
        return -EBADF;

    }
    if (new_fd < 0 || new_fd >= OPEN_MAX) {
        return -EBADF;
    }
    
    if (old_fd == new_fd) {
        return new_fd;
    }

    
    if (curproc->file_table[new_fd] != NULL) {
        sys_close(new_fd);
    }
    curproc->file_table[new_fd] = oldFileHandle;
    curproc->file_table[new_fd]->fh_refcount += 1;
    return new_fd;
}