/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Further, this software is distributed without any warranty that it is
 * free of the rightful claim of any third person regarding infringement
 * or the like.	 Any license provided herein, whether implied or
 * otherwise, applies only to this software file.  Patent licenses, if
 * any, provided herein do not apply to combinations of this program with
 * other software, or any other product whatsoever.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Contact information: Silicon Graphics, Inc., 1600 Amphitheatre Pkwy,
 * Mountain View, CA  94043, or:
 *
 * http://www.sgi.com
 *
 * For further information regarding this notice, see:
 *
 * http://oss.sgi.com/projects/GenInfo/SGIGPLNoticeExplan/
 */
#include <linux/uaccess.h>
#include "dmapi.h"
#include "dmapi_kern.h"
#include "dmapi_private.h"

/* The "rights" portion of the DMAPI spec is not currently implemented.	 A
   framework for rights is provided in the code, but turns out to be a noop
   in practice.	 The following comments are a brain dump to serve as input to
   the poor soul that eventually has to get DMAPI rights working in IRIX.

   A DMAPI right is similar but not identical to the mrlock_t mechanism
   already used within the kernel.  The similarities are that it is a
   sleeping lock, and that a multiple-reader, single-writer protocol is used.
   How locks are obtained and dropped are different however.  With a mrlock_t,
   a thread grabs the lock, does some stuff, then drops the lock, and all other
   threads block in the meantime (assuming a write lock).  There is a one-to-
   one relationship between the lock and the thread which obtained the lock.
   Not so with DMAPI right locks.  A DMAPI lock is associated with a particular
   session/token/hanp/hlen quad; since there is a dm_tokdata_t structure for
   each such quad, you can think of it as a one-to-one relationship between the
   lock and a dm_tokdata_t.  Any application thread which presents the correct
   quad is entitled to grab or release the lock, or to use the rights
   associated with that lock.  The thread that grabs the lock does not have to
   be the one to use the lock, nor does it have to be the thread which drops
   the lock.  The lock can be held for very long periods of time, even across
   multiple systems calls by multiple application threads.  The idea is that a
   coordinated group of DMAPI application threads can grab the lock, issue a
   series of inode accesses and/or updates, then drop the lock, and be assured
   that no other thread in the system could be modifying the inode at the same
   time.  The kernel is expected to blindly trust that the application will
   not forget to unlock inodes it has locked, and will not deadlock itself
   against the kernel.

   There are two types of DMAPI rights, file object (inode) and filesystem
   object (superblock?).  An inode right is the equivalent of the combination
   of both the XFS ilock and iolock; if held exclusively, no data or metadata
   within the file can be changed by non-lock-holding threads.	The filesystem
   object lock is a little fuzzier; I think that if it is held, things like
   unmounts can be blocked, plus there is an event mask associated with the
   filesystem which can't be updated without the lock.	(By the way, that
   event mask is supposed to be persistent in the superblock; add that to
   your worklist :-)

   All events generated by XFS currently arrive with no rights, i.e.
   DM_RIGHT_NULL, and return to the filesystem with no rights.	It would be
   smart to leave it this way if possible, because it otherwise becomes more
   likely that an application thread will deadlock against the kernel if the
   one responsible for calling dm_get_events() happens to touch a file which
   was locked at the time the event was queued.	 Since the thread is blocked,
   it can't read the event in order to find and drop the lock.	Catch-22.  If
   you do have events that arrive with non-null rights, then dm_enqueue() needs
   to have code added for synchronous events which atomically switches the
   right from being a thread-based right to a dm_tokdata_t-based right without
   allowing the lock to drop in between.  You will probably have to add a new
   dm_fsys_vector entry point to do this.  The lock can't be lost during the
   switch, or other threads might change the inode or superblock in between.
   Likewise, if you need to return to the filesystem holding a right, then
   you need a DMAPI-to-thread atomic switch to occur, most likely in
   dm_change_right().  Again, the lock must not be lost during the switch; the
   DMAPI spec spends a couple of pages stressing this.	Another dm_fsys_vector
   entry point is probably the answer.

   There are several assumptions implied in the current layout of the code.
   First of all, if an event returns to the filesystem with a return value of
   zero, then the filesystem can assume that any locks (rights) held at the
   start of the event are still in effect at the end of the event.  (Note that
   the application could have temporarily dropped and reaquired the right
   while the event was outstanding, however).  If the event returns to the
   filesystem with an errno, then the filesystem must assume that it has lost
   any and all rights associated with any of the objects in the event.	This
   was done for a couple of reasons.  First of all, since an errno is being
   returned, most likely the filesystem is going to immediately drop all the
   locks anyway.  If the DMAPI code was required to unconditionally reobtain
   all locks before returning to the filesystem, then dm_pending() wouldn't
   work for NFS server threads because the process would block indefinitely
   trying to get its thread-based rights back, because the DMAPI-rights
   associated with the dm_tokdata_t in the outstanding event would prevent
   the rights from being obtained.  That would be a bad thing.	We wouldn't
   be able to let users Cntl-C out of read/write/truncate events either.

   If a case should ever surface where the thread has lost its rights even
   though it has a zero return status, or where the thread has rights even
   though it is returning with an errno, then this logic will have to be
   reworked.  This could be done by changing the 'right' parameters on all
   the event calls to (dm_right_t *), so that they could serve both as IN
   and OUT parameters.

   Some events such as DM_EVENT_DESTROY arrive without holding an inode
   reference; if you don't have an inode reference, you can't have a right
   on the file.

   One more quirk.  The DM_EVENT_UNMOUNT event is defined to be synchronous
   when it's behavior is asynchronous.	If an unmount event arrives with
   rights, the event should return with the same rights and should NOT leave
   any rights in the dm_tokdata_t where the application could use them.
*/


