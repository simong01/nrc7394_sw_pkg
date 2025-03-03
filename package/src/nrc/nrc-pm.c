/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * Power management features
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <net/dst.h>
#include <net/xfrm.h>
#include <net/mac80211.h>
#include <net/ieee80211_radiotap.h>
#include <linux/if_arp.h>
#include <linux/rtnetlink.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/ktime.h>
#include <net/genetlink.h>
#include <linux/spi/spi.h>
#include <linux/ieee80211.h>

#include "nrc-mac80211.h"
#include "nrc-hif.h"
#include "wim.h"
#include "nrc-debug.h"

/**
 * DOC: STA powersaving
 */

#define BSS_MAX_IDLE_TIMER_PERIOD_MS	1000

static int tx_h_sta_pm(struct nrc_trx_data *tx)
{
#ifdef CONFIG_SUPPORT_AFTER_KERNEL_3_0_36
	struct ieee80211_hw *hw = tx->nw->hw;
#endif
	struct sk_buff *skb = tx->skb;
	struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *mh = (void *) skb->data;
	__le16 fc = mh->frame_control;

#ifdef CONFIG_SUPPORT_PS
	if (!ieee80211_hw_check(hw, SUPPORTS_PS) ||
	    !(hw->conf.flags & IEEE80211_CONF_PS) ||
	    hw->conf.dynamic_ps_timeout > 0)
		return 0;
#endif

	if (tx->vif->type != NL80211_IFTYPE_STATION)
		return 0;

	/*
	 * mac80211 does not set PM field for normal data frames, so we
	 * need to update that based on the current PS mode.
	 */
	if (!(txi->flags & IEEE80211_TX_CTL_NO_ACK) &&
	    ieee80211_is_data(fc) && !ieee80211_has_pm(fc)) {
		mh->frame_control |= cpu_to_le16(IEEE80211_FCTL_PM);
		fc = mh->frame_control;
	}

	if (ieee80211_is_pspoll(fc)) {
		struct nrc_vif *i_vif = to_i_vif(tx->vif);

		i_vif->ps_polling = true;
	}

	return 0;
}
TXH(tx_h_sta_pm, BIT(NL80211_IFTYPE_STATION));


static void nrc_mac_rx_fictitious_ps_poll_response(struct ieee80211_vif *vif)
{
	struct nrc_vif *nvif = (struct nrc_vif *) vif->drv_priv;
	struct nrc *nw = nvif->nw;
	struct sk_buff *skb;
	struct ieee80211_hdr_3addr *nullfunc;
	struct ieee80211_rx_status *status;

#if KERNEL_VERSION(4, 14, 17) <= NRC_TARGET_KERNEL_VERSION
#if KERNEL_VERSION(6, 1, 0) <= NRC_TARGET_KERNEL_VERSION
	skb= ieee80211_nullfunc_get(nw->hw, vif, vif->bss_conf.link_id, false);
#else
	skb = ieee80211_nullfunc_get(nw->hw, vif, false);
#endif
#else
	skb = ieee80211_nullfunc_get(nw->hw, vif);
#endif
	if (!skb)
		return;

	nullfunc = (struct ieee80211_hdr_3addr *) skb->data;
	nullfunc->frame_control = cpu_to_le16(IEEE80211_FTYPE_DATA |
					      IEEE80211_STYPE_NULLFUNC |
					      IEEE80211_FCTL_FROMDS);

	/* RA: vif->addr,TA = SA: vif->bss_conf->bssid */
	ether_addr_copy(nullfunc->addr1, vif->addr);
	ether_addr_copy(nullfunc->addr2, vif->bss_conf.bssid);
	ether_addr_copy(nullfunc->addr3, vif->bss_conf.bssid);

	status = IEEE80211_SKB_RXCB(skb);
	memset(status, 0, sizeof(*status));
	status->freq = nw->center_freq;
	status->band = nw->band;
	status->rate_idx = 0;

	ieee80211_rx_irqsafe(nw->hw, skb);
}

/**
 * rx_h_fixup_ps_poll_sp() - fix up the incomplete PS-Poll SP
 *
 * Once a non-AP STA does not receive a buffered frame in response
 * to the PS-Poll it sent, the mac80211 does not send further PS-Poll
 * even if TIM in the subsequent beacons includes the AID for this STA,
 * until it receives a unicast frame destined to it.
 *
 * Our workaround is to pretend as if we had received a Null frame from
 * the AP, when we receive another beacon from the AP (whether or not
 * TIM includes the STA).
 */
static int rx_h_fixup_ps_poll_sp(struct nrc_trx_data *rx)
{
	struct ieee80211_hw *hw = rx->nw->hw;
	struct nrc_vif *i_vif = to_i_vif(rx->vif);
	struct ieee80211_hdr *mh = (void *) rx->skb->data;
	__le16 fc = mh->frame_control;

	if (!(hw->conf.flags & IEEE80211_CONF_PS) ||
	    hw->conf.dynamic_ps_timeout > 0)
		return 0;

	/*
	 * I am only interested if @rx->vif is of STATION type, and
	 * it is in PS-Poll service period
	 */
	if (rx->vif->type != NL80211_IFTYPE_STATION || !i_vif->ps_polling)
		return 0;

	/* This frame is not coming from the serving AP */
	if (!ether_addr_equal(ieee80211_get_SA(mh), rx->vif->bss_conf.bssid))
		return 0;

	if (ieee80211_is_beacon(fc)) {
		i_vif->ps_polling = false;
		nrc_mac_rx_fictitious_ps_poll_response(rx->vif);
	} else if (ieee80211_is_data(fc))
		i_vif->ps_polling = false;

	return 0;
}
RXH(rx_h_fixup_ps_poll_sp, BIT(NL80211_IFTYPE_STATION));


/**
 * DOC: 802.11v
 *
 */

static u8 *find_bss_max_idle_ie(struct sk_buff *skb)
{
	struct ieee80211_mgmt *mgmt = (void *) skb->data;
	__le16 fc = mgmt->frame_control;
	u8 *start, *end, *pos;

	if (ieee80211_is_assoc_req(fc))
		start = (u8 *) mgmt->u.assoc_req.variable;
	else if (ieee80211_is_reassoc_req(fc))
		start = (u8 *) mgmt->u.reassoc_req.variable;
	else if (ieee80211_is_assoc_resp(fc) || ieee80211_is_reassoc_resp(fc)){
		start = (u8 *)mgmt->u.assoc_resp.variable;
#if defined(INCLUDE_NEW_CHANNEL_CTX)
		/*
		* S1G band does not use AID field in the assoc response
		*/
		start-=2;
#endif /*  defined(INCLUDE_NEW_CHANNEL_CTX) */
	}
	else
		return NULL;

	end = skb->data + skb->len;

	pos = (u8 *) cfg80211_find_ie(WLAN_EID_BSS_MAX_IDLE_PERIOD,
				      start, end - start);
	if (!pos || pos[1] != 3)
		return NULL;

	return pos + 2;
}

static int ieee80211_disconnect_sta(struct ieee80211_vif *vif,
				    struct ieee80211_sta *sta)
{
	struct nrc_sta *i_sta = to_i_sta(sta);
	struct ieee80211_hw *hw = i_sta->nw->hw;
	struct sk_buff *skb;
	struct ieee80211_tx_info *txi;
#ifdef CONFIG_SUPPORT_TX_CONTROL
	struct ieee80211_tx_control control = { .sta = sta, };
#endif

	/* Send a deauth to @sta */
	skb = ieee80211_deauth_get(hw, sta->addr, vif->addr, vif->addr,
				   WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY, sta, true);
	if (!skb)
		return -1;

	skb_set_queue_mapping(skb, IEEE80211_AC_VO);

	txi = IEEE80211_SKB_CB(skb);
	txi->control.vif = vif;

#ifdef CONFIG_SUPPORT_NEW_MAC_TX
	nrc_mac_tx(hw, &control, skb);
#else
	nrc_mac_tx(hw, skb);
#endif

	/* Pretend to receive a deauth from @sta */
	skb = ieee80211_deauth_get(hw, vif->addr, sta->addr, vif->addr,
				   WLAN_REASON_DEAUTH_LEAVING, sta, false);
	if (!skb)
		return -1;

	ieee80211_rx_irqsafe(hw, skb);


