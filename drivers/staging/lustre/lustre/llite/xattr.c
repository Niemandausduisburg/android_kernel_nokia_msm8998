/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2012, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/selinux.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include "../include/obd_support.h"
#include "../include/lustre_lite.h"
#include "../include/lustre_dlm.h"
#include "../include/lustre_ver.h"
#include "../include/lustre_eacl.h"

#include "llite_internal.h"

#define XATTR_USER_T	    (1)
#define XATTR_TRUSTED_T	 (2)
#define XATTR_SECURITY_T	(3)
#define XATTR_ACL_ACCESS_T      (4)
#define XATTR_ACL_DEFAULT_T     (5)
#define XATTR_LUSTRE_T	  (6)
#define XATTR_OTHER_T	   (7)

static
int get_xattr_type(const char *name)
{
	if (!strcmp(name, XATTR_NAME_POSIX_ACL_ACCESS))
		return XATTR_ACL_ACCESS_T;

	if (!strcmp(name, XATTR_NAME_POSIX_ACL_DEFAULT))
		return XATTR_ACL_DEFAULT_T;

	if (!strncmp(name, XATTR_USER_PREFIX,
		     sizeof(XATTR_USER_PREFIX) - 1))
		return XATTR_USER_T;

	if (!strncmp(name, XATTR_TRUSTED_PREFIX,
		     sizeof(XATTR_TRUSTED_PREFIX) - 1))
		return XATTR_TRUSTED_T;

	if (!strncmp(name, XATTR_SECURITY_PREFIX,
		     sizeof(XATTR_SECURITY_PREFIX) - 1))
		return XATTR_SECURITY_T;

	if (!strncmp(name, XATTR_LUSTRE_PREFIX,
		     sizeof(XATTR_LUSTRE_PREFIX) - 1))
		return XATTR_LUSTRE_T;

	return XATTR_OTHER_T;
}

static
int xattr_type_filter(struct ll_sb_info *sbi, int xattr_type)
{
	if ((xattr_type == XATTR_ACL_ACCESS_T ||
	     xattr_type == XATTR_ACL_DEFAULT_T) &&
	   !(sbi->ll_flags & LL_SBI_ACL))
		return -EOPNOTSUPP;

	if (xattr_type == XATTR_USER_T && !(sbi->ll_flags & LL_SBI_USER_XATTR))
		return -EOPNOTSUPP;
	if (xattr_type == XATTR_TRUSTED_T && !capable(CFS_CAP_SYS_ADMIN))
		return -EPERM;
	if (xattr_type == XATTR_OTHER_T)
		return -EOPNOTSUPP;

	return 0;
}

static
int ll_getxattr_common(struct inode *inode, const char *name,
		       void *buffer, size_t size, __u64 valid)
{
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct ptlrpc_request *req = NULL;
	struct mdt_body *body;
	int xattr_type, rc;
	void *xdata;
	struct rmtacl_ctl_entry *rce = NULL;
	struct ll_inode_info *lli = ll_i2info(inode);

	CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p)\n",
	       inode->i_ino, inode->i_generation, inode);

	/* listxattr have slightly different behavior from of ext3:
	 * without 'user_xattr' ext3 will list all xattr names but
	 * filtered out "^user..*"; we list them all for simplicity.
	 */
	if (!name) {
		xattr_type = XATTR_OTHER_T;
		goto do_getxattr;
	}

	xattr_type = get_xattr_type(name);
	rc = xattr_type_filter(sbi, xattr_type);
	if (rc)
		return rc;

	/* b15587: ignore security.capability xattr for now */
	if ((xattr_type == XATTR_SECURITY_T &&
	    strcmp(name, "security.capability") == 0))
		return -ENODATA;

	/* LU-549:  Disable security.selinux when selinux is disabled */
	if (xattr_type == XATTR_SECURITY_T && !selinux_is_enabled() &&
	    strcmp(name, "security.selinux") == 0)
		return -EOPNOTSUPP;

#ifdef CONFIG_FS_POSIX_ACL
	if (sbi->ll_flags & LL_SBI_RMT_CLIENT &&
	    (xattr_type == XATTR_ACL_ACCESS_T ||
	    xattr_type == XATTR_ACL_DEFAULT_T)) {
		rce = rct_search(&sbi->ll_rct, current_pid());
		if (rce == NULL ||
		    (rce->rce_ops != RMT_LSETFACL &&
		    rce->rce_ops != RMT_LGETFACL &&
		    rce->rce_ops != RMT_RSETFACL &&
		    rce->rce_ops != RMT_RGETFACL))
			return -EOPNOTSUPP;
	}

	/* posix acl is under protection of LOOKUP lock. when calling to this,
	 * we just have path resolution to the target inode, so we have great
	 * chance that cached ACL is uptodate.
	 */
	if (xattr_type == XATTR_ACL_ACCESS_T &&
	    !(sbi->ll_flags & LL_SBI_RMT_CLIENT)) {

		struct posix_acl *acl;

		spin_lock(&lli->lli_lock);
		acl = posix_acl_dup(lli->lli_posix_acl);
		spin_unlock(&lli->lli_lock);

		if (!acl)
			return -ENODATA;

		rc = posix_acl_to_xattr(&init_user_ns, acl, buffer, size);
		posix_acl_release(acl);
		return rc;
	}
	if (xattr_type == XATTR_ACL_DEFAULT_T && !S_ISDIR(inode->i_mode))
		return -ENODATA;
#endif

