// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2015 by Delphix. All rights reserved.
 * Copyright (c) 2017, Intel Corporation.
 * Copyright (c) 2024-2025, Klara, Inc.
 */

/*
 * ZFS fault injection
 *
 * To handle fault injection, we keep track of a series of zinject_record_t
 * structures which describe which logical block(s) should be injected with a
 * fault.  These are kept in a global list.  Each record corresponds to a given
 * spa_t and maintains a special hold on the spa_t so that it cannot be deleted
 * or exported while the injection record exists.
 *
 * Device level injection is done using the 'zi_guid' field.  If this is set, it
 * means that the error is destined for a particular device, not a piece of
 * data.
 *
 * This is a rather poor data structure and algorithm, but we don't expect more
 * than a few faults at any one time, so it should be sufficient for our needs.
 */

#include <sys/arc.h>
#include <sys/zio.h>
#include <sys/zfs_ioctl.h>
#include <sys/vdev_impl.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_dataset.h>
#include <sys/fs/zfs.h>

uint32_t zio_injection_enabled = 0;

/*
 * Data describing each zinject handler registered on the system, and
 * contains the list node linking the handler in the global zinject
 * handler list.
 */
typedef struct inject_handler {
	int			zi_id;
	spa_t			*zi_spa;
	char			*zi_spa_name; /* ZINJECT_DELAY_IMPORT only */
	zinject_record_t	zi_record;
	uint64_t		*zi_lanes;
	int			zi_next_lane;
	list_node_t		zi_link;
} inject_handler_t;

/*
 * List of all zinject handlers registered on the system, protected by
 * the inject_lock defined below.
 */
static list_t inject_handlers;

/*
 * This protects insertion into, and traversal of, the inject handler
 * list defined above; as well as the inject_delay_count. Any time a
 * handler is inserted or removed from the list, this lock should be
 * taken as a RW_WRITER; and any time traversal is done over the list
 * (without modification to it) this lock should be taken as a RW_READER.
 */
static krwlock_t inject_lock;

/*
 * This holds the number of zinject delay handlers that have been
 * registered on the system. It is protected by the inject_lock defined
 * above. Thus modifications to this count must be a RW_WRITER of the
 * inject_lock, and reads of this count must be (at least) a RW_READER
 * of the lock.
 */
static int inject_delay_count = 0;

/*
 * This lock is used only in zio_handle_io_delay(), refer to the comment
 * in that function for more details.
 */
static kmutex_t inject_delay_mtx;

/*
 * Used to assign unique identifying numbers to each new zinject handler.
 */
static int inject_next_id = 1;

/*
 * Test if the requested frequency was triggered
 */
static boolean_t
freq_triggered(uint32_t frequency)
{
	/*
	 * zero implies always (100%)
	 */
	if (frequency == 0)
		return (B_TRUE);

	/*
	 * Note: we still handle legacy (unscaled) frequency values
	 */
	uint32_t maximum = (frequency <= 100) ? 100 : ZI_PERCENTAGE_MAX;

	return (random_in_range(maximum) < frequency);
}

/*
 * Returns true if the given record matches the I/O in progress.
 */
static boolean_t
zio_match_handler(const zbookmark_phys_t *zb, uint64_t type, int dva,
    zinject_record_t *record, int error)
{
	boolean_t matched = B_FALSE;
	boolean_t injected = B_FALSE;

	/*
	 * Check for a match against the MOS, which is based on type
	 */
	if (zb->zb_objset == DMU_META_OBJSET &&
	    record->zi_objset == DMU_META_OBJSET &&
	    record->zi_object == DMU_META_DNODE_OBJECT) {
		if (record->zi_type == DMU_OT_NONE ||
		    type == record->zi_type)
			matched = B_TRUE;
		goto done;
	}

	/*
	 * Check for an exact match.
	 */
	if (zb->zb_objset == record->zi_objset &&
	    zb->zb_object == record->zi_object &&
	    zb->zb_level == record->zi_level &&
	    zb->zb_blkid >= record->zi_start &&
	    zb->zb_blkid <= record->zi_end &&
	    (record->zi_dvas == 0 ||
	    (dva != ZI_NO_DVA && (record->zi_dvas & (1ULL << dva)))) &&
	    error == record->zi_error) {
		matched = B_TRUE;
		goto done;
	}

done:
	if (matched) {
		record->zi_match_count++;
		injected = freq_triggered(record->zi_freq);
	}

	if (injected)
		record->zi_inject_count++;

	return (injected);
}