#define GETNEXTOFF(vdat)	((vdat).vd_offset + (vdat).vd_length)
#define HANDLE_SIZE(tdp)	\
	((tdp)->td_type & DM_TDT_VFS ? DM_FSHSIZE : DM_HSIZE((tdp)->td_handle))


/* Given an inode pointer in a filesystem known to support DMAPI,
   build a tdp structure for the corresponding inode.
*/

static dm_tokdata_t *
dm_ip_data(
	struct inode	*ip,
	dm_right_t	right,
	int		referenced)	/* != 0, caller holds inode reference */
{
	int		error;
	dm_tokdata_t	*tdp;
	int		filetype;

	tdp = kmem_cache_alloc(dm_tokdata_cachep, GFP_KERNEL);
	if (tdp == NULL) {
		printk("%s/%d: kmem_cache_alloc(dm_tokdata_cachep) returned NULL\n", __FUNCTION__, __LINE__);
		return NULL;
	}

	tdp->td_next = NULL;
	tdp->td_tevp = NULL;
	tdp->td_app_ref = 0;
	tdp->td_orig_right = right;
	tdp->td_right = right;
	tdp->td_flags = DM_TDF_ORIG;
	if (referenced) {
		tdp->td_flags |= DM_TDF_EVTREF;
	}

	filetype = ip->i_mode & S_IFMT;
	if (filetype == S_IFREG) {
		tdp->td_type = DM_TDT_REG;
	} else if (filetype == S_IFDIR) {
		tdp->td_type = DM_TDT_DIR;
	} else if (filetype == S_IFLNK) {
		tdp->td_type = DM_TDT_LNK;
	} else {
		tdp->td_type = DM_TDT_OTH;
	}

	if (referenced) {
		tdp->td_ip = ip;
	} else {
		tdp->td_ip = NULL;
	}
	tdp->td_vcount = 0;

	if ((error = dm_ip_to_handle(ip, &tdp->td_handle)) != 0) {
		panic("dm_ip_data: dm_ip_to_handle failed for ip %p in "
			"a DMAPI filesystem, errno %d\n", ip, error);
	}

	return(tdp);
}


