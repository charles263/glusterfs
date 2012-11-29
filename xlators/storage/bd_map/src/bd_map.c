/*
  BD translator - Exports Block devices on server side as regular
  files to client

  Now only exporting Logical volumes supported.

  Copyright IBM, Corp. 2012

  This file is part of GlusterFS.

  Author:
  M. Mohan Kumar <mohan@in.ibm.com>

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include <time.h>
#include <lvm2app.h>
#include <openssl/md5.h>

#include "bd_map.h"
#include "bd_map_help.h"
#include "defaults.h"
#include "glusterfs3-xdr.h"


/* Regular fops */

int
bd_access (call_frame_t *frame, xlator_t *this,
                loc_t *loc, int32_t mask, dict_t *xdict)
{
        int32_t   op_ret         = -1;
        int32_t   op_errno       = 0;
        char      path[PATH_MAX] = {0, };

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (loc, out);

        sprintf (path, "/dev/mapper/%s", loc->path);
        op_ret = access (path, mask & 07);
        if (op_ret == -1) {
                op_errno = errno;
                gf_log (this->name, GF_LOG_ERROR, "access failed on %s: %s",
                                loc->path, strerror (op_errno));
                goto out;
        }
        op_ret = 0;
out:
        STACK_UNWIND_STRICT (access, frame, op_ret, op_errno, NULL);

        return 0;
}

int32_t
bd_open (call_frame_t *frame, xlator_t *this,
                loc_t *loc, int32_t flags, fd_t *fd, dict_t *xdata)
{
        int32_t         op_ret          = -1;
        int32_t         op_errno        = 0;
        int32_t         _fd             = -1;
        bd_fd_t         *bd_fd          = NULL;
        bd_entry_t      *lventry        = NULL;
        bd_priv_t       *priv           = NULL;
        char            *devpath        = NULL;

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (this->private, out);
        VALIDATE_OR_GOTO (loc, out);
        VALIDATE_OR_GOTO (fd, out);

        priv = this->private;
        VALIDATE_OR_GOTO (priv, out);

        BD_ENTRY (priv, lventry, loc->path);
        if (!lventry) {
                op_errno = ENOENT;
                goto out;
        }

        gf_asprintf (&devpath, "/dev/%s/%s", lventry->parent->name,
                      lventry->name);
        _fd = open (devpath, flags, 0);
        if (_fd == -1) {
                op_errno = errno;
                gf_log (this->name, GF_LOG_ERROR,
                                "open on %s: %s", devpath, strerror (op_errno));
                goto out;
        }

        bd_fd = GF_CALLOC (1, sizeof(*bd_fd), gf_bd_fd);
        if (!bd_fd) {
                op_errno = errno;
                goto out;
        }
        bd_fd->entry = lventry;
        bd_fd->fd = _fd;

        op_ret = fd_ctx_set (fd, this, (uint64_t)(long)bd_fd);
        if (op_ret) {
                gf_log (this->name, GF_LOG_WARNING,
                                "failed to set the fd context path=%s fd=%p",
                                loc->name, fd);
                goto out;
        }

        op_ret = 0;
out:
        if (op_ret == -1) {
                if (_fd != -1)
                        close (_fd);
                /* FIXME: Should we call fd_ctx_set with NULL? */
                if (bd_fd)
                        GF_FREE (bd_fd);
                if (lventry)
                        BD_PUT_ENTRY (priv, lventry);
        }
        if (devpath)
                GF_FREE (devpath);

        STACK_UNWIND_STRICT (open, frame, op_ret, op_errno, fd, NULL);

        return 0;
}

int
bd_readv (call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
                off_t offset, uint32_t flags, dict_t *xdata)
{
        uint64_t        tmp_bd_fd  = 0;
        int32_t         op_ret     = -1;
        int32_t         op_errno   = 0;
        int             _fd        = -1;
        bd_priv_t       *priv      = NULL;
        struct iobuf    *iobuf     = NULL;
        struct iobref   *iobref    = NULL;
        struct iovec    vec        = {0, };
        bd_fd_t         *bd_fd     = NULL;
        int             ret        = -1;
        struct iatt     stbuf      = {0, };

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (fd, out);
        VALIDATE_OR_GOTO (this->private, out);

        priv = this->private;
        VALIDATE_OR_GOTO (priv, out);

        ret = fd_ctx_get (fd, this, &tmp_bd_fd);
        if (ret < 0) {
                op_errno = -EINVAL;
                gf_log (this->name, GF_LOG_WARNING,
                                "bd_fd is NULL from fd=%p", fd);
                goto out;
        }
        bd_fd = (bd_fd_t *)(long)tmp_bd_fd;
        if (!size) {
                op_errno = EINVAL;
                gf_log (this->name, GF_LOG_WARNING, "size=%"GF_PRI_SIZET, size);
                goto out;
        }
        iobuf = iobuf_get2 (this->ctx->iobuf_pool, size);
        if (!iobuf) {
                op_errno = ENOMEM;
                goto out;
        }
        _fd = bd_fd->fd;
        op_ret = pread (_fd, iobuf->ptr, size, offset);
        if (op_ret == -1) {
                op_errno = errno;
                gf_log (this->name, GF_LOG_ERROR,
                                "read failed on fd=%p: %s", fd,
                                strerror (op_errno));
                goto out;
        }

        vec.iov_base = iobuf->ptr;
        vec.iov_len = op_ret;

        iobref = iobref_new ();
        iobref_add (iobref, iobuf);
        BD_ENTRY_UPDATE_ATIME (bd_fd->entry);

        memcpy (&stbuf, bd_fd->entry->attr, sizeof(stbuf));

        /* Hack to notify higher layers of EOF. */
        if (bd_fd->entry->size == 0)
                op_errno = ENOENT;
        else if ((offset + vec.iov_len) >= bd_fd->entry->size)
                op_errno = ENOENT;
        op_ret = vec.iov_len;
out:
        STACK_UNWIND_STRICT (readv, frame, op_ret, op_errno,
                        &vec, 1, &stbuf, iobref, NULL);

        if (iobref)
                iobref_unref (iobref);
        if (iobuf)
                iobuf_unref (iobuf);
        return 0;
}

 int32_t