do_getxattr:
	if (sbi->ll_xattr_cache_enabled && xattr_type != XATTR_ACL_ACCESS_T) {
		rc = ll_xattr_cache_get(inode, name, buffer, size, valid);
		if (rc == -EAGAIN)
			goto getxattr_nocache;
		if (rc < 0)
			goto out_xattr;

		/* Add "system.posix_acl_access" to the list */
		if (lli->lli_posix_acl != NULL && valid & OBD_MD_FLXATTRLS) {
			if (size == 0) {
				rc += sizeof(XATTR_NAME_ACL_ACCESS);
			} else if (size - rc >= sizeof(XATTR_NAME_ACL_ACCESS)) {
				memcpy(buffer + rc, XATTR_NAME_ACL_ACCESS,
				       sizeof(XATTR_NAME_ACL_ACCESS));
				rc += sizeof(XATTR_NAME_ACL_ACCESS);
			} else {
				rc = -ERANGE;
				goto out_xattr;
			}
		}
	} else {
getxattr_nocache:
		rc = md_getxattr(sbi->ll_md_exp, ll_inode2fid(inode),
				valid | (rce ? rce_ops2valid(rce->rce_ops) : 0),
				name, NULL, 0, size, 0, &req);

		if (rc < 0)
			goto out_xattr;

		body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);
		LASSERT(body);

		/* only detect the xattr size */
		if (size == 0) {
			rc = body->eadatasize;
			goto out;
		}

		if (size < body->eadatasize) {
			CERROR("server bug: replied size %u > %u\n",
				body->eadatasize, (int)size);
			rc = -ERANGE;
			goto out;
		}

		if (body->eadatasize == 0) {
			rc = -ENODATA;
			goto out;
		}

		/* do not need swab xattr data */
		xdata = req_capsule_server_sized_get(&req->rq_pill, &RMF_EADATA,
							body->eadatasize);
		if (!xdata) {
			rc = -EFAULT;
			goto out;
		}

		memcpy(buffer, xdata, body->eadatasize);
		rc = body->eadatasize;
	}

#ifdef CONFIG_FS_POSIX_ACL
	if (rce && rce->rce_ops == RMT_LSETFACL) {
		ext_acl_xattr_header *acl;

		acl = lustre_posix_acl_xattr_2ext(
					(posix_acl_xattr_header *)buffer, rc);
		if (IS_ERR(acl)) {
			rc = PTR_ERR(acl);
			goto out;
		}

		rc = ee_add(&sbi->ll_et, current_pid(), ll_inode2fid(inode),
			    xattr_type, acl);
		if (unlikely(rc < 0)) {
			lustre_ext_acl_xattr_free(acl);
			goto out;
		}
	}
#endif

out_xattr:
	if (rc == -EOPNOTSUPP && xattr_type == XATTR_USER_T) {
		LCONSOLE_INFO(
			"%s: disabling user_xattr feature because it is not supported on the server: rc = %d\n",
			ll_get_fsname(inode->i_sb, NULL, 0), rc);
		sbi->ll_flags &= ~LL_SBI_USER_XATTR;
	}
out:
	ptlrpc_req_finished(req);
	return rc;
}

ssize_t ll_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	struct inode *inode = d_inode(dentry);
	int rc = 0, rc2 = 0;
	struct lov_mds_md *lmm = NULL;
	struct ptlrpc_request *request = NULL;
	int lmmsize;

	LASSERT(inode);

	CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p)\n",
	       inode->i_ino, inode->i_generation, inode);

	ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_LISTXATTR, 1);

	rc = ll_getxattr_common(inode, NULL, buffer, size, OBD_MD_FLXATTRLS);
	if (rc < 0)
		goto out;

	if (buffer != NULL) {
		struct ll_sb_info *sbi = ll_i2sbi(inode);
		char *xattr_name = buffer;
		int xlen, rem = rc;

		while (rem > 0) {
			xlen = strnlen(xattr_name, rem - 1) + 1;
			rem -= xlen;
			if (xattr_type_filter(sbi,
					get_xattr_type(xattr_name)) == 0) {
				/* skip OK xattr type
				 * leave it in buffer
				 */
				xattr_name += xlen;
				continue;
			}
			/* move up remaining xattrs in buffer
			 * removing the xattr that is not OK
			 */
			memmove(xattr_name, xattr_name + xlen, rem);
			rc -= xlen;
		}
	}
	if (S_ISREG(inode->i_mode)) {
		if (!ll_i2info(inode)->lli_has_smd)
			rc2 = -1;
	} else if (S_ISDIR(inode->i_mode)) {
		rc2 = ll_dir_getstripe(inode, &lmm, &lmmsize, &request);
	}

	if (rc2 < 0) {
		rc2 = 0;
		goto out;
	} else if (S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode)) {
		const int prefix_len = sizeof(XATTR_LUSTRE_PREFIX) - 1;
		const size_t name_len   = sizeof("lov") - 1;
		const size_t total_len  = prefix_len + name_len + 1;

		if (((rc + total_len) > size) && (buffer != NULL)) {
			ptlrpc_req_finished(request);
			return -ERANGE;
		}

		if (buffer != NULL) {
			buffer += rc;
			memcpy(buffer, XATTR_LUSTRE_PREFIX, prefix_len);
			memcpy(buffer + prefix_len, "lov", name_len);
			buffer[prefix_len + name_len] = '\0';
		}
		rc2 = total_len;
	}
out:
	ptlrpc_req_finished(request);
	rc = rc + rc2;

	return rc;
}
