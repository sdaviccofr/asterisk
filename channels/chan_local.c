/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \brief Local Proxy Channel
 * 
 * \ingroup channel_drivers
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <fcntl.h>
#include <sys/signal.h>

#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/sched.h"
#include "asterisk/io.h"
#include "asterisk/acl.h"
#include "asterisk/callerid.h"
#include "asterisk/file.h"
#include "asterisk/cli.h"
#include "asterisk/app.h"
#include "asterisk/musiconhold.h"
#include "asterisk/manager.h"
#include "asterisk/stringfields.h"
#include "asterisk/devicestate.h"

/*** DOCUMENTATION
	<manager name="LocalOptimizeAway" language="en_US">
		<synopsis>
			Optimize away a local channel when possible.
		</synopsis>
		<syntax>
			<xi:include xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])" />
			<parameter name="Channel" required="true">
				<para>The channel name to optimize away.</para>
			</parameter>
		</syntax>
		<description>
			<para>A local channel created with "/n" will not automatically optimize away.
			Calling this command on the local channel will clear that flag and allow
			it to optimize away if it's bridged or when it becomes bridged.</para>
		</description>
	</manager>
 ***/

static const char tdesc[] = "Local Proxy Channel Driver";

#define IS_OUTBOUND(a,b) (a == b->chan ? 1 : 0)

static struct ast_jb_conf g_jb_conf = {
	.flags = 0,
	.max_size = -1,
	.resync_threshold = -1,
	.impl = "",
	.target_extra = -1,
};

static struct ast_channel *local_request(const char *type, format_t format, const struct ast_channel *requestor, void *data, int *cause);
static int local_digit_begin(struct ast_channel *ast, char digit);
static int local_digit_end(struct ast_channel *ast, char digit, unsigned int duration);
static int local_call(struct ast_channel *ast, char *dest, int timeout);
static int local_hangup(struct ast_channel *ast);
static int local_answer(struct ast_channel *ast);
static struct ast_frame *local_read(struct ast_channel *ast);
static int local_write(struct ast_channel *ast, struct ast_frame *f);
static int local_indicate(struct ast_channel *ast, int condition, const void *data, size_t datalen);
static int local_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);
static int local_sendhtml(struct ast_channel *ast, int subclass, const char *data, int datalen);
static int local_sendtext(struct ast_channel *ast, const char *text);
static int local_devicestate(void *data);
static struct ast_channel *local_bridgedchannel(struct ast_channel *chan, struct ast_channel *bridge);
static int local_queryoption(struct ast_channel *ast, int option, void *data, int *datalen);

/* PBX interface structure for channel registration */
static const struct ast_channel_tech local_tech = {
	.type = "Local",
	.description = tdesc,
	.capabilities = -1,
	.requester = local_request,
	.send_digit_begin = local_digit_begin,
	.send_digit_end = local_digit_end,
	.call = local_call,
	.hangup = local_hangup,
	.answer = local_answer,
	.read = local_read,
	.write = local_write,
	.write_video = local_write,
	.exception = local_read,
	.indicate = local_indicate,
	.fixup = local_fixup,
	.send_html = local_sendhtml,
	.send_text = local_sendtext,
	.devicestate = local_devicestate,
	.bridged_channel = local_bridgedchannel,
	.queryoption = local_queryoption,
};

/*! \brief the local pvt structure for all channels

	The local channel pvt has two ast_chan objects - the "owner" and the "next channel", the outbound channel

	ast_chan owner -> local_pvt -> ast_chan chan -> yet-another-pvt-depending-on-channel-type

*/
struct local_pvt {
	ast_mutex_t lock;			/*!< Channel private lock */
	unsigned int flags;                     /*!< Private flags */
	char context[AST_MAX_CONTEXT];		/*!< Context to call */
	char exten[AST_MAX_EXTENSION];		/*!< Extension to call */
	int reqformat;				/*!< Requested format */
	struct ast_jb_conf jb_conf;		/*!< jitterbuffer configuration for this local channel */
	struct ast_channel *owner;		/*!< Master Channel - Bridging happens here */
	struct ast_channel *chan;		/*!< Outbound channel - PBX is run here */
	struct ast_module_user *u_owner;	/*!< reference to keep the module loaded while in use */
	struct ast_module_user *u_chan;		/*!< reference to keep the module loaded while in use */
	AST_LIST_ENTRY(local_pvt) list;		/*!< Next entity */
};

#define LOCAL_GLARE_DETECT    (1 << 0) /*!< Detect glare on hangup */
#define LOCAL_CANCEL_QUEUE    (1 << 1) /*!< Cancel queue */
#define LOCAL_ALREADY_MASQED  (1 << 2) /*!< Already masqueraded */
#define LOCAL_LAUNCHED_PBX    (1 << 3) /*!< PBX was launched */
#define LOCAL_NO_OPTIMIZATION (1 << 4) /*!< Do not optimize using masquerading */
#define LOCAL_BRIDGE          (1 << 5) /*!< Report back the "true" channel as being bridged to */
#define LOCAL_MOH_PASSTHRU    (1 << 6) /*!< Pass through music on hold start/stop frames */