	return 0;
}
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
static void ap_max_idle_period_expire(unsigned long data)
{
	struct nrc_vif *i_vif = (struct nrc_vif *) data;
#else
static void ap_max_idle_period_expire(struct timer_list *t)
{
	struct nrc_vif *i_vif = from_timer(i_vif, t, max_idle_timer);
#endif
	struct nrc_sta *i_sta = NULL, *tmp = NULL;
	unsigned long flags;
	u_int16_t sta_num = 0;

	spin_lock_irqsave(&i_vif->preassoc_sta_lock, flags);
	list_for_each_entry_safe(i_sta, tmp, &i_vif->preassoc_sta_list, list) {
		sta_num++;
		if(i_sta->max_idle.sta_idle_timer){
			if(--i_sta->max_idle.sta_idle_timer == 0){
				struct ieee80211_sta *sta = to_ieee80211_sta(i_sta);
				struct ieee80211_vif *vif = i_sta->vif;
				/* TO DO - BSS_MAX_ILDE_DEAUTH_LIMIT_COUNT -> 0 */
				if (++i_sta->max_idle.timeout_cnt >= BSS_MAX_ILDE_DEAUTH_LIMIT_COUNT) {
					/* Inactivity (BSS MAX IDLE) timeout =>  disconnect the station */
					i_sta->max_idle.timeout_cnt = 0;
					nrc_mac_dbg("[AP] keep-alive fail! Disconnecting inactive sta:%pM",sta->addr);
					ieee80211_disconnect_sta(vif, sta);
				} else {
					/* Re-arm the timer
						: apply backoff for avoiding frequent deauth */
					i_sta->max_idle.sta_idle_timer = i_sta->max_idle.idle_period;
					nrc_mac_dbg("[AP] keep-alive timeout!(cnt:%d vs limit:%d) Rearm timer(%u) STA(%pM)",
						i_sta->max_idle.timeout_cnt , BSS_MAX_ILDE_DEAUTH_LIMIT_COUNT, i_sta->max_idle.sta_idle_timer, sta->addr);
				}
			}
		}
	}
	spin_unlock_irqrestore(&i_vif->preassoc_sta_lock, flags);

	mod_timer(&i_vif->max_idle_timer, jiffies + msecs_to_jiffies(BSS_MAX_IDLE_TIMER_PERIOD_MS));
}

void ap_max_idle_timer_start (struct nrc *nw, struct nrc_vif *i_vif)
{
	if(!timer_pending(&i_vif->max_idle_timer)){
		dev_info(nw->dev, "vif(%d) Start AP bss_max_idle timer", i_vif->index);
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
		setup_timer(&i_vif->max_idle_timer, ap_max_idle_period_expire, (unsigned long) i_vif);
#else
		timer_setup(&i_vif->max_idle_timer, ap_max_idle_period_expire, 0);
#endif
		mod_timer(&i_vif->max_idle_timer, jiffies + msecs_to_jiffies(BSS_MAX_IDLE_TIMER_PERIOD_MS));
	}
}

void ap_max_idle_timer_stop (struct nrc *nw, struct nrc_vif *i_vif)
{
	if(timer_pending(&i_vif->max_idle_timer)){
		dev_info(nw->dev, "vif(%d) Stop AP bss_max_idle timer", i_vif->index);
		del_timer_sync(&i_vif->max_idle_timer);
	}
}

#define IEEE80211_STYPE_QOS_NULL       0x00C0
struct ieee80211_hdr_3addr_qos {
	u16 frame_control;
	u16 duration_id;
	u8 addr1[ETH_ALEN];
	u8 addr2[ETH_ALEN];
	u8 addr3[ETH_ALEN];
	u16 seq_ctl;
	u16 qc;
};

#if KERNEL_VERSION(4, 15, 0) > NRC_TARGET_KERNEL_VERSION
static void sta_max_idle_period_expire(unsigned long data)
{
	struct nrc_vif *i_vif = (struct nrc_vif *) data;
#else
static void sta_max_idle_period_expire(struct timer_list *t)
{
	struct nrc_vif *i_vif = from_timer(i_vif, t, max_idle_timer);
#endif
	struct ieee80211_hw *hw = i_vif->nw->hw;
	struct nrc_sta *i_sta = NULL, *tmp = NULL, *tmp_sta = NULL;
	unsigned long flags;
#ifdef CONFIG_SUPPORT_TX_CONTROL
	struct ieee80211_tx_control control;
#endif
#ifdef CONFIG_SUPPORT_CHANNEL_INFO
	struct ieee80211_chanctx_conf *chanctx_conf;
#else
	struct ieee80211_conf *chanctx_conf;
#endif
	struct sk_buff *skb;
	int band;
	struct ieee80211_hdr_3addr_qos *qosnullfunc;

	spin_lock_irqsave(&i_vif->preassoc_sta_lock, flags);
	list_for_each_entry_safe(tmp_sta, tmp, &i_vif->preassoc_sta_list, list) {
		i_sta = tmp_sta;
		break;
	}
	spin_unlock_irqrestore(&i_vif->preassoc_sta_lock, flags);

	if(!i_sta){
		nrc_mac_dbg("Fail to sending a keep-alive. Fail to find nrc_sta");
		return;
	}
	control.sta = to_ieee80211_sta(i_sta);

#ifdef CONFIG_QOS_NULL_OFFLOAD /* QoS Null For keep alive is sent within uCode */
	if (power_save == NRC_PS_DEEPSLEEP_TIM && i_sta->nw->drv_state == NRC_DRV_PS) {
		nrc_mac_dbg("%s: skipping a keep-alive (QoS Null Frame)", __func__);
		goto skip_qos_null;
	}
#endif

	nrc_mac_dbg("%s: sending a keep-alive (QoS Null Frame)", __func__);
	/* Send a Null frame as a keep alive frame */
#if KERNEL_VERSION(4, 14, 17) <= NRC_TARGET_KERNEL_VERSION
#if KERNEL_VERSION(6, 1, 0) <= NRC_TARGET_KERNEL_VERSION
	skb= ieee80211_nullfunc_get(hw, i_sta->vif, i_sta->vif->bss_conf.link_id, false);
#else
	skb = ieee80211_nullfunc_get(hw, i_sta->vif, false);
#endif
#else
	skb = ieee80211_nullfunc_get(hw, i_sta->vif);
#endif
	skb_put(skb, 2);
	qosnullfunc = (struct ieee80211_hdr_3addr_qos *) skb->data;
	qosnullfunc->frame_control |= cpu_to_le16(IEEE80211_STYPE_QOS_NULL);
	qosnullfunc->qc = cpu_to_le16(7);
	skb_set_queue_mapping(skb, IEEE80211_AC_VO);

#ifdef CONFIG_SUPPORT_CHANNEL_INFO
#ifdef CONFIG_USE_BSS_CHAN_CONF
	chanctx_conf = rcu_dereference(i_sta->vif->bss_conf.chanctx_conf);
#else
	chanctx_conf = rcu_dereference(i_sta->vif->chanctx_conf);
#endif /* ifdef CONFIG_USE_BSS_CHAN_CONF */
	if (!chanctx_conf)
		goto drop;

	band = chanctx_conf->def.chan->band;
	if (!ieee80211_tx_prepare_skb(hw, i_sta->vif, skb, band, NULL))
		goto done;
#else
	chanctx_conf = &hw->conf;
	if (!chanctx_conf)
		goto drop;

	band = chanctx_conf->channel->band;
#endif

#ifdef CONFIG_SUPPORT_NEW_MAC_TX
	nrc_mac_tx(hw, &control, skb);
#else
	nrc_mac_tx(hw, skb);
#endif

done:
#ifdef CONFIG_QOS_NULL_OFFLOAD
skip_qos_null:
#endif
	/* Re-arm the timer */
	mod_timer(&i_vif->max_idle_timer, jiffies + i_sta->max_idle.idle_period);

	return;
 drop:
	dev_kfree_skb_any(skb);
}


/**
 * sta_h_bss_max_idle_period() - enable/disable BSS idle period
 *
 * @sta: the station of which activity to monitor
 * @enable: true to start, false to stop.
 *
 */
static int sta_h_bss_max_idle_period(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_sta *sta,
				     enum ieee80211_sta_state old_state,
				     enum ieee80211_sta_state new_state)
{
	struct nrc_vif *i_vif = NULL;
	struct nrc_sta *i_sta = NULL;
	u64 timeout_ms = 0; 
	u32 max_idle_period = 0;
	int idle_offset = 0;

	if (sta == NULL) {
#if defined(CONFIG_SUPPORT_BEACON_BYPASS)
		if(!enable_beacon_bypass)
#endif /* CONFIG_SUPPORT_BEACON_BYPASS */
			nrc_mac_dbg("%s rx->sta is NULL",__func__);
		return 0;
	} else {
		i_sta = to_i_sta(sta);
	}

	if (vif == NULL) {
		nrc_mac_dbg("%s vif is NULL",__func__);
		return 0;
	} else {
		i_vif = to_i_vif(vif);
	}

	if (vif->type != NL80211_IFTYPE_AP &&
#if defined (CONFIG_SUPPORT_IBSS)
		vif->type != NL80211_IFTYPE_ADHOC &&
#endif
		vif->type != NL80211_IFTYPE_STATION) {
		nrc_mac_dbg("%s STA_TYPE(%d) is not neither AP or STA",__func__, vif->type);
		return 0;
    }

#define state_changed(old, new)	\
(old_state == IEEE80211_STA_##old && new_state == IEEE80211_STA_##new)

	if (state_changed(ASSOC, AUTH)) {
		if(vif->type == NL80211_IFTYPE_STATION &&
		  i_sta->max_idle.period > 0) {
			nrc_mac_dbg("STA(%pM) deauth. Delete bss_max_idle timer(%u)",
				sta->addr,i_sta->max_idle.idle_period);
			del_timer_sync(&i_vif->max_idle_timer);
			i_sta->max_idle.idle_period = 0;
		}
		return 0;
	} else if (!state_changed(ASSOC, AUTHORIZED)) {
		return 0;
	}

	/* old_state == ASSOC && new_state == ATHORIZED */

	max_idle_period = i_sta->max_idle.period;

	if (vif->type == NL80211_IFTYPE_STATION) {
		struct nrc *nw = i_vif->nw;
		if (nw->twt_sched) {
			nrc_mac_dbg("%s: TWT takes over keep-alive from driver", __func__);
			return 0;
		}
	}

	if (max_idle_period == 0) {
		nrc_mac_dbg("[%s] max_idle_period is 0",__func__);
		return 0;
	}

	/* Extended BSS Max Idle Period */
	if (nrc_mac_is_s1g(hw->priv)) {
		u8 usf = (max_idle_period >> 14) & 0x3;
		max_idle_period &= ~0xc000;
		max_idle_period *= ieee80211_usf_to_sf(usf);
		nrc_mac_dbg("%s: origin(16bit):0x%x [unscaled interval(14bit):%u, usf(2bit):%d] => total(%u x 1024 ms)",
				__func__, i_sta->max_idle.period, (i_sta->max_idle.period & ~0xc000), usf, max_idle_period);
	} else {
		nrc_mac_dbg("%s: max_idle_period=%d ms", __func__,
				max_idle_period * 1024);
	}

	if (bss_max_idle_offset < 0 &&
		max_idle_period * 1024 <= (-1*bss_max_idle_offset)) {
		nrc_mac_dbg("%s: invalid max_idle_period_offset(%d ms)",
			__func__, bss_max_idle_offset);
		bss_max_idle_offset = 0;
	}
  
	/* Start STA inactivity monitoring */
	if (vif->type == NL80211_IFTYPE_STATION) {
		/* STA should send keep-alive frame before bss max idle timer on AP expires */
		if (bss_max_idle_offset == 0) {
		    if (max_idle_period > 2)
			idle_offset = -768; //768ms
		    else
			idle_offset = -100; //100ms (if max idle period <= 2)
		} else {
			idle_offset = bss_max_idle_offset;
		}
	} else {
		/* AP: offset is only used for add margin (only plus) */
		if (bss_max_idle_offset > 0)
			idle_offset = bss_max_idle_offset;
	}

	timeout_ms = (u64)max_idle_period * 1024 + (u64)idle_offset;
	if (timeout_ms < 924) {
		/* min = bss_max_idle - margin (100ms) = 924ms for safty
		(BSS_MAX_IDLE MIN = 1 * 1000TU =1024ms) */
		nrc_mac_dbg("bss max idle is set as min (924ms)");
		timeout_ms = 924;
	}

	if (vif->type == NL80211_IFTYPE_STATION) {
		nrc_mac_dbg("%s: vif(%d) Start STA bss_max_idle timer", __func__, i_vif->index);
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
		setup_timer(&i_vif->max_idle_timer, sta_max_idle_period_expire,
			    (unsigned long) i_vif);
#else
		timer_setup(&i_vif->max_idle_timer, sta_max_idle_period_expire,
				0);
#endif

		/* Save jiffies (msecs_to_jiffies(max_idle_period * 1024ms(1000TU) + idle_offset) */
		if(timeout_ms > __UINT32_MAX__)
			/* msecs_to_jiffies param max value is uint32 -> max send keep alive period is 124 days(max jiffies).*/
			timeout_ms = __UINT32_MAX__;
		
		i_sta->max_idle.idle_period = msecs_to_jiffies(timeout_ms);
		mod_timer(&i_vif->max_idle_timer, jiffies + i_sta->max_idle.idle_period);
	}

	nrc_mac_dbg("%s: associated[%pM], timer (%lu %s)",
		__func__, sta->addr, i_sta->max_idle.idle_period,
		vif->type == NL80211_IFTYPE_STATION ? "jiffies" : "sec");

	return 0;
}
STAH(sta_h_bss_max_idle_period);


static int tx_h_bss_max_idle_period(struct nrc_trx_data *tx)
{
	struct nrc_vif *i_vif = NULL;
	struct nrc_sta *i_sta = NULL;
	struct bss_max_idle_period_ie *ie;
	struct ieee80211_hdr *mh = (void *) tx->skb->data;
	__le16 fc = mh->frame_control;

		/* Handle TX (Re)Assoc REQ (STA) and TX (Re)Assoc RESP (AP) */
	if (!ieee80211_is_assoc_resp(fc) && !ieee80211_is_reassoc_resp(fc) &&
	    !ieee80211_is_assoc_req(fc) && !ieee80211_is_reassoc_req(fc))
		return 0;

	if (tx->vif == NULL) {
		nrc_mac_dbg("%s tx->vif is NULL",__func__);
		return 0;
	} else {
		i_vif = to_i_vif(tx->vif);
	}

	if (tx->sta) {
		i_sta = to_i_sta(tx->sta);
		if (!i_sta) {
			nrc_mac_dbg("%s i_sta is NULL",__func__);
			return 0;
		}
	}else {
		nrc_mac_dbg("%s sta is NULL",__func__);
		return 0;
	}

#if 0 //Remove not-reached codes (tx->sta is NULL). TBD: need to move codes in right place
	/* (STA) Update bss_max_idle timer whenever sending data frame */
	if (tx->vif->type == NL80211_IFTYPE_STATION && i_sta->state == IEEE80211_STA_AUTHORIZED &&
		i_sta->max_idle.period > 0 && ieee80211_is_data(fc)) {
		unsigned long timeout;
		if (ieee80211_is_qos_nullfunc(fc)) {
			/* skip QoS Null data sending for keep alive */
			return 0;
		}
		timeout = jiffies + i_sta->max_idle.idle_period;
		nrc_mac_dbg("%s: (STA) TX Data. Update bss_max_idle timer (%lu)", __func__,
			i_sta->max_idle.idle_period);
		if (timer_pending(&i_sta->max_idle.timer) &&
		    time_after(timeout, i_sta->max_idle.timer.expires)) {
			mod_timer(&i_sta->max_idle.timer, timeout);
		}
		return 0;
	}
#endif

	ie = (void *) find_bss_max_idle_ie(tx->skb);

	if (ie) {
		/*
		 * The hostapd added EID_BSS_MAX_IDLE_PERIOD in (re)assoc resp.
		 * Conditions are:
		 * - hostapd is built with CONFIG_WNM
		 * - hostapd.conf includes non-zero ap_max_inactivity setting
		 * - hw->wiphy.feature & NL80211_FEATURE_INACTIVITY_TIMER  = 0
		 *
		 * In this case, we disable inactivity monitoring, and let the
		 * hostapd take care of everything.
		 */
		nrc_mac_dbg("%s: BSS_MAX_IDLE_PERIOD IE exists but hostapd will handle the value",
			    __func__);
		i_vif->max_idle_period = 0;
		if (i_sta) {
			i_sta->max_idle.idle_period = 0;
			i_sta->max_idle.period = 0;
		}

		goto out;
	}

	if(tx->vif->type == NL80211_IFTYPE_STATION ||
	   (tx->vif->type == NL80211_IFTYPE_AP && i_vif->max_idle_period == 0))
		return 0;

	/* Add Extended BSS Max Idle Period IE */
	ie = (void *) ieee80211_append_ie(tx->skb,
					  WLAN_EID_BSS_MAX_IDLE_PERIOD,
					  3);
	if (!ie) {
		nrc_mac_dbg("%s: failed to add BSS_MAX_IDLE_PERIOD IE",
			    __func__);
		if (i_sta) i_sta->max_idle.period = 0;
		goto out;
	}

	ie->max_idle_period = i_sta->max_idle.period;
	ie->idle_option = i_sta->max_idle.options;

 out:
	nrc_mac_dbg("%s: %s, max_idle_period(16bit)=%u", __func__,
		    ieee80211_is_assoc_req(fc) ? "AssocReq" :
		    ieee80211_is_reassoc_req(fc) ? "ReAssocReq" :
		    ieee80211_is_assoc_resp(fc) ? "AssocResp" : "ReAssocResp",
		    ie->max_idle_period);

	return 0;
}
TXH(tx_h_bss_max_idle_period, NL80211_IFTYPE_ALL);

/**
 * rx_h_bss_max_idle_period() - bss max idle period rx handler
 *
 * This function handles (Re)Assoc. Req. and (Re)Assoc. Resp
 * frames depending on vif type.
 *
 */

#define EXTRA_TIMEOUT		1		/* 1 sec for delaying timeout */
static int rx_h_bss_max_idle_period(struct nrc_trx_data *rx)
{
	struct ieee80211_hdr *mh = (void *) rx->skb->data;
	__le16 fc = mh->frame_control;
	struct nrc_sta *i_sta = NULL;
	struct nrc_vif *i_vif = NULL;
	struct bss_max_idle_period_ie *ie;

	if (rx->sta == NULL) {
#if defined(CONFIG_SUPPORT_BEACON_BYPASS)
		if(!enable_beacon_bypass)
#endif /* CONFIG_SUPPORT_BEACON_BYPASS */
			nrc_mac_dbg("%s rx->sta is NULL",__func__);
		return 0;
	} else {
		i_sta = to_i_sta(rx->sta);
	}

	if (rx->vif == NULL) {
		nrc_mac_dbg("%s rx->vif is NULL",__func__);
		return 0;
	} else {
		i_vif = to_i_vif(rx->vif);
	}

	/* AP updates bss_max_idle timer whenever receiving qos null data from STA*/
	if (rx->vif->type == NL80211_IFTYPE_AP &&
		i_sta->max_idle.period > 0 && ieee80211_is_data(fc)) {
		if(i_sta->max_idle.sta_idle_timer > 0){
			i_sta->max_idle.timeout_cnt = 0;
			i_sta->max_idle.sta_idle_timer = i_sta->max_idle.idle_period + EXTRA_TIMEOUT;
		}
		return 0;
	}

	/* Handle RX (Re)Assoc RESP (STA) and RX (Re)Assoc REQ (AP) */
	if (!ieee80211_is_assoc_resp(fc) && !ieee80211_is_reassoc_resp(fc) &&
	    !ieee80211_is_assoc_req(fc) && !ieee80211_is_reassoc_req(fc))
		return 0;

	if (WARN_ON(!ether_addr_equal(ieee80211_get_DA(mh), rx->vif->addr)))
		return 0; /* @rx->skb not destined to @rx->vif */

	/* Find BSS Max Idle Element and Save fields in i_sta->max_idle.period and options */
	ie = (struct bss_max_idle_period_ie *) find_bss_max_idle_ie(rx->skb);
	if (ie) {
		if (rx->vif->type == NL80211_IFTYPE_AP) {
			nrc_mac_dbg("%s: IE exist, period from vif %lu", __FUNCTION__, i_vif->max_idle_period);
		} else {
			if (ie->max_idle_period != 0) {
				i_sta->max_idle.period = ie->max_idle_period;
				i_sta->max_idle.options = ie->idle_option;
			} else {
				/* An case that can never happen */
				nrc_mac_dbg("%s: max_idle_period is zero in IE, no set!!!", __FUNCTION__);
			}
		}
	} else {
		if (rx->vif->type == NL80211_IFTYPE_AP) {
			nrc_mac_dbg("%s: IE not exist, period from vif %lu", __FUNCTION__, i_vif->max_idle_period);
		} else {
			i_sta->max_idle.period = 0;
			i_sta->max_idle.options = 0;
		}
	}

	nrc_mac_dbg("%s: %s, bss_max_idle_period(16bit)=%u", __func__,
		    ieee80211_is_assoc_req(fc) ? "AssocReq" :
		    ieee80211_is_reassoc_req(fc) ? "ReAssocReq" :
		    ieee80211_is_assoc_resp(fc) ? "AssocResp" : "ReAssocResp",
		    i_sta->max_idle.period);

	return 0;
}
RXH(rx_h_bss_max_idle_period, NL80211_IFTYPE_ALL);