/* Given a sb pointer to a filesystem known to support DMAPI, build a tdp
   structure for that sb.
*/
static dm_tokdata_t *
dm_sb_data(
	struct super_block *sb,
	struct inode	*ip,		/* will be NULL for DM_EVENT_UNMOUNT */
	dm_right_t	right)
{
	dm_tokdata_t	*tdp;
	struct filesystem_dmapi_operations *dops;
	dm_fsid_t	fsid;

	dops = dm_fsys_ops(sb);
	ASSERT(dops);
	dops->get_fsid(sb, &fsid);

	tdp = kmem_cache_alloc(dm_tokdata_cachep, GFP_KERNEL);
	if (tdp == NULL) {
		printk("%s/%d: kmem_cache_alloc(dm_tokdata_cachep) returned NULL\n", __FUNCTION__, __LINE__);
		return NULL;
	}

	tdp->td_next = NULL;
	tdp->td_tevp = NULL;
	tdp->td_app_ref = 0;
	tdp->td_orig_right = right;
	tdp->td_right = right;
	tdp->td_flags = DM_TDF_ORIG;
	if (ip) {
		tdp->td_flags |= DM_TDF_EVTREF;
	}
	tdp->td_type = DM_TDT_VFS;
	tdp->td_ip = ip;
	tdp->td_vcount = 0;

	memcpy(&tdp->td_handle.ha_fsid, &fsid, sizeof(fsid));
	memset((char *)&tdp->td_handle.ha_fsid + sizeof(fsid), 0,
		sizeof(tdp->td_handle) - sizeof(fsid));

	return(tdp);
}


/* Link a tdp structure into the tevp. */

static void
dm_add_handle_to_event(
	dm_tokevent_t	*tevp,
	dm_tokdata_t	*tdp)
{
	tdp->td_next = tevp->te_tdp;
	tevp->te_tdp = tdp;
	tdp->td_tevp = tevp;
}


/* Generate the given data event for the inode, and wait for a reply.  The
   caller must guarantee that the inode's reference count is greater than zero
   so that the filesystem can't disappear while the request is outstanding.
*/

int
dm_send_data_event(
	dm_eventtype_t	event,
	struct inode	*ip,
	dm_right_t	vp_right,	/* current right for ip */
	dm_off_t	offset,
	size_t		length,
	int		flags)		/* 0 or DM_FLAGS_NDELAY */
{
	dm_data_event_t *datap;
	dm_tokevent_t	*tevp;
	dm_tokdata_t	*tdp;
	int		error;

	tdp = dm_ip_data(ip, vp_right, /* reference held */ 1);
	if (tdp == NULL)
		return -ENOMEM;

	/* Calculate the size of the event in bytes, create an event structure
	   for it, and insert the file's handle into the event.
	*/

	tevp = dm_evt_create_tevp(event, HANDLE_SIZE(tdp), (void **)&datap);
	if (tevp == NULL) {
		kmem_cache_free(dm_tokdata_cachep, tdp);
		return(-ENOMEM);
	}
	dm_add_handle_to_event(tevp, tdp);

	/* Now fill in all the dm_data_event_t fields. */

	datap->de_handle.vd_offset = sizeof(*datap);
	datap->de_handle.vd_length = HANDLE_SIZE(tdp);
	memcpy((char *)datap + datap->de_handle.vd_offset, &tdp->td_handle,
		datap->de_handle.vd_length);
	datap->de_offset = offset;
	datap->de_length = length;

	/* Queue the message and wait for the reply. */

	error = dm_enqueue_normal_event(ip->i_sb, &tevp, flags);

	/* If no errors occurred, we must leave with the same rights we had
	   upon entry.	If errors occurred, we must leave with no rights.
	*/

	dm_evt_rele_tevp(tevp, error);

	return(error);
}


/* Generate the destroy event for the inode and wait until the request has been
   queued.  The caller does not hold an inode reference or a right on the inode,
   but it must otherwise lock down the inode such that the filesystem can't
   disappear while the request is waiting to be queued.	 While waiting to be
   queued, the inode must not be referenceable either by path or by a call
   to dm_handle_to_ip().
*/

