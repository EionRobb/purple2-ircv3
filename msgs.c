/**
 * @file msgs.c
 *
 * purple
 *
 * Copyright (C) 2003, 2012 Ethan Blanton <elb@pidgin.im>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

/*
 * Note: If you change any of these functions to use additional args you
 * MUST ensure the arg count is correct in parse.c. Otherwise it may be
 * possible for a malicious server or man-in-the-middle to trigger a crash.
 */

#include "irc.h"
#include "purple.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_CYRUS_SASL
#include <sasl/sasl.h>
#endif

static char *
irc_mask_nick(const char *mask);
static char *
irc_mask_userhost(const char *mask);
static void
irc_chat_remove_buddy(PurpleConversation *convo, char *data[2]);
static void
irc_buddy_status(char *name, struct irc_buddy *ib, struct irc_conn *irc);
static void
irc_connected(struct irc_conn *irc, const char *nick);

static void
irc_msg_handle_privmsg(struct irc_conn *irc, const char *name, const char *from, const char *to, const char *rawmsg, gboolean notice);

#ifdef HAVE_CYRUS_SASL
static void
irc_sasl_finish(struct irc_conn *irc);
#endif

static char *
irc_mask_nick(const char *mask)
{
	char *end, *buf;

	end = strchr(mask, '!');
	if (!end)
		buf = g_strdup(mask);
	else
		buf = g_strndup(mask, end - mask);

	return buf;
}

static char *
irc_mask_userhost(const char *mask)
{
	char *sep = strchr(mask, '!');
	char *host = "";

	if (sep) {
		host = sep + 1;
	}

	return g_strdup(host);
}

static void
irc_chat_remove_buddy(PurpleConversation *convo, char *data[2])
{
	char *message, *stripped;

	stripped = data[1] ? irc_mirc2txt(data[1]) : NULL;
	message = g_strdup_printf("quit: %s", stripped);
	g_free(stripped);

	if (purple_conv_chat_find_user(PURPLE_CONV_CHAT(convo), data[0]))
		purple_conv_chat_remove_user(PURPLE_CONV_CHAT(convo), data[0], message);

	g_free(message);
}

static void
irc_connected(struct irc_conn *irc, const char *nick)
{
	PurpleConnection *gc;
	PurpleStatus *status;
	GSList *buddies;
	PurpleAccount *account;

	if ((gc = purple_account_get_connection(irc->account)) == NULL || PURPLE_CONNECTION_IS_CONNECTED(gc))
		return;

	purple_connection_set_display_name(gc, nick);
	purple_connection_set_state(gc, PURPLE_CONNECTED);
	account = purple_connection_get_account(gc);

	/* If we're away then set our away message */
	status = purple_account_get_active_status(irc->account);
	if (purple_status_type_get_primitive(purple_status_get_type(status)) != PURPLE_STATUS_AVAILABLE) {
		PurplePluginProtocolInfo *prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(gc->prpl);
		prpl_info->set_status(irc->account, status);
	}

	/* this used to be in the core, but it's not now */
	for (buddies = purple_find_buddies(account, NULL); buddies;
		 buddies = g_slist_delete_link(buddies, buddies)) {
		PurpleBuddy *b = buddies->data;
		struct irc_buddy *ib = g_new0(struct irc_buddy, 1);
		ib->name = g_strdup(purple_buddy_get_name(b));
		ib->ref = 1;
		g_hash_table_replace(irc->buddies, ib->name, ib);
	}

	irc_blist_timeout(irc);
	if (!irc->timer)
		irc->timer = purple_timeout_add_seconds(45, (GSourceFunc) irc_blist_timeout, (gpointer) irc);

	if (irc->cap_metadata_2) {
		char *buf = irc_format(irc, "v", "METADATA * SUB avatar");
		irc_send(irc, buf);
		g_free(buf);
	}
}

/* This function is ugly, but it's really an error handler. */
void
irc_msg_default(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	int i;
	const char *end, *cur, *numeric = NULL;
	char *clean, *tmp, *convname;
	PurpleConversation *convo;

	for (cur = args[0], i = 0; i < 4; i++) {
		end = strchr(cur, ' ');
		if (end == NULL) {
			goto undirected;
		}
		/* Check for 3-digit numeric in second position */
		if (i == 1) {
			if (end - cur != 3 || !g_ascii_isdigit(cur[0]) || !g_ascii_isdigit(cur[1]) || !g_ascii_isdigit(cur[2])) {
				goto undirected;
			}
			/* Save the numeric for printing to the channel */
			numeric = cur;
		}
		/* Don't advance cur if we're on the final iteration. */
		if (i != 3) {
			cur = end + 1;
		}
	}

	/* At this point, cur is the beginning of the fourth position,
	 * end is the following space, and there are remaining
	 * arguments.  We'll check to see if this argument is a
	 * currently active conversation (private message or channel,
	 * either one), and print the numeric to that conversation if it
	 * is. */

	tmp = g_strndup(cur, end - cur);
	convname = purple_utf8_salvage(tmp);
	g_free(tmp);

	/* Check for an existing conversation */
	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY,
												  convname,
												  irc->account);
	g_free(convname);

	if (convo == NULL) {
		goto undirected;
	}

	/* end + 1 is the first argument past the target.  The initial
	 * arguments we've skipped are routing info, numeric, recipient
	 * (this account's nick, most likely), and target (this
	 * channel).  If end + 1 is an ASCII :, skip it, because it's
	 * meaningless in this context.  This won't catch all
	 * :-arguments, but it'll catch the easy case. */
	if (*++end == ':') {
		end++;
	}

	/* We then print "numeric: remainder". */
	clean = purple_utf8_salvage(end);
	tmp = g_strdup_printf("%.3s: %s", numeric, clean);
	g_free(clean);
	purple_conversation_write(convo, "", tmp, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_RAW | PURPLE_MESSAGE_NO_LINKIFY, time(NULL));
	g_free(tmp);
	return;

undirected:
	/* This, too, should be escaped somehow (smarter) */
	clean = purple_utf8_salvage(args[0]);
	purple_debug(PURPLE_DEBUG_INFO, "irc", "Unrecognized message: %s\n", clean);
	g_free(clean);
}

void
irc_msg_features(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	gchar **features;
	int i;

	features = g_strsplit(args[1], " ", -1);
	for (i = 0; features[i]; i++) {
		char *val;
		if (!strncmp(features[i], "PREFIX=", 7)) {
			if ((val = strchr(features[i] + 7, ')')) != NULL)
				irc->mode_chars = g_strdup(val + 1);
		} else if (strcmp(features[i], "UTF8ONLY") == 0) {
			irc->utf8only = TRUE;
		} else if (strcmp(features[i], "WHOX") == 0 || strncmp(features[i], "WHOX=", 5) == 0) {
			irc->whox_supported = TRUE;
		} else if (strncmp(features[i], "CHATHISTORY=", 12) == 0) {
			irc->chathistory_limit = atoi(features[i] + 12);
		} else if (strcmp(features[i], "CHATHISTORY") == 0) {
			irc->chathistory_limit = 50;
		} else if (strncmp(features[i], "MONITOR=", 8) == 0) {
			irc->monitor_supported = TRUE;
			irc->monitor_limit = atoi(features[i] + 8);
		} else if (strcmp(features[i], "MONITOR") == 0) {
			irc->monitor_supported = TRUE;
			irc->monitor_limit = 0;
		}
	}

	g_strfreev(features);
}

void
irc_msg_luser(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	if (purple_strequal(name, "251")) {
		/* 251 is required, so we pluck our nick from here and
		 * finalize connection */
		irc_connected(irc, args[0]);
		/* Some IRC servers seem to not send a 255 numeric, so
		 * I guess we can't require it; 251 will do. */
		/* } else if (purple_strequal(name, "255")) { */
	}
}

void
irc_msg_away(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc;
	char *msg;

	if (irc->whois.nick && !purple_utf8_strcasecmp(irc->whois.nick, args[1])) {
		/* We're doing a whois, show this in the whois dialog */
		irc_msg_whois(irc, name, from, args);
		return;
	}

	gc = purple_account_get_connection(irc->account);
	if (gc) {
		msg = g_markup_escape_text(args[2], -1);
		serv_got_im(gc, args[1], msg, PURPLE_MESSAGE_AUTO_RESP, time(NULL));
		g_free(msg);
	}
}

void
irc_msg_away_notify(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc;
	char *nick;
	GSList *chats;

	gc = purple_account_get_connection(irc->account);
	if (!gc || !from)
		return;

	nick = irc_mask_nick(from);
	if (!nick)
		return;

	if (args[0] != NULL) {
		if (*args[0] != '\0') {
			purple_prpl_got_user_status(irc->account, nick, "away", "message", args[0], NULL);
		} else {
			purple_prpl_got_user_status(irc->account, nick, "away", NULL);
		}
	} else {
		purple_prpl_got_user_status(irc->account, nick, "available", NULL);
	}

	chats = gc->buddy_chats;
	while (chats) {
		PurpleConvChat *chat = PURPLE_CONV_CHAT(chats->data);
		if (purple_conv_chat_find_user(chat, nick)) {
			PurpleConvChatBuddyFlags flags = purple_conv_chat_user_get_flags(chat, nick);
			if (args[0] != NULL) {
				purple_conv_chat_user_set_flags(chat, nick, flags | PURPLE_CBFLAGS_AWAY);
			} else {
				purple_conv_chat_user_set_flags(chat, nick, flags & ~PURPLE_CBFLAGS_AWAY);
			}
		}
		chats = chats->next;
	}

	g_free(nick);
}

static gboolean
irc_is_batch_chathistory(struct irc_conn *irc)
{
	if (!irc->current_tags || !irc->active_batches)
		return FALSE;

	gchar **tags = g_strsplit(irc->current_tags, ";", -1);
	gboolean is_ch = FALSE;
	int i;
	for (i = 0; tags[i] != NULL; i++) {
		if (g_str_has_prefix(tags[i], "batch=")) {
			const char *ref = tags[i] + 6;
			struct irc_batch *batch = g_hash_table_lookup(irc->active_batches, ref);
			if (batch && (g_strcmp0(batch->type, "chathistory") == 0 || g_strcmp0(batch->type, "draft/chathistory") == 0)) {
				is_ch = TRUE;
				break;
			}
		}
	}
	g_strfreev(tags);
	return is_ch;
}

static time_t
irc_parse_server_time(struct irc_conn *irc, time_t default_time)
{
	if (!irc->current_tags)
		return default_time;

	gchar **tags = g_strsplit(irc->current_tags, ";", -1);
	time_t t = default_time;
	int i;
	for (i = 0; tags[i] != NULL; i++) {
		if (g_str_has_prefix(tags[i], "time=")) {
			t = purple_str_to_time(tags[i] + 5, TRUE, NULL, NULL, NULL);
			break;
		}
	}
	g_strfreev(tags);
	return (t != 0) ? t : default_time;
}

void
irc_msg_account(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc;
	char *nick;
	const char *accountname;
	GSList *chats;

	gc = purple_account_get_connection(irc->account);
	if (!gc || !from || !args || !args[0])
		return;

	nick = irc_mask_nick(from);
	if (!nick)
		return;

	accountname = args[0];

	chats = gc->buddy_chats;
	while (chats) {
		PurpleConvChat *chat = PURPLE_CONV_CHAT(chats->data);
		PurpleConvChatBuddy *cb = purple_conv_chat_cb_find(chat, nick);
		if (cb) {
			const char *val = (accountname && strcmp(accountname, "*") != 0) ? accountname : NULL;
			purple_conv_chat_cb_set_attribute(chat, cb, "account", val);
		}
		chats = chats->next;
	}

	PurpleBuddy *buddy = purple_find_buddy(irc->account, nick);
	if (buddy) {
		const char *val = (accountname && strcmp(accountname, "*") != 0) ? accountname : NULL;
		purple_blist_node_set_string(PURPLE_BLIST_NODE(buddy), "account", val);
	}

	g_free(nick);
}

void
irc_msg_chghost(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc;
	char *nick, *userhost;
	const char *newuser, *newhost;
	GSList *chats;

	gc = purple_account_get_connection(irc->account);
	if (!gc || !from || !args || !args[0] || !args[1])
		return;

	nick = irc_mask_nick(from);
	if (!nick)
		return;

	newuser = args[0];
	newhost = args[1];
	userhost = g_strdup_printf("%s@%s", newuser, newhost);

	chats = gc->buddy_chats;
	while (chats) {
		PurpleConvChat *chat = PURPLE_CONV_CHAT(chats->data);
		PurpleConvChatBuddy *cb = purple_conv_chat_cb_find(chat, nick);
		if (cb) {
			purple_conv_chat_cb_set_attribute(chat, cb, "userhost", userhost);
		}
		chats = chats->next;
	}

	PurpleBuddy *buddy = purple_find_buddy(irc->account, nick);
	if (buddy) {
		purple_blist_node_set_string(PURPLE_BLIST_NODE(buddy), "userhost", userhost);
	}

	g_free(userhost);
	g_free(nick);
}

void
irc_msg_badmode(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);

	g_return_if_fail(gc);

	purple_notify_error(gc, NULL, _("Bad mode"), args[1]);
}