/*
 * Panic the system when a config change happens in the function
 * specified by tag.
 */
void
zio_handle_panic_injection(spa_t *spa, const char *tag, uint64_t type)
{
	inject_handler_t *handler;

	rw_enter(&inject_lock, RW_READER);

	for (handler = list_head(&inject_handlers); handler != NULL;
	    handler = list_next(&inject_handlers, handler)) {

		if (spa != handler->zi_spa)
			continue;

		if (handler->zi_record.zi_type == type &&
		    strcmp(tag, handler->zi_record.zi_func) == 0) {
			handler->zi_record.zi_match_count++;
			handler->zi_record.zi_inject_count++;
			panic("Panic requested in function %s\n", tag);
		}
	}

	rw_exit(&inject_lock);
}

/*
 * Inject a decryption failure. Decryption failures can occur in
 * both the ARC and the ZIO layers.
 */
int
zio_handle_decrypt_injection(spa_t *spa, const zbookmark_phys_t *zb,
    uint64_t type, int error)
{
	int ret = 0;
	inject_handler_t *handler;

	rw_enter(&inject_lock, RW_READER);

	for (handler = list_head(&inject_handlers); handler != NULL;
	    handler = list_next(&inject_handlers, handler)) {

		if (spa != handler->zi_spa ||
		    handler->zi_record.zi_cmd != ZINJECT_DECRYPT_FAULT)
			continue;

		if (zio_match_handler(zb, type, ZI_NO_DVA,
		    &handler->zi_record, error)) {
			ret = error;
			break;
		}
	}

	rw_exit(&inject_lock);
	return (ret);
}

/*
 * If this is a physical I/O for a vdev child determine which DVA it is
 * for. We iterate backwards through the DVAs matching on the offset so
 * that we end up with ZI_NO_DVA (-1) if we don't find a match.
 */
static int
zio_match_dva(zio_t *zio)
{
	int i = ZI_NO_DVA;

	if (zio->io_bp != NULL && zio->io_vd != NULL &&
	    zio->io_child_type == ZIO_CHILD_VDEV) {
		for (i = BP_GET_NDVAS(zio->io_bp) - 1; i >= 0; i--) {
			dva_t *dva = &zio->io_bp->blk_dva[i];
			uint64_t off = DVA_GET_OFFSET(dva);
			vdev_t *vd = vdev_lookup_top(zio->io_spa,
			    DVA_GET_VDEV(dva));

			/* Compensate for vdev label added to leaves */
			if (zio->io_vd->vdev_ops->vdev_op_leaf)
				off += VDEV_LABEL_START_SIZE;

			if (zio->io_vd == vd && zio->io_offset == off)
				break;
		}
	}

	return (i);
}


/*
 * Determine if the I/O in question should return failure.  Returns the errno
 * to be returned to the caller.
 */
int
zio_handle_fault_injection(zio_t *zio, int error)
{
	int ret = 0;
	inject_handler_t *handler;

	/*
	 * Ignore I/O not associated with any logical data.
	 */
	if (zio->io_logical == NULL)
		return (0);

	/*
	 * Currently, we only support fault injection on reads.
	 */
	if (zio->io_type != ZIO_TYPE_READ)
		return (0);

	/*
	 * A rebuild I/O has no checksum to verify.
	 */
	if (zio->io_priority == ZIO_PRIORITY_REBUILD && error == ECKSUM)
		return (0);

	rw_enter(&inject_lock, RW_READER);

	for (handler = list_head(&inject_handlers); handler != NULL;
	    handler = list_next(&inject_handlers, handler)) {
		if (zio->io_spa != handler->zi_spa ||
		    handler->zi_record.zi_cmd != ZINJECT_DATA_FAULT)
			continue;

		/* If this handler matches, return the specified error */
		if (zio_match_handler(&zio->io_logical->io_bookmark,
		    zio->io_bp ? BP_GET_TYPE(zio->io_bp) : DMU_OT_NONE,
		    zio_match_dva(zio), &handler->zi_record, error)) {
			ret = error;
			break;
		}
	}

	rw_exit(&inject_lock);

	return (ret);
}

/*
 * Determine if the zio is part of a label update and has an injection
 * handler associated with that portion of the label. Currently, we
 * allow error injection in either the nvlist or the uberblock region of
 * of the vdev label.
 */