static AST_LIST_HEAD_STATIC(locals, local_pvt);

/*! \brief Adds devicestate to local channels */
static int local_devicestate(void *data)
{
	char *exten = ast_strdupa(data);
	char *context = NULL, *opts = NULL;
	int res;
	struct local_pvt *lp;

	if (!(context = strchr(exten, '@'))) {
		ast_log(LOG_WARNING, "Someone used Local/%s somewhere without a @context. This is bad.\n", exten);
		return AST_DEVICE_INVALID;	
	}

	*context++ = '\0';

	/* Strip options if they exist */
	if ((opts = strchr(context, '/')))
		*opts = '\0';

	ast_debug(3, "Checking if extension %s@%s exists (devicestate)\n", exten, context);

	res = ast_exists_extension(NULL, context, exten, 1, NULL);
	if (!res)		
		return AST_DEVICE_INVALID;
	
	res = AST_DEVICE_NOT_INUSE;
	AST_LIST_LOCK(&locals);
	AST_LIST_TRAVERSE(&locals, lp, list) {
		if (!strcmp(exten, lp->exten) && !strcmp(context, lp->context) && lp->owner) {
			res = AST_DEVICE_INUSE;
			break;
		}
	}
	AST_LIST_UNLOCK(&locals);

	return res;
}

/*!
 * \note Assumes the pvt is no longer in the pvts list
 */
static struct local_pvt *local_pvt_destroy(struct local_pvt *pvt)
{
	ast_mutex_destroy(&pvt->lock);
	ast_free(pvt);
	return NULL;
}

/*! \brief Return the bridged channel of a Local channel */
static struct ast_channel *local_bridgedchannel(struct ast_channel *chan, struct ast_channel *bridge)
{
	struct local_pvt *p = bridge->tech_pvt;
	struct ast_channel *bridged = bridge;

	if (!p) {
		ast_debug(1, "Asked for bridged channel on '%s'/'%s', returning <none>\n",
			chan->name, bridge->name);
		return NULL;
	}

	ast_mutex_lock(&p->lock);

	if (ast_test_flag(p, LOCAL_BRIDGE)) {
		/* Find the opposite channel */
		bridged = (bridge == p->owner ? p->chan : p->owner);
		
		/* Now see if the opposite channel is bridged to anything */
		if (!bridged) {
			bridged = bridge;
		} else if (bridged->_bridge) {
			bridged = bridged->_bridge;
		}
	}

	ast_mutex_unlock(&p->lock);

	return bridged;
}

static int local_queryoption(struct ast_channel *ast, int option, void *data, int *datalen)
{
	struct local_pvt *p = ast->tech_pvt;
	struct ast_channel *chan, *bridged;
	int res;

	if (!p) {
		return -1;
	}

	if (option != AST_OPTION_T38_STATE) {
		/* AST_OPTION_T38_STATE is the only supported option at this time */
		return -1;
	}

	ast_mutex_lock(&p->lock);
	chan = IS_OUTBOUND(ast, p) ? p->owner : p->chan;

try_again:
	if (!chan) {
		ast_mutex_unlock(&p->lock);
		return -1;
	}

	if (ast_channel_trylock(chan)) {
		DEADLOCK_AVOIDANCE(&p->lock);
		chan = IS_OUTBOUND(ast, p) ? p->owner : p->chan;
		goto try_again;
	}

	bridged = ast_bridged_channel(chan);
	if (!bridged) {
		/* can't query channel unless we are bridged */
		ast_mutex_unlock(&p->lock);
		ast_channel_unlock(chan);
		return -1;
	}

	if (ast_channel_trylock(bridged)) {
		ast_channel_unlock(chan);
		DEADLOCK_AVOIDANCE(&p->lock);
		chan = IS_OUTBOUND(ast, p) ? p->owner : p->chan;
		goto try_again;
	}

	res = ast_channel_queryoption(bridged, option, data, datalen, 0);
	ast_mutex_unlock(&p->lock);
	ast_channel_unlock(chan);
	ast_channel_unlock(bridged);
	return res;
}

static int local_queue_frame(struct local_pvt *p, int isoutbound, struct ast_frame *f, 
	struct ast_channel *us, int us_locked)
{
	struct ast_channel *other = NULL;

	/* Recalculate outbound channel */
	other = isoutbound ? p->owner : p->chan;

	if (!other) {
		return 0;
	}

	/* do not queue frame if generator is on both local channels */
	if (us && us->generator && other->generator) {
		return 0;
	}

	/* Set glare detection */
	ast_set_flag(p, LOCAL_GLARE_DETECT);