void
irc_msg_ban(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *convo;

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT,
												  args[1],
												  irc->account);

	if (purple_strequal(name, "367")) {
		char *msg = NULL;
		/* Ban list entry */
		if (args[3] && args[4]) {
			/* This is an extended syntax, not in RFC 1459 */
			int t1 = atoi(args[4]);
			time_t t2 = time(NULL);
			char *time = purple_str_seconds_to_string(t2 - t1);
			msg = g_strdup_printf(_("Ban on %s by %s, set %s ago"),
								  args[2],
								  args[3],
								  time);
			g_free(time);
		} else {
			msg = g_strdup_printf(_("Ban on %s"), args[2]);
		}
		if (convo) {
			purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", msg, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
		} else {
			purple_debug_info("irc", "%s\n", msg);
		}
		g_free(msg);
	} else if (purple_strequal(name, "368")) {
		if (!convo)
			return;
		/* End of ban list */
		purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", _("End of ban list"), PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
	}
}

void
irc_msg_banned(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	char *buf;

	g_return_if_fail(gc);

	buf = g_strdup_printf(_("You are banned from %s."), args[1]);
	purple_notify_error(gc, _("Banned"), _("Banned"), buf);
	g_free(buf);
}

void
irc_msg_badkey(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	char *buf;

	g_return_if_fail(gc);

	buf = g_strdup_printf(_("Cannot join %s: Incorrect or missing channel key."), args[1]);
	purple_notify_error(gc, _("Requires password"), _("Cannot join channel"), buf);
	g_free(buf);
}

void
irc_msg_chanfull(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	char *buf;

	g_return_if_fail(gc);

	buf = g_strdup_printf(_("Cannot join %s: Channel user limit has been reached."), args[1]);
	purple_notify_error(gc, _("Channel full"), _("Cannot join channel"), buf);
	g_free(buf);
}

void
irc_msg_toomanychan(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	char *buf;

	g_return_if_fail(gc);

	buf = g_strdup_printf(_("Cannot join %s: You have joined the maximum number of allowed channels."), args[1]);
	purple_notify_error(gc, _("Too many channels"), _("Cannot join channel"), buf);
	g_free(buf);
}

void
irc_msg_badchanmask(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	char *buf;

	g_return_if_fail(gc);

	buf = g_strdup_printf(_("Cannot join %s: Invalid channel name."), args[1]);
	purple_notify_error(gc, _("Bad channel name"), _("Cannot join channel"), buf);
	g_free(buf);
}

void
irc_msg_banfull(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *convo;
	char *buf, *nick;

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[1], irc->account);
	if (!convo)
		return;

	nick = g_markup_escape_text(args[2], -1);
	buf = g_strdup_printf(_("Cannot ban %s: banlist is full"), nick);
	g_free(nick);
	purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", buf, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
	g_free(buf);
}

void
irc_msg_chanmode(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *convo;
	char *buf, *escaped;

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[1], irc->account);
	if (!convo) /* XXX punt on channels we are not in for now */
		return;

	escaped = (args[3] != NULL) ? g_markup_escape_text(args[3], -1) : NULL;
	buf = g_strdup_printf("mode for %s: %s %s", args[1], args[2], escaped ? escaped : "");
	purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", buf, PURPLE_MESSAGE_SYSTEM, time(NULL));
	g_free(escaped);
	g_free(buf);

	return;
}

void
irc_msg_whois(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	if (!irc->whois.nick) {
		purple_debug(PURPLE_DEBUG_WARNING, "irc", "Unexpected %s reply for %s\n", purple_strequal(name, "314") ? "WHOWAS" : "WHOIS", args[1]);
		return;
	}

	if (purple_utf8_strcasecmp(irc->whois.nick, args[1])) {
		purple_debug(PURPLE_DEBUG_WARNING, "irc", "Got %s reply for %s while waiting for %s\n", purple_strequal(name, "314") ? "WHOWAS" : "WHOIS", args[1], irc->whois.nick);
		return;
	}

	if (purple_strequal(name, "301")) {
		irc->whois.away = g_strdup(args[2]);
	} else if (purple_strequal(name, "307")) {
		irc->whois.identified = 1;
	} else if (purple_strequal(name, "311") || purple_strequal(name, "314")) {
		irc->whois.ident = g_strdup(args[2]);
		irc->whois.host = g_strdup(args[3]);
		irc->whois.real = g_strdup(args[5]);
	} else if (purple_strequal(name, "312")) {
		irc->whois.server = g_strdup(args[2]);
		irc->whois.serverinfo = g_strdup(args[3]);
	} else if (purple_strequal(name, "313")) {
		irc->whois.ircop = 1;
	} else if (purple_strequal(name, "317")) {
		irc->whois.idle = atoi(args[2]);
		if (args[3])
			irc->whois.signon = (time_t) atoi(args[3]);
	} else if (purple_strequal(name, "319")) {
		if (irc->whois.channels == NULL) {
			irc->whois.channels = g_string_new(args[2]);
		} else {
			irc->whois.channels = g_string_append(irc->whois.channels, args[2]);
		}
	} else if (purple_strequal(name, "320")) {
		irc->whois.identified = 1;
	} else if (purple_strequal(name, "330")) {
		irc->whois.login = g_strdup(args[2]);
	} else if (purple_strequal(name, "335")) {
		irc->whois.bot = 1;
	} else if (purple_strequal(name, "378")) {
		irc->whois.connected_from = g_strdup(args[2]);
	} else if (purple_strequal(name, "379")) {
		irc->whois.modes = g_strdup(args[2]);
	} else if (purple_strequal(name, "671") || purple_strequal(name, "275")) {
		irc->whois.secure = g_strdup(args[2]);
	} else if (purple_strequal(name, "276")) {
		irc->whois.certfp = g_strdup(args[2]);
	}
}

void
irc_msg_endwhois(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc;
	char *tmp, *tmp2;
	PurpleNotifyUserInfo *user_info;

	if (!irc->whois.nick) {
		purple_debug(PURPLE_DEBUG_WARNING, "irc", "Unexpected End of %s for %s\n", purple_strequal(name, "369") ? "WHOWAS" : "WHOIS", args[1]);
		return;
	}
	if (purple_utf8_strcasecmp(irc->whois.nick, args[1])) {
		purple_debug(PURPLE_DEBUG_WARNING, "irc", "Received end of %s for %s, expecting %s\n", purple_strequal(name, "369") ? "WHOWAS" : "WHOIS", args[1], irc->whois.nick);
		return;
	}

	user_info = purple_notify_user_info_new();

	tmp2 = g_markup_escape_text(args[1], -1);
	tmp = g_strdup_printf("%s%s%s%s", tmp2, (irc->whois.ircop ? _(" <i>(ircop)</i>") : ""), (irc->whois.identified ? _(" <i>(identified)</i>") : ""), (irc->whois.bot ? _(" <i>(bot)</i>") : ""));
	purple_notify_user_info_add_pair(user_info, _("Nick"), tmp);
	g_free(tmp2);
	g_free(tmp);

	if (irc->whois.away) {
		tmp = g_markup_escape_text(irc->whois.away, strlen(irc->whois.away));
		g_free(irc->whois.away);
		purple_notify_user_info_add_pair(user_info, _("Away"), tmp);
		g_free(tmp);
	}
	if (irc->whois.real) {
		purple_notify_user_info_add_pair_plaintext(user_info, _("Real name"), irc->whois.real);
		g_free(irc->whois.real);
	}
	if (irc->whois.login) {
		purple_notify_user_info_add_pair_plaintext(user_info, _("Login name"), irc->whois.login);
		g_free(irc->whois.login);
	}
	if (irc->whois.ident) {
		purple_notify_user_info_add_pair_plaintext(user_info, _("Ident name"), irc->whois.ident);
		g_free(irc->whois.ident);
	}
	if (irc->whois.host) {
		purple_notify_user_info_add_pair_plaintext(user_info, _("Host name"), irc->whois.host);
		g_free(irc->whois.host);
	}
	if (irc->whois.connected_from) {
		purple_notify_user_info_add_pair_plaintext(user_info, _("Connected from"), irc->whois.connected_from);
		g_free(irc->whois.connected_from);
	}
	if (irc->whois.secure) {
		purple_notify_user_info_add_pair_plaintext(user_info, _("Secure connection"), irc->whois.secure);
		g_free(irc->whois.secure);
	}
	if (irc->whois.certfp) {
		purple_notify_user_info_add_pair_plaintext(user_info, _("Certificate fingerprint"), irc->whois.certfp);
		g_free(irc->whois.certfp);
	}
	if (irc->whois.modes) {
		purple_notify_user_info_add_pair_plaintext(user_info, _("User modes"), irc->whois.modes);
		g_free(irc->whois.modes);
	}
	if (irc->whois.server) {
		tmp = g_strdup_printf("%s (%s)", irc->whois.server, irc->whois.serverinfo);
		purple_notify_user_info_add_pair(user_info, _("Server"), tmp);
		g_free(tmp);
		g_free(irc->whois.server);
		g_free(irc->whois.serverinfo);
	}
	if (irc->whois.channels) {
		purple_notify_user_info_add_pair(user_info, _("Currently on"), irc->whois.channels->str);
		g_string_free(irc->whois.channels, TRUE);
	}
	if (irc->whois.idle) {
		gchar *timex = purple_str_seconds_to_string(irc->whois.idle);
		purple_notify_user_info_add_pair(user_info, _("Idle for"), timex);
		g_free(timex);
		purple_notify_user_info_add_pair(user_info,
										 _("Online since"),
										 purple_date_format_full(localtime(&irc->whois.signon)));
	}
	if (purple_strequal(irc->whois.nick, "Paco-Paco")) {
		purple_notify_user_info_add_pair(user_info,
										 _("<b>Defining adjective:</b>"),
										 _("Glorious"));
	}

	gc = purple_account_get_connection(irc->account);

	purple_notify_userinfo(gc, irc->whois.nick, user_info, NULL, NULL);
	purple_notify_user_info_destroy(user_info);

	g_free(irc->whois.nick);
	memset(&irc->whois, 0, sizeof(irc->whois));
}

void
irc_msg_who(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	if (purple_strequal(name, "352")) {
		PurpleConversation *conv;
		PurpleConvChat *chat;
		PurpleConvChatBuddy *cb;

		char *cur, *userhost, *realname;

		PurpleConvChatBuddyFlags flags;
		GList *keys = NULL, *values = NULL;

		if (args[6][0] == 'G') {
			purple_prpl_got_user_status(irc->account, args[5], "away", NULL);
		} else if (args[6][0] == 'H') {
			purple_prpl_got_user_status(irc->account, args[5], "available", NULL);
		}

		conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[1], irc->account);
		if (!conv) {
			return;
		}

		cb = purple_conv_chat_cb_find(PURPLE_CONV_CHAT(conv), args[5]);
		if (!cb) {
			return;
		}

		chat = PURPLE_CONV_CHAT(conv);

		userhost = g_strdup_printf("%s@%s", args[2], args[3]);

		/* The final argument is a :-argument, but annoyingly
		 * contains two "words", the hop count and real name. */
		for (cur = args[7]; *cur; cur++) {
			if (*cur == ' ') {
				cur++;
				break;
			}
		}
		realname = g_strdup(cur);

		keys = g_list_prepend(keys, "userhost");
		values = g_list_prepend(values, userhost);

		keys = g_list_prepend(keys, "realname");
		values = g_list_prepend(values, realname);

		purple_conv_chat_cb_set_attributes(chat, cb, keys, values);

		g_list_free(keys);
		g_list_free(values);

		g_free(userhost);
		g_free(realname);

		flags = cb->flags;

		if (args[6][0] == 'G' && !(flags & PURPLE_CBFLAGS_AWAY)) {
			purple_conv_chat_user_set_flags(chat, cb->name, flags | PURPLE_CBFLAGS_AWAY);
		} else if (args[6][0] == 'H' && (flags & PURPLE_CBFLAGS_AWAY)) {
			purple_conv_chat_user_set_flags(chat, cb->name, flags & ~PURPLE_CBFLAGS_AWAY);
		}
	}
}