int
zio_handle_label_injection(zio_t *zio, int error)
{
	inject_handler_t *handler;
	vdev_t *vd = zio->io_vd;
	uint64_t offset = zio->io_offset;
	int label;
	int ret = 0;

	if (offset >= VDEV_LABEL_START_SIZE &&
	    offset < vd->vdev_psize - VDEV_LABEL_END_SIZE)
		return (0);

	rw_enter(&inject_lock, RW_READER);

	for (handler = list_head(&inject_handlers); handler != NULL;
	    handler = list_next(&inject_handlers, handler)) {
		uint64_t start = handler->zi_record.zi_start;
		uint64_t end = handler->zi_record.zi_end;

		if (handler->zi_record.zi_cmd != ZINJECT_LABEL_FAULT)
			continue;

		/*
		 * The injection region is the relative offsets within a
		 * vdev label. We must determine the label which is being
		 * updated and adjust our region accordingly.
		 */
		label = vdev_label_number(vd->vdev_psize, offset);
		start = vdev_label_offset(vd->vdev_psize, label, start);
		end = vdev_label_offset(vd->vdev_psize, label, end);

		if (zio->io_vd->vdev_guid == handler->zi_record.zi_guid &&
		    (offset >= start && offset <= end)) {
			handler->zi_record.zi_match_count++;
			handler->zi_record.zi_inject_count++;
			ret = error;
			break;
		}
	}
	rw_exit(&inject_lock);
	return (ret);
}

static int
zio_inject_bitflip_cb(void *data, size_t len, void *private)
{
	zio_t *zio = private;
	uint8_t *buffer = data;
	uint_t byte = random_in_range(len);

	ASSERT3U(zio->io_type, ==, ZIO_TYPE_READ);

	/* flip a single random bit in an abd data buffer */
	buffer[byte] ^= 1 << random_in_range(8);

	return (1);	/* stop after first flip */
}

/* Test if this zio matches the iotype from the injection record. */
static boolean_t
zio_match_iotype(zio_t *zio, uint32_t iotype)
{
	ASSERT3P(zio, !=, NULL);

	/* Unknown iotype, maybe from a newer version of zinject. Reject it. */
	if (iotype >= ZINJECT_IOTYPES)
		return (B_FALSE);

	/* Probe IOs only match IOTYPE_PROBE, regardless of their type. */
	if (zio->io_flags & ZIO_FLAG_PROBE)
		return (iotype == ZINJECT_IOTYPE_PROBE);

	/* Standard IO types, match against ZIO type. */
	if (iotype < ZINJECT_IOTYPE_ALL)
		return (iotype == zio->io_type);

	/* Match any standard IO type. */
	if (iotype == ZINJECT_IOTYPE_ALL)
		return (B_TRUE);

	return (B_FALSE);
}

static int
zio_handle_device_injection_impl(vdev_t *vd, zio_t *zio, int err1, int err2)
{
	inject_handler_t *handler;
	int ret = 0;

	/*
	 * We skip over faults in the labels unless it's during device open
	 * (i.e. zio == NULL) or a device flush (offset is meaningless). We let
	 * probe IOs through so we can match them to probe inject records.
	 */
	if (zio != NULL && zio->io_type != ZIO_TYPE_FLUSH &&
	    !(zio->io_flags & ZIO_FLAG_PROBE)) {
		uint64_t offset = zio->io_offset;

		if (offset < VDEV_LABEL_START_SIZE ||
		    offset >= vd->vdev_psize - VDEV_LABEL_END_SIZE)
			return (0);
	}

	rw_enter(&inject_lock, RW_READER);

	for (handler = list_head(&inject_handlers); handler != NULL;
	    handler = list_next(&inject_handlers, handler)) {

		if (handler->zi_record.zi_cmd != ZINJECT_DEVICE_FAULT)
			continue;

		if (vd->vdev_guid == handler->zi_record.zi_guid) {
			if (handler->zi_record.zi_failfast &&
			    (zio == NULL || (zio->io_flags &
			    (ZIO_FLAG_IO_RETRY | ZIO_FLAG_TRYHARD)))) {
				continue;
			}

			/* Handle type specific I/O failures */
			if (zio != NULL && !zio_match_iotype(zio,
			    handler->zi_record.zi_iotype))
				continue;

			if (handler->zi_record.zi_error == err1 ||
			    handler->zi_record.zi_error == err2) {
				handler->zi_record.zi_match_count++;

				/*
				 * limit error injection if requested
				 */
				if (!freq_triggered(handler->zi_record.zi_freq))
					continue;

				handler->zi_record.zi_inject_count++;

				/*
				 * For a failed open, pretend like the device
				 * has gone away.
				 */
				if (err1 == ENXIO)
					vd->vdev_stat.vs_aux =
					    VDEV_AUX_OPEN_FAILED;

				/*
				 * Treat these errors as if they had been
				 * retried so that all the appropriate stats
				 * and FMA events are generated.
				 */
				if (!handler->zi_record.zi_failfast &&
				    zio != NULL)
					zio->io_flags |= ZIO_FLAG_IO_RETRY;

				/*
				 * EILSEQ means flip a bit after a read
				 */
				if (handler->zi_record.zi_error == EILSEQ) {
					if (zio == NULL)
						break;

					/* locate buffer data and flip a bit */
					(void) abd_iterate_func(zio->io_abd, 0,
					    zio->io_size, zio_inject_bitflip_cb,
					    zio);
					break;
				}

				ret = handler->zi_record.zi_error;
				break;
			}
			if (handler->zi_record.zi_error == ENXIO) {
				handler->zi_record.zi_match_count++;
				handler->zi_record.zi_inject_count++;
				ret = SET_ERROR(EIO);
				break;
			}
		}
	}

	rw_exit(&inject_lock);

	return (ret);
}