bd_ftruncate (call_frame_t *frame, xlator_t *this,
                fd_t *fd, off_t offset, dict_t *xdict)
{
        int32_t        op_ret      = -1;
        int32_t        op_errno    = 0;
        struct iatt    preop       = {0, };
        struct iatt    postop      = {0, };
        bd_fd_t        *bd_fd      = NULL;
        int            ret         = -1;
        uint64_t       tmp_bd_fd   = 0;

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (fd, out);

        ret = fd_ctx_get (fd, this, &tmp_bd_fd);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_WARNING,
                                "bd_fd is NULL, fd=%p", fd);
                op_errno = -ret;
                goto out;
        }
        bd_fd = (bd_fd_t *)(long)tmp_bd_fd;

        memcpy (&preop, bd_fd->entry->attr, sizeof(preop));
        if (offset >= bd_fd->entry->size) {
                op_errno = EFBIG;
                goto out;
        }
        /* If the requested size is less then current size
         * we will not update that in bd_fd->entry->attr
         * because it will result in showing size of this file less
         * instead we will return 0 for less size truncation
         */
        BD_ENTRY_UPDATE_MTIME (bd_fd->entry);
        memcpy (&postop, bd_fd->entry->attr, sizeof(postop));

        op_ret = 0;
out:
        STACK_UNWIND_STRICT (ftruncate, frame, op_ret, op_errno, &preop,
                        &postop, NULL);
        return 0;
}

int32_t
bd_truncate (call_frame_t *frame, xlator_t *this, loc_t *loc,
                off_t offset, dict_t *xdict)
{
        int32_t         op_ret     = -1;
        int32_t         op_errno   = 0;
        struct iatt     prebuf     = {0, };
        struct iatt     postbuf    = {0, };
        bd_entry_t      *lventry   = NULL;
        bd_priv_t       *priv      = NULL;

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (loc, out);

        priv = this->private;
        BD_ENTRY (priv, lventry, loc->path);
        if (!lventry) {
                op_errno = ENOENT;
                gf_log (this->name, GF_LOG_ERROR,
                                "pre-operation lstat on %s failed: %s",
                                loc->path, strerror (op_errno));
                goto out;
        }
        memcpy (&prebuf, lventry->attr, sizeof(prebuf));
        if (offset >= lventry->size) {
                op_errno = EFBIG;
                goto out;
        }
        BD_ENTRY_UPDATE_MTIME (lventry);
        memcpy (&postbuf, lventry->attr, sizeof(postbuf));
        BD_PUT_ENTRY (priv, lventry);
        op_ret = 0;
out:
        STACK_UNWIND_STRICT (truncate, frame, op_ret, op_errno,
                        &prebuf, &postbuf, NULL);
        return 0;
}

int32_t
__bd_pwritev (int fd, struct iovec *vector, int count, off_t offset,
                uint64_t bd_size)
{
        int32_t    op_ret          = 0;
        int        index           = 0;
        int        retval          = 0;
        off_t      internal_offset = 0;
        int        no_space        = 0;

        if (!vector)
                return -EFAULT;

        internal_offset = offset;
        for (index = 0; index < count; index++) {
                if (internal_offset >= bd_size) {
                        op_ret = -ENOSPC;
                        goto err;
                }
                if (internal_offset + vector[index].iov_len >= bd_size) {
                        vector[index].iov_len = bd_size - internal_offset;
                        no_space = 1;
                }

                retval = pwrite (fd, vector[index].iov_base,
                                vector[index].iov_len, internal_offset);
                if (retval == -1) {
                        gf_log (THIS->name, GF_LOG_WARNING,
                                "base %p, length %ld, offset %ld, message %s",
                                vector[index].iov_base, vector[index].iov_len,
                                internal_offset, strerror (errno));
                        op_ret = -errno;
                        goto err;
                }
                op_ret += retval;
                internal_offset += retval;
                if (no_space)
                        break;
        }
err:
        return op_ret;
}