void
irc_msg_whox(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *conv;
	PurpleConvChat *chat;
	PurpleConvChatBuddy *cb;
	PurpleBuddy *buddy;

	const char *chan = args[2];
	const char *user = args[3];
	const char *host = args[4];
	const char *nick = args[5];
	const char *flags_str = args[6];
	const char *account = args[7];
	const char *realname = args[8];

	char *userhost;
	GList *keys = NULL, *values = NULL;

	if (flags_str[0] == 'G') {
		purple_prpl_got_user_status(irc->account, nick, "away", NULL);
	} else if (flags_str[0] == 'H') {
		purple_prpl_got_user_status(irc->account, nick, "available", NULL);
	}

	userhost = g_strdup_printf("%s@%s", user, host);

	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, chan, irc->account);
	if (conv) {
		chat = PURPLE_CONV_CHAT(conv);
		cb = purple_conv_chat_cb_find(chat, nick);
		if (cb) {
			PurpleConvChatBuddyFlags flags = cb->flags;

			keys = g_list_prepend(keys, "userhost");
			values = g_list_prepend(values, userhost);

			keys = g_list_prepend(keys, "realname");
			values = g_list_prepend(values, (gpointer) realname);

			if (account && strcmp(account, "0") != 0 && strcmp(account, "*") != 0) {
				keys = g_list_prepend(keys, "account");
				values = g_list_prepend(values, (gpointer) account);
			}

			purple_conv_chat_cb_set_attributes(chat, cb, keys, values);

			if (flags_str[0] == 'G' && !(flags & PURPLE_CBFLAGS_AWAY)) {
				purple_conv_chat_user_set_flags(chat, cb->name, flags | PURPLE_CBFLAGS_AWAY);
			} else if (flags_str[0] == 'H' && (flags & PURPLE_CBFLAGS_AWAY)) {
				purple_conv_chat_user_set_flags(chat, cb->name, flags & ~PURPLE_CBFLAGS_AWAY);
			}

			g_list_free(keys);
			g_list_free(values);
		}
	}

	buddy = purple_find_buddy(irc->account, nick);
	if (buddy) {
		if (account && strcmp(account, "0") != 0 && strcmp(account, "*") != 0) {
			purple_blist_node_set_string(PURPLE_BLIST_NODE(buddy), "account", account);
		}
		if (userhost && *userhost) {
			purple_blist_node_set_string(PURPLE_BLIST_NODE(buddy), "userhost", userhost);
		}
	}

	g_free(userhost);
}

void
irc_msg_list(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	if (!irc->roomlist)
		return;

	if (purple_strequal(name, "321")) {
		purple_roomlist_set_in_progress(irc->roomlist, TRUE);
		return;
	}

	if (purple_strequal(name, "323")) {
		purple_roomlist_set_in_progress(irc->roomlist, FALSE);
		purple_roomlist_unref(irc->roomlist);
		irc->roomlist = NULL;
		return;
	}

	if (purple_strequal(name, "322")) {
		PurpleRoomlistRoom *room;
		char *topic;

		if (!purple_roomlist_get_in_progress(irc->roomlist)) {
			purple_debug_warning("irc", "Buggy server didn't send RPL_LISTSTART.\n");
			purple_roomlist_set_in_progress(irc->roomlist, TRUE);
		}

		room = purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM, args[1], NULL);
		purple_roomlist_room_add_field(irc->roomlist, room, args[1]);
		purple_roomlist_room_add_field(irc->roomlist, room, GINT_TO_POINTER(strtol(args[2], NULL, 10)));
		topic = irc_mirc2txt(args[3]);
		purple_roomlist_room_add_field(irc->roomlist, room, topic);
		g_free(topic);
		purple_roomlist_room_add(irc->roomlist, room);
	}
}

void
irc_msg_topic(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	char *chan, *topic, *msg, *nick, *tmp, *tmp2;
	PurpleConversation *convo;

	if (purple_strequal(name, "topic")) {
		chan = args[0];
		topic = irc_mirc2txt(args[1]);
	} else {
		chan = args[1];
		topic = irc_mirc2txt(args[2]);
	}

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, chan, irc->account);
	if (!convo) {
		purple_debug(PURPLE_DEBUG_ERROR, "irc", "Got a topic for %s, which doesn't exist\n", chan);
		g_free(topic);
		return;
	}

	/* If this is an interactive update, print it out */
	tmp = g_markup_escape_text(topic, -1);
	tmp2 = purple_markup_linkify(tmp);
	g_free(tmp);
	time_t msg_time = irc_parse_server_time(irc, time(NULL));
	if (purple_strequal(name, "topic")) {
		const char *current_topic = purple_conv_chat_get_topic(PURPLE_CONV_CHAT(convo));
		if (!(current_topic != NULL && purple_strequal(tmp2, current_topic))) {
			char *nick_esc;
			nick = irc_mask_nick(from);
			nick_esc = g_markup_escape_text(nick, -1);
			purple_conv_chat_set_topic(PURPLE_CONV_CHAT(convo), nick, topic);
			if (*tmp2)
				msg = g_strdup_printf(_("%s has changed the topic to: %s"), nick_esc, tmp2);
			else
				msg = g_strdup_printf(_("%s has cleared the topic."), nick_esc);
			g_free(nick_esc);
			g_free(nick);
			purple_conv_chat_write(PURPLE_CONV_CHAT(convo), from, msg, PURPLE_MESSAGE_SYSTEM, msg_time);
			g_free(msg);
		}
	} else {
		char *chan_esc = g_markup_escape_text(chan, -1);
		msg = g_strdup_printf(_("The topic for %s is: %s"), chan_esc, tmp2);
		g_free(chan_esc);
		purple_conv_chat_set_topic(PURPLE_CONV_CHAT(convo), NULL, topic);
		purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", msg, PURPLE_MESSAGE_SYSTEM, msg_time);
		g_free(msg);
	}
	g_free(tmp2);
	g_free(topic);
}

void
irc_msg_topicinfo(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *convo;
	struct tm *tm;
	time_t t;
	char *msg, *timestamp, *datestamp;

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[1], irc->account);
	if (!convo) {
		purple_debug(PURPLE_DEBUG_ERROR, "irc", "Got topic info for %s, which doesn't exist\n", args[1]);
		return;
	}

	t = (time_t) atol(args[3]);
	if (t == 0) {
		purple_debug(PURPLE_DEBUG_ERROR, "irc", "Got apparently nonsensical topic timestamp %s\n", args[3]);
		return;
	}
	tm = localtime(&t);

	timestamp = g_strdup(purple_time_format(tm));
	datestamp = g_strdup(purple_date_format_short(tm));
	msg = g_strdup_printf(_("Topic for %s set by %s at %s on %s"), args[1], args[2], timestamp, datestamp);
	purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", msg, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LINKIFY, time(NULL));
	g_free(timestamp);
	g_free(datestamp);
	g_free(msg);
}

void
irc_msg_unknown(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	char *buf;

	g_return_if_fail(gc);

	buf = g_strdup_printf(_("Unknown message '%s'"), args[1]);
	purple_notify_error(gc, _("Unknown message"), buf, _("The IRC server received a message it did not understand."));
	g_free(buf);
}

void
irc_msg_names(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	char *names, *cur, *end, *tmp, *msg;
	PurpleConversation *convo;

	if (purple_strequal(name, "366")) {
		convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY, args[1], irc->account);
		if (!convo) {
			purple_debug(PURPLE_DEBUG_ERROR, "irc", "Got a NAMES list for %s, which doesn't exist\n", args[1]);
			g_string_free(irc->names, TRUE);
			irc->names = NULL;
			return;
		}

		names = cur = g_string_free(irc->names, FALSE);
		irc->names = NULL;
		if (purple_conversation_get_data(convo, IRC_NAMES_FLAG)) {
			msg = g_strdup_printf(_("Users on %s: %s"), args[1], names ? names : "");
			if (purple_conversation_get_type(convo) == PURPLE_CONV_TYPE_CHAT)
				purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", msg, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
			else
				purple_conv_im_write(PURPLE_CONV_IM(convo), "", msg, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
			g_free(msg);
		} else if (cur != NULL) {
			GList *users = NULL;
			GList *flags = NULL;

			while (*cur) {
				PurpleConvChatBuddyFlags f = PURPLE_CBFLAGS_NONE;
				end = strchr(cur, ' ');
				if (!end)
					end = cur + strlen(cur);
				if (*cur == '@') {
					f = PURPLE_CBFLAGS_OP;
					cur++;
				} else if (*cur == '%') {
					f = PURPLE_CBFLAGS_HALFOP;
					cur++;
				} else if (*cur == '+') {
					f = PURPLE_CBFLAGS_VOICE;
					cur++;
				} else if (irc->mode_chars && strchr(irc->mode_chars, *cur)) {
					if (*cur == '~')
						f = PURPLE_CBFLAGS_FOUNDER;
					cur++;
				}
				tmp = g_strndup(cur, end - cur);
				users = g_list_prepend(users, tmp);
				flags = g_list_prepend(flags, GINT_TO_POINTER(f));
				cur = end;
				if (*cur)
					cur++;
			}

			if (users != NULL) {
				GList *l;

				purple_conv_chat_add_users(PURPLE_CONV_CHAT(convo), users, NULL, flags, FALSE);

				for (l = users; l != NULL; l = l->next)
					g_free(l->data);

				g_list_free(users);
				g_list_free(flags);
			}

			purple_conversation_set_data(convo, IRC_NAMES_FLAG, GINT_TO_POINTER(TRUE));
		}
		g_free(names);
	} else {
		if (!irc->names)
			irc->names = g_string_new("");

		if (irc->names->len && irc->names->str[irc->names->len - 1] != ' ')
			irc->names = g_string_append_c(irc->names, ' ');
		irc->names = g_string_append(irc->names, args[3]);
	}
}

void
irc_msg_motd(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	char *escaped;

	if (purple_strequal(name, "375")) {
		if (irc->motd) {
			g_string_free(irc->motd, TRUE);
			irc->motd = NULL;
		}
		irc->motd = g_string_new("");
		return;
	} else if (purple_strequal(name, "376")) {
		/* dircproxy 1.0.5 does not send 251 on reconnection, so
		 * finalize the connection here if it is not already done. */
		irc_connected(irc, args[0]);
		return;
	} else if (purple_strequal(name, "422")) {
		/* in case there is no 251, and no MOTD set, finalize the connection.
		 * (and clear the motd for good measure). */

		if (irc->motd) {
			g_string_free(irc->motd, TRUE);
			irc->motd = NULL;
		}

		irc_connected(irc, args[0]);
		return;
	}

	if (!irc->motd) {
		purple_debug_error("irc", "IRC server sent MOTD without STARTMOTD\n");
		return;
	}

	if (!args[1])
		return;

	escaped = g_markup_escape_text(args[1], -1);
	g_string_append_printf(irc->motd, "%s<br>", escaped);
	g_free(escaped);
}

void
irc_msg_time(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);

	g_return_if_fail(gc);

	purple_notify_message(gc, PURPLE_NOTIFY_MSG_INFO, _("Time Response"), _("The IRC server's local time is:"), args[2], NULL, NULL);
}

void
irc_msg_nochan(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);

	g_return_if_fail(gc);

	purple_notify_error(gc, NULL, _("No such channel"), args[1]);
}

void
irc_msg_nonick(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc;
	PurpleConversation *convo;

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY, args[1], irc->account);
	if (convo) {
		if (purple_conversation_get_type(convo) == PURPLE_CONV_TYPE_CHAT) /* does this happen? */
			purple_conv_chat_write(PURPLE_CONV_CHAT(convo), args[1], _("no such channel"), PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
		else
			purple_conv_im_write(PURPLE_CONV_IM(convo), args[1], _("User is not logged in"), PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
	} else {
		if ((gc = purple_account_get_connection(irc->account)) == NULL)
			return;
		purple_notify_error(gc, NULL, _("No such nick or channel"), args[1]);
	}

	if (irc->whois.nick && !purple_utf8_strcasecmp(irc->whois.nick, args[1])) {
		g_free(irc->whois.nick);
		irc->whois.nick = NULL;
	}
}

void
irc_msg_nosend(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc;
	PurpleConversation *convo;

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[1], irc->account);
	if (convo) {
		purple_conv_chat_write(PURPLE_CONV_CHAT(convo), args[1], args[2], PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
	} else {
		if ((gc = purple_account_get_connection(irc->account)) == NULL)
			return;
		purple_notify_error(gc, NULL, _("Could not send"), args[2]);
	}
}

void
irc_msg_notinchan(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[1], irc->account);

	purple_debug(PURPLE_DEBUG_INFO, "irc", "We're apparently not in %s, but tried to use it\n", args[1]);
	if (convo) {
		/*g_slist_remove(irc->gc->buddy_chats, convo);
		  purple_conversation_set_account(convo, NULL);*/
		purple_conv_chat_write(PURPLE_CONV_CHAT(convo), args[1], args[2], PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
	}
}

void
irc_msg_notop(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *convo;

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[1], irc->account);
	if (!convo)
		return;

	purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", args[2], PURPLE_MESSAGE_SYSTEM, time(NULL));
}

void
irc_msg_invite(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	GHashTable *components;
	gchar *nick;

	g_return_if_fail(gc);
	nick = irc_mask_nick(from);

	// If the invitee is not us, display it as an info message
	if (!purple_strequal(args[0], purple_connection_get_display_name(gc))) {
		PurpleConversation *convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[1], irc->account);
		if (convo) {
			gchar *msg = g_strdup_printf(_("%s has invited %s to the channel %s"), nick, args[0], args[1]);
			purple_conv_chat_write(PURPLE_CONV_CHAT(convo), args[1], msg, PURPLE_MESSAGE_SYSTEM, time(NULL));
			g_free(msg);
		}
		g_free(nick);
		return;
	}

	components = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	g_hash_table_insert(components, g_strdup("channel"), g_strdup(args[1]));

	serv_got_chat_invite(gc, args[1], nick, NULL, components);
	g_free(nick);
}

void
irc_msg_inviting(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *convo;
	const char *nick, *channel;
	char *chan_clean, *sp, *buf;

	if (!args || !args[1] || !args[2])
		return;

	if (irc_ischannel(args[1])) {
		channel = args[1];
		nick = args[2];
	} else {
		nick = args[1];
		channel = args[2];
	}

	chan_clean = g_strdup(channel);
	sp = strchr(chan_clean, ' ');
	if (sp)
		*sp = '\0';

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, chan_clean, irc->account);
	if (convo) {
		buf = g_strdup_printf(_("Invited %s to %s"), nick, chan_clean);
		purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", buf, PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_free(buf);
	}
	g_free(chan_clean);
}