int
zio_handle_device_injection(vdev_t *vd, zio_t *zio, int error)
{
	return (zio_handle_device_injection_impl(vd, zio, error, INT_MAX));
}

int
zio_handle_device_injections(vdev_t *vd, zio_t *zio, int err1, int err2)
{
	return (zio_handle_device_injection_impl(vd, zio, err1, err2));
}

/*
 * Simulate hardware that ignores cache flushes.  For requested number
 * of seconds nix the actual writing to disk.
 */
void
zio_handle_ignored_writes(zio_t *zio)
{
	inject_handler_t *handler;

	rw_enter(&inject_lock, RW_READER);

	for (handler = list_head(&inject_handlers); handler != NULL;
	    handler = list_next(&inject_handlers, handler)) {

		/* Ignore errors not destined for this pool */
		if (zio->io_spa != handler->zi_spa ||
		    handler->zi_record.zi_cmd != ZINJECT_IGNORED_WRITES)
			continue;

		handler->zi_record.zi_match_count++;

		/*
		 * Positive duration implies # of seconds, negative
		 * a number of txgs
		 */
		if (handler->zi_record.zi_timer == 0) {
			if (handler->zi_record.zi_duration > 0)
				handler->zi_record.zi_timer = ddi_get_lbolt64();
			else
				handler->zi_record.zi_timer = zio->io_txg;
		}

		/* Have a "problem" writing 60% of the time */
		if (random_in_range(100) < 60) {
			handler->zi_record.zi_inject_count++;
			zio->io_pipeline &= ~ZIO_VDEV_IO_STAGES;
		}
		break;
	}

	rw_exit(&inject_lock);
}

void
spa_handle_ignored_writes(spa_t *spa)
{
	inject_handler_t *handler;

	if (zio_injection_enabled == 0)
		return;

	rw_enter(&inject_lock, RW_READER);

	for (handler = list_head(&inject_handlers); handler != NULL;
	    handler = list_next(&inject_handlers, handler)) {

		if (spa != handler->zi_spa ||
		    handler->zi_record.zi_cmd != ZINJECT_IGNORED_WRITES)
			continue;

		handler->zi_record.zi_match_count++;
		handler->zi_record.zi_inject_count++;

		if (handler->zi_record.zi_duration > 0) {
			VERIFY(handler->zi_record.zi_timer == 0 ||
			    ddi_time_after64(
			    (int64_t)handler->zi_record.zi_timer +
			    handler->zi_record.zi_duration * hz,
			    ddi_get_lbolt64()));
		} else {
			/* duration is negative so the subtraction here adds */
			VERIFY(handler->zi_record.zi_timer == 0 ||
			    handler->zi_record.zi_timer -
			    handler->zi_record.zi_duration >=
			    spa_syncing_txg(spa));
		}
	}

	rw_exit(&inject_lock);
}