int
bd_writev (call_frame_t *frame, xlator_t *this, fd_t *fd,
                struct iovec *vector, int32_t count, off_t offset,
                uint32_t flags, struct iobref *iobref, dict_t *xdict)
{
        int32_t         op_ret    = -1;
        int32_t         op_errno  = 0;
        int             _fd       = -1;
        bd_priv_t       *priv     = NULL;
        bd_fd_t         *bd_fd    = NULL;
        int             ret       = -1;
        struct iatt     preop     = {0, };
        struct iatt     postop    = {0, };
        uint64_t        tmp_bd_fd = 0;

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (fd, out);
        VALIDATE_OR_GOTO (vector, out);
        VALIDATE_OR_GOTO (this->private, out);

        priv = this->private;
        VALIDATE_OR_GOTO (priv, out);

        ret = fd_ctx_get (fd, this, &tmp_bd_fd);
        if (ret < 0) {
                op_errno = -ret;
                gf_log (this->name, GF_LOG_WARNING,
                                "bd_fd is NULL from fd=%p", fd);
                goto out;
        }
        bd_fd = (bd_fd_t *)(long)tmp_bd_fd;
        _fd = bd_fd->fd;

        memcpy (&preop, bd_fd->entry->attr, sizeof(preop));
        op_ret = __bd_pwritev (_fd, vector, count, offset, bd_fd->entry->size);
        if (op_ret < 0) {
                op_errno = -op_ret;
                op_ret = -1;
                gf_log (this->name, GF_LOG_ERROR, "write failed: offset %"PRIu64
                                ", %s", offset, strerror (op_errno));
                goto out;
        }
        BD_ENTRY_UPDATE_MTIME (bd_fd->entry);
        memcpy (&postop, bd_fd->entry->attr, sizeof(postop));

out:
        STACK_UNWIND_STRICT (writev, frame, op_ret, op_errno, &preop,
                        &postop, NULL);

        return 0;
}

int32_t
bd_lookup (call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xattr_req)
{
        struct iatt    buf          = {0, };
        int32_t        op_ret       = -1;
        int32_t        entry_ret    = 0;
        int32_t        op_errno     = 0;
        char           *pathdup     = NULL;
        bd_entry_t     *bdentry     = NULL;
        struct iatt    postparent   = {0, };
        bd_priv_t      *priv        = NULL;
        char           *p           = NULL;

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (loc, out);
        VALIDATE_OR_GOTO (loc->path, out);

        priv = this->private;
        VALIDATE_OR_GOTO (priv, out);

        BD_ENTRY (priv, bdentry, loc->path);
        if (!bdentry) {
                op_errno = ENOENT;
                entry_ret = -1;
                goto parent;
        }
        memcpy (&buf, bdentry->attr, sizeof(buf));
        BD_PUT_ENTRY (priv, bdentry);

parent:
        if (loc->parent) {
                pathdup = p = gf_strdup (loc->path);
                if (!pathdup) {
                        op_errno = ENOMEM;
                        entry_ret = -1;
                        goto out;
                }
                p = strrchr (pathdup, '/');
                if (p == pathdup)
                        *(p+1) = '\0';
                else
                        *p = '\0';
                BD_ENTRY (priv, bdentry, pathdup);
                if (!bdentry) {
                        op_errno = ENOENT;
                        gf_log (this->name, GF_LOG_ERROR,
                                "post-operation lookup on parent of %s "
                                "failed: %s",
                                loc->path, strerror (op_errno));
                        goto out;
                }
                memcpy (&postparent, bdentry->attr, sizeof(postparent));
                BD_PUT_ENTRY (priv, bdentry);
        }

        op_ret = entry_ret;
out:
        if (pathdup)
                GF_FREE (pathdup);

        STACK_UNWIND_STRICT (lookup, frame, op_ret, op_errno,
                             (loc)?loc->inode:NULL, &buf, NULL, &postparent);

        return 0;
}

int32_t
bd_stat (call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xdata)
{
        struct iatt     buf      = {0,};
        int32_t         op_ret   = -1;
        int32_t         op_errno = 0;
        bd_entry_t      *bdentry = NULL;
        bd_priv_t       *priv    = NULL;

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (loc, out);

        priv = this->private;
        VALIDATE_OR_GOTO (priv, out);

        BD_ENTRY (priv, bdentry, loc->path);
        if (!bdentry) {
                op_errno = ENOENT;
                gf_log (this->name, GF_LOG_ERROR, "stat on %s failed: %s",
                                loc->path, strerror (op_errno));
                goto out;
        }
        memcpy (&buf, bdentry->attr, sizeof(buf));
        BD_PUT_ENTRY (priv, bdentry);
        op_ret = 0;

out:
        STACK_UNWIND_STRICT (stat, frame, op_ret, op_errno, &buf, NULL);

        return 0;
}

int32_t
bd_fstat (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
        int            ret         = -1;
        int32_t        op_ret      = -1;
        int32_t        op_errno    = 0;
        uint64_t       tmp_bd_fd   = 0;
        struct iatt    buf         = {0, };
        bd_fd_t        *bd_fd      = NULL;
        int            _fd         = -1;

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (fd, out);

        ret = fd_ctx_get (fd, this, &tmp_bd_fd);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_WARNING,
                                "bd_fd is NULL, fd=%p", fd);
                op_errno = -EINVAL;
                goto out;
        }
        bd_fd = (bd_fd_t *)(long)tmp_bd_fd;
        _fd = bd_fd->fd;

        memcpy (&buf, bd_fd->entry->attr, sizeof(buf));
        op_ret = 0;

out:
        STACK_UNWIND_STRICT (stat, frame, op_ret, op_errno, &buf, NULL);
        return 0;
}