void
irc_msg_inviteonly(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	char *buf;

	g_return_if_fail(gc);

	buf = g_strdup_printf(_("Joining %s requires an invitation."), args[1]);
	purple_notify_error(gc, _("Invitation only"), _("Invitation only"), buf);
	g_free(buf);
}

void
irc_msg_knock(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *convo;
	char *nick, *buf;

	if (!args || !args[1] || !args[2])
		return;

	nick = irc_mask_nick(args[2]);
	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[1], irc->account);
	if (convo) {
		if (args[3] && *args[3]) {
			buf = g_strdup_printf(_("%s has knocked on %s (%s)"), nick, args[1], args[3]);
		} else {
			buf = g_strdup_printf(_("%s has knocked on %s"), nick, args[1]);
		}
		purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", buf, PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_free(buf);
	}
	g_free(nick);
}

void
irc_msg_knockdlvr(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *convo;
	char *buf;

	if (!args || !args[1])
		return;

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[1], irc->account);
	if (convo) {
		buf = g_strdup_printf(_("Your knock to %s has been delivered."), args[1]);
		purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", buf, PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_free(buf);
	} else {
		PurpleConnection *gc = purple_account_get_connection(irc->account);
		if (gc) {
			buf = g_strdup_printf(_("Your knock to %s has been delivered."), args[1]);
			purple_notify_info(gc, _("Knock Delivered"), _("Knock Delivered"), buf);
			g_free(buf);
		}
	}
}

void
irc_msg_knockerr(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	char *buf;
	const char *detail = (args[1] && args[2]) ? args[2] : (args[1] ? args[1] : "");

	g_return_if_fail(gc);

	if (purple_strequal(name, "712")) {
		buf = g_strdup_printf(_("Cannot knock on %s: Too many knocks (%s)."), args[1], detail);
	} else if (purple_strequal(name, "713")) {
		buf = g_strdup_printf(_("Cannot knock on %s: Channel is open (%s)."), args[1], detail);
	} else if (purple_strequal(name, "714")) {
		buf = g_strdup_printf(_("Cannot knock on %s: You are already on that channel (%s)."), args[1], detail);
	} else {
		buf = g_strdup_printf(_("Cannot knock: %s"), detail);
	}

	purple_notify_error(gc, _("Cannot Knock"), _("Cannot Knock"), buf);
	g_free(buf);
}

void
irc_msg_ison(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	char **nicks;
	struct irc_buddy *ib;
	int i;

	nicks = g_strsplit(args[1], " ", -1);
	for (i = 0; nicks[i]; i++) {
		if ((ib = g_hash_table_lookup(irc->buddies, (gconstpointer) nicks[i])) == NULL) {
			continue;
		}
		ib->new_online_status = TRUE;
	}
	g_strfreev(nicks);

	if (irc->ison_outstanding)
		irc_buddy_query(irc);

	if (!irc->ison_outstanding)
		g_hash_table_foreach(irc->buddies, (GHFunc) irc_buddy_status, (gpointer) irc);
}

static void
irc_buddy_status(char *name, struct irc_buddy *ib, struct irc_conn *irc)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	PurpleBuddy *buddy = purple_find_buddy(irc->account, name);

	if (!gc || !buddy)
		return;

	if (ib->online && !ib->new_online_status) {
		purple_prpl_got_user_status(irc->account, name, "offline", NULL);
		ib->online = FALSE;
	} else if (!ib->online && ib->new_online_status) {
		purple_prpl_got_user_status(irc->account, name, "available", NULL);
		ib->online = TRUE;
	}
}

void
irc_msg_join(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	PurpleConversation *convo;
	PurpleConvChat *chat;
	PurpleConvChatBuddy *cb;

	char *nick, *userhost;
	char *chan = NULL, *account = NULL, *realname = NULL;
	char *p, *space;
	struct irc_buddy *ib;
	static int id = 1;

	g_return_if_fail(gc);

	p = args[0];
	space = strchr(p, ' ');
	if (space != NULL) {
		chan = g_strndup(p, space - p);
		p = space + 1;
		while (*p == ' ') p++;
		space = strchr(p, ' ');
		if (space != NULL) {
			account = g_strndup(p, space - p);
			p = space + 1;
			while (*p == ' ') p++;
			if (*p == ':') p++;
			realname = g_strdup(p);
		} else {
			account = g_strdup(p);
		}
	} else {
		chan = g_strdup(p);
	}

	nick = irc_mask_nick(from);

	if (irc_is_batch_chathistory(irc)) {
		time_t msg_time = irc_parse_server_time(irc, time(NULL));
		convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, chan, irc->account);
		if (convo) {
			char *msg;
			if (!purple_utf8_strcasecmp(nick, purple_connection_get_display_name(gc))) {
				msg = g_strdup_printf(_("You joined %s"), chan);
			} else {
				msg = g_strdup_printf(_("%s joined %s"), nick, chan);
			}
			purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "", msg, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, msg_time);
			g_free(msg);
		}
		g_free(nick);
		g_free(chan);
		g_free(account);
		g_free(realname);
		return;
	}

	if (!purple_utf8_strcasecmp(nick, purple_connection_get_display_name(gc))) {
		/* We are joining a channel for the first time */
		serv_got_joined_chat(gc, id++, chan);
		g_free(nick);
		convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT,
													  chan,
													  irc->account);

		if (convo == NULL) {
			purple_debug_error("irc", "tried to join %s but couldn't\n", chan);
			g_free(chan);
			g_free(account);
			g_free(realname);
			return;
		}
		purple_conversation_set_data(convo, IRC_NAMES_FLAG, GINT_TO_POINTER(FALSE));

		// Get the real name and user host for all participants.
		irc_send_who(irc, chan);

		if (irc->cap_chathistory) {
			guint limit = irc->chathistory_limit > 0 ? irc->chathistory_limit : 50;
			char *limit_str = g_strdup_printf("%u", limit);
			const char *last_time = irc_get_last_msg_time(irc, chan);
			char *ref_param;
			if (last_time && *last_time) {
				ref_param = g_strdup_printf("timestamp=%s", last_time);
			} else {
				ref_param = g_strdup("*");
			}
			char *buf = irc_format(irc, "vvvvn", "CHATHISTORY", "LATEST", chan, ref_param, limit_str);
			irc_send(irc, buf);
			g_free(buf);
			g_free(ref_param);
			g_free(limit_str);
		}

		/* Until purple_conversation_present does something that
		 * one would expect in Pidgin, this call produces buggy
		 * behavior both for the /join and auto-join cases. */
		/* purple_conversation_present(convo); */
		g_free(chan);
		g_free(account);
		g_free(realname);
		return;
	}

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, chan, irc->account);
	if (convo == NULL) {
		purple_debug(PURPLE_DEBUG_ERROR, "irc", "JOIN for %s failed\n", chan);
		g_free(nick);
		g_free(chan);
		g_free(account);
		g_free(realname);
		return;
	}

	userhost = irc_mask_userhost(from);
	chat = PURPLE_CONV_CHAT(convo);

	purple_conv_chat_add_user(chat, nick, userhost, PURPLE_CBFLAGS_NONE, TRUE);

	cb = purple_conv_chat_cb_find(chat, nick);

	if (cb) {
		GList *keys = NULL, *values = NULL;

		keys = g_list_prepend(keys, "userhost");
		values = g_list_prepend(values, userhost);

		if (realname && *realname) {
			keys = g_list_prepend(keys, "realname");
			values = g_list_prepend(values, realname);
		}
		if (account && strcmp(account, "*") != 0) {
			keys = g_list_prepend(keys, "account");
			values = g_list_prepend(values, account);
		}

		purple_conv_chat_cb_set_attributes(chat, cb, keys, values);
		g_list_free(keys);
		g_list_free(values);
	}

	PurpleBuddy *buddy = purple_find_buddy(irc->account, nick);
	if (buddy) {
		if (account && strcmp(account, "*") != 0) {
			purple_blist_node_set_string(PURPLE_BLIST_NODE(buddy), "account", account);
		}
		if (userhost && *userhost) {
			purple_blist_node_set_string(PURPLE_BLIST_NODE(buddy), "userhost", userhost);
		}
	}

	if ((ib = g_hash_table_lookup(irc->buddies, nick)) != NULL) {
		ib->new_online_status = TRUE;
		irc_buddy_status(nick, ib, irc);
	}

	g_free(userhost);
	g_free(nick);
	g_free(chan);
	g_free(account);
	g_free(realname);
}

void
irc_msg_kick(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	PurpleConversation *convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[0], irc->account);
	char *nick, *buf;

	g_return_if_fail(gc);

	nick = irc_mask_nick(from);

	if (irc_is_batch_chathistory(irc)) {
		time_t msg_time = irc_parse_server_time(irc, time(NULL));
		if (!purple_utf8_strcasecmp(purple_connection_get_display_name(gc), args[1])) {
			buf = g_strdup_printf(_("You were kicked by %s: (%s)"), nick, args[2]);
		} else {
			buf = g_strdup_printf(_("%s was kicked by %s (%s)"), args[1], nick, args[2]);
		}
		purple_conv_chat_write(PURPLE_CONV_CHAT(convo), args[0], buf, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, msg_time);
		g_free(buf);
		g_free(nick);
		return;
	}

	if (!convo) {
		purple_debug(PURPLE_DEBUG_ERROR, "irc", "Received a KICK for unknown channel %s\n", args[0]);
		g_free(nick);
		return;
	}

	if (!purple_utf8_strcasecmp(purple_connection_get_display_name(gc), args[1])) {
		buf = g_strdup_printf(_("You have been kicked by %s: (%s)"), nick, args[2]);
		purple_conv_chat_write(PURPLE_CONV_CHAT(convo), args[0], buf, PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_free(buf);
		serv_got_chat_left(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(convo)));
	} else {
		buf = g_strdup_printf(_("Kicked by %s (%s)"), nick, args[2]);
		purple_conv_chat_remove_user(PURPLE_CONV_CHAT(convo), args[1], buf);
		g_free(buf);
	}

	g_free(nick);
	return;
}

void
irc_msg_mode(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *convo;
	char *nick = irc_mask_nick(from), *buf;

	if (*args[0] == '#' || *args[0] == '&') { /* Channel	*/
		char *escaped;
		convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[0], irc->account);
		if (!convo) {
			purple_debug(PURPLE_DEBUG_ERROR, "irc", "MODE received for %s, which we are not in\n", args[0]);
			g_free(nick);
			return;
		}
		time_t msg_time = irc_parse_server_time(irc, time(NULL));
		escaped = (args[2] != NULL) ? g_markup_escape_text(args[2], -1) : NULL;
		buf = g_strdup_printf(_("mode (%s %s) by %s"), args[1], escaped ? escaped : "", nick);
		purple_conv_chat_write(PURPLE_CONV_CHAT(convo), args[0], buf, PURPLE_MESSAGE_SYSTEM, msg_time);
		g_free(escaped);
		g_free(buf);
		if (args[2]) {
			PurpleConvChatBuddyFlags newflag, flags;
			char *mcur, *cur, *end, *user;
			gboolean add = FALSE;
			mcur = args[1];
			cur = args[2];
			while (*cur && *mcur) {
				if ((*mcur == '+') || (*mcur == '-')) {
					add = (*mcur == '+') ? TRUE : FALSE;
					mcur++;
					continue;
				}
				end = strchr(cur, ' ');
				if (!end)
					end = cur + strlen(cur);
				user = g_strndup(cur, end - cur);
				flags = purple_conv_chat_user_get_flags(PURPLE_CONV_CHAT(convo), user);
				newflag = PURPLE_CBFLAGS_NONE;
				if (*mcur == 'o')
					newflag = PURPLE_CBFLAGS_OP;
				else if (*mcur == 'h')
					newflag = PURPLE_CBFLAGS_HALFOP;
				else if (*mcur == 'v')
					newflag = PURPLE_CBFLAGS_VOICE;
				else if (irc->mode_chars && strchr(irc->mode_chars, '~') && (*mcur == 'q'))
					newflag = PURPLE_CBFLAGS_FOUNDER;
				if (newflag) {
					if (add)
						flags |= newflag;
					else
						flags &= ~newflag;
					purple_conv_chat_user_set_flags(PURPLE_CONV_CHAT(convo), user, flags);
				}
				g_free(user);
				cur = end;
				if (*cur)
					cur++;
				if (*mcur)
					mcur++;
			}
		}
	} else { /* User		*/
	}
	g_free(nick);
}