hrtime_t
zio_handle_io_delay(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	inject_handler_t *min_handler = NULL;
	hrtime_t min_target = 0;

	rw_enter(&inject_lock, RW_READER);

	/*
	 * inject_delay_count is a subset of zio_injection_enabled that
	 * is only incremented for delay handlers. These checks are
	 * mainly added to remind the reader why we're not explicitly
	 * checking zio_injection_enabled like the other functions.
	 */
	IMPLY(inject_delay_count > 0, zio_injection_enabled > 0);
	IMPLY(zio_injection_enabled == 0, inject_delay_count == 0);

	/*
	 * If there aren't any inject delay handlers registered, then we
	 * can short circuit and simply return 0 here. A value of zero
	 * informs zio_delay_interrupt() that this request should not be
	 * delayed. This short circuit keeps us from acquiring the
	 * inject_delay_mutex unnecessarily.
	 */
	if (inject_delay_count == 0) {
		rw_exit(&inject_lock);
		return (0);
	}

	/*
	 * Each inject handler has a number of "lanes" associated with
	 * it. Each lane is able to handle requests independently of one
	 * another, and at a latency defined by the inject handler
	 * record's zi_timer field. Thus if a handler in configured with
	 * a single lane with a 10ms latency, it will delay requests
	 * such that only a single request is completed every 10ms. So,
	 * if more than one request is attempted per each 10ms interval,
	 * the average latency of the requests will be greater than
	 * 10ms; but if only a single request is submitted each 10ms
	 * interval the average latency will be 10ms.
	 *
	 * We need to acquire this mutex to prevent multiple concurrent
	 * threads being assigned to the same lane of a given inject
	 * handler. The mutex allows us to perform the following two
	 * operations atomically:
	 *
	 *	1. determine the minimum handler and minimum target
	 *	   value of all the possible handlers
	 *	2. update that minimum handler's lane array
	 *
	 * Without atomicity, two (or more) threads could pick the same
	 * lane in step (1), and then conflict with each other in step
	 * (2). This could allow a single lane handler to process
	 * multiple requests simultaneously, which shouldn't be possible.
	 */
	mutex_enter(&inject_delay_mtx);

	for (inject_handler_t *handler = list_head(&inject_handlers);
	    handler != NULL; handler = list_next(&inject_handlers, handler)) {
		if (handler->zi_record.zi_cmd != ZINJECT_DELAY_IO)
			continue;

		if (vd->vdev_guid != handler->zi_record.zi_guid)
			continue;

		/* also match on I/O type (e.g., -T read) */
		if (!zio_match_iotype(zio, handler->zi_record.zi_iotype))
			continue;

		/*
		 * Defensive; should never happen as the array allocation
		 * occurs prior to inserting this handler on the list.
		 */
		ASSERT3P(handler->zi_lanes, !=, NULL);

		/*
		 * This should never happen, the zinject command should
		 * prevent a user from setting an IO delay with zero lanes.
		 */
		ASSERT3U(handler->zi_record.zi_nlanes, !=, 0);

		ASSERT3U(handler->zi_record.zi_nlanes, >,
		    handler->zi_next_lane);

		handler->zi_record.zi_match_count++;

		/* Limit the use of this handler if requested */
		if (!freq_triggered(handler->zi_record.zi_freq))
			continue;

		/*
		 * We want to issue this IO to the lane that will become
		 * idle the soonest, so we compare the soonest this
		 * specific handler can complete the IO with all other
		 * handlers, to find the lowest value of all possible
		 * lanes. We then use this lane to submit the request.
		 *
		 * Since each handler has a constant value for its
		 * delay, we can just use the "next" lane for that
		 * handler; as it will always be the lane with the
		 * lowest value for that particular handler (i.e. the
		 * lane that will become idle the soonest). This saves a
		 * scan of each handler's lanes array.
		 *
		 * There's two cases to consider when determining when
		 * this specific IO request should complete. If this
		 * lane is idle, we want to "submit" the request now so
		 * it will complete after zi_timer milliseconds. Thus,
		 * we set the target to now + zi_timer.
		 *
		 * If the lane is busy, we want this request to complete
		 * zi_timer milliseconds after the lane becomes idle.
		 * Since the 'zi_lanes' array holds the time at which
		 * each lane will become idle, we use that value to
		 * determine when this request should complete.
		 */
		hrtime_t idle = handler->zi_record.zi_timer + gethrtime();
		hrtime_t busy = handler->zi_record.zi_timer +
		    handler->zi_lanes[handler->zi_next_lane];
		hrtime_t target = MAX(idle, busy);

		if (min_handler == NULL) {
			min_handler = handler;
			min_target = target;
			continue;
		}

		ASSERT3P(min_handler, !=, NULL);
		ASSERT3U(min_target, !=, 0);

		/*
		 * We don't yet increment the "next lane" variable since
		 * we still might find a lower value lane in another
		 * handler during any remaining iterations. Once we're
		 * sure we've selected the absolute minimum, we'll claim
		 * the lane and increment the handler's "next lane"
		 * field below.
		 */

		if (target < min_target) {
			min_handler = handler;
			min_target = target;
		}
	}

	/*
	 * 'min_handler' will be NULL if no IO delays are registered for
	 * this vdev, otherwise it will point to the handler containing
	 * the lane that will become idle the soonest.
	 */
	if (min_handler != NULL) {
		ASSERT3U(min_target, !=, 0);
		min_handler->zi_lanes[min_handler->zi_next_lane] = min_target;

		/*
		 * If we've used all possible lanes for this handler,
		 * loop back and start using the first lane again;
		 * otherwise, just increment the lane index.
		 */
		min_handler->zi_next_lane = (min_handler->zi_next_lane + 1) %
		    min_handler->zi_record.zi_nlanes;

		min_handler->zi_record.zi_inject_count++;

	}

	mutex_exit(&inject_delay_mtx);
	rw_exit(&inject_lock);

	return (min_target);
}