int32_t
bd_opendir (call_frame_t *frame, xlator_t *this,
               loc_t *loc, fd_t *fd, dict_t *xdata)
{
        int32_t           op_ret   = -1;
        int32_t           op_errno = EINVAL;
        bd_fd_t           *bd_fd   = NULL;
        bd_entry_t        *bdentry = NULL;
        bd_priv_t         *priv    = NULL;

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (loc, out);
        VALIDATE_OR_GOTO (loc->path, out);
        VALIDATE_OR_GOTO (fd, out);

        priv = this->private;
        VALIDATE_OR_GOTO (priv, out);

        BD_ENTRY (priv, bdentry, loc->path);
        if (!bdentry) {
                op_errno = ENOENT;
                gf_log (this->name, GF_LOG_ERROR, "opendir failed on %s: %s",
                        loc->path, strerror (op_errno));
                goto out;
        }
        bd_fd = GF_CALLOC (1, sizeof(*bd_fd), gf_bd_fd);
        if (!bd_fd) {
                op_errno = errno;
                BD_PUT_ENTRY (priv, bdentry);
                goto out;
        }

        bd_fd->p_entry = bdentry;

        bdentry = list_entry ((&bdentry->child)->next, typeof(*bdentry), child);
        if (!bdentry) {
                op_errno = EINVAL;
                gf_log (this->name, GF_LOG_ERROR, "bd_entry NULL");
                goto out;
        }
        bdentry = list_entry ((&bdentry->sibling), typeof(*bdentry), sibling);
        if (!bdentry) {
                op_errno = EINVAL;
                gf_log (this->name, GF_LOG_ERROR, "bd_entry NULL");
                goto out;
        }

        bd_fd->entry = bdentry;

        op_ret = fd_ctx_set (fd, this, (uint64_t) (long)bd_fd);
        if (op_ret) {
                gf_log (this->name, GF_LOG_ERROR,
                        "failed to set the fd context path=%s fd=%p",
                        loc->path, fd);
                goto out;
        }

        op_ret = 0;
out:
        if (op_ret == -1) {
                BD_PUT_ENTRY (priv, bd_fd->p_entry);
                if (bd_fd)
                        GF_FREE (bd_fd);
        }

        STACK_UNWIND_STRICT (opendir, frame, op_ret, op_errno, fd, NULL);
        return 0;
}

int32_t
bd_releasedir (xlator_t *this, fd_t *fd)
{
        bd_fd_t      *bd_fd    = NULL;
        uint64_t     tmp_bd_fd = 0;
        int          ret       = 0;
        bd_priv_t    *priv     = NULL;

        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (fd, out);

        priv = this->private;
        VALIDATE_OR_GOTO (priv, out);

        ret = fd_ctx_del (fd, this, &tmp_bd_fd);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_DEBUG, "bd_fd from fd=%p is NULL",
                                fd);
                goto out;
        }
        bd_fd = (bd_fd_t *) (long)tmp_bd_fd;
        BD_PUT_ENTRY (priv, bd_fd->p_entry);

        bd_fd = (bd_fd_t *) (long)tmp_bd_fd;
        GF_FREE (bd_fd);
out:
        return 0;
}

/*
 * bd_statfs: Mimics statfs by returning used/free extents in the VG
 * TODO: IF more than one VG allowed per volume, this functions needs some
 * change
 */
int32_t
bd_statfs (call_frame_t *frame, xlator_t *this,
                loc_t *loc, dict_t *xdata)
{
        int32_t                op_ret       = -1;
        int32_t                ret          = -1;
        int32_t                op_errno     = 0;
        bd_priv_t              *priv        = NULL;
        struct statvfs         buf          = {0, };
        vg_t                   vg           = NULL;
        char                   *vg_name     = NULL;
        uint64_t               size         = 0;
        uint64_t               fr_size      = 0;

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (this->private, out);
        VALIDATE_OR_GOTO (loc, out);

        priv = this->private;
        VALIDATE_OR_GOTO (priv, out);

        ret = dict_get_str (this->options, "export", &vg_name);
        if (ret) {
                gf_log (this->name, GF_LOG_CRITICAL,
                        "FATAL: storage/bd does not specify volume groups");
                op_errno = EINVAL;
                goto out;
        }

        BD_RD_LOCK (&priv->lock);

        vg = lvm_vg_open (priv->handle, vg_name, "r", 0);
        size += lvm_vg_get_size (vg);
        fr_size += lvm_vg_get_free_size (vg);
        lvm_vg_close (vg);

        BD_UNLOCK (&priv->lock);

        if (statvfs ("/", &buf) < 0) {
                op_errno = errno;
                goto out;
        }
        op_ret = 0;
        buf.f_blocks = size / buf.f_frsize;
        buf.f_bfree = fr_size / buf.f_frsize;
        buf.f_bavail = fr_size / buf.f_frsize;
out:
        STACK_UNWIND_STRICT (statfs, frame, op_ret, op_errno, &buf, NULL);
        return 0;
}

int32_t
bd_release (xlator_t *this, fd_t *fd)
{
        bd_fd_t      *bd_fd    = NULL;
        int          ret       = -1;
        uint64_t     tmp_bd_fd = 0;
        bd_priv_t    *priv     = NULL;

        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (fd, out);

        priv = this->private;
        VALIDATE_OR_GOTO (priv, out);

        ret = fd_ctx_get (fd, this, &tmp_bd_fd);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_WARNING, "bd_fd is NULL from fd=%p",
                                fd);
                goto out;
        }
        bd_fd = (bd_fd_t *) (long)tmp_bd_fd;
        close (bd_fd->fd);
        BD_PUT_ENTRY (priv, bd_fd->entry);

        GF_FREE (bd_fd);
out:
        return 0;
}

int32_t
bd_fsync (call_frame_t *frame, xlator_t *this,
                fd_t *fd, int32_t datasync, dict_t *xdata)
{
        int             _fd             = -1;
        int             ret             = -1;
        int32_t         op_ret          = -1;
        int32_t         op_errno        = 0;
        uint64_t        tmp_bd_fd       = 0;
        bd_fd_t         *bd_fd          = NULL;
        struct iatt     preop           = {0, };
        struct iatt     postop          = {0, };

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (fd, out);

        ret = fd_ctx_get (fd, this, &tmp_bd_fd);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_WARNING,
                                "bd_fd is NULL, fd=%p", fd);
                op_errno = -ret;
                goto out;
        }
        bd_fd = (bd_fd_t *)(long)tmp_bd_fd;

        _fd = bd_fd->fd;
        memcpy (&preop, &bd_fd->entry->attr, sizeof(preop));
        if (datasync) {
                ;
#ifdef HAVE_FDATASYNC
                op_ret = fdatasync (_fd);
                if (op_ret == -1) {
                        gf_log (this->name, GF_LOG_ERROR,
                                "fdatasync on fd=%p failed: %s",
                                fd, strerror (errno));
                }
#endif
        } else {
                op_ret = fsync (_fd);
                if (op_ret == -1) {
                        op_errno = errno;
                        gf_log (this->name, GF_LOG_ERROR,
                                "fsync on fd=%p failed: %s",
                                 fd, strerror (op_errno));
                        goto out;
                }
        }

        memcpy (&postop, bd_fd->entry->attr, sizeof(postop));
        op_ret = 0;

out:
        STACK_UNWIND_STRICT (fsync, frame, op_ret, op_errno, &preop,
                        &postop, NULL);

        return 0;
}

int32_t
bd_flush (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdict)
{
        int32_t     op_ret    = -1;
        int32_t     op_errno  = 0;
        int         ret       = -1;
        uint64_t    tmp_bd_fd = 0;

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (fd, out);

        ret = fd_ctx_get (fd, this, &tmp_bd_fd);
        if (ret < 0) {
                op_errno = -EINVAL;
                gf_log (this->name, GF_LOG_WARNING,
                                "bd_fd is NULL on fd=%p", fd);
                goto out;
        }
        op_ret = 0;
out:
        STACK_UNWIND_STRICT (flush, frame, op_ret, op_errno, NULL);

        return 0;
}

int
__bd_fill_readdir (pthread_rwlock_t *bd_lock, bd_fd_t *bd_fd, off_t off,
                size_t size, gf_dirent_t *entries)
{
        size_t          filled      = 0;
        int             count       = 0;
        struct dirent   entry       = {0, };
        int32_t         this_size   = -1;
        gf_dirent_t     *this_entry = NULL;
        bd_entry_t      *bdentry    = NULL;
        bd_entry_t      *cur_entry  = NULL;
        bd_entry_t      *n_entry    = NULL;

        BD_RD_LOCK (bd_lock);

        bdentry = list_entry ((&bd_fd->p_entry->child)->next, typeof(*n_entry),
                        child);

        if (off) {
                int i = 0;
                list_for_each_entry (n_entry, &bd_fd->entry->sibling, sibling) {
                        if (i == off && strcmp (n_entry->name, "")) {
                                bd_fd->entry = n_entry;
                                break;
                        }
                }
        } else
                bd_fd->entry = list_entry ((&bdentry->sibling),
                                typeof(*n_entry), sibling);

        while (filled <= size) {
                cur_entry = bd_fd->entry;

                n_entry = list_entry ((&bd_fd->entry->sibling)->next,
                          typeof (*cur_entry), sibling);
                if (&n_entry->sibling == (&bdentry->sibling))
                        break;

                strcpy (entry.d_name, n_entry->name);
                entry.d_ino = n_entry->attr->ia_ino;
                entry.d_off = off;
                if (n_entry->attr->ia_type == IA_IFDIR)
                        entry.d_type = DT_DIR;
                else
                        entry.d_type = DT_REG;

                this_size = max (sizeof(gf_dirent_t),
                                 sizeof (gfs3_dirplist))
                        + strlen (entry.d_name) + 1;

                if (this_size + filled > size)
                        break;

                bd_fd->entry = n_entry;

                this_entry = gf_dirent_for_name (entry.d_name);
                if (!this_entry) {
                        gf_log (THIS->name, GF_LOG_ERROR,
                                "could not create gf_dirent for entry %s",
                                entry.d_name);
                        goto out;
                }
                this_entry->d_off = off;
                this_entry->d_ino = entry.d_ino;
                this_entry->d_type = entry.d_type;
                off++;

                list_add_tail (&this_entry->list, &entries->list);

                filled += this_size;
                count++;
        }
out:
        BD_UNLOCK (bd_lock);
        return count;
}

