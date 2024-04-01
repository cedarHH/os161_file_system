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
            curproc->file_table[fd] = fh;
            return fd;
        }
    }
    return -1;
}

int sys_open(const char *filename, int flags, int mode, int *retval) {
    struct vnode *vn;
    struct file_handle *fh;
    int fd, result;         /* File Descriptor, an integer value that uniquely identifies an open file or other I/O resource */

    /* Copy the filename string from userspace to kernel */
    char kfilename[PATH_MAX];                                                          /* kernel file name[the max len of file path]*/
    result = copyinstr((const_userptr_t)filename, kfilename, sizeof(kfilename), NULL); /* const_userptr_t is a pointer to user space */
    if (result) {
        return -result; /* return error */
    }

    result = vfs_open(kfilename, flags, mode, &vn); /* mode is permissions of the newly created file when the flags contains the O_CREAT flag */
    if (result) {
        return result;
    }

    fh = kmalloc(sizeof(*fh));
    if (fh == NULL) {
        vfs_close(vn);
        return -ENOMEM;
    }
    fh->fh_vnode = vn;
    fh->fh_offset = 0;
    fh->fh_flags = flags;
    fh->fh_refcount = 1;

    /* allocate file description */
    fd = allocate_fd_for_current_proc(fh);
    if (fd < 0) {
        vfs_close(vn);
        kfree(fh);
        return -EMFILE; /* too many files */
    }

    *retval = fd; /* return fd */
    return 0;
}

int sys_close(int fd) {
    if (fd < 0 || fd >= OPEN_MAX) {
        return -EBADF; /* bad file descriptor */
    }

    struct file_handle *fh = curproc->file_table[fd];
    if (fh == NULL) {
        return -EBADF;
    }

    fh->fh_refcount--;

    if (fh->fh_refcount == 0) {
        vfs_close(fh->fh_vnode);
        kfree(fh);
    }

    curproc->file_table[fd] = NULL;

    return 0;
}

ssize_t sys_read(int fd, void *buf, size_t buflen) {
    if (fd < 0 || fd >= OPEN_MAX) {
        return -EBADF;
    }

    struct file_handle *fh = curproc->file_table[fd];
    if (fh == NULL) {
        return -EBADF;
    }

    if ((fh->fh_flags & O_ACCMODE) == O_WRONLY) {
        return -EBADF;
    }

    struct iovec iov;
    struct uio u;
    iov.iov_ubase = (userptr_t)buf;
    iov.iov_len = buflen;
    u.uio_iov = &iov;
    u.uio_iovcnt = 1;                  /* iov count 1 */
    u.uio_offset = fh->fh_offset;
    u.uio_resid = buflen;              /* remaining to be read */
    u.uio_segflg = UIO_USERSPACE;      /* user space */
    u.uio_rw = UIO_READ;               /* READ operation */
    u.uio_space = curproc->p_addrspace;

    int result = VOP_READ(fh->fh_vnode, &u); /* pointer to uio */

    if (result) {
        return -EIO; /* IO error */
    }

    ssize_t bytes_read = buflen - u.uio_resid;  /* bytes actually read into buffer buf */
    fh->fh_offset += bytes_read;

    return bytes_read;
}

ssize_t sys_write(int fd, const void *buf, size_t nbytes) {
    if (fd < 0 || fd >= OPEN_MAX) {
        return -EBADF;
    }
    if (buf == NULL) {
        return -EFAULT;
    }

    struct file_handle *fh = curproc->file_table[fd];
    if (fh == NULL) {
        return -EBADF;
    }

    if ((fh->fh_flags & O_ACCMODE) == O_RDONLY) {
        return -EBADF;
    }

    struct iovec iov;
    struct uio u;
    iov.iov_ubase = (userptr_t)buf;
    iov.iov_len = nbytes;
    u.uio_iov = &iov;
    u.uio_iovcnt = 1;
    u.uio_resid = nbytes;
    u.uio_offset = fh->fh_offset;
    u.uio_segflg = UIO_USERSPACE;
    u.uio_rw = UIO_WRITE;
    u.uio_space = curproc->p_addrspace;

    int result = VOP_WRITE(fh->fh_vnode, &u);

    if (result) {
        return -EIO;
    }

    ssize_t bytes_written = nbytes - u.uio_resid;
    fh->fh_offset += bytes_written;

    return bytes_written;
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