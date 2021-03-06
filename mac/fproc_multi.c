/*
 * This file is part of the UWB stack for linux.
 *
 * Copyright (c) 2020-2021 Qorvo US, Inc.
 *
 * This software is provided under the GNU General Public License, version 2
 * (GPLv2), as well as under a Qorvo commercial license.
 *
 * You may choose to use this software under the terms of the GPLv2 License,
 * version 2 ("GPLv2"), as published by the Free Software Foundation.
 * You should have received a copy of the GPLv2 along with this program.  If
 * not, see <http://www.gnu.org/licenses/>.
 *
 * This program is distributed under the GPLv2 in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GPLv2 for more
 * details.
 *
 * If you cannot meet the requirements of the GPLv2, you may not use this
 * software for any purpose without first obtaining a commercial license from
 * Qorvo. Please contact Qorvo to inquire about licensing terms.
 */

#include <linux/errno.h>

#include "mcps802154_i.h"
#include "llhw-ops.h"

static int mcps802154_fproc_multi_handle_frame(struct mcps802154_local *local,
					       struct mcps802154_access *access,
					       size_t frame_idx);

static int
mcps802154_fproc_multi_restore_hw_addr_filt(struct mcps802154_local *local,
					    struct mcps802154_access *access)
{
	int r = 0;

	if (access->hw_addr_filt_changed) {
		struct ieee802154_hw_addr_filt hw_addr_filt;

		hw_addr_filt.pan_id = local->pib.mac_pan_id;
		hw_addr_filt.short_addr = local->pib.mac_short_addr;
		hw_addr_filt.ieee_addr = local->pib.mac_extended_addr;
		r = llhw_set_hw_addr_filt(local, &hw_addr_filt,
					  access->hw_addr_filt_changed);
	}

	return r;
}

static int mcps802154_fproc_multi_restore(struct mcps802154_local *local,
					  struct mcps802154_access *access)
{
	if (access->channel) {
		int r;
		const struct mcps802154_channel *channel =
			&local->pib.phy_current_channel;

		r = llhw_set_channel(local, channel->page, channel->channel,
				     channel->preamble_code);
		if (r)
			return r;
	}

	return mcps802154_fproc_multi_restore_hw_addr_filt(local, access);
}

/**
 * mcps802154_fproc_multi_next() - Continue with the next frame, or next
 * access.
 * @local: MCPS private data.
 * @access: Current access to handle.
 * @frame_idx: Frame index in current access, must be valid, will be
 *             incremented.
 */
static void mcps802154_fproc_multi_next(struct mcps802154_local *local,
					struct mcps802154_access *access,
					size_t frame_idx)
{
	int r;

	frame_idx++;
	if (frame_idx < access->n_frames) {
		/* Next frame. */
		r = mcps802154_fproc_multi_handle_frame(local, access,
							frame_idx);
		if (r) {
			if (r == -ETIME) {
				mcps802154_fproc_access_done(local, 0);
				mcps802154_fproc_access_now(local);
			} else {
				mcps802154_fproc_access_done(local, r);
				mcps802154_fproc_broken_handle(local);
			}
		}
	} else {
		r = mcps802154_fproc_multi_restore(local, access);
		if (r) {
			mcps802154_fproc_access_done(local, r);
			mcps802154_fproc_broken_handle(local);
			return;
		}
		/* Next access. */
		mcps802154_fproc_access_done(local, 0);
		if (access->duration_dtu) {
			u32 next_access_dtu =
				access->timestamp_dtu + access->duration_dtu;
			mcps802154_fproc_access(local, next_access_dtu);
		} else {
			mcps802154_fproc_access_now(local);
		}
	}
}

static void mcps802154_fproc_multi_rx_rx_frame(struct mcps802154_local *local)
{
	struct mcps802154_access *access = local->fproc.access;
	size_t frame_idx = local->fproc.frame_idx;
	struct mcps802154_access_frame *frame = &access->frames[frame_idx];
	int r;

	/* Read frame. */
	struct sk_buff *skb = NULL;
	struct mcps802154_rx_frame_info info = {
		.flags = frame->rx.frame_info_flags_request,
	};
	r = llhw_rx_get_frame(local, &skb, &info);
	if (!r)
		access->ops->rx_frame(access, frame_idx, skb, &info,
				      MCPS802154_RX_ERROR_NONE);

	if (r && r != -EBUSY) {
		mcps802154_fproc_access_done(local, r);
		mcps802154_fproc_broken_handle(local);
	} else {
		/* Next. */
		mcps802154_fproc_multi_next(local, access, frame_idx);
	}
}

static void mcps802154_fproc_multi_rx_rx_timeout(struct mcps802154_local *local)
{
	struct mcps802154_access *access = local->fproc.access;
	size_t frame_idx = local->fproc.frame_idx;

	access->ops->rx_frame(access, frame_idx, NULL, NULL,
			      MCPS802154_RX_ERROR_TIMEOUT);

	/* Next. */
	mcps802154_fproc_multi_next(local, access, frame_idx);
}

static void
mcps802154_fproc_multi_rx_rx_error(struct mcps802154_local *local,
				   enum mcps802154_rx_error_type error)
{
	struct mcps802154_access *access = local->fproc.access;
	size_t frame_idx = local->fproc.frame_idx;
	struct mcps802154_rx_frame_info info = {
		.flags = MCPS802154_RX_INFO_TIMESTAMP_DTU,
	};

	llhw_rx_get_error_frame(local, &info);
	access->ops->rx_frame(access, frame_idx, NULL, &info, error);