int32_t
bd_do_readdir (call_frame_t *frame, xlator_t *this,
                  fd_t *fd, size_t size, off_t off, int whichop)
{
        uint64_t      tmp_bd_fd    = 0;
        bd_fd_t       *bd_fd       = NULL;
        int           ret          = -1;
        int           count        = 0;
        int32_t       op_ret       = -1;
        int32_t       op_errno     = 0;
        gf_dirent_t   entries;
        gf_dirent_t   *tmp_entry   = NULL;
        bd_entry_t    *bdentry     = NULL;
        bd_priv_t     *priv        = NULL;
        char          *devpath     = NULL;

        VALIDATE_OR_GOTO (frame, out);
        VALIDATE_OR_GOTO (this, out);
        VALIDATE_OR_GOTO (fd, out);

        priv = this->private;
        VALIDATE_OR_GOTO (priv, out);

        INIT_LIST_HEAD (&entries.list);

        ret = fd_ctx_get (fd, this, &tmp_bd_fd);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_WARNING, "bd_fd is NULL, fd=%p", fd);
                op_errno = -EINVAL;
                goto out;
        }
        bd_fd = (bd_fd_t *) (long)tmp_bd_fd;
        LOCK (&fd->lock);
        {
                count = __bd_fill_readdir (&priv->lock, bd_fd, off,
                                size, &entries);
        }
        UNLOCK (&fd->lock);

        /* pick ENOENT to indicate EOF */
        op_errno = errno;
        op_ret = count;

        if (whichop != GF_FOP_READDIRP)
                goto out;

        BD_RD_LOCK (&priv->lock);
        list_for_each_entry (tmp_entry, &entries.list, list) {
                char path[PATH_MAX];
                sprintf (path, "%s/%s", bd_fd->p_entry->name,
                                tmp_entry->d_name);
                bdentry = bd_entry_get (path);
                if (!bdentry) {
                        gf_log (this->name, GF_LOG_WARNING,
                                        "entry failed %s\n", tmp_entry->d_name);
                        continue;
                }
                if (bdentry->attr->ia_ino)
                        tmp_entry->d_ino = bdentry->attr->ia_ino;
                memcpy (&tmp_entry->d_stat,
                                bdentry->attr, sizeof (tmp_entry->d_stat));
                bd_entry_put (bdentry);
                GF_FREE (devpath);
        }
        BD_UNLOCK (&priv->lock);

out:
        STACK_UNWIND_STRICT (readdir, frame, op_ret, op_errno, &entries, NULL);

        gf_dirent_free (&entries);

        return 0;
}

int32_t
bd_readdir (call_frame_t *frame, xlator_t *this,
               fd_t *fd, size_t size, off_t off, dict_t *dict)
{
        bd_do_readdir (frame, this, fd, size, off, GF_FOP_READDIR);
        return 0;
}


int32_t
bd_readdirp (call_frame_t *frame, xlator_t *this,
                fd_t *fd, size_t size, off_t off, dict_t *dict)
{
        bd_do_readdir (frame, this, fd, size, off, GF_FOP_READDIRP);
        return 0;
}

int32_t
bd_priv (xlator_t *this)
{
        return 0;
}

int32_t
bd_inode (xlator_t *this)
{
        return 0;
}

/* unsupported interfaces */
int bd_setattr (call_frame_t *frame, xlator_t *this, loc_t *loc,
                struct iatt *stbuf, int32_t valid, dict_t *xdata)
{
        STACK_UNWIND_STRICT (setattr, frame, -1, ENOSYS, NULL, NULL, NULL);
        return 0;
}

int32_t
bd_readlink (call_frame_t *frame, xlator_t *this,
                                loc_t *loc, size_t size, dict_t *xdata)
{
        struct iatt stbuf    = {0, };
        char        *dest    = NULL;

        dest = alloca (size + 1);
        STACK_UNWIND_STRICT (readlink, frame, -1, ENOSYS, dest, &stbuf, NULL);
        return 0;
}

int
bd_mknod (call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
                dev_t dev, mode_t umask, dict_t *xdata)
{
        struct iatt     stbuf      = {0, };
        struct iatt     preparent  = {0, };
        struct iatt     postparent = {0, };

        STACK_UNWIND_STRICT (mknod, frame, -1, ENOSYS,
                        (loc)?loc->inode:NULL, &stbuf, &preparent,
                        &postparent, NULL);
        return 0;
}

int
bd_mkdir (call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
                mode_t umask, dict_t *xdata)
{
        struct iatt     stbuf      = {0, };
        struct iatt     preparent  = {0, };
        struct iatt     postparent = {0, };

        STACK_UNWIND_STRICT (mkdir, frame, -1, ENOSYS,
                        (loc)?loc->inode:NULL, &stbuf, &preparent,
                        &postparent, NULL);
        return 0;
}

int
bd_rmdir (call_frame_t *frame, xlator_t *this, loc_t *loc, int flags,
                dict_t *xdata)
{
        struct iatt     preparent  = {0, };
        struct iatt     postparent = {0, };

        STACK_UNWIND_STRICT (rmdir, frame, -1, ENOSYS,
                        &preparent, &postparent, NULL);
        return 0;
}

int
bd_link (call_frame_t *frame, xlator_t *this,
            loc_t *oldloc, loc_t *newloc, dict_t *xdata)
{
        struct iatt           stbuf        = {0, };
        struct iatt           preparent    = {0,};
        struct iatt           postparent   = {0,};

        STACK_UNWIND_STRICT (link, frame, -1, ENOSYS,
                             (oldloc)?oldloc->inode:NULL, &stbuf, &preparent,
                             &postparent, NULL);

        return 0;
}

int32_t
bd_setxattr (call_frame_t *frame, xlator_t *this,
                loc_t *loc, dict_t *dict, int flags, dict_t *xdata)
{
        STACK_UNWIND_STRICT (setxattr, frame, -1, ENOSYS, NULL);
        return 0;
}