static void
zio_handle_pool_delay(spa_t *spa, hrtime_t elapsed, zinject_type_t command)
{
	inject_handler_t *handler;
	hrtime_t delay = 0;
	int id = 0;

	rw_enter(&inject_lock, RW_READER);

	for (handler = list_head(&inject_handlers);
	    handler != NULL && handler->zi_record.zi_cmd == command;
	    handler = list_next(&inject_handlers, handler)) {
		ASSERT3P(handler->zi_spa_name, !=, NULL);
		if (strcmp(spa_name(spa), handler->zi_spa_name) == 0) {
			handler->zi_record.zi_match_count++;
			uint64_t pause =
			    SEC2NSEC(handler->zi_record.zi_duration);
			if (pause > elapsed) {
				handler->zi_record.zi_inject_count++;
				delay = pause - elapsed;
			}
			id = handler->zi_id;
			break;
		}
	}

	rw_exit(&inject_lock);

	if (delay) {
		if (command == ZINJECT_DELAY_IMPORT) {
			spa_import_progress_set_notes(spa, "injecting %llu "
			    "sec delay", (u_longlong_t)NSEC2SEC(delay));
		}
		zfs_sleep_until(gethrtime() + delay);
	}
	if (id) {
		/* all done with this one-shot handler */
		zio_clear_fault(id);
	}
}

/*
 * For testing, inject a delay during an import
 */
void
zio_handle_import_delay(spa_t *spa, hrtime_t elapsed)
{
	zio_handle_pool_delay(spa, elapsed, ZINJECT_DELAY_IMPORT);
}

/*
 * For testing, inject a delay during an export
 */
void
zio_handle_export_delay(spa_t *spa, hrtime_t elapsed)
{
	zio_handle_pool_delay(spa, elapsed, ZINJECT_DELAY_EXPORT);
}

static int
zio_calculate_range(const char *pool, zinject_record_t *record)
{
	dsl_pool_t *dp;
	dsl_dataset_t *ds;
	objset_t *os = NULL;
	dnode_t *dn = NULL;
	int error;

	/*
	 * Obtain the dnode for object using pool, objset, and object
	 */
	error = dsl_pool_hold(pool, FTAG, &dp);
	if (error)
		return (error);

	error = dsl_dataset_hold_obj(dp, record->zi_objset, FTAG, &ds);
	dsl_pool_rele(dp, FTAG);
	if (error)
		return (error);

	error = dmu_objset_from_ds(ds, &os);
	dsl_dataset_rele(ds, FTAG);
	if (error)
		return (error);

	error = dnode_hold(os, record->zi_object, FTAG, &dn);
	if (error)
		return (error);

	/*
	 * Translate the range into block IDs
	 */
	if (record->zi_start != 0 || record->zi_end != -1ULL) {
		record->zi_start >>= dn->dn_datablkshift;
		record->zi_end >>= dn->dn_datablkshift;
	}
	if (record->zi_level > 0) {
		if (record->zi_level >= dn->dn_nlevels) {
			dnode_rele(dn, FTAG);
			return (SET_ERROR(EDOM));
		}

		if (record->zi_start != 0 || record->zi_end != 0) {
			int shift = dn->dn_indblkshift - SPA_BLKPTRSHIFT;

			for (int level = record->zi_level; level > 0; level--) {
				record->zi_start >>= shift;
				record->zi_end >>= shift;
			}
		}
	}

	dnode_rele(dn, FTAG);
	return (0);
}