int
dm_send_destroy_event(
	struct inode	*ip,
	dm_right_t	vp_right)	/* always DM_RIGHT_NULL */
{
	dm_fsys_vector_t *fsys_vector;
	dm_tokevent_t	*tevp;
	dm_tokdata_t	*tdp;
	dm_destroy_event_t *destp;
	dm_attrname_t	attrname;
	char		*value;
	int		value_len;
	int		error;

	tdp = dm_ip_data(ip, vp_right, /* no reference held */ 0);
	if (tdp == NULL)
		return -ENOMEM;

	if ((error = dm_waitfor_destroy_attrname(ip->i_sb, &attrname)) != 0)
		return(error);

	/* If a return-on-destroy attribute name exists for this filesystem,
	   see if the object being deleted has this attribute.	If the object
	   doesn't have the attribute or if we encounter an error, then send
	   the event without the attribute.
	 */

	value_len = -1;		/* because zero is a valid attribute length */
	if (attrname.an_chars[0] != '\0') {
		fsys_vector = dm_fsys_vector(ip);
		error = fsys_vector->get_destroy_dmattr(ip, vp_right, &attrname,
			&value, &value_len);
		if (error && error != -ENODATA)
			return error;
	}

	/* Now that we know the size of the attribute value, if any, calculate
	   the size of the event in bytes, create an event structure for it,
	   and insert the handle into the event.
	*/

	tevp = dm_evt_create_tevp(DM_EVENT_DESTROY,
		HANDLE_SIZE(tdp) + (value_len >= 0 ? value_len : 0),
		(void **)&destp);
	if (tevp == NULL) {
		kmem_cache_free(dm_tokdata_cachep, tdp);
		if (value_len > 0)
			kfree(value);
		return(-ENOMEM);
	}
	dm_add_handle_to_event(tevp, tdp);

	/* Now fill in all the dm_destroy_event_t fields. */

	destp->ds_handle.vd_offset = sizeof(*destp);
	destp->ds_handle.vd_length = HANDLE_SIZE(tdp);
	memcpy((char *)destp + destp->ds_handle.vd_offset, &tdp->td_handle,
		destp->ds_handle.vd_length);
	if (value_len >= 0) {
		destp->ds_attrname = attrname;
		destp->ds_attrcopy.vd_length = value_len;
		if (value_len == 0) {
			destp->ds_attrcopy.vd_offset = 0;
		} else {
			destp->ds_attrcopy.vd_offset = GETNEXTOFF(destp->ds_handle);
			memcpy((char *)destp + destp->ds_attrcopy.vd_offset, value,
				value_len);
			kfree(value);
		}
	}

	/* Queue the message asynchronously. */

	error = dm_enqueue_normal_event(ip->i_sb, &tevp, 0);

	/* Since we had no rights upon entry, we have none to reobtain before
	   leaving.
	*/

	dm_evt_rele_tevp(tevp, 1);

	return(error);
}


/* The dm_mount_event_t event is sent in turn to all sessions that have asked
   for it until one either rejects it or accepts it.  The filesystem is not
   going anywhere because the mount is blocked until the event is answered.
*/