	/* Ensure that we have both channels locked */
	while (other && ast_channel_trylock(other)) {
		int res;
		if ((res = ast_mutex_unlock(&p->lock))) {
			ast_log(LOG_ERROR, "chan_local bug! '&p->lock' was not locked when entering local_queue_frame! (%s)\n", strerror(res));
			return -1;
		}
		if (us && us_locked) {
			do {
				CHANNEL_DEADLOCK_AVOIDANCE(us);
			} while (ast_mutex_trylock(&p->lock));
		} else {
			usleep(1);
			ast_mutex_lock(&p->lock);
		}
		other = isoutbound ? p->owner : p->chan;
	}

	/* Since glare detection only occurs within this function, and because
	 * a pvt flag cannot be set without having the pvt lock, this is the only
	 * location where we could detect a cancelling of the queue. */
	if (ast_test_flag(p, LOCAL_CANCEL_QUEUE)) {
		/* We had a glare on the hangup.  Forget all this business,
		return and destroy p.  */
		ast_mutex_unlock(&p->lock);
		p = local_pvt_destroy(p);
		if (other) {
			ast_channel_unlock(other);
		}
		return -1;
	}

	if (other) {
		if (f->frametype == AST_FRAME_CONTROL && f->subclass.integer == AST_CONTROL_RINGING) {
			ast_setstate(other, AST_STATE_RINGING);
		}
		ast_queue_frame(other, f);
		ast_channel_unlock(other);
	}

	ast_clear_flag(p, LOCAL_GLARE_DETECT);

	return 0;
}

static int local_answer(struct ast_channel *ast)
{
	struct local_pvt *p = ast->tech_pvt;
	int isoutbound;
	int res = -1;

	if (!p)
		return -1;

	ast_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	if (isoutbound) {
		/* Pass along answer since somebody answered us */
		struct ast_frame answer = { AST_FRAME_CONTROL, { AST_CONTROL_ANSWER } };
		res = local_queue_frame(p, isoutbound, &answer, ast, 1);
	} else
		ast_log(LOG_WARNING, "Huh?  Local is being asked to answer?\n");
	if (!res)
		ast_mutex_unlock(&p->lock);
	return res;
}

/*!
 * \internal
 * \note This function assumes that we're only called from the "outbound" local channel side
 */
static void check_bridge(struct local_pvt *p)
{
	struct ast_channel_monitor *tmp;
	if (ast_test_flag(p, LOCAL_ALREADY_MASQED) || ast_test_flag(p, LOCAL_NO_OPTIMIZATION) || !p->chan || !p->owner || (p->chan->_bridge != ast_bridged_channel(p->chan)))
		return;

	/* only do the masquerade if we are being called on the outbound channel,
	   if it has been bridged to another channel and if there are no pending
	   frames on the owner channel (because they would be transferred to the
	   outbound channel during the masquerade)
	*/
	if (p->chan->_bridge /* Not ast_bridged_channel!  Only go one step! */ && AST_LIST_EMPTY(&p->owner->readq)) {
		/* Masquerade bridged channel into owner */
		/* Lock everything we need, one by one, and give up if
		   we can't get everything.  Remember, we'll get another
		   chance in just a little bit */
		if (!ast_channel_trylock(p->chan->_bridge)) {
			if (!ast_check_hangup(p->chan->_bridge)) {
				if (!ast_channel_trylock(p->owner)) {
					if (!ast_check_hangup(p->owner)) {
						if (p->owner->monitor && !p->chan->_bridge->monitor) {
							/* If a local channel is being monitored, we don't want a masquerade
							 * to cause the monitor to go away. Since the masquerade swaps the monitors,
							 * pre-swapping the monitors before the masquerade will ensure that the monitor
							 * ends up where it is expected.
							 */
							tmp = p->owner->monitor;
							p->owner->monitor = p->chan->_bridge->monitor;
							p->chan->_bridge->monitor = tmp;
						}
						if (p->chan->audiohooks) {
							struct ast_audiohook_list *audiohooks_swapper;
							audiohooks_swapper = p->chan->audiohooks;
							p->chan->audiohooks = p->owner->audiohooks;
							p->owner->audiohooks = audiohooks_swapper;
						}

						/* If any Caller ID was set, preserve it after masquerade like above. We must check
						 * to see if Caller ID was set because otherwise we'll mistakingly copy info not
						 * set from the dialplan and will overwrite the real channel Caller ID. The reason
						 * for this whole preswapping action is because the Caller ID is set on the channel
						 * thread (which is the to be masqueraded away local channel) before both local
						 * channels are optimized away.
						 */
						if (p->owner->caller.id.name.valid || p->owner->caller.id.number.valid
							|| p->owner->caller.id.subaddress.valid || p->owner->caller.ani.name.valid
							|| p->owner->caller.ani.number.valid || p->owner->caller.ani.subaddress.valid) {
							struct ast_party_caller tmp;
							tmp = p->owner->caller;
							p->owner->caller = p->chan->_bridge->caller;
							p->chan->_bridge->caller = tmp;
						}
						if (p->owner->redirecting.from.name.valid || p->owner->redirecting.from.number.valid
							|| p->owner->redirecting.from.subaddress.valid || p->owner->redirecting.to.name.valid
							|| p->owner->redirecting.to.number.valid || p->owner->redirecting.to.subaddress.valid) {
							struct ast_party_redirecting tmp;
							tmp = p->owner->redirecting;
							p->owner->redirecting = p->chan->_bridge->redirecting;
							p->chan->_bridge->redirecting = tmp;
						}
						if (p->owner->dialed.number.str || p->owner->dialed.subaddress.valid) {
							struct ast_party_dialed tmp;
							tmp = p->owner->dialed;
							p->owner->dialed = p->chan->_bridge->dialed;
							p->chan->_bridge->dialed = tmp;
						}


						ast_app_group_update(p->chan, p->owner);
						ast_channel_masquerade(p->owner, p->chan->_bridge);
						ast_set_flag(p, LOCAL_ALREADY_MASQED);
					}
					ast_channel_unlock(p->owner);
				}
				ast_channel_unlock(p->chan->_bridge);
			}
		}
	}
}