void
irc_msg_nick(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	PurpleConversation *conv;
	GSList *chats;
	char *nick = irc_mask_nick(from);

	irc->nickused = FALSE;

	if (!gc) {
		g_free(nick);
		return;
	}

	if (irc_is_batch_chathistory(irc)) {
		time_t msg_time = irc_parse_server_time(irc, time(NULL));
		char *msg = g_strdup_printf(_("%s is now known as %s"), nick, args[0]);
		GSList *chats = gc->buddy_chats;
		while (chats) {
			PurpleConvChat *chat = PURPLE_CONV_CHAT(chats->data);
			purple_conv_chat_write(chat, "", msg, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, msg_time);
			chats = chats->next;
		}
		g_free(msg);
		g_free(nick);
		return;
	}

	chats = gc->buddy_chats;

	if (!purple_utf8_strcasecmp(nick, purple_connection_get_display_name(gc))) {
		purple_connection_set_display_name(gc, args[0]);
	}

	while (chats) {
		PurpleConvChat *chat = PURPLE_CONV_CHAT(chats->data);
		/* This is ugly ... */
		if (purple_conv_chat_find_user(chat, nick))
			purple_conv_chat_rename_user(chat, nick, args[0]);
		chats = chats->next;
	}

	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, nick, irc->account);
	if (conv != NULL)
		purple_conversation_set_name(conv, args[0]);

	g_free(nick);
}

void
irc_msg_badnick(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	if (purple_connection_get_state(gc) == PURPLE_CONNECTED) {
		purple_notify_error(gc, _("Invalid nickname"), _("Invalid nickname"), _("Your selected nickname was rejected by the server.  It probably contains invalid characters."));

	} else {
		purple_connection_error_reason(gc,
									   PURPLE_CONNECTION_ERROR_INVALID_SETTINGS,
									   _("Your selected account name was rejected by the server.  It probably contains invalid characters."));
	}
}

void
irc_msg_nickused(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	char *newnick, *buf, *end;
	PurpleConnection *gc = purple_account_get_connection(irc->account);

	if (gc && purple_connection_get_state(gc) == PURPLE_CONNECTED) {
		/* We only want to do the following dance if the connection
		   has not been successfully completed.  If it has, just
		   notify the user that their /nick command didn't go. */
		buf = g_strdup_printf(_("The nickname \"%s\" is already being used."),
							  irc->reqnick);
		purple_notify_error(gc, _("Nickname in use"), _("Nickname in use"), buf);
		g_free(buf);
		g_free(irc->reqnick);
		irc->reqnick = NULL;
		return;
	}

	if (strlen(args[1]) < strlen(irc->reqnick) || irc->nickused)
		newnick = g_strdup(args[1]);
	else
		newnick = g_strdup_printf("%s0", args[1]);
	end = newnick + strlen(newnick) - 1;
	/* try fallbacks */
	if ((*end < '9') && (*end >= '1')) {
		*end = *end + 1;
	} else
		*end = '1';

	g_free(irc->reqnick);
	irc->reqnick = newnick;
	irc->nickused = TRUE;

	purple_connection_set_display_name(
	  purple_account_get_connection(irc->account), newnick);

	buf = irc_format(irc, "vn", "NICK", newnick);
	irc_send(irc, buf);
	g_free(buf);
}

void
irc_msg_notice(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	irc_msg_handle_privmsg(irc, name, from, args[0], args[1], TRUE);
}

void
irc_msg_nochangenick(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);

	g_return_if_fail(gc);

	purple_notify_error(gc, _("Cannot change nick"), _("Could not change nick"), args[2]);
}

void
irc_msg_part(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	PurpleConversation *convo;
	char *nick, *msg, *channel;

	g_return_if_fail(gc);

	/* Undernet likes to :-quote the channel name, for no good reason
	 * that I can see.  This catches that. */
	channel = (args[0][0] == ':') ? &args[0][1] : args[0];

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, channel, irc->account);
	if (!convo) {
		purple_debug(PURPLE_DEBUG_INFO, "irc", "Got a PART on %s, which doesn't exist -- probably closed\n", channel);
		return;
	}

	nick = irc_mask_nick(from);

	if (irc_is_batch_chathistory(irc)) {
		time_t msg_time = irc_parse_server_time(irc, time(NULL));
		if (!purple_utf8_strcasecmp(nick, purple_connection_get_display_name(gc))) {
			char *escaped = args[1] ? g_markup_escape_text(args[1], -1) : NULL;
			msg = g_strdup_printf(_("You parted the channel%s%s"),
								  (args[1] && *args[1]) ? ": " : "",
								  (escaped && *escaped) ? escaped : "");
			g_free(escaped);
			purple_conv_chat_write(PURPLE_CONV_CHAT(convo), channel, msg, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, msg_time);
			g_free(msg);
		} else {
			char *escaped = args[1] ? g_markup_escape_text(args[1], -1) : NULL;
			msg = g_strdup_printf(_("%s has parted %s%s%s"),
								  nick, channel,
								  (args[1] && *args[1]) ? ": " : "",
								  (escaped && *escaped) ? escaped : "");
			g_free(escaped);
			purple_conv_chat_write(PURPLE_CONV_CHAT(convo), channel, msg, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, msg_time);
			g_free(msg);
		}
		g_free(nick);
		return;
	}

	if (!purple_utf8_strcasecmp(nick, purple_connection_get_display_name(gc))) {
		char *escaped = args[1] ? g_markup_escape_text(args[1], -1) : NULL;
		msg = g_strdup_printf(_("You have parted the channel%s%s"),
							  (args[1] && *args[1]) ? ": " : "",
							  (escaped && *escaped) ? escaped : "");
		g_free(escaped);
		purple_conv_chat_write(PURPLE_CONV_CHAT(convo), channel, msg, PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_free(msg);
		serv_got_chat_left(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(convo)));
	} else {
		msg = args[1] ? irc_mirc2txt(args[1]) : NULL;
		purple_conv_chat_remove_user(PURPLE_CONV_CHAT(convo), nick, msg);
		g_free(msg);
	}
	g_free(nick);
}

void
irc_msg_ping(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	char *buf;

	buf = irc_format(irc, "v:", "PONG", args[0]);
	irc_priority_send(irc, buf);
	g_free(buf);
}

void
irc_msg_pong(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConversation *convo;
	PurpleConnection *gc;
	char **parts, *msg;
	time_t oldstamp;

	parts = g_strsplit(args[1], " ", 2);

	if (!parts[0] || !parts[1]) {
		g_strfreev(parts);
		return;
	}

	if (sscanf(parts[1], "%lu", &oldstamp) != 1) {
		msg = g_strdup(_("Error: invalid PONG from server"));
	} else {
		msg = g_strdup_printf(_("PING reply -- Lag: %lu seconds"), time(NULL) - oldstamp);
	}

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY, parts[0], irc->account);
	g_strfreev(parts);
	if (convo) {
		if (purple_conversation_get_type(convo) == PURPLE_CONV_TYPE_CHAT)
			purple_conv_chat_write(PURPLE_CONV_CHAT(convo), "PONG", msg, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
		else
			purple_conv_im_write(PURPLE_CONV_IM(convo), "PONG", msg, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
	} else {
		gc = purple_account_get_connection(irc->account);
		if (!gc) {
			g_free(msg);
			return;
		}
		purple_notify_info(gc, NULL, "PONG", msg);
	}
	g_free(msg);
}

void
irc_msg_privmsg(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	irc_msg_handle_privmsg(irc, name, from, args[0], args[1], FALSE);
}

static void
irc_msg_handle_privmsg(struct irc_conn *irc, const char *name, const char *from, const char *to, const char *rawmsg, gboolean notice)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	PurpleConversation *convo;
	char *tmp;
	char *msg;
	char *nick;

	if (!gc)
		return;

	nick = irc_mask_nick(from);
	tmp = irc_parse_ctcp(irc, nick, to, rawmsg, notice);
	if (!tmp) {
		g_free(nick);
		return;
	}

	msg = irc_escape_privmsg(tmp, -1);
	g_free(tmp);

	tmp = irc_mirc2html(msg);
	g_free(msg);
	msg = tmp;
	if (notice) {
		tmp = g_strdup_printf("(notice) %s", msg);
		g_free(msg);
		msg = tmp;
	}

	time_t now = time(NULL);
	gboolean self_sent = FALSE;
	const gchar *time_tag = NULL;
	if (irc->current_tags) {
		// look for @label in current_tags string, check irc->sent_messages to see if we sent it, and skip if we did
		gchar **tags = g_strsplit(irc->current_tags, ";", -1);
		int i;
		for (i = 0; tags[i] != NULL; i++) {
			if (g_str_has_prefix(tags[i], "label=")) {
				if (g_hash_table_remove(irc->sent_messages, tags[i] + 6)) { // Skip "label="
					if (!purple_utf8_strcasecmp(nick, purple_connection_get_display_name(gc))) {
						self_sent = TRUE;
					}
				}
			} else if (g_str_has_prefix(tags[i], "time=")) {
				time_tag = tags[i] + 5; // Skip "time="
				now = purple_str_to_time(time_tag, TRUE, NULL, NULL, NULL);
			} else if (g_str_has_prefix(tags[i], "account=")) {
				const gchar *acc_str = tags[i] + 8; // Skip "account="
				if (acc_str && *acc_str) {
					PurpleBuddy *buddy = purple_find_buddy(irc->account, nick);
					if (buddy) {
						purple_blist_node_set_string(PURPLE_BLIST_NODE(buddy), "account", acc_str);
					}
					convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, irc_nick_skip_mode(irc, to), irc->account);
					if (convo) {
						PurpleConvChat *chat = PURPLE_CONV_CHAT(convo);
						PurpleConvChatBuddy *cb = purple_conv_chat_cb_find(chat, nick);
						if (cb) {
							purple_conv_chat_cb_set_attribute(chat, cb, "account", acc_str);
						}
					}
				}
			} else if (g_str_has_prefix(tags[i], "bot")) {
				PurpleBuddy *buddy = purple_find_buddy(irc->account, nick);
				if (buddy) {
					purple_blist_node_set_bool(PURPLE_BLIST_NODE(buddy), "bot", TRUE);
				}
				convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, irc_nick_skip_mode(irc, to), irc->account);
				if (convo) {
					PurpleConvChat *chat = PURPLE_CONV_CHAT(convo);
					PurpleConvChatBuddy *cb = purple_conv_chat_cb_find(chat, nick);
					if (cb) {
						purple_conv_chat_cb_set_attribute(chat, cb, "bot", "TRUE");
					}
				}
			} else if (g_str_has_prefix(tags[i], "batch=")) {
				const char *batch_ref = tags[i] + 6;
				struct irc_batch *batch = g_hash_table_lookup(irc->active_batches, batch_ref);
				if (batch) {
					if (batch->self_sent && !purple_utf8_strcasecmp(nick, purple_connection_get_display_name(gc))) {
						self_sent = TRUE;
					}
					if (g_strcmp0(batch->type, "draft/multiline") == 0 || g_strcmp0(batch->type, "multiline") == 0) {
						gboolean has_concat = FALSE;
						int j;
						for (j = 0; tags[j] != NULL; j++) {
							if (g_strcmp0(tags[j], "draft/multiline-concat") == 0 || g_strcmp0(tags[j], "multiline-concat") == 0) {
								has_concat = TRUE;
								break;
							}
						}
						if (!batch->from)
							batch->from = g_strdup(nick);

						if (batch->last_concat) {
							g_string_append(batch->content, rawmsg);
						} else {
							if (batch->content->len > 0)
								g_string_append_c(batch->content, '\n');
							g_string_append(batch->content, rawmsg);
						}
						batch->last_concat = has_concat;

						g_strfreev(tags);
						g_free(nick);
						return;
					}
				}
			}
		}
		g_strfreev(tags);
	}

	char iso_buf[64];
	if (time_tag) {
		g_strlcpy(iso_buf, time_tag, sizeof(iso_buf));
	} else {
		struct tm *tm = gmtime(&now);
		strftime(iso_buf, sizeof(iso_buf), "%Y-%m-%dT%H:%M:%SZ", tm);
	}

	const char *target = !purple_utf8_strcasecmp(to, purple_connection_get_display_name(gc)) ? nick : to;
	irc_set_last_msg_time(irc, target, iso_buf);

	if (strchr(rawmsg, '\007') != NULL) {
		purple_prpl_got_attention(gc, nick, 0);
	}

	if (irc_ischannel(to)) {
		convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, irc_nick_skip_mode(irc, to), irc->account);
		if (convo) {
			if (self_sent) {
				/* Message was sent locally by this client and already displayed in irc_chat_send */
			} else if (!purple_utf8_strcasecmp(nick, purple_connection_get_display_name(gc))) {
				purple_conversation_write(convo, nick, msg, PURPLE_MESSAGE_SEND, now);
			} else {
				serv_got_chat_in(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(convo)), nick, notice ? PURPLE_MESSAGE_NOTIFY : 0, msg, now);
			}
		} else {
			purple_debug_error("irc", "Got a %s on %s, which does not exist\n", notice ? "NOTICE" : "PRIVMSG", to);
		}
	} else {
		if (!purple_utf8_strcasecmp(to, purple_connection_get_display_name(gc)) || g_strcmp0(to, "*") == 0) {
			if (self_sent && !purple_utf8_strcasecmp(nick, purple_connection_get_display_name(gc))) {
				/* Self-sent message echoed back to us; already displayed on send */
			} else {
				serv_got_im(gc, nick, msg, notice ? PURPLE_MESSAGE_NOTIFY : 0, now);
			}
		} else {
			if (!self_sent) {
				convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, to, irc->account);
				if (convo) {
					purple_conversation_write(convo, nick, msg, PURPLE_MESSAGE_SEND, now);
				} else {
					serv_got_im(gc, to, msg, PURPLE_MESSAGE_SEND, now);
				}
			}
		}
	}
	g_free(msg);
	g_free(nick);
}