int
dm_send_mount_event(
	struct super_block *sb,		/* filesystem being mounted */
	dm_right_t	vfsp_right,
	struct inode	*ip,		/* mounted on directory */
	dm_right_t	vp_right,
	struct inode	*rootip,
	dm_right_t	rootvp_right,
	char		*name1,		/* mount path */
	char		*name2)		/* filesystem device name */
{
	int		error;
	dm_tokevent_t	*tevp = NULL;
	dm_tokdata_t	*tdp1 = NULL;	/* filesystem handle for event */
	dm_tokdata_t	*tdp2 = NULL;	/* file handle for mounted-on dir. */
	dm_tokdata_t	*tdp3 = NULL;	/* file handle for root inode */
	dm_mount_event_t *mp;
	size_t		nextoff;

	/* Convert the sb to a filesystem handle, and ip and rootip into
	   file handles.  ip (the mounted-on directory) may not have a handle
	   if it is a different filesystem type which does not support DMAPI.
	*/

	tdp1 = dm_sb_data(sb, rootip, vfsp_right);
	if (tdp1 == NULL)
		goto out_nomem;

	if ((ip == NULL) || dm_check_dmapi_ip(ip)) {
		ip = NULL;	/* we are mounting on non-DMAPI FS */
	} else {
		tdp2 = dm_ip_data(ip, vp_right, /* reference held */ 1);
		if (tdp2 == NULL)
			goto out_nomem;
	}

	tdp3 = dm_ip_data(rootip, rootvp_right, /* reference held */ 1);
	if (tdp3 == NULL)
		goto out_nomem;

	/* Calculate the size of the event in bytes, create an event structure
	   for it, and insert the handles into the event.
	*/

	tevp = dm_evt_create_tevp(DM_EVENT_MOUNT,
			HANDLE_SIZE(tdp1) + (ip ? HANDLE_SIZE(tdp2) : 0) +
			HANDLE_SIZE(tdp3) + strlen(name1) + 1 +
			strlen(name2) + 1, (void **)&mp);
	if (tevp == NULL)
		goto out_nomem;

	dm_add_handle_to_event(tevp, tdp1);
	if (ip)
		dm_add_handle_to_event(tevp, tdp2);
	dm_add_handle_to_event(tevp, tdp3);

	/* Now fill in all the dm_mount_event_t fields. */

	mp->me_handle1.vd_offset = sizeof(*mp);
	mp->me_handle1.vd_length = HANDLE_SIZE(tdp1);
	memcpy((char *) mp + mp->me_handle1.vd_offset, &tdp1->td_handle,
			mp->me_handle1.vd_length);
	nextoff = GETNEXTOFF(mp->me_handle1);

	if (ip) {
		mp->me_handle2.vd_offset = nextoff;
		mp->me_handle2.vd_length = HANDLE_SIZE(tdp2);
		memcpy((char *)mp + mp->me_handle2.vd_offset, &tdp2->td_handle,
			mp->me_handle2.vd_length);
		nextoff = GETNEXTOFF(mp->me_handle2);
	}

	mp->me_name1.vd_offset = nextoff;
	mp->me_name1.vd_length = strlen(name1) + 1;
	memcpy((char *)mp + mp->me_name1.vd_offset, name1, mp->me_name1.vd_length);
	nextoff = GETNEXTOFF(mp->me_name1);

	mp->me_name2.vd_offset = nextoff;
	mp->me_name2.vd_length = strlen(name2) + 1;
	memcpy((char *)mp + mp->me_name2.vd_offset, name2, mp->me_name2.vd_length);
	nextoff = GETNEXTOFF(mp->me_name2);

	mp->me_roothandle.vd_offset = nextoff;
	mp->me_roothandle.vd_length = HANDLE_SIZE(tdp3);
	memcpy((char *)mp + mp->me_roothandle.vd_offset, &tdp3->td_handle,
			mp->me_roothandle.vd_length);

	mp->me_mode = (sb->s_flags & MS_RDONLY ? DM_MOUNT_RDONLY : 0);

	/* Queue the message and wait for the reply. */

	error = dm_enqueue_mount_event(sb, tevp);

	/* If no errors occurred, we must leave with the same rights we had
	   upon entry.	If errors occurred, we must leave with no rights.
	*/

	dm_evt_rele_tevp(tevp, error);

	return(error);

out_nomem:
	if (tevp)
		kfree(tevp);
	if (tdp1)
		kmem_cache_free(dm_tokdata_cachep, tdp1);
	if (tdp2)
		kmem_cache_free(dm_tokdata_cachep, tdp2);
	if (tdp3)
		kmem_cache_free(dm_tokdata_cachep, tdp3);
	return -ENOMEM;
}