static struct ast_frame  *local_read(struct ast_channel *ast)
{
	return &ast_null_frame;
}

static int local_write(struct ast_channel *ast, struct ast_frame *f)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	int isoutbound;

	if (!p)
		return -1;

	/* Just queue for delivery to the other side */
	ast_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	if (isoutbound && f && (f->frametype == AST_FRAME_VOICE || f->frametype == AST_FRAME_VIDEO))
		check_bridge(p);
	if (!ast_test_flag(p, LOCAL_ALREADY_MASQED))
		res = local_queue_frame(p, isoutbound, f, ast, 1);
	else {
		ast_debug(1, "Not posting to queue since already masked on '%s'\n", ast->name);
		res = 0;
	}
	if (!res)
		ast_mutex_unlock(&p->lock);
	return res;
}

static int local_fixup(struct ast_channel *oldchan, struct ast_channel *newchan)
{
	struct local_pvt *p = newchan->tech_pvt;

	if (!p)
		return -1;

	ast_mutex_lock(&p->lock);

	if ((p->owner != oldchan) && (p->chan != oldchan)) {
		ast_log(LOG_WARNING, "Old channel wasn't %p but was %p/%p\n", oldchan, p->owner, p->chan);
		ast_mutex_unlock(&p->lock);
		return -1;
	}
	if (p->owner == oldchan)
		p->owner = newchan;
	else
		p->chan = newchan;
	ast_mutex_unlock(&p->lock);
	return 0;
}

static int local_indicate(struct ast_channel *ast, int condition, const void *data, size_t datalen)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = 0;
	struct ast_frame f = { AST_FRAME_CONTROL, };
	int isoutbound;

	if (!p)
		return -1;

	/* If this is an MOH hold or unhold, do it on the Local channel versus real channel */
	if (!ast_test_flag(p, LOCAL_MOH_PASSTHRU) && condition == AST_CONTROL_HOLD) {
		ast_moh_start(ast, data, NULL);
	} else if (!ast_test_flag(p, LOCAL_MOH_PASSTHRU) && condition == AST_CONTROL_UNHOLD) {
		ast_moh_stop(ast);
	} else if (condition == AST_CONTROL_CONNECTED_LINE || condition == AST_CONTROL_REDIRECTING) {
		struct ast_channel *this_channel;
		struct ast_channel *the_other_channel;
		/* A connected line update frame may only contain a partial amount of data, such
		 * as just a source, or just a ton, and not the full amount of information. However,
		 * the collected information is all stored in the outgoing channel's connectedline
		 * structure, so when receiving a connected line update on an outgoing local channel,
		 * we need to transmit the collected connected line information instead of whatever
		 * happens to be in this control frame. The same applies for redirecting information, which
		 * is why it is handled here as well.*/
		ast_mutex_lock(&p->lock);
		isoutbound = IS_OUTBOUND(ast, p);
		if (isoutbound) {
			this_channel = p->chan;
			the_other_channel = p->owner;
		} else {
			this_channel = p->owner;
			the_other_channel = p->chan;
		}
		if (the_other_channel) {
			unsigned char frame_data[1024];
			if (condition == AST_CONTROL_CONNECTED_LINE) {
				if (isoutbound) {
					ast_connected_line_copy_to_caller(&the_other_channel->caller, &this_channel->connected);
				}
				f.datalen = ast_connected_line_build_data(frame_data, sizeof(frame_data), &this_channel->connected, NULL);
			} else {
				f.datalen = ast_redirecting_build_data(frame_data, sizeof(frame_data), &this_channel->redirecting, NULL);
			}
			f.subclass.integer = condition;
			f.data.ptr = frame_data;
			if (!(res = local_queue_frame(p, isoutbound, &f, ast, 1))) {
				ast_mutex_unlock(&p->lock);
			}
		} else {
			ast_mutex_unlock(&p->lock);
		}
	} else {
		/* Queue up a frame representing the indication as a control frame */
		ast_mutex_lock(&p->lock);
		isoutbound = IS_OUTBOUND(ast, p);
		f.subclass.integer = condition;
		f.data.ptr = (void*)data;
		f.datalen = datalen;
		if (!(res = local_queue_frame(p, isoutbound, &f, ast, 1)))
			ast_mutex_unlock(&p->lock);
	}

	return res;
}