void
irc_msg_regonly(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	PurpleConversation *convo;
	char *msg;

	g_return_if_fail(gc);

	convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, args[1], irc->account);
	if (convo) {
		/* This is a channel we're already in; for some reason,
		 * freenode feels the need to notify us that in some
		 * hypothetical other situation this might not have
		 * succeeded.  Suppress that. */
		return;
	}

	msg = g_strdup_printf(_("Cannot join %s: Registration is required."), args[1]);
	purple_notify_error(gc, _("Cannot join channel"), msg, args[2]);
	g_free(msg);
}

void
irc_msg_quit(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	struct irc_buddy *ib;
	char *data[2];

	g_return_if_fail(gc);

	if (irc_is_batch_chathistory(irc)) {
		time_t msg_time = irc_parse_server_time(irc, time(NULL));
		char *nick = irc_mask_nick(from);
		char *escaped = args[0] ? g_markup_escape_text(args[0], -1) : NULL;
		char *msg = g_strdup_printf(_("%s has quit%s%s"),
									nick,
									(args[0] && *args[0]) ? ": " : "",
									(escaped && *escaped) ? escaped : "");
		g_free(escaped);
		GSList *chats = gc->buddy_chats;
		while (chats) {
			PurpleConvChat *chat = PURPLE_CONV_CHAT(chats->data);
			purple_conv_chat_write(chat, "", msg, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, msg_time);
			chats = chats->next;
		}
		g_free(msg);
		g_free(nick);
		return;
	}

	data[0] = irc_mask_nick(from);
	data[1] = args[0];
	/* XXX this should have an API, I shouldn't grab this directly */
	g_slist_foreach(gc->buddy_chats, (GFunc) irc_chat_remove_buddy, data);

	if ((ib = g_hash_table_lookup(irc->buddies, data[0])) != NULL) {
		ib->new_online_status = FALSE;
		irc_buddy_status(data[0], ib, irc);
	}
	g_free(data[0]);

	return;
}

void
irc_msg_unavailable(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);

	purple_notify_error(gc, NULL, _("Nick or channel is temporarily unavailable."), args[1]);
}

void
irc_msg_wallops(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	char *nick, *msg;

	g_return_if_fail(gc);

	nick = irc_mask_nick(from);
	msg = g_strdup_printf(_("Wallops from %s"), nick);
	g_free(nick);
	purple_notify_info(gc, NULL, msg, args[0]);
	g_free(msg);
}

#ifdef HAVE_CYRUS_SASL
static int
irc_sasl_cb_secret(sasl_conn_t *conn, void *ctx, int id, sasl_secret_t **secret)
{
	struct irc_conn *irc = ctx;
	sasl_secret_t *sasl_secret;
	const char *pw;
	size_t len;

	pw = purple_account_get_password(irc->account);

	if (!conn || !secret || id != SASL_CB_PASS)
		return SASL_BADPARAM;

	len = strlen(pw);
	/* Not an off-by-one because sasl_secret_t defines char data[1] */
	/* TODO: This can probably be moved to glib's allocator */
	sasl_secret = malloc(sizeof(sasl_secret_t) + len);
	if (!sasl_secret)
		return SASL_NOMEM;

	sasl_secret->len = len;
	strcpy((char *) sasl_secret->data, pw);

	*secret = sasl_secret;
	return SASL_OK;
}

static int
irc_sasl_cb_log(void *context, int level, const char *message)
{
	if (level <= SASL_LOG_TRACE)
		purple_debug_info("sasl", "%s\n", message);

	return SASL_OK;
}

static int
irc_sasl_cb_simple(void *ctx, int id, const char **res, unsigned *len)
{
	struct irc_conn *irc = ctx;
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	const char *saslname =
	  purple_account_get_string(irc->account, "saslname", NULL);

	if (saslname == NULL || *saslname == '\0') {
		saslname = purple_connection_get_display_name(gc);
	}

	switch (id) {
	case SASL_CB_AUTHNAME:
		*res = saslname;
		break;
	case SASL_CB_USER:
		*res = "";
		break;
	default:
		return SASL_BADPARAM;
	}
	if (len)
		*len = strlen((char *) *res);
	return SASL_OK;
}

static void
irc_auth_start_cyrus(struct irc_conn *irc)
{
	int ret = 0;
	char *buf;
	sasl_security_properties_t secprops;
	PurpleAccount *account = irc->account;
	PurpleConnection *gc = purple_account_get_connection(account);

	gboolean plaintext;
	gboolean again = FALSE;

	/* Set up security properties and options */
	secprops.min_ssf = 0;
	secprops.security_flags = SASL_SEC_NOANONYMOUS;

	if (!irc->gsc) {
		secprops.max_ssf = -1;
		secprops.maxbufsize = 4096;
		plaintext = purple_account_get_bool(account, "auth_plain_in_clear", FALSE);
		if (!plaintext)
			secprops.security_flags |= SASL_SEC_NOPLAINTEXT;
	} else {
		secprops.max_ssf = 0;
		secprops.maxbufsize = 0;
		/* this isn't checked anywhere else, but I'm going to guess it's because
		 * something is incomplete?.
		 */
		/* plaintext = TRUE; */
	}

	secprops.property_names = 0;
	secprops.property_values = 0;

	do {
		gchar *tmp = NULL;
		again = FALSE;

		ret = sasl_client_new("irc", irc->server, NULL, NULL, irc->sasl_cb, 0, &irc->sasl_conn);

		if (ret != SASL_OK) {
			purple_debug_error("irc", "sasl_client_new failed: %d\n", ret);
			tmp = g_strdup_printf(_("Failed to initialize SASL authentication: %s"),
								  sasl_errdetail(irc->sasl_conn));
			purple_connection_error_reason(gc,
										   PURPLE_CONNECTION_ERROR_OTHER_ERROR,
										   tmp);
			g_free(tmp);
			return;
		}

		sasl_setprop(irc->sasl_conn, SASL_AUTH_EXTERNAL, irc->account->username);
		if (irc->gsc) {
			sasl_ssf_t ssf = 256;
			sasl_setprop(irc->sasl_conn, SASL_SSF, &ssf);
		}
		sasl_setprop(irc->sasl_conn, SASL_SEC_PROPS, &secprops);

		ret = sasl_client_start(irc->sasl_conn, irc->sasl_mechs->str, NULL, NULL, NULL, &irc->current_mech);

		switch (ret) {
		case SASL_OK:
		case SASL_CONTINUE:
			irc->mech_works = FALSE;
			break;
		case SASL_NOMECH:
			purple_connection_error_reason(gc,
										   PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE,
										   _("SASL authentication failed: No worthy authentication mechanisms found."));

			irc_sasl_finish(irc);
			return;
		case SASL_BADPARAM:
		case SASL_NOMEM:
			tmp = g_strdup_printf(_("SASL authentication failed: %s"), sasl_errdetail(irc->sasl_conn));
			purple_connection_error_reason(gc,
										   PURPLE_CONNECTION_ERROR_OTHER_ERROR,
										   tmp);
			g_free(tmp);

			irc_sasl_finish(irc);
			return;
		default:
			purple_debug_error("irc", "sasl_client_start failed: %s\n", sasl_errdetail(irc->sasl_conn));

			if (irc->current_mech && *irc->current_mech) {
				char *pos;
				if ((pos = strstr(irc->sasl_mechs->str, irc->current_mech))) {
					size_t index = pos - irc->sasl_mechs->str;
					g_string_erase(irc->sasl_mechs, index, strlen(irc->current_mech));

					/* Remove space which separated this mech from the next */
					if ((irc->sasl_mechs->str)[index] == ' ') {
						g_string_erase(irc->sasl_mechs, index, 1);
					}
				}

				again = TRUE;
			}
			irc_sasl_finish(irc);
		}
	} while (again);

	purple_debug_info("irc", "Using SASL: %s\n", irc->current_mech);

	buf = irc_format(irc, "vv", "AUTHENTICATE", irc->current_mech);
	irc_priority_send(irc, buf);
	g_free(buf);
}

void
irc_msg_auth(struct irc_conn *irc, char *arg)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	char *buf, *authinfo;
	char *serverin = NULL;
	gsize serverinlen = 0;
	const gchar *c_out;
	unsigned int clen;
	int ret;

	irc->mech_works = TRUE;

	if (!arg)
		return;

	if (arg[0] != '+')
		serverin = (char *) purple_base64_decode(arg, &serverinlen);

	ret = sasl_client_step(irc->sasl_conn, serverin, serverinlen, NULL, &c_out, &clen);

	if (ret != SASL_OK && ret != SASL_CONTINUE) {

		gchar *tmp = g_strdup_printf(_("SASL authentication failed: %s"),
									 sasl_errdetail(irc->sasl_conn));
		purple_connection_error_reason(gc,
									   PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE,
									   tmp);
		g_free(tmp);

		irc_sasl_finish(irc);
		g_free(serverin);
		return;
	}

	if (clen > 0)
		authinfo = purple_base64_encode((const guchar *) c_out, clen);
	else
		authinfo = g_strdup("+");

	buf = irc_format(irc, "vv", "AUTHENTICATE", authinfo);
	irc_priority_send(irc, buf);
	g_free(buf);
	g_free(authinfo);
	g_free(serverin);
}

void
irc_msg_authenticate(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	irc_msg_auth(irc, args[0]);
}

void
irc_msg_authok(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	char *buf;

	sasl_dispose(&irc->sasl_conn);
	irc->sasl_conn = NULL;
	purple_debug_info("irc", "Successfully authenticated using SASL.\n");

	/* Finish auth session */
	buf = irc_format(irc, "vv", "CAP", "END");
	irc_priority_send(irc, buf);
	g_free(buf);
}

void
irc_msg_authtryagain(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);

	/* We already received at least one AUTHENTICATE reply from the
	 * server. This suggests it supports this mechanism, but the
	 * password was incorrect. It would be better to abort and inform
	 * the user than to try again with a different mechanism, so they
	 * aren't told the server supports no worthy mechanisms.
	 */
	if (irc->mech_works) {
		purple_connection_error_reason(gc,
									   PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
									   _("Incorrect Password"));

		irc_sasl_finish(irc);

		return;
	}

	if (irc->current_mech) {
		char *pos;
		if ((pos = strstr(irc->sasl_mechs->str, irc->current_mech))) {
			size_t index = pos - irc->sasl_mechs->str;
			g_string_erase(irc->sasl_mechs, index, strlen(irc->current_mech));

			/* Remove space which separated this mech from the next */
			if ((irc->sasl_mechs->str)[index] == ' ') {
				g_string_erase(irc->sasl_mechs, index, 1);
			}
		}
	}
	if (*irc->sasl_mechs->str) {
		sasl_dispose(&irc->sasl_conn);

		purple_debug_info("irc", "Now trying with %s\n", irc->sasl_mechs->str);
		irc_auth_start_cyrus(irc);
	} else {
		purple_connection_error_reason(gc,
									   PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE,
									   _("SASL authentication failed: No worthy mechanisms found"));

		irc_sasl_finish(irc);
	}
}

void
irc_msg_authfail(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);

	/* Only show an error if we did not abort ourselves. */
	if (irc->sasl_conn) {
		purple_debug_info("irc", "SASL authentication failed: %s", sasl_errdetail(irc->sasl_conn));

		purple_connection_error_reason(gc,
									   PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
									   _("Incorrect Password"));
	}

	irc_sasl_finish(irc);
}

void
irc_msg_saslmechs(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	if (!args || !args[1])
		return;

	purple_debug_info("irc", "Server advertised SASL mechanisms: %s\n", args[1]);

	if (irc->sasl_mechs) {
		gchar **server_mechs = g_strsplit(args[1], ",", -1);
		GString *filtered = g_string_new("");
		int i;

		/* If we have a client certificate and server supports EXTERNAL, prioritize it */
		if (irc->tls_cert_path && irc->gsc) {
			for (i = 0; server_mechs[i] != NULL; i++) {
				if (g_ascii_strcasecmp(server_mechs[i], "EXTERNAL") == 0) {
					g_string_append(filtered, "EXTERNAL ");
					break;
				}
			}
		}

		for (i = 0; server_mechs[i] != NULL; i++) {
			if (g_ascii_strcasecmp(server_mechs[i], "EXTERNAL") == 0)
				continue;
			if (strstr(irc->sasl_mechs->str, server_mechs[i])) {
				g_string_append_printf(filtered, "%s ", server_mechs[i]);
			}
		}

		g_strfreev(server_mechs);

		if (filtered->len > 0) {
			g_string_truncate(filtered, filtered->len - 1);
			g_string_free(irc->sasl_mechs, TRUE);
			irc->sasl_mechs = filtered;
			purple_debug_info("irc", "Filtered SASL mechanisms: %s\n", irc->sasl_mechs->str);
		} else {
			g_string_free(filtered, TRUE);
		}
	}
}