/* Generate an DM_EVENT_UNMOUNT event and wait for a reply.  The 'retcode'
   field indicates whether this is a successful or unsuccessful unmount.
   If successful, the filesystem is already unmounted, and any pending handle
   reference to the filesystem will be failed.	If the unmount was
   unsuccessful, then the filesystem will be placed back into full service.

   The DM_EVENT_UNMOUNT event should really be asynchronous, because the
   application has no control over whether or not the unmount succeeds.	 (The
   DMAPI spec defined it that way because asynchronous events aren't always
   guaranteed to be delivered.)

   Since the filesystem is already unmounted in the successful case, the
   DM_EVENT_UNMOUNT event can't make available any inode to be used in
   subsequent sid/hanp/hlen/token calls by the application.  The event will
   hang around until the application does a DM_RESP_CONTINUE, but the handle
   within the event is unusable by the application.
*/

void
dm_send_unmount_event(
	struct super_block *sb,
	struct inode	*ip,		/* NULL if unmount successful */
	dm_right_t	vfsp_right,
	mode_t		mode,
	int		retcode,	/* errno, if unmount failed */
	int		flags)
{
	dm_namesp_event_t	*np;
	dm_tokevent_t	*tevp;
	dm_tokdata_t	*tdp1;

	/* If the unmount failed, put the filesystem back into full service,
	   allowing blocked handle references to finish.  If it succeeded, put
	   the filesystem into the DM_STATE_UNMOUNTED state and fail all
	   blocked DM_NO_TOKEN handle accesses.
	*/

	if (retcode != 0) {	/* unmount was unsuccessful */
		dm_change_fsys_entry(sb, DM_STATE_MOUNTED);
	} else {
		dm_change_fsys_entry(sb, DM_STATE_UNMOUNTED);
	}

	/* If the event wasn't in the filesystem dm_eventset_t, just remove
	   the filesystem from the list of DMAPI filesystems and return.
	*/

	if (flags & DM_FLAGS_UNWANTED) {
		if (retcode == 0)
			dm_remove_fsys_entry(sb);
		return;
	}

	/* Calculate the size of the event in bytes and allocate zeroed memory
	   for it.
	*/

	tdp1 = dm_sb_data(sb, ip, vfsp_right);
	if (tdp1 == NULL)
		return;

	tevp = dm_evt_create_tevp(DM_EVENT_UNMOUNT, HANDLE_SIZE(tdp1),
		(void **)&np);
	if (tevp == NULL) {
		kmem_cache_free(dm_tokdata_cachep, tdp1);
		return;
	}

	dm_add_handle_to_event(tevp, tdp1);

	/* Now copy in all the dm_namesp_event_t specific fields. */

	np->ne_handle1.vd_offset = sizeof(*np);
	np->ne_handle1.vd_length = HANDLE_SIZE(tdp1);
	memcpy((char *) np + np->ne_handle1.vd_offset, &tdp1->td_handle,
			np->ne_handle1.vd_length);
	np->ne_mode = mode;
	np->ne_retcode = retcode;

	/* Since DM_EVENT_UNMOUNT is effectively asynchronous, queue the
	   message and ignore any error return for DM_EVENT_UNMOUNT.
	*/

	(void)dm_enqueue_normal_event(sb, &tevp, flags);

	if (retcode == 0)
		dm_remove_fsys_entry(sb);

	dm_evt_rele_tevp(tevp, 0);
}


/* Generate the given namespace event and wait for a reply (if synchronous) or
   until the event has been queued (asynchronous).  The caller must guarantee
   that at least one inode within the filesystem has had its reference count
   bumped so that the filesystem can't disappear while the event is
   outstanding.
*/