static int local_digit_begin(struct ast_channel *ast, char digit)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	struct ast_frame f = { AST_FRAME_DTMF_BEGIN, };
	int isoutbound;

	if (!p)
		return -1;

	ast_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	f.subclass.integer = digit;
	if (!(res = local_queue_frame(p, isoutbound, &f, ast, 0)))
		ast_mutex_unlock(&p->lock);

	return res;
}

static int local_digit_end(struct ast_channel *ast, char digit, unsigned int duration)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	struct ast_frame f = { AST_FRAME_DTMF_END, };
	int isoutbound;

	if (!p)
		return -1;

	ast_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	f.subclass.integer = digit;
	f.len = duration;
	if (!(res = local_queue_frame(p, isoutbound, &f, ast, 0)))
		ast_mutex_unlock(&p->lock);

	return res;
}

static int local_sendtext(struct ast_channel *ast, const char *text)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	struct ast_frame f = { AST_FRAME_TEXT, };
	int isoutbound;

	if (!p)
		return -1;

	ast_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	f.data.ptr = (char *) text;
	f.datalen = strlen(text) + 1;
	if (!(res = local_queue_frame(p, isoutbound, &f, ast, 0)))
		ast_mutex_unlock(&p->lock);
	return res;
}

static int local_sendhtml(struct ast_channel *ast, int subclass, const char *data, int datalen)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	struct ast_frame f = { AST_FRAME_HTML, };
	int isoutbound;

	if (!p)
		return -1;
	
	ast_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	f.subclass.integer = subclass;
	f.data.ptr = (char *)data;
	f.datalen = datalen;
	if (!(res = local_queue_frame(p, isoutbound, &f, ast, 0)))
		ast_mutex_unlock(&p->lock);
	return res;
}

/*! \brief Initiate new call, part of PBX interface 
 * 	dest is the dial string */
static int local_call(struct ast_channel *ast, char *dest, int timeout)
{
	struct local_pvt *p = ast->tech_pvt;
	int res;
	struct ast_var_t *varptr = NULL, *new;
	size_t len, namelen;
	char *reduced_dest = ast_strdupa(dest);
	char *slash;

	if (!p)
		return -1;

	/* If you value your sanity, please don't look at this code */
start_over:
	while (ast_channel_trylock(p->chan)) {
		ast_channel_unlock(p->owner);
		usleep(1);
		ast_channel_lock(p->owner);
	}

	/* p->owner and p->chan are locked now. Let's get p locked */
	if (ast_mutex_trylock(&p->lock)) {
		/* @#$&$@ */
		ast_channel_unlock(p->chan);
		ast_channel_unlock(p->owner);
		usleep(1);
		ast_channel_lock(p->owner);
		goto start_over;
	}

	/*
	 * Note that cid_num and cid_name aren't passed in the ast_channel_alloc
	 * call, so it's done here instead.
	 *
	 * All these failure points just return -1. The individual strings will
	 * be cleared when we destroy the channel.
	 */
	ast_party_redirecting_copy(&p->chan->redirecting, &p->owner->redirecting);

	ast_party_dialed_copy(&p->chan->dialed, &p->owner->dialed);

	ast_connected_line_copy_to_caller(&p->chan->caller, &p->owner->connected);
	ast_connected_line_copy_from_caller(&p->chan->connected, &p->owner->caller);

	ast_string_field_set(p->chan, language, p->owner->language);
	ast_string_field_set(p->chan, accountcode, p->owner->accountcode);
	ast_string_field_set(p->chan, musicclass, p->owner->musicclass);
	ast_cdr_update(p->chan);

	ast_channel_cc_params_init(p->chan, ast_channel_get_cc_config_params(p->owner));

	if (!ast_exists_extension(NULL, p->chan->context, p->chan->exten, 1,
		S_COR(p->owner->caller.id.number.valid, p->owner->caller.id.number.str, NULL))) {
		ast_log(LOG_NOTICE, "No such extension/context %s@%s while calling Local channel\n", p->chan->exten, p->chan->context);
		ast_mutex_unlock(&p->lock);
		ast_channel_unlock(p->chan);
		return -1;
	}

	/* Make sure we inherit the ANSWERED_ELSEWHERE flag if it's set on the queue/dial call request in the dialplan */
	if (ast_test_flag(ast, AST_FLAG_ANSWERED_ELSEWHERE)) {
		ast_set_flag(p->chan, AST_FLAG_ANSWERED_ELSEWHERE);
	}

	/* copy the channel variables from the incoming channel to the outgoing channel */
	/* Note that due to certain assumptions, they MUST be in the same order */
	AST_LIST_TRAVERSE(&p->owner->varshead, varptr, entries) {
		namelen = strlen(varptr->name);
		len = sizeof(struct ast_var_t) + namelen + strlen(varptr->value) + 2;
		if ((new = ast_calloc(1, len))) {
			memcpy(new, varptr, len);
			new->value = &(new->name[0]) + namelen + 1;
			AST_LIST_INSERT_TAIL(&p->chan->varshead, new, entries);
		}
	}
	ast_channel_datastore_inherit(p->owner, p->chan);
	/* If the local channel has /n or /b on the end of it,
	 * we need to lop that off for our argument to setting
	 * up the CC_INTERFACES variable
	 */
	if ((slash = strrchr(reduced_dest, '/'))) {
		*slash = '\0';
	}
	ast_set_cc_interfaces_chanvar(p->chan, reduced_dest);

	/* Start switch on sub channel */
	if (!(res = ast_pbx_start(p->chan)))
		ast_set_flag(p, LOCAL_LAUNCHED_PBX);

	ast_mutex_unlock(&p->lock);
	ast_channel_unlock(p->chan);
	return res;
}

/*! \brief Hangup a call through the local proxy channel */
static int local_hangup(struct ast_channel *ast)
{
	struct local_pvt *p = ast->tech_pvt;
	int isoutbound;
	struct ast_frame f = { AST_FRAME_CONTROL, { AST_CONTROL_HANGUP }, .data.uint32 = ast->hangupcause };
	struct ast_channel *ochan = NULL;
	int glaredetect = 0, res = 0;

	if (!p)
		return -1;

	ast_mutex_lock(&p->lock);

	isoutbound = IS_OUTBOUND(ast, p);

	if (p->chan && ast_test_flag(ast, AST_FLAG_ANSWERED_ELSEWHERE)) {
		ast_set_flag(p->chan, AST_FLAG_ANSWERED_ELSEWHERE);
		ast_debug(2, "This local call has the ANSWERED_ELSEWHERE flag set.\n");
	}

	if (isoutbound) {
		const char *status = pbx_builtin_getvar_helper(p->chan, "DIALSTATUS");
		if ((status) && (p->owner)) {
			/* Deadlock avoidance */
			while (p->owner && ast_channel_trylock(p->owner)) {
				ast_mutex_unlock(&p->lock);
				if (p->chan) {
					ast_channel_unlock(p->chan);
				}
				usleep(1);
				if (p->chan) {
					ast_channel_lock(p->chan);
				}
				ast_mutex_lock(&p->lock);
			}
			if (p->owner) {
				pbx_builtin_setvar_helper(p->owner, "CHANLOCALSTATUS", status);
				ast_channel_unlock(p->owner);
			}
		}
		p->chan = NULL;
		ast_clear_flag(p, LOCAL_LAUNCHED_PBX);
		ast_module_user_remove(p->u_chan);
	} else {
		ast_module_user_remove(p->u_owner);
		while (p->chan && ast_channel_trylock(p->chan)) {
				ast_mutex_unlock(&p->lock);
				if (p->owner) {
					ast_channel_unlock(p->owner);
				}
				usleep(1);
				if (p->owner) {
					ast_channel_lock(p->owner);
				}
				ast_mutex_lock(&p->lock);
		}

		p->owner = NULL;
		if (p->chan) {
			ast_queue_hangup(p->chan);
			ast_channel_unlock(p->chan);
		}
	}
	
	ast->tech_pvt = NULL;
	
	if (!p->owner && !p->chan) {
		/* Okay, done with the private part now, too. */
		glaredetect = ast_test_flag(p, LOCAL_GLARE_DETECT);
		/* If we have a queue holding, don't actually destroy p yet, but
		   let local_queue do it. */
		if (glaredetect)
			ast_set_flag(p, LOCAL_CANCEL_QUEUE);
		/* Remove from list */
		AST_LIST_LOCK(&locals);
		AST_LIST_REMOVE(&locals, p, list);
		AST_LIST_UNLOCK(&locals);
		ast_mutex_unlock(&p->lock);
		/* And destroy */
		if (!glaredetect) {
			p = local_pvt_destroy(p);
		}
		return 0;
	}
	if (p->chan && !ast_test_flag(p, LOCAL_LAUNCHED_PBX))
		/* Need to actually hangup since there is no PBX */
		ochan = p->chan;
	else
		res = local_queue_frame(p, isoutbound, &f, NULL, 1);
	if (!res)
		ast_mutex_unlock(&p->lock);
	if (ochan)
		ast_hangup(ochan);
	return 0;
}