static void
irc_sasl_finish(struct irc_conn *irc)
{
	char *buf;

	sasl_dispose(&irc->sasl_conn);
	irc->sasl_conn = NULL;

	g_free(irc->sasl_cb);
	irc->sasl_cb = NULL;

	/* Auth failed, abort */
	buf = irc_format(irc, "vv", "CAP", "END");
	irc_priority_send(irc, buf);
	g_free(buf);
}
#endif

static void
irc_parse_sts(struct irc_conn *irc, const char *val)
{
	gchar **tokens;
	int i;
	int port = 0;
	int duration = -1;

	if (!val)
		return;

	tokens = g_strsplit(val, ",", -1);
	for (i = 0; tokens[i] != NULL; i++) {
		if (strncmp(tokens[i], "port=", 5) == 0) {
			port = atoi(tokens[i] + 5);
		} else if (strncmp(tokens[i], "duration=", 9) == 0) {
			duration = atoi(tokens[i] + 9);
		}
	}
	g_strfreev(tokens);

	if (irc->gsc == NULL) {
		/* Insecure connection: process upgrade policy */
		if (port > 0) {
			purple_debug_info("irc", "STS upgrade policy received: switching to SSL on port %d\n", port);
			purple_account_set_bool(irc->account, "ssl", TRUE);
			purple_account_set_int(irc->account, "port", port);
			purple_account_set_int(irc->account, "sts_port", port);

			purple_account_disconnect(irc->account);
			purple_account_connect(irc->account);
		}
	} else {
		/* Secure connection: process persistence policy */
		if (duration >= 0) {
			if (duration > 0) {
				time_t expiry = time(NULL) + duration;
				purple_debug_info("irc", "STS persistence policy saved: duration %d, expiry %ld\n", duration, (long) expiry);
				purple_account_set_int(irc->account, "sts_duration", duration);
				purple_account_set_int(irc->account, "sts_expiry", (int) expiry);
			} else {
				purple_debug_info("irc", "STS persistence policy cleared\n");
				purple_account_set_int(irc->account, "sts_duration", 0);
				purple_account_set_int(irc->account, "sts_expiry", 0);
			}
		}
	}
}

/* IRCv3 capabilities negotiation */
void
irc_msg_cap(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	const char *subcmd = args[1];
	char *caps = g_strstrip(args[2]);

	if (strncmp(subcmd, "LS", 2) == 0) {
		gchar **cap_array;
		int i;
		GString *req = g_string_new("");

		cap_array = g_strsplit(caps, " ", -1);
		for (i = 0; cap_array[i] != NULL; i++) {
			if (strcmp(cap_array[i], "message-tags") == 0) {
				g_string_append(req, "message-tags ");
			} else if (strcmp(cap_array[i], "echo-message") == 0) {
				g_string_append(req, "echo-message ");
			} else if (strcmp(cap_array[i], "labeled-response") == 0) {
				g_string_append(req, "labeled-response ");
			} else if (strcmp(cap_array[i], "server-time") == 0) {
				g_string_append(req, "server-time ");
			} else if (strcmp(cap_array[i], "invite-notify") == 0) {
				g_string_append(req, "invite-notify ");
			} else if (strcmp(cap_array[i], "away-notify") == 0) {
				g_string_append(req, "away-notify ");
			} else if (strcmp(cap_array[i], "extended-join") == 0) {
				g_string_append(req, "extended-join ");
			} else if (strcmp(cap_array[i], "account-notify") == 0) {
				g_string_append(req, "account-notify ");
			} else if (strcmp(cap_array[i], "account-tag") == 0) {
				g_string_append(req, "account-tag ");
			} else if (strcmp(cap_array[i], "chghost") == 0) {
				g_string_append(req, "chghost ");
			} else if (strcmp(cap_array[i], "extended-monitor") == 0 || strcmp(cap_array[i], "draft/extended-monitor") == 0) {
				g_string_append(req, "extended-monitor ");
			} else if (g_str_has_prefix(cap_array[i], "draft/multiline") || g_str_has_prefix(cap_array[i], "multiline")) {
				g_string_append(req, "draft/multiline ");
				const char *opts = strchr(cap_array[i], '=');
				if (opts) {
					gchar **kv = g_strsplit(opts + 1, ",", -1);
					int k;
					for (k = 0; kv[k] != NULL; k++) {
						if (strncmp(kv[k], "max-bytes=", 10) == 0) {
							irc->multiline_max_bytes = atoi(kv[k] + 10);
						} else if (strncmp(kv[k], "max-lines=", 10) == 0) {
							irc->multiline_max_lines = atoi(kv[k] + 10);
						}
					}
					g_strfreev(kv);
				}
			} else if (strcmp(cap_array[i], "batch") == 0) {
				g_string_append(req, "batch ");
			} else if (strcmp(cap_array[i], "draft/chathistory") == 0 || strcmp(cap_array[i], "chathistory") == 0) {
				g_string_append(req, "draft/chathistory ");
			} else if (strcmp(cap_array[i], "draft/event-playback") == 0 || strcmp(cap_array[i], "event-playback") == 0) {
				g_string_append(req, "draft/event-playback ");
			} else if (strcmp(cap_array[i], "draft/metadata-2") == 0) {
				g_string_append(req, "draft/metadata-2 ");
			} else if (strcmp(cap_array[i], "sts") == 0 || strncmp(cap_array[i], "sts=", 4) == 0) {
				const char *val = (strncmp(cap_array[i], "sts=", 4) == 0) ? cap_array[i] + 4 : NULL;
				irc_parse_sts(irc, val);
			}
#ifdef HAVE_CYRUS_SASL
			else if ((strcmp(cap_array[i], "sasl") == 0 || strncmp(cap_array[i], "sasl=", 5) == 0) &&
					 purple_account_get_bool(irc->account, "sasl", FALSE)) {
				g_string_append(req, "sasl ");
			}
#endif
		}
		g_strfreev(cap_array);

		if (req->len > 0) {
			char *buf = irc_format(irc, "vv:", "CAP", "REQ", req->str);
			irc_priority_send(irc, buf);
			g_free(buf);
		} else {
			char *buf = irc_format(irc, "vv", "CAP", "END");
			irc_priority_send(irc, buf);
			g_free(buf);
		}
		g_string_free(req, TRUE);
	} else if (strncmp(subcmd, "DEL", 3) == 0) {
		gchar **cap_array = g_strsplit(caps, " ", -1);
		int i;
		for (i = 0; cap_array[i] != NULL; i++) {
			if (strcmp(cap_array[i], "message-tags") == 0) {
				irc->cap_message_tags = FALSE;
			} else if (strcmp(cap_array[i], "labeled-response") == 0) {
				irc->cap_labeled_response = FALSE;
			} else if (strcmp(cap_array[i], "away-notify") == 0) {
				irc->cap_away_notify = FALSE;
			} else if (strcmp(cap_array[i], "extended-join") == 0) {
				irc->cap_extended_join = FALSE;
			} else if (strcmp(cap_array[i], "account-notify") == 0) {
				irc->cap_account_notify = FALSE;
			} else if (strcmp(cap_array[i], "account-tag") == 0) {
				irc->cap_account_tag = FALSE;
			} else if (strcmp(cap_array[i], "chghost") == 0) {
				irc->cap_chghost = FALSE;
			} else if (strcmp(cap_array[i], "extended-monitor") == 0 || strcmp(cap_array[i], "draft/extended-monitor") == 0) {
				irc->cap_extended_monitor = FALSE;
			} else if (g_str_has_prefix(cap_array[i], "draft/multiline") || g_str_has_prefix(cap_array[i], "multiline")) {
				irc->cap_multiline = FALSE;
			} else if (strcmp(cap_array[i], "batch") == 0) {
				irc->cap_batch = FALSE;
			} else if (strcmp(cap_array[i], "draft/chathistory") == 0 || strcmp(cap_array[i], "chathistory") == 0) {
				irc->cap_chathistory = FALSE;
			} else if (strcmp(cap_array[i], "draft/event-playback") == 0 || strcmp(cap_array[i], "event-playback") == 0) {
				irc->cap_event_playback = FALSE;
			} else if (strcmp(cap_array[i], "draft/metadata-2") == 0) {
				irc->cap_metadata_2 = FALSE;
			}
		}
		g_strfreev(cap_array);
	} else if (strncmp(subcmd, "NEW", 3) == 0) {
		gchar **cap_array = g_strsplit(caps, " ", -1);
		int i;
		for (i = 0; cap_array[i] != NULL; i++) {
			if (strcmp(cap_array[i], "message-tags") == 0) {
				irc->cap_message_tags = TRUE;
			} else if (strcmp(cap_array[i], "labeled-response") == 0) {
				irc->cap_labeled_response = TRUE;
			} else if (strcmp(cap_array[i], "away-notify") == 0) {
				irc->cap_away_notify = TRUE;
			} else if (strcmp(cap_array[i], "extended-join") == 0) {
				irc->cap_extended_join = TRUE;
			} else if (strcmp(cap_array[i], "account-notify") == 0) {
				irc->cap_account_notify = TRUE;
			} else if (strcmp(cap_array[i], "account-tag") == 0) {
				irc->cap_account_tag = TRUE;
			} else if (strcmp(cap_array[i], "chghost") == 0) {
				irc->cap_chghost = TRUE;
			} else if (strcmp(cap_array[i], "extended-monitor") == 0 || strcmp(cap_array[i], "draft/extended-monitor") == 0) {
				irc->cap_extended_monitor = TRUE;
			} else if (g_str_has_prefix(cap_array[i], "draft/multiline") || g_str_has_prefix(cap_array[i], "multiline")) {
				irc->cap_multiline = TRUE;
			} else if (strcmp(cap_array[i], "batch") == 0) {
				irc->cap_batch = TRUE;
			} else if (strcmp(cap_array[i], "draft/chathistory") == 0 || strcmp(cap_array[i], "chathistory") == 0) {
				irc->cap_chathistory = TRUE;
			} else if (strcmp(cap_array[i], "draft/event-playback") == 0 || strcmp(cap_array[i], "event-playback") == 0) {
				irc->cap_event_playback = TRUE;
			} else if (strcmp(cap_array[i], "draft/metadata-2") == 0) {
				irc->cap_metadata_2 = TRUE;
			} else if (strcmp(cap_array[i], "sts") == 0 || strncmp(cap_array[i], "sts=", 4) == 0) {
				const char *val = (strncmp(cap_array[i], "sts=", 4) == 0) ? cap_array[i] + 4 : NULL;
				irc_parse_sts(irc, val);
			}
		}
		g_strfreev(cap_array);
	} else if (strncmp(subcmd, "ACK", 3) == 0) {
		gchar **cap_array = g_strsplit(caps, " ", -1);
		int i;
		gboolean sasl_acked = FALSE;

		for (i = 0; cap_array[i] != NULL; i++) {
			if (strcmp(cap_array[i], "message-tags") == 0) {
				irc->cap_message_tags = TRUE;
			} else if (strcmp(cap_array[i], "labeled-response") == 0) {
				irc->cap_labeled_response = TRUE;
			} else if (strcmp(cap_array[i], "away-notify") == 0) {
				irc->cap_away_notify = TRUE;
			} else if (strcmp(cap_array[i], "extended-join") == 0) {
				irc->cap_extended_join = TRUE;
			} else if (strcmp(cap_array[i], "account-notify") == 0) {
				irc->cap_account_notify = TRUE;
			} else if (strcmp(cap_array[i], "account-tag") == 0) {
				irc->cap_account_tag = TRUE;
			} else if (strcmp(cap_array[i], "chghost") == 0) {
				irc->cap_chghost = TRUE;
			} else if (strcmp(cap_array[i], "extended-monitor") == 0 || strcmp(cap_array[i], "draft/extended-monitor") == 0) {
				irc->cap_extended_monitor = TRUE;
			} else if (g_str_has_prefix(cap_array[i], "draft/multiline") || g_str_has_prefix(cap_array[i], "multiline")) {
				irc->cap_multiline = TRUE;
			} else if (strcmp(cap_array[i], "batch") == 0) {
				irc->cap_batch = TRUE;
			} else if (strcmp(cap_array[i], "draft/chathistory") == 0 || strcmp(cap_array[i], "chathistory") == 0) {
				irc->cap_chathistory = TRUE;
			} else if (strcmp(cap_array[i], "draft/event-playback") == 0 || strcmp(cap_array[i], "event-playback") == 0) {
				irc->cap_event_playback = TRUE;
			} else if (strcmp(cap_array[i], "draft/metadata-2") == 0) {
				irc->cap_metadata_2 = TRUE;
			}
#ifdef HAVE_CYRUS_SASL
			else if (strcmp(cap_array[i], "sasl") == 0) {
				sasl_acked = TRUE;
			}
#endif
		}
		g_strfreev(cap_array);

#ifdef HAVE_CYRUS_SASL
		if (sasl_acked) {
			int ret;
			const char *mech_list = NULL;
			char *pos;
			size_t index;
			int id = 0;

			if (sasl_client_init(NULL) != SASL_OK) {
				const char *tmp =
				  _("SASL authentication failed: Initializing SASL failed.");
				purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, tmp);
				return;
			}

			irc->sasl_cb = g_new0(sasl_callback_t, 5);

			irc->sasl_cb[id].id = SASL_CB_AUTHNAME;
			irc->sasl_cb[id].proc = (void *) irc_sasl_cb_simple;
			irc->sasl_cb[id].context = (void *) irc;
			id++;

			irc->sasl_cb[id].id = SASL_CB_USER;
			irc->sasl_cb[id].proc = (void *) irc_sasl_cb_simple;
			irc->sasl_cb[id].context = (void *) irc;
			id++;

			irc->sasl_cb[id].id = SASL_CB_PASS;
			irc->sasl_cb[id].proc = (void *) irc_sasl_cb_secret;
			irc->sasl_cb[id].context = (void *) irc;
			id++;

			irc->sasl_cb[id].id = SASL_CB_LOG;
			irc->sasl_cb[id].proc = (void *) irc_sasl_cb_log;
			irc->sasl_cb[id].context = (void *) irc;
			id++;

			irc->sasl_cb[id].id = SASL_CB_LIST_END;

			ret = sasl_client_new("irc", irc->server, NULL, NULL, irc->sasl_cb, 0, &irc->sasl_conn);

			sasl_listmech(irc->sasl_conn, NULL, "", " ", "", &mech_list, NULL, NULL);
			purple_debug_info("irc", "SASL: we have available: %s\n", mech_list);

			if (ret != SASL_OK) {
				gchar *tmp;

				purple_debug_error("irc", "sasl_client_new failed: %d\n", ret);
				tmp = g_strdup_printf(_("Failed to initialize SASL authentication: %s"),
									  sasl_errdetail(irc->sasl_conn));
				purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, tmp);
				g_free(tmp);

				return;
			}

			irc->sasl_mechs = g_string_new(mech_list);
			if (irc->tls_cert_path && irc->gsc) {
				/* Prepend EXTERNAL so Cyrus SASL tries client cert first */
				if ((pos = strstr(irc->sasl_mechs->str, "EXTERNAL"))) {
					index = pos - irc->sasl_mechs->str;
					g_string_erase(irc->sasl_mechs, index, strlen("EXTERNAL"));
					if (index < irc->sasl_mechs->len && (irc->sasl_mechs->str)[index] == ' ') {
						g_string_erase(irc->sasl_mechs, index, 1);
					}
				}
				if (irc->sasl_mechs->len > 0)
					g_string_prepend(irc->sasl_mechs, "EXTERNAL ");
				else
					g_string_assign(irc->sasl_mechs, "EXTERNAL");
			} else {
				/* No client certificate configured, strip EXTERNAL */
				if ((pos = strstr(irc->sasl_mechs->str, "EXTERNAL"))) {
					index = pos - irc->sasl_mechs->str;
					g_string_erase(irc->sasl_mechs, index, strlen("EXTERNAL"));
					if (index < irc->sasl_mechs->len && (irc->sasl_mechs->str)[index] == ' ') {
						g_string_erase(irc->sasl_mechs, index, 1);
					}
				}
			}

			irc_auth_start_cyrus(irc);
		} else {
#endif
			/* If SASL wasn't requested/acked, just end CAP */
			char *buf = irc_format(irc, "vv", "CAP", "END");
			irc_priority_send(irc, buf);
			g_free(buf);
#ifdef HAVE_CYRUS_SASL
		}