int32_t
bd_fsetxattr (call_frame_t *frame, xlator_t *this,
                fd_t *fd, dict_t *dict, int flags, dict_t *xdata)
{
        STACK_UNWIND_STRICT (setxattr, frame, -1, ENOSYS, NULL);
        return 0;
}

int32_t
bd_getxattr (call_frame_t *frame, xlator_t *this,
                loc_t *loc, const char *name, dict_t *xdata)
{
        STACK_UNWIND_STRICT (getxattr, frame, -1, ENOSYS, NULL, NULL);
        return 0;
}

int32_t
bd_fgetxattr (call_frame_t *frame, xlator_t *this,
                 fd_t *fd, const char *name, dict_t *xdata)
{
        STACK_UNWIND_STRICT (fgetxattr, frame, -1, ENOSYS, NULL, NULL);

        return 0;
}

int32_t
bd_removexattr (call_frame_t *frame, xlator_t *this,
                   loc_t *loc, const char *name, dict_t *xdata)
{
        STACK_UNWIND_STRICT (removexattr, frame, -1, ENOSYS, NULL);
        return 0;
}

int32_t
bd_fremovexattr (call_frame_t *frame, xlator_t *this,
                    fd_t *fd, const char *name, dict_t *xdata)
{
        STACK_UNWIND_STRICT (fremovexattr, frame, -1, ENOSYS, NULL);
        return 0;
}

int32_t
bd_fsyncdir (call_frame_t *frame, xlator_t *this,
                fd_t *fd, int datasync, dict_t *xdata)
{
        STACK_UNWIND_STRICT (fsyncdir, frame, -1, ENOSYS, NULL);
        return 0;
}

static int gf_bd_lk_log;
int32_t
bd_lk (call_frame_t *frame, xlator_t *this,
          fd_t *fd, int32_t cmd, struct gf_flock *lock, dict_t *xdata)
{
        struct gf_flock nullock = {0, };

        GF_LOG_OCCASIONALLY (gf_bd_lk_log, this->name, GF_LOG_CRITICAL,
                             "\"features/locks\" translator is "
                             "not loaded. You need to use it for proper "
                             "functioning of your application.");

        STACK_UNWIND_STRICT (lk, frame, -1, ENOSYS, &nullock, NULL);
        return 0;
}

int32_t
bd_inodelk (call_frame_t *frame, xlator_t *this,
               const char *volume, loc_t *loc, int32_t cmd,
               struct gf_flock *lock, dict_t *xdata)
{
        GF_LOG_OCCASIONALLY (gf_bd_lk_log, this->name, GF_LOG_CRITICAL,
                             "\"features/locks\" translator is "
                             "not loaded. You need to use it for proper "
                             "functioning of your application.");

        STACK_UNWIND_STRICT (inodelk, frame, -1, ENOSYS, NULL);
        return 0;
}

int32_t
bd_finodelk (call_frame_t *frame, xlator_t *this,
                const char *volume, fd_t *fd, int32_t cmd,
                struct gf_flock *lock, dict_t *xdata)
{
        GF_LOG_OCCASIONALLY (gf_bd_lk_log, this->name, GF_LOG_CRITICAL,
                             "\"features/locks\" translator is "
                             "not loaded. You need to use it for proper "
                             "functioning of your application.");

        STACK_UNWIND_STRICT (finodelk, frame, -1, ENOSYS, NULL);
        return 0;
}


int32_t
bd_entrylk (call_frame_t *frame, xlator_t *this,
               const char *volume, loc_t *loc, const char *basename,
               entrylk_cmd cmd, entrylk_type type, dict_t *xdata)
{
        GF_LOG_OCCASIONALLY (gf_bd_lk_log, this->name, GF_LOG_CRITICAL,
                             "\"features/locks\" translator is "
                             "not loaded. You need to use it for proper "
                             "functioning of your application.");

        STACK_UNWIND_STRICT (entrylk, frame, -1, ENOSYS, NULL);
        return 0;
}

int32_t
bd_fentrylk (call_frame_t *frame, xlator_t *this,
                const char *volume, fd_t *fd, const char *basename,
                entrylk_cmd cmd, entrylk_type type, dict_t *xdata)
{
        GF_LOG_OCCASIONALLY (gf_bd_lk_log, this->name, GF_LOG_CRITICAL,
                             "\"features/locks\" translator is "
                             "not loaded. You need to use it for proper "
                             "functioning of your application.");

        STACK_UNWIND_STRICT (fentrylk, frame, -1, ENOSYS, NULL);
        return 0;
}

int32_t
bd_rchecksum (call_frame_t *frame, xlator_t *this,
                 fd_t *fd, off_t offset, int32_t len, dict_t *xdata)
{
        int32_t weak_checksum = 0;
        unsigned char strong_checksum[MD5_DIGEST_LENGTH];

        STACK_UNWIND_STRICT (rchecksum, frame, -1, ENOSYS,
                             weak_checksum, strong_checksum, NULL);
        return 0;
}

int
bd_xattrop (call_frame_t *frame, xlator_t *this,
               loc_t *loc, gf_xattrop_flags_t optype, dict_t *xattr,
               dict_t *xdata)
{
        STACK_UNWIND_STRICT (xattrop, frame, -1, ENOSYS, xattr, NULL);
        return 0;
}


int
bd_fxattrop (call_frame_t *frame, xlator_t *this,
                fd_t *fd, gf_xattrop_flags_t optype, dict_t *xattr,
                dict_t *xdata)
{
        STACK_UNWIND_STRICT (xattrop, frame, -1, ENOSYS, xattr, NULL);
        return 0;
}