	/* Next. */
	mcps802154_fproc_multi_next(local, access, frame_idx);
}

static void
mcps802154_fproc_multi_rx_schedule_change(struct mcps802154_local *local)
{
	/* If the Rx is done without a timeout, disable RX and change the access. */
	struct mcps802154_access *access = local->fproc.access;
	int frame_idx = local->fproc.frame_idx;
	struct mcps802154_access_frame *frame = &access->frames[frame_idx];

	if (frame->rx.info.timeout_dtu == -1) {
		/* Disable RX. */
		int r = llhw_rx_disable(local);

		if (r == -EBUSY)
			/* Wait for RX result. */
			return;

		access->ops->rx_frame(access, frame_idx, NULL, NULL,
				      MCPS802154_RX_ERROR_TIMEOUT);
		if (r) {
			mcps802154_fproc_access_done(local, r);
			mcps802154_fproc_broken_handle(local);
		} else {
			/* Next. */
			mcps802154_fproc_multi_next(local, access, frame_idx);
		}
	}
}

static const struct mcps802154_fproc_state mcps802154_fproc_multi_rx = {
	.name = "multi_rx",
	.rx_frame = mcps802154_fproc_multi_rx_rx_frame,
	.rx_timeout = mcps802154_fproc_multi_rx_rx_timeout,
	.rx_error = mcps802154_fproc_multi_rx_rx_error,
	.schedule_change = mcps802154_fproc_multi_rx_schedule_change,
};

static void mcps802154_fproc_multi_tx_tx_done(struct mcps802154_local *local)
{
	struct mcps802154_access *access = local->fproc.access;
	size_t frame_idx = local->fproc.frame_idx;

	access->ops->tx_return(access, frame_idx, local->fproc.tx_skb,
			       MCPS802154_ACCESS_TX_RETURN_REASON_CONSUMED);
	local->fproc.tx_skb = NULL;

	/* Next. */
	mcps802154_fproc_multi_next(local, access, frame_idx);
}

static void
mcps802154_fproc_multi_tx_schedule_change(struct mcps802154_local *local)
{
	/* Wait for end of current frame. */
}

static const struct mcps802154_fproc_state mcps802154_fproc_multi_tx = {
	.name = "multi_tx",
	.tx_done = mcps802154_fproc_multi_tx_tx_done,
	.schedule_change = mcps802154_fproc_multi_tx_schedule_change,
};

/**
 * mcps802154_fproc_multi_handle_frame() - Handle a single frame and change
 * state.
 * @local: MCPS private data.
 * @access: Current access to handle.
 * @frame_idx: Frame index in current access, must be valid.
 *
 * Return: 0 or error.
 */
static int mcps802154_fproc_multi_handle_frame(struct mcps802154_local *local,
					       struct mcps802154_access *access,
					       size_t frame_idx)
{
	struct mcps802154_access_frame *frame;
	struct sk_buff *skb;
	int r;

	local->fproc.frame_idx = frame_idx;

	frame = &access->frames[frame_idx];
	if (!frame->is_tx) {
		if (frame->rx.info.flags & MCPS802154_RX_INFO_AACK)
			return -EINVAL;

		if (frame->sts_params) {
			r = llhw_set_sts_params(local, frame->sts_params);
			if (r)
				return r;
		}

		r = llhw_rx_enable(local, &frame->rx.info, frame_idx, 0);
		if (r)
			return r;

		mcps802154_fproc_change_state(local,
					      &mcps802154_fproc_multi_rx);
	} else {
		if (frame->tx_frame_info.rx_enable_after_tx_dtu)
			return -EINVAL;

		skb = access->ops->tx_get_frame(access, frame_idx);

		/* TODO: prepare next RX directly. */

		if (frame->sts_params) {
			r = llhw_set_sts_params(local, frame->sts_params);
			if (r) {
				access->ops->tx_return(
					access, frame_idx, skb,
					MCPS802154_ACCESS_TX_RETURN_REASON_CANCEL);
				return r;
			}
		}

		r = llhw_tx_frame(local, skb, &frame->tx_frame_info, frame_idx,
				  0);
		if (r) {
			access->ops->tx_return(
				access, frame_idx, skb,
				MCPS802154_ACCESS_TX_RETURN_REASON_CANCEL);
			return r;
		}

		local->fproc.tx_skb = skb;
		mcps802154_ca_access_hold(local);
		mcps802154_fproc_change_state(local,
					      &mcps802154_fproc_multi_tx);
	}

	return 0;
}

int mcps802154_fproc_multi_handle(struct mcps802154_local *local,
				  struct mcps802154_access *access)
{
	int i = 1;
	int r;

	if (access->n_frames == 0 || !access->frames)
		return -EINVAL;
	for (; i < access->n_frames; i++) {
		struct mcps802154_access_frame *frame = &access->frames[i];
		/* Only first Rx can be without timeout. */
		if (!frame->is_tx && frame->rx.info.timeout_dtu == -1)
			return -EINVAL;
	}
	if (access->hw_addr_filt_changed) {
		r = llhw_set_hw_addr_filt(local, &access->hw_addr_filt,
					  access->hw_addr_filt_changed);
		if (r)
			return r;
	}
	if (access->channel) {
		r = llhw_set_channel(local, access->channel->page,
				     access->channel->channel,
				     access->channel->preamble_code);
		if (r) {
			mcps802154_fproc_multi_restore_hw_addr_filt(local,
								    access);
			return r;
		}
	}
	return mcps802154_fproc_multi_handle_frame(local, access, 0);
}