#endif
	}
}

void
irc_msg_tagmsg(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	gchar **tags_arr, **kv;
	int i;
	char *nick;

	if (!irc->current_tags)
		return;

	nick = irc_mask_nick(from);
	if (nick && !purple_utf8_strcasecmp(nick, purple_connection_get_display_name(gc))) {
		g_free(nick);
		return;
	}

	tags_arr = g_strsplit(irc->current_tags, ";", -1);
	for (i = 0; tags_arr[i] != NULL; i++) {
		kv = g_strsplit(tags_arr[i], "=", 2);
		if (kv[0] != NULL && strcmp(kv[0], "+typing") == 0 && kv[1] != NULL) {
			PurpleTypingState state = PURPLE_NOT_TYPING;
			if (strcmp(kv[1], "active") == 0)
				state = PURPLE_TYPING;
			else if (strcmp(kv[1], "paused") == 0)
				state = PURPLE_TYPED;

			nick = irc_mask_nick(from);
			if (args && args[0] && args[0][0] == '#') {
				// Group chat
				PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY, args[0], irc->account);
				if (conv) {
					PurpleConvChat *chat = purple_conversation_get_chat_data(conv);
					if (chat) {
						PurpleConvChatBuddyFlags flags = purple_conv_chat_user_get_flags(chat, nick);
						if (state == PURPLE_TYPING) {
							flags |= PURPLE_CBFLAGS_TYPING;
						} else {
							flags &= ~PURPLE_CBFLAGS_TYPING;
						}
						purple_conv_chat_user_set_flags(chat, nick, flags);
					}
				}
			} else {
				// Private chat
				serv_got_typing(gc, nick, state == PURPLE_TYPED ? 30 : 6, state);
			}
			g_free(nick);
		}
		g_strfreev(kv);
	}
	g_strfreev(tags_arr);
}

void
irc_msg_ignore(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	return;
}

struct irc_avatar_fetch {
	struct irc_conn *irc;
	char *nick;
	char *url;
};

static void
irc_avatar_fetch_cb(PurpleUtilFetchUrlData *url_data, gpointer user_data, const gchar *url_text, gsize len, const gchar *error_message)
{
	struct irc_avatar_fetch *req = user_data;
	if (error_message == NULL && len > 0) {
		purple_buddy_icons_set_for_user(req->irc->account, req->nick, g_memdup((gpointer) url_text, len), len, req->url);
	} else if (error_message != NULL) {
		purple_debug_warning("irc", "Failed to fetch avatar for %s: %s\n", req->nick, error_message);
	}
	g_free(req->url);
	g_free(req->nick);
	g_free(req);
}

void
irc_msg_metadata(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	const char *target, *key, *value;

	if (g_strcmp0(name, "761") == 0) {
		/* 761 <client> <target> <key> <visibility> :<value> */
		target = args[1];
		key = args[2];
		value = args[4];
	} else {
		/* METADATA <target> <key> <visibility> :<value> */
		target = args[0];
		key = args[1];
		value = args[3];
	}

	if (g_strcmp0(key, "avatar") == 0) {
		if (value && *value) {
			PurpleBuddy *buddy = purple_find_buddy(irc->account, target);
			const char *url_to_fetch = value;

			const char *existing_url = purple_buddy_icons_get_checksum_for_user(buddy);
			if (g_strcmp0(existing_url, url_to_fetch) != 0) {
				struct irc_avatar_fetch *req;

				req = g_new0(struct irc_avatar_fetch, 1);
				req->irc = irc;
				req->nick = g_strdup(target);
				req->url = g_strdup(url_to_fetch);
				purple_util_fetch_url_request_len_with_account(irc->account, url_to_fetch, TRUE, purple_core_get_ui(), TRUE, NULL, FALSE, 10 * 1024 * 1024, irc_avatar_fetch_cb, req);
			}
		} else {
			/* Avatar cleared */
			purple_buddy_icons_set_for_user(irc->account, target, NULL, 0, NULL);
		}
	}
}

void
irc_msg_batch(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	/* BATCH message: args[0] contains raw params after BATCH, e.g. "+ref chathistory #channel" or "-ref" */
	if (!args || !args[0])
		return;

	purple_debug_info("irc", "Received BATCH: %s\n", args[0]);

	if (args[0][0] == '+') {
		gchar **tokens = g_strsplit(args[0], " ", -1);
		if (tokens[0] && tokens[1]) {
			const char *ref = tokens[0] + 1;
			const char *type = tokens[1];
			const char *target = tokens[2];
			gboolean is_self_sent = FALSE;

			if (irc->current_tags) {
				gchar **tags = g_strsplit(irc->current_tags, ";", -1);
				int i;
				for (i = 0; tags[i] != NULL; i++) {
					if (g_str_has_prefix(tags[i], "label=")) {
						if (g_hash_table_remove(irc->sent_messages, tags[i] + 6)) {
							is_self_sent = TRUE;
						}
					}
				}
				g_strfreev(tags);
			}

			struct irc_batch *batch = g_new0(struct irc_batch, 1);
			batch->ref = g_strdup(ref);
			batch->type = g_strdup(type);
			batch->target = g_strdup(target);
			if (from)
				batch->from = irc_mask_nick(from);
			batch->content = g_string_new("");
			batch->self_sent = is_self_sent;
			g_hash_table_replace(irc->active_batches, g_strdup(ref), batch);
		}
		g_strfreev(tokens);
	} else if (args[0][0] == '-') {
		const char *ref = args[0] + 1;
		struct irc_batch *batch = g_hash_table_lookup(irc->active_batches, ref);
		if (batch) {
			if (!batch->self_sent && (g_strcmp0(batch->type, "draft/multiline") == 0 || g_strcmp0(batch->type, "multiline") == 0)) {
				PurpleConnection *gc = purple_account_get_connection(irc->account);
				if (gc && batch->content && batch->target) {
					char *nick = batch->from ? batch->from : g_strdup("");
					time_t now = time(NULL);
					char *msg = irc_mirc2html(batch->content->str);
					if (irc_ischannel(batch->target)) {
						PurpleConversation *convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, batch->target, irc->account);
						if (convo) {
							if (!purple_utf8_strcasecmp(nick, purple_connection_get_display_name(gc))) {
								purple_conversation_write(convo, nick, msg, PURPLE_MESSAGE_SEND, now);
							} else {
								serv_got_chat_in(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(convo)), nick, PURPLE_MESSAGE_RECV, msg, now);
							}
						}
					} else {
						if (!purple_utf8_strcasecmp(batch->target, purple_connection_get_display_name(gc))) {
							serv_got_im(gc, nick, msg, PURPLE_MESSAGE_RECV, now);
						} else {
							PurpleConversation *convo = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, batch->target, irc->account);
							if (convo) {
								purple_conversation_write(convo, nick, msg, PURPLE_MESSAGE_SEND, now);
							} else {
								serv_got_im(gc, batch->target, msg, PURPLE_MESSAGE_SEND, now);
							}
						}
					}
					g_free(msg);
					if (batch->from) g_free(nick);
				}
			}
			g_hash_table_remove(irc->active_batches, ref);
		}
	}
}

void
irc_msg_mononline(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	gchar **targets;
	int i;

	if (!args || !args[1])
		return;

	targets = g_strsplit(args[1], ",", -1);
	for (i = 0; targets[i] != NULL; i++) {
		char *target = g_strstrip(targets[i]);
		char *nick, *userhost = NULL;
		char *excl;

		if (!*target)
			continue;

		excl = strchr(target, '!');
		if (excl) {
			*excl = '\0';
			nick = target;
			userhost = excl + 1;
		} else {
			nick = target;
		}

		purple_prpl_got_user_status(irc->account, nick, "available", NULL);

		struct irc_buddy *ib = g_hash_table_lookup(irc->buddies, nick);
		if (ib) {
			ib->online = TRUE;
		}

		if (userhost && *userhost) {
			PurpleBuddy *buddy = purple_find_buddy(irc->account, nick);
			if (buddy) {
				purple_blist_node_set_string(PURPLE_BLIST_NODE(buddy), "userhost", userhost);
			}
		}
	}
	g_strfreev(targets);
}

void
irc_msg_monoffline(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	gchar **targets;
	int i;

	if (!args || !args[1])
		return;

	targets = g_strsplit(args[1], ",", -1);
	for (i = 0; targets[i] != NULL; i++) {
		char *nick = g_strstrip(targets[i]);
		if (!*nick)
			continue;

		purple_prpl_got_user_status(irc->account, nick, "offline", NULL);

		struct irc_buddy *ib = g_hash_table_lookup(irc->buddies, nick);
		if (ib) {
			ib->online = FALSE;
		}
	}
	g_strfreev(targets);
}

static void
irc_monitor_add_cb(char *name, struct irc_buddy *ib, GString *str)
{
	if (!ib || !ib->name)
		return;
	if (str->len > 0)
		g_string_append_c(str, ',');
	g_string_append(str, ib->name);
}

void
irc_send_monitor_add_all(struct irc_conn *irc)
{
	GString *str;
	char *buf;

	if (!irc || !irc->monitor_supported || !irc->buddies)
		return;

	str = g_string_new("");
	g_hash_table_foreach(irc->buddies, (GHFunc) irc_monitor_add_cb, str);

	if (str->len > 0) {
		buf = irc_format(irc, "vvv", "MONITOR", "+", str->str);
		irc_send(irc, buf);
		g_free(buf);
	}
	g_string_free(str, TRUE);
}

void
irc_msg_monlist(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	if (!args || !args[1])
		return;

	purple_debug_info("irc", "MONITOR list: %s\n", args[1]);
}

void
irc_msg_monfull(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	if (!args || !args[1])
		return;

	purple_debug_warning("irc", "MONITOR list full (limit %s): unable to monitor %s\n", args[1], args[2] ? args[2] : "");
}

void
irc_msg_whoisbot(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	if (!args || !args[1])
		return;

	const char *target = args[1];
	if (irc->whois.nick && !purple_utf8_strcasecmp(irc->whois.nick, target)) {
		irc->whois.bot = 1;
	}
	PurpleBuddy *buddy = purple_find_buddy(irc->account, target);
	if (buddy) {
		purple_blist_node_set_bool(PURPLE_BLIST_NODE(buddy), "bot", TRUE);
	}
}