/**
 * notify - when parent sends PARENT_UP, send CHILD_UP event from here
 */
int32_t
notify (xlator_t *this,
        int32_t event,
        void *data,
        ...)
{
        switch (event)
        {
        case GF_EVENT_PARENT_UP:
        {
                /* Tell the parent that bd xlator is up */
                default_notify (this, GF_EVENT_CHILD_UP, data);
        }
        break;
        default:
                break;
        }
        return 0;
}

int32_t
mem_acct_init (xlator_t *this)
{
        int     ret = -1;

        if (!this)
                return ret;

        ret = xlator_mem_acct_init (this, gf_bd_mt_end + 1);

        if (ret != 0) {
                gf_log (this->name, GF_LOG_ERROR, "Memory accounting init"
                       "failed");
                return ret;
        }

        return ret;
}


/**
 * init - Constructs lists of LVs in the given VG
 */
int
init (xlator_t *this)
{
        bd_priv_t  *_private  = NULL;
        int        ret        = 0;
        char       *vg        = NULL;
        char       *device    = NULL;

        LOCK_INIT (&inode_lk);

        bd_rootp = bd_entry_add_root ();
        if (!bd_rootp) {
                gf_log (this->name, GF_LOG_CRITICAL,
                                "FATAL: adding root entry failed");
                return -1;
        }

        if (this->children) {
                gf_log (this->name, GF_LOG_CRITICAL,
                        "FATAL: storage/bd cannot have subvolumes");
                ret = -1;
                goto out;
        }

        if (!this->parents) {
                gf_log (this->name, GF_LOG_WARNING,
                        "Volume is dangling. Please check the volume file.");
        }

        ret = dict_get_str (this->options, "device", &device);
        if (ret) {
                gf_log (this->name, GF_LOG_CRITICAL,
                        "FATAL: storage/bd does not specify backend");
                return -1;
        }

        /* Now we support only LV device */
        if (strcasecmp (device, BACKEND_VG)) {
                gf_log (this->name, GF_LOG_CRITICAL,
                        "FATAL: unknown %s backend %s", BD_XLATOR, device);
                return -1;
        }

        ret = dict_get_str (this->options, "export", &vg);
        if (ret) {
                gf_log (this->name, GF_LOG_CRITICAL,
                        "FATAL: storage/bd does not specify volume groups");
                return -1;
        }

        ret = 0;
        _private = GF_CALLOC (1, sizeof(*_private), gf_bd_private);
        if (!_private)
                goto error;

        pthread_rwlock_init (&_private->lock, NULL);
        this->private = (void *)_private;
        _private->handle = NULL;
        _private->vg = gf_strdup (vg);
        if (!_private->vg) {
                goto error;
        }

        if (bd_build_lv_list (this->private, vg) < 0)
                goto error;

out:
        return 0;
error:
        BD_WR_LOCK (&_private->lock);
        bd_entry_cleanup ();
        lvm_quit (_private->handle);
        if (_private->vg)
                GF_FREE (_private->vg);
        GF_FREE (_private);
        return -1;
}

void
fini (xlator_t *this)
{
        bd_priv_t *priv = this->private;
        if (!priv)
                return;
        lvm_quit (priv->handle);
        BD_WR_LOCK (&priv->lock);
        bd_entry_cleanup ();
        BD_UNLOCK (&priv->lock);
        GF_FREE (priv->vg);
        this->private = NULL;
        GF_FREE (priv);
        return;
}

struct xlator_dumpops dumpops = {
        .priv    = bd_priv,
        .inode   = bd_inode,
};

struct xlator_fops fops = {
        /* Not supported */
        .readlink    = bd_readlink,
        .mknod       = bd_mknod,
        .mkdir       = bd_mkdir,
        .rmdir       = bd_rmdir,
        .link        = bd_link,
        .setxattr    = bd_setxattr,
        .fsetxattr   = bd_fsetxattr,
        .getxattr    = bd_getxattr,
        .fgetxattr   = bd_fgetxattr,
        .removexattr = bd_removexattr,
        .fremovexattr= bd_fremovexattr,
        .fsyncdir    = bd_fsyncdir,
        .lk          = bd_lk,
        .inodelk     = bd_inodelk,
        .finodelk    = bd_finodelk,
        .entrylk     = bd_entrylk,
        .fentrylk    = bd_fentrylk,
        .rchecksum   = bd_rchecksum,
        .xattrop     = bd_xattrop,
        .setattr     = bd_setattr,

        /* Supported */
        .lookup      = bd_lookup,
        .opendir     = bd_opendir,
        .readdir     = bd_readdir,
        .readdirp    = bd_readdirp,
        .stat        = bd_stat,
        .statfs      = bd_statfs,
        .open        = bd_open,
        .access      = bd_access,
        .flush       = bd_flush,
        .readv       = bd_readv,
        .fstat       = bd_fstat,
        .truncate    = bd_truncate,
        .ftruncate   = bd_ftruncate,
        .fsync       = bd_fsync,
        .writev      = bd_writev,
};

struct xlator_cbks cbks = {
        .releasedir  = bd_releasedir,
        .release     = bd_release,
};

struct volume_options options[] = {
        { .key = {"export"},
          .type = GF_OPTION_TYPE_STR},
        { .key = {"device"},
          .type = GF_OPTION_TYPE_STR},
        { .key = {NULL} }
};