/*! \brief Create a call structure */
static struct local_pvt *local_alloc(const char *data, int format)
{
	struct local_pvt *tmp = NULL;
	char *c = NULL, *opts = NULL;

	if (!(tmp = ast_calloc(1, sizeof(*tmp))))
		return NULL;

	/* Initialize private structure information */
	ast_mutex_init(&tmp->lock);
	ast_copy_string(tmp->exten, data, sizeof(tmp->exten));

	memcpy(&tmp->jb_conf, &g_jb_conf, sizeof(tmp->jb_conf));

	/* Look for options */
	if ((opts = strchr(tmp->exten, '/'))) {
		*opts++ = '\0';
		if (strchr(opts, 'n'))
			ast_set_flag(tmp, LOCAL_NO_OPTIMIZATION);
		if (strchr(opts, 'j')) {
			if (ast_test_flag(tmp, LOCAL_NO_OPTIMIZATION))
				ast_set_flag(&tmp->jb_conf, AST_JB_ENABLED);
			else {
				ast_log(LOG_ERROR, "You must use the 'n' option for chan_local "
					"to use the 'j' option to enable the jitterbuffer\n");
			}
		}
		if (strchr(opts, 'b')) {
			ast_set_flag(tmp, LOCAL_BRIDGE);
		}
		if (strchr(opts, 'm')) {
			ast_set_flag(tmp, LOCAL_MOH_PASSTHRU);
		}
	}

	/* Look for a context */
	if ((c = strchr(tmp->exten, '@')))
		*c++ = '\0';

	ast_copy_string(tmp->context, c ? c : "default", sizeof(tmp->context));

	tmp->reqformat = format;

#if 0
	/* We can't do this check here, because we don't know the CallerID yet, and
	 * the CallerID could potentially affect what step is actually taken (or
	 * even if that step exists). */
	if (!ast_exists_extension(NULL, tmp->context, tmp->exten, 1, NULL)) {
		ast_log(LOG_NOTICE, "No such extension/context %s@%s creating local channel\n", tmp->exten, tmp->context);
		tmp = local_pvt_destroy(tmp);
	} else {
#endif
		/* Add to list */
		AST_LIST_LOCK(&locals);
		AST_LIST_INSERT_HEAD(&locals, tmp, list);
		AST_LIST_UNLOCK(&locals);
#if 0
	}
#endif
	
	return tmp;
}

/*! \brief Start new local channel */
static struct ast_channel *local_new(struct local_pvt *p, int state, const char *linkedid)
{
	struct ast_channel *tmp = NULL, *tmp2 = NULL;
	int randnum = ast_random() & 0xffff, fmt = 0;
	const char *t;
	int ama;

	/* Allocate two new Asterisk channels */
	/* safe accountcode */
	if (p->owner && p->owner->accountcode)
		t = p->owner->accountcode;
	else
		t = "";

	if (p->owner)
		ama = p->owner->amaflags;
	else
		ama = 0;
	if (!(tmp = ast_channel_alloc(1, state, 0, 0, t, p->exten, p->context, linkedid, ama, "Local/%s@%s-%04x;1", p->exten, p->context, randnum)) 
		|| !(tmp2 = ast_channel_alloc(1, AST_STATE_RING, 0, 0, t, p->exten, p->context, linkedid, ama, "Local/%s@%s-%04x;2", p->exten, p->context, randnum))) {
		if (tmp) {
			tmp = ast_channel_release(tmp);
		}
		ast_log(LOG_WARNING, "Unable to allocate channel structure(s)\n");
		return NULL;
	}

	tmp2->tech = tmp->tech = &local_tech;

	tmp->nativeformats = p->reqformat;
	tmp2->nativeformats = p->reqformat;

	/* Determine our read/write format and set it on each channel */
	fmt = ast_best_codec(p->reqformat);
	tmp->writeformat = fmt;
	tmp2->writeformat = fmt;
	tmp->rawwriteformat = fmt;
	tmp2->rawwriteformat = fmt;
	tmp->readformat = fmt;
	tmp2->readformat = fmt;
	tmp->rawreadformat = fmt;
	tmp2->rawreadformat = fmt;

	tmp->tech_pvt = p;
	tmp2->tech_pvt = p;

	p->owner = tmp;
	p->chan = tmp2;
	p->u_owner = ast_module_user_add(p->owner);
	p->u_chan = ast_module_user_add(p->chan);

	ast_copy_string(tmp->context, p->context, sizeof(tmp->context));
	ast_copy_string(tmp2->context, p->context, sizeof(tmp2->context));
	ast_copy_string(tmp2->exten, p->exten, sizeof(tmp->exten));
	tmp->priority = 1;
	tmp2->priority = 1;