static boolean_t
zio_pool_handler_exists(const char *name, zinject_type_t command)
{
	boolean_t exists = B_FALSE;

	rw_enter(&inject_lock, RW_READER);
	for (inject_handler_t *handler = list_head(&inject_handlers);
	    handler != NULL; handler = list_next(&inject_handlers, handler)) {
		if (command != handler->zi_record.zi_cmd)
			continue;

		const char *pool = (handler->zi_spa_name != NULL) ?
		    handler->zi_spa_name : spa_name(handler->zi_spa);
		if (strcmp(name, pool) == 0) {
			exists = B_TRUE;
			break;
		}
	}
	rw_exit(&inject_lock);

	return (exists);
}
/*
 * Create a new handler for the given record.  We add it to the list, adding
 * a reference to the spa_t in the process.  We increment zio_injection_enabled,
 * which is the switch to trigger all fault injection.
 */
int
zio_inject_fault(char *name, int flags, int *id, zinject_record_t *record)
{
	inject_handler_t *handler;
	int error;
	spa_t *spa;

	/*
	 * If this is pool-wide metadata, make sure we unload the corresponding
	 * spa_t, so that the next attempt to load it will trigger the fault.
	 * We call spa_reset() to unload the pool appropriately.
	 */
	if (flags & ZINJECT_UNLOAD_SPA)
		if ((error = spa_reset(name)) != 0)
			return (error);

	if (record->zi_cmd == ZINJECT_DELAY_IO) {
		/*
		 * A value of zero for the number of lanes or for the
		 * delay time doesn't make sense.
		 */
		if (record->zi_timer == 0 || record->zi_nlanes == 0)
			return (SET_ERROR(EINVAL));

		/*
		 * The number of lanes is directly mapped to the size of
		 * an array used by the handler. Thus, to ensure the
		 * user doesn't trigger an allocation that's "too large"
		 * we cap the number of lanes here.
		 */
		if (record->zi_nlanes >= UINT16_MAX)
			return (SET_ERROR(EINVAL));
	}

	/*
	 * If the supplied range was in bytes -- calculate the actual blkid
	 */
	if (flags & ZINJECT_CALC_RANGE) {
		error = zio_calculate_range(name, record);
		if (error != 0)
			return (error);
	}

	if (!(flags & ZINJECT_NULL)) {
		/*
		 * Pool delays for import or export don't take an
		 * injection reference on the spa. Instead they
		 * rely on matching by name.
		 */
		if (record->zi_cmd == ZINJECT_DELAY_IMPORT ||
		    record->zi_cmd == ZINJECT_DELAY_EXPORT) {
			if (record->zi_duration <= 0)
				return (SET_ERROR(EINVAL));
			/*
			 * Only one import | export delay handler per pool.
			 */
			if (zio_pool_handler_exists(name, record->zi_cmd))
				return (SET_ERROR(EEXIST));

			mutex_enter(&spa_namespace_lock);
			boolean_t has_spa = spa_lookup(name) != NULL;
			mutex_exit(&spa_namespace_lock);

			if (record->zi_cmd == ZINJECT_DELAY_IMPORT && has_spa)
				return (SET_ERROR(EEXIST));
			if (record->zi_cmd == ZINJECT_DELAY_EXPORT && !has_spa)
				return (SET_ERROR(ENOENT));
			spa = NULL;
		} else {
			/*
			 * spa_inject_ref() will add an injection reference,
			 * which will prevent the pool from being removed
			 * from the namespace while still allowing it to be
			 * unloaded.
			 */
			if ((spa = spa_inject_addref(name)) == NULL)
				return (SET_ERROR(ENOENT));
		}

		handler = kmem_alloc(sizeof (inject_handler_t), KM_SLEEP);
		handler->zi_spa = spa;	/* note: can be NULL */
		handler->zi_record = *record;

		if (handler->zi_record.zi_cmd == ZINJECT_DELAY_IO) {
			handler->zi_lanes = kmem_zalloc(
			    sizeof (*handler->zi_lanes) *
			    handler->zi_record.zi_nlanes, KM_SLEEP);
			handler->zi_next_lane = 0;
		} else {
			handler->zi_lanes = NULL;
			handler->zi_next_lane = 0;
		}

		if (handler->zi_spa == NULL)
			handler->zi_spa_name = spa_strdup(name);
		else
			handler->zi_spa_name = NULL;

		rw_enter(&inject_lock, RW_WRITER);

		/*
		 * We can't move this increment into the conditional
		 * above because we need to hold the RW_WRITER lock of
		 * inject_lock, and we don't want to hold that while
		 * allocating the handler's zi_lanes array.
		 */
		if (handler->zi_record.zi_cmd == ZINJECT_DELAY_IO) {
			ASSERT3S(inject_delay_count, >=, 0);
			inject_delay_count++;
			ASSERT3S(inject_delay_count, >, 0);
		}

		*id = handler->zi_id = inject_next_id++;
		list_insert_tail(&inject_handlers, handler);
		atomic_inc_32(&zio_injection_enabled);

		rw_exit(&inject_lock);
	}

	/*
	 * Flush the ARC, so that any attempts to read this data will end up
	 * going to the ZIO layer.  Note that this is a little overkill, but
	 * we don't have the necessary ARC interfaces to do anything else, and
	 * fault injection isn't a performance critical path.
	 */
	if (flags & ZINJECT_FLUSH_ARC)
		/*
		 * We must use FALSE to ensure arc_flush returns, since
		 * we're not preventing concurrent ARC insertions.
		 */
		arc_flush(NULL, FALSE);

	return (0);
}