int
dm_send_namesp_event(
	dm_eventtype_t	event,
	struct super_block *sb,		/* used by PREUNMOUNT */
	struct inode	*ip1,
	dm_right_t	vp1_right,
	struct inode	*ip2,
	dm_right_t	vp2_right,
	const char	*name1,
	const char	*name2,
	mode_t		mode,
	int		retcode,
	int		flags)
{
	dm_namesp_event_t	*np;
	dm_tokevent_t	*tevp;
	dm_tokdata_t	*tdp1 = NULL;	/* primary handle for event */
	dm_tokdata_t	*tdp2 = NULL;	/* additional handle for event */
	size_t		nextoff;
	int		error;

	if (sb == NULL)
		sb = ip1->i_sb;

	switch (event) {
	case DM_EVENT_PREUNMOUNT:
		/*
		 *  PREUNMOUNT - Send the file system handle in handle1,
		 *  and the handle for the root dir in the second.  Otherwise
		 *  it's a normal sync message; i.e. succeeds or fails
		 *  depending on the app's return code.
		 *	ip1 and ip2 are both the root dir of mounted FS
		 *	vp1_right is the filesystem right.
		 *	vp2_right is the root inode right.
		 */

		if (flags & DM_FLAGS_UNWANTED) {
			dm_change_fsys_entry(sb, DM_STATE_UNMOUNTING);
			return(0);
		}
		if (ip1 == NULL) {
			/* If preunmount happens after kill_super then
			 * it's too late; there's nothing left with which
			 * to construct an event.
			 */
			return(0);
		}
		tdp1 = dm_sb_data(sb, ip1, vp1_right);
		if (tdp1 == NULL)
			return -ENOMEM;
		tdp2 = dm_ip_data(ip2, vp2_right, /* reference held */ 1);
		if (tdp2 == NULL) {
			kmem_cache_free(dm_tokdata_cachep, tdp1);
			return -ENOMEM;
		}
		break;

	case DM_EVENT_NOSPACE:
		/* vp1_right is the filesystem right. */

		tdp1 = dm_sb_data(sb, ip1, vp1_right);
		if (tdp1 == NULL)
			return -ENOMEM;
		tdp2 = dm_ip_data(ip2, vp2_right, /* reference held */ 1); /* additional info - not in the spec */
		if (tdp2 == NULL) {
			kmem_cache_free(dm_tokdata_cachep, tdp1);
			return -ENOMEM;
		}
		break;

	default:
		/* All other events only pass in inodes and don't require any
		   special cases.
		*/

		tdp1 = dm_ip_data(ip1, vp1_right, /* reference held */ 1);
		if (tdp1 == NULL)
			return -ENOMEM;
		if (ip2) {
			tdp2 = dm_ip_data(ip2, vp2_right, /* reference held */ 1);
			if (tdp2 == NULL) {
				kmem_cache_free(dm_tokdata_cachep, tdp1);
				return -ENOMEM;
			}
		}
	}

	/* Calculate the size of the event in bytes and allocate zeroed memory
	   for it.
	*/

	tevp = dm_evt_create_tevp(event,
		HANDLE_SIZE(tdp1) + (ip2 ? HANDLE_SIZE(tdp2) : 0) +
		(name1 ? strlen(name1) + 1 : 0) +
		(name2 ? strlen(name2) + 1 : 0), (void **)&np);
	if (tevp == NULL) {
		if (tdp1)
			kmem_cache_free(dm_tokdata_cachep, tdp1);
		if (tdp2)
			kmem_cache_free(dm_tokdata_cachep, tdp2);
		return(-ENOMEM);
	}

	dm_add_handle_to_event(tevp, tdp1);
	if (ip2)
		dm_add_handle_to_event(tevp, tdp2);

	/* Now copy in all the dm_namesp_event_t specific fields. */

	np->ne_handle1.vd_offset = sizeof(*np);
	np->ne_handle1.vd_length = HANDLE_SIZE(tdp1);
	memcpy((char *) np + np->ne_handle1.vd_offset, &tdp1->td_handle,
			np->ne_handle1.vd_length);
	nextoff = GETNEXTOFF(np->ne_handle1);
	if (ip2) {
		np->ne_handle2.vd_offset = nextoff;
		np->ne_handle2.vd_length = HANDLE_SIZE(tdp2);
		memcpy((char *)np + np->ne_handle2.vd_offset, &tdp2->td_handle,
				np->ne_handle2.vd_length);
		nextoff = GETNEXTOFF(np->ne_handle2);
	}
	if (name1) {
		np->ne_name1.vd_offset = nextoff;
		np->ne_name1.vd_length = strlen(name1) + 1;
		memcpy((char *)np + np->ne_name1.vd_offset, name1,
				np->ne_name1.vd_length);
		nextoff = GETNEXTOFF(np->ne_name1);
	}
	if (name2) {
		np->ne_name2.vd_offset = nextoff;
		np->ne_name2.vd_length = strlen(name2) + 1;
		memcpy((char *)np + np->ne_name2.vd_offset, name2,
				np->ne_name2.vd_length);
	}
	np->ne_mode = mode;
	np->ne_retcode = retcode;

	/* Queue the message and wait for the reply. */

	error = dm_enqueue_normal_event(sb, &tevp, flags);

	/* If no errors occurred, we must leave with the same rights we had
	   upon entry.	If errors occurred, we must leave with no rights.
	*/

	dm_evt_rele_tevp(tevp, error);

	if (!error && event == DM_EVENT_PREUNMOUNT) {
		dm_change_fsys_entry(sb, DM_STATE_UNMOUNTING);
	}

	return(error);
}