	ast_jb_configure(tmp, &p->jb_conf);

	return tmp;
}

/*! \brief Part of PBX interface */
static struct ast_channel *local_request(const char *type, format_t format, const struct ast_channel *requestor, void *data, int *cause)
{
	struct local_pvt *p = NULL;
	struct ast_channel *chan = NULL;

	/* Allocate a new private structure and then Asterisk channel */
	if ((p = local_alloc(data, format))) {
		if (!(chan = local_new(p, AST_STATE_DOWN, requestor ? requestor->linkedid : NULL))) {
			AST_LIST_LOCK(&locals);
			AST_LIST_REMOVE(&locals, p, list);
			AST_LIST_UNLOCK(&locals);
			p = local_pvt_destroy(p);
		}
		if (chan && ast_channel_cc_params_init(chan, requestor ? ast_channel_get_cc_config_params((struct ast_channel *)requestor) : NULL)) {
			chan = ast_channel_release(chan);
			p = local_pvt_destroy(p);
		}
	}

	return chan;
}

/*! \brief CLI command "local show channels" */
static char *locals_show(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct local_pvt *p = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "local show channels";
		e->usage =
			"Usage: local show channels\n"
			"       Provides summary information on active local proxy channels.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

	AST_LIST_LOCK(&locals);
	if (!AST_LIST_EMPTY(&locals)) {
		AST_LIST_TRAVERSE(&locals, p, list) {
			ast_mutex_lock(&p->lock);
			ast_cli(a->fd, "%s -- %s@%s\n", p->owner ? p->owner->name : "<unowned>", p->exten, p->context);
			ast_mutex_unlock(&p->lock);
		}
	} else
		ast_cli(a->fd, "No local channels in use\n");
	AST_LIST_UNLOCK(&locals);

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_local[] = {
	AST_CLI_DEFINE(locals_show, "List status of local channels"),
};

static int manager_optimize_away(struct mansession *s, const struct message *m)
{
	const char *channel;
	struct local_pvt *p, *tmp = NULL;
	struct ast_channel *c;
	int found = 0;

	channel = astman_get_header(m, "Channel");

	if (ast_strlen_zero(channel)) {
		astman_send_error(s, m, "'Channel' not specified.");
		return 0;
	}

	c = ast_channel_get_by_name(channel);
	if (!c) {
		astman_send_error(s, m, "Channel does not exist.");
		return 0;
	}

	p = c->tech_pvt;
	ast_channel_unref(c);
	c = NULL;

	if (AST_LIST_LOCK(&locals)) {
		astman_send_error(s, m, "Unable to lock the monitor");
		return 0;
	}


	AST_LIST_TRAVERSE(&locals, tmp, list) {
		if (tmp == p) {
			ast_mutex_lock(&tmp->lock);
			found = 1;
			ast_clear_flag(tmp, LOCAL_NO_OPTIMIZATION);
			ast_mutex_unlock(&tmp->lock);
			break;
		}
	}
	AST_LIST_UNLOCK(&locals);

	if (found) {
		astman_send_ack(s, m, "Queued channel to be optimized away");
	} else {
		astman_send_error(s, m, "Unable to find channel");
	}

	return 0;
}


/*! \brief Load module into PBX, register channel */
static int load_module(void)
{
	/* Make sure we can register our channel type */
	if (ast_channel_register(&local_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel class 'Local'\n");
		return AST_MODULE_LOAD_FAILURE;
	}
	ast_cli_register_multiple(cli_local, sizeof(cli_local) / sizeof(struct ast_cli_entry));
	ast_manager_register_xml("LocalOptimizeAway", EVENT_FLAG_SYSTEM|EVENT_FLAG_CALL, manager_optimize_away);

	return AST_MODULE_LOAD_SUCCESS;
}

/*! \brief Unload the local proxy channel from Asterisk */
static int unload_module(void)
{
	struct local_pvt *p = NULL;

	/* First, take us out of the channel loop */
	ast_cli_unregister_multiple(cli_local, sizeof(cli_local) / sizeof(struct ast_cli_entry));
	ast_manager_unregister("LocalOptimizeAway");
	ast_channel_unregister(&local_tech);
	if (!AST_LIST_LOCK(&locals)) {
		/* Hangup all interfaces if they have an owner */
		AST_LIST_TRAVERSE(&locals, p, list) {
			if (p->owner)
				ast_softhangup(p->owner, AST_SOFTHANGUP_APPUNLOAD);
		}
		AST_LIST_UNLOCK(&locals);
	} else {
		ast_log(LOG_WARNING, "Unable to lock the monitor\n");
		return -1;
	}		
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Local Proxy Channel (Note: used internally by other modules)",
		.load = load_module,
		.unload = unload_module,
		.load_pri = AST_MODPRI_CHANNEL_DRIVER,
	);