/*
 * Returns the next record with an ID greater than that supplied to the
 * function.  Used to iterate over all handlers in the system.
 */
int
zio_inject_list_next(int *id, char *name, size_t buflen,
    zinject_record_t *record)
{
	inject_handler_t *handler;
	int ret;

	mutex_enter(&spa_namespace_lock);
	rw_enter(&inject_lock, RW_READER);

	for (handler = list_head(&inject_handlers); handler != NULL;
	    handler = list_next(&inject_handlers, handler))
		if (handler->zi_id > *id)
			break;

	if (handler) {
		*record = handler->zi_record;
		*id = handler->zi_id;
		ASSERT(handler->zi_spa || handler->zi_spa_name);
		if (handler->zi_spa != NULL)
			(void) strlcpy(name, spa_name(handler->zi_spa), buflen);
		else
			(void) strlcpy(name, handler->zi_spa_name, buflen);
		ret = 0;
	} else {
		ret = SET_ERROR(ENOENT);
	}

	rw_exit(&inject_lock);
	mutex_exit(&spa_namespace_lock);

	return (ret);
}

/*
 * Clear the fault handler with the given identifier, or return ENOENT if none
 * exists.
 */
int
zio_clear_fault(int id)
{
	inject_handler_t *handler;

	rw_enter(&inject_lock, RW_WRITER);

	for (handler = list_head(&inject_handlers); handler != NULL;
	    handler = list_next(&inject_handlers, handler))
		if (handler->zi_id == id)
			break;

	if (handler == NULL) {
		rw_exit(&inject_lock);
		return (SET_ERROR(ENOENT));
	}

	if (handler->zi_record.zi_cmd == ZINJECT_DELAY_IO) {
		ASSERT3S(inject_delay_count, >, 0);
		inject_delay_count--;
		ASSERT3S(inject_delay_count, >=, 0);
	}

	list_remove(&inject_handlers, handler);
	rw_exit(&inject_lock);

	if (handler->zi_record.zi_cmd == ZINJECT_DELAY_IO) {
		ASSERT3P(handler->zi_lanes, !=, NULL);
		kmem_free(handler->zi_lanes, sizeof (*handler->zi_lanes) *
		    handler->zi_record.zi_nlanes);
	} else {
		ASSERT3P(handler->zi_lanes, ==, NULL);
	}

	if (handler->zi_spa_name != NULL)
		spa_strfree(handler->zi_spa_name);

	if (handler->zi_spa != NULL)
		spa_inject_delref(handler->zi_spa);
	kmem_free(handler, sizeof (inject_handler_t));
	atomic_dec_32(&zio_injection_enabled);

	return (0);
}

void
zio_inject_init(void)
{
	rw_init(&inject_lock, NULL, RW_DEFAULT, NULL);
	mutex_init(&inject_delay_mtx, NULL, MUTEX_DEFAULT, NULL);
	list_create(&inject_handlers, sizeof (inject_handler_t),
	    offsetof(inject_handler_t, zi_link));
}

void
zio_inject_fini(void)
{
	list_destroy(&inject_handlers);
	mutex_destroy(&inject_delay_mtx);
	rw_destroy(&inject_lock);
}

#if defined(_KERNEL)
EXPORT_SYMBOL(zio_injection_enabled);
EXPORT_SYMBOL(zio_inject_fault);
EXPORT_SYMBOL(zio_inject_list_next);
EXPORT_SYMBOL(zio_clear_fault);
EXPORT_SYMBOL(zio_handle_fault_injection);
EXPORT_SYMBOL(zio_handle_device_injection);
EXPORT_SYMBOL(zio_handle_label_injection);
#endif