/*
 *  Send a message of type "DM_EVENT_USER".  Since no inode is involved, we
 *  don't have to worry about rights here.
 */

int
dm_send_msg(
	dm_sessid_t	targetsid,
	dm_msgtype_t	msgtype,		/* SYNC or ASYNC */
	size_t		buflen,
	void		__user *bufp)
{
	dm_tokevent_t	*tevp;
	int		sync;
	void		*msgp;
	int		error;

	if (buflen > DM_MAX_MSG_DATA)
		return(-E2BIG);
	if (msgtype == DM_MSGTYPE_ASYNC) {
		sync = 0;
	} else if (msgtype == DM_MSGTYPE_SYNC) {
		sync = 1;
	} else {
		return(-EINVAL);
	}

	tevp = dm_evt_create_tevp(DM_EVENT_USER, buflen, (void **)&msgp);
	if (tevp == NULL)
		return -ENOMEM;

	if (buflen && copy_from_user(msgp, bufp, buflen)) {
		dm_evt_rele_tevp(tevp, 0);
		return(-EFAULT);
	}

	/* Enqueue the request and wait for the reply. */

	error = dm_enqueue_sendmsg_event(targetsid, tevp, sync);

	/* Destroy the tevp and return the reply.  (dm_pending is not
	   supported here.)
	*/

	dm_evt_rele_tevp(tevp, error);

	return(error);
}


/*
 *  Send a message of type "DM_EVENT_USER".  Since no inode is involved, we
 *  don't have to worry about rights here.
 */

int
dm_create_userevent(
	dm_sessid_t	sid,
	size_t		msglen,
	void		__user *msgdatap,
	dm_token_t	__user *tokenp)		/* return token created */
{
	dm_tokevent_t	*tevp;
	dm_token_t	token;
	int		error;
	void		*msgp;

	if (msglen > DM_MAX_MSG_DATA)
		return(-E2BIG);

	tevp = dm_evt_create_tevp(DM_EVENT_USER, msglen, (void **)&msgp);
	if (tevp == NULL)
		return(-ENOMEM);

	if (msglen && copy_from_user(msgp, msgdatap, msglen)) {
		dm_evt_rele_tevp(tevp, 0);
		return(-EFAULT);
	}

	/* Queue the message.  If that didn't work, free the tevp structure. */

	if ((error = dm_enqueue_user_event(sid, tevp, &token)) != 0)
		dm_evt_rele_tevp(tevp, 0);

	if (!error && copy_to_user(tokenp, &token, sizeof(token)))
		error = -EFAULT;

	return(error);
}