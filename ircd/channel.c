/************************************************************************
 *   IRC - Internet Relay Chat, ircd/channel.c
 *   Copyright (C) 1990 Jarkko Oikarinen and
 *                      University of Oulu, Co Center
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* -- Jto -- 09 Jul 1990
 * Bug fix
 */

/* -- Jto -- 03 Jun 1990
 * Moved m_channel() and related functions from s_msg.c to here
 * Many changes to start changing into string channels...
 */

/* -- Jto -- 24 May 1990
 * Moved is_full() from list.c
 */

#ifndef	lint
static	char sccsid[] = "@(#)channel.c	2.27 3/22/93 (C) 1990 University of Oulu, Computing\
 Center and Jarkko Oikarinen";
#endif

#include "struct.h"
#include "sys.h"
#include "common.h"
#include "numeric.h"
#include "channel.h"
#include "h.h"

aChannel *channel = NullChn;

static	void	sub1_from_channel PROTO((aChannel *));
static	void	add_invite PROTO((aClient *, aChannel *));
static	int	add_banid PROTO((aChannel *, char *));
static	int	del_banid PROTO((aChannel *, char *));
static	int	check_channelmask PROTO((aClient *, aClient *, char *));
static	int set_mode PROTO((aClient *,aChannel *,int,char **,char *,char *));
static	Link	*is_banned PROTO((aClient *, aChannel *));
static	int	can_join PROTO((aClient *, aChannel *, char *));

void	clean_channelname PROTO((char *));
void	del_invite PROTO((aClient *, aChannel *));

static	char	*PartFmt = ":%s PART %s";
static	char	namebuf[NICKLEN+USERLEN+HOSTLEN+3];
/*
 * some buffers for rebuilding channel/nick lists with ,'s
 */
static	char	nickbuf[BUFSIZE], buf[BUFSIZE];
static	char	modebuf[MODEBUFLEN], parabuf[MODEBUFLEN];

/*
 * return the length (>=0) of a chain of links.
 */
static	int	list_length(lp)
Reg1	Link	*lp;
{
	Reg2	int	count = 0;

	for (; lp; lp = lp->next)
		count++;
	return count;
}

/*
** find_chasing
**	Find the client structure for a nick name (user) using history
**	mechanism if necessary. If the client is not found, an error
**	message (NO SUCH NICK) is generated. If the client was found
**	through the history, chasing will be 1 and otherwise 0.
*/
static	aClient *find_chasing(sptr, user, chasing)
aClient *sptr;
char	*user;
Reg1	int	*chasing;
{
	Reg2	aClient *who = find_client(user, (aClient *)NULL);

	if (chasing)
		*chasing = 0;
	if (who)
		return who;
	if (!(who = get_history(user, (long)KILLCHASETIMELIMIT)))
	    {
		sendto_one(sptr, err_str(ERR_NOSUCHNICK),
			   me.name, sptr->name, user);
		return NULL;
	    }
	if (chasing)
		*chasing = 1;
	return who;
}

/*
 *  Fixes a string so that the first white space found becomes an end of
 * string marker (`\-`).  returns the 'fixed' string or "*" if the string
 * was NULL length or a NULL pointer.
 */
static	char	*check_string(s)
Reg1	char *s;
{
	static	char	star[2] = "*";
	char	*str = s;

	if (BadPtr(s))
		return star;

	for ( ;*s; s++)
		if (isspace(*s))
		    {
			*s = '\0';
			break;
		    }

	return (BadPtr(str)) ? star : str;
}

/*
 * create a string of form "foo!bar@fubar" given foo, bar and fubar
 * as the parameters.  If NULL, they become "*".
 */
static	char *make_nick_user_host(nick, name, host)
Reg1	char	*nick, *name, *host;
{
	bzero(namebuf, sizeof(namebuf));
	nick = check_string(nick);
	(void)strncpy(namebuf, nick, NICKLEN);
	(void)strcat(namebuf, "!");
	name = check_string(name);
	(void)strncat(namebuf, name, REALLEN);
	(void)strcat(namebuf, "@");
	host = check_string(host);
	(void)strncat(namebuf, host, HOSTLEN);

	return (namebuf);
}

/*
 * Ban functions to work with mode +b
 */
/* add_banid - add an id to be banned to the channel  (belongs to cptr) */

static	int	add_banid(chptr, banid)
aChannel *chptr;
char	*banid;
{
	Reg1	Link	*ban;

	for (ban = chptr->banlist; ban; ban = ban->next)
		if (mycmp(banid, ban->value.cp) == 0)
			return -1;
	ban = make_link();
	bzero((char *)ban, sizeof(Link));
	ban->next = chptr->banlist;
	ban->value.cp = (char *)MyMalloc(strlen(banid)+1);
	(void)strcpy(ban->value.cp, banid);
	chptr->banlist = ban;
	return 0;
}

/*
 * del_banid - delete an id belonging to cptr
 * if banid is null, deleteall banids belonging to cptr.
 */
static	int	del_banid(chptr, banid)
aChannel *chptr;
char	*banid;
{
	Reg1 Link **ban;
	Reg2 Link *tmp;

	if (!chptr || !banid)
		return -1;
	for (ban = &(chptr->banlist); *ban; ban = &((*ban)->next))
		if (mycmp(banid, (*ban)->value.cp)==0)
		    {
			tmp = *ban;
			*ban = tmp->next;
			(void)free(tmp->value.cp);
			free_link(tmp);
			break;
		    }
	return 0;
}

/*
 * is_banned - returns a pointer to the ban structure if banned else NULL
 */
static	Link	*is_banned(cptr, chptr)
aClient *cptr;
aChannel *chptr;
{
	Reg1	Link	*tmp;

	if (!IsPerson(cptr))
		return NULL;

	(void)make_nick_user_host(cptr->name, cptr->user->username,
				  cptr->user->host);

	for (tmp = chptr->banlist; tmp; tmp = tmp->next)
		if (matches(tmp->value.cp, namebuf) == 0)
			break;
	return (tmp);
}

/*
 * adds a user to a channel by adding another link to the channels member
 * chain.
 */
static	void	add_user_to_channel(chptr, who, flags)
aChannel *chptr;
aClient *who;
int	flags;
{
	Reg1	Link *ptr;

	if (who->user)
	    {
		ptr = make_link();
		ptr->value.cptr = who;
		ptr->flags = flags;
		ptr->next = chptr->members;
		chptr->members = ptr;
		chptr->users++;

		ptr = make_link();
		ptr->value.chptr = chptr;
		ptr->next = who->user->channel;
		who->user->channel = ptr;
	    }
}

void	remove_user_from_channel(sptr, chptr)
aClient *sptr;
aChannel *chptr;
{
	Reg1	Link	**curr;
	Reg2	Link	*tmp;

	for (curr = &(chptr->members); *curr; curr = &((*curr)->next))
		if ((*curr)->value.cptr == sptr)
		    {
			tmp = *curr;
			*curr = tmp->next;
			free_link(tmp);
			break;
		    }
	for (curr = &(sptr->user->channel); *curr; curr = &((*curr)->next))
		if ((*curr)->value.chptr == chptr)
		    {
			tmp = *curr;
			*curr = tmp->next;
			free_link(tmp);
			break;
		    }
	sub1_from_channel(chptr);
}

static	void	change_chan_flag(lp, chptr)
Link	*lp;
aChannel *chptr;
{
	Reg1	Link *tmp;

	if (tmp = find_user_link(chptr->members, lp->value.cptr))
		if (lp->flags & MODE_ADD)
			tmp->flags |= lp->flags & MODE_FLAGS;
		else
			tmp->flags &= ~lp->flags & MODE_FLAGS;
}

int	is_chan_op(cptr, chptr)
aClient *cptr;
aChannel *chptr;
{
	Reg1	Link	*lp;

	if (chptr)
		if (lp = find_user_link(chptr->members, cptr))
			return (lp->flags & CHFL_CHANOP);

	return 0;
}

int	has_voice(cptr, chptr)
aClient *cptr;
aChannel *chptr;
{
	Reg1	Link	*lp;

	if (chptr)
		if (lp = find_user_link(chptr->members, cptr))
			return (lp->flags & CHFL_VOICE);

	return 0;
}

int	can_send(cptr, chptr)
aClient *cptr;
aChannel *chptr;
{
	Reg1	Link	*lp;
	Reg2	int	member;

	member = IsMember(cptr, chptr);
	lp = find_user_link(chptr->members, cptr);

	if (chptr->mode.mode & MODE_MODERATED &&
	    !(lp->flags & (CHFL_CHANOP|CHFL_VOICE)))
		return (MODE_MODERATED);

	if (chptr->mode.mode & MODE_NOPRIVMSGS && !member)
		return (MODE_NOPRIVMSGS);

	return 0;
}

aChannel *find_channel(chname, chptr)
Reg1	char	*chname;
Reg2	aChannel *chptr;
{
	return hash_find_channel(chname, chptr);
}

/*
 * write the "simple" list of channel modes for channel chptr onto buffer mbuf
 * with the parameters in pbuf.
 */
static	void	channel_modes(mbuf, pbuf, chptr)
Reg1	char	*mbuf, *pbuf;
aChannel *chptr;
{
	*mbuf++ = '+';
	if (chptr->mode.mode & MODE_SECRET)
		*mbuf++ = 's';
	else if (chptr->mode.mode & MODE_PRIVATE)
		*mbuf++ = 'p';
	if (chptr->mode.mode & MODE_MODERATED)
		*mbuf++ = 'm';
	if (chptr->mode.mode & MODE_TOPICLIMIT)
		*mbuf++ = 't';
	if (chptr->mode.mode & MODE_INVITEONLY)
		*mbuf++ = 'i';
	if (chptr->mode.mode & MODE_NOPRIVMSGS)
		*mbuf++ = 'n';
	if (chptr->mode.mode & MODE_KEY)
		*mbuf++ = 'k';
	if (chptr->mode.limit)
	    {
		*mbuf++ = 'l'; 
		(void)sprintf(pbuf, "%d", chptr->mode.limit);
	    }
	*mbuf++ = '\0';
	return;
}

/*
 * send "cptr" a full list of the modes for channel chptr.
 */
void	send_channel_modes(cptr, chptr)
aClient *cptr;
aChannel *chptr;
{
	Reg1	Link	*lp, *lp2;
	Reg2	aClient *acptr;
	Reg3	char	*cp;
	int	count = 0, send = 0;

	*modebuf = *parabuf = '\0';
	channel_modes(modebuf, parabuf, chptr);
	cp = modebuf + strlen(modebuf);
	if (*parabuf)	/* mode +l xx */
		count = 1;
	if (chptr->mode.mode & MODE_KEY)
	    {
		(void)strcat(parabuf, " ");
		(void)strcat(parabuf, chptr->mode.key);
		count++;
	    }
	for (lp = chptr->members ; lp; )
	    {
		acptr = lp->value.cptr;
		lp2 = find_user_link(chptr->members, acptr);
		if (lp2 && !(lp2->flags & (CHFL_VOICE|CHFL_CHANOP)))
		    {
			lp = lp->next;
			continue;
		    }
		if (strlen(parabuf) + strlen(acptr->name) + 10 < MODEBUFLEN)
		    {
			(void)strcat(parabuf, " ");
			(void)strcat(parabuf, acptr->name);
			count++;
			if (lp2->flags == CHFL_VOICE)
				*cp++ = 'v';
			else
				*cp++ = 'o';
			*cp = '\0';
			lp = lp->next;
		    }
		else if (*parabuf)
			send = 1;
		if (count == 3)
			send = 1;
		if (send)
		    {
			sendto_one(cptr, ":%s MODE %s %s %s",
				   me.name, chptr->chname, modebuf, parabuf);
			send = 0;
			count = 0;
			*parabuf = '\0';
			cp = modebuf;
			*cp++ = '+';
			*cp = '\0';
		    }
	    }
	for (lp = chptr->banlist ; lp; )
	    {
		if (strlen(parabuf) + strlen(lp->value.cp) + 10 < MODEBUFLEN)
		    {
			(void)strcat(parabuf, " ");
			(void)strcat(parabuf, lp->value.cp);
			count++;
			*cp++ = 'b';
			*cp = '\0';
			lp = lp->next;
		    }
		else if (*parabuf)
			send = 1;
		if (count == 3)
			send = 1;
		if (send)
		    {
			sendto_one(cptr, ":%s MODE %s %s %s",
				   me.name, chptr->chname, modebuf, parabuf);
			send = 0;
			count = 0;
			*parabuf = '\0';
			cp = modebuf;
			*cp++ = '+';
			*cp = '\0';
		    }
	    }
	if (modebuf[1] || *parabuf)
		sendto_one(cptr, ":%s MODE %s %s %s",
			   me.name, chptr->chname, modebuf, parabuf);
}

/*
 * m_mode
 * parv[0] - sender
 * parv[1] - channel
 */

int	m_mode(cptr, sptr, parc, parv)
aClient *cptr;
aClient *sptr;
int	parc;
char	*parv[];
{
	int	mcount = 0, chanop;
	aChannel *chptr;

	if (check_registered(sptr))
		return 0;

	/* Now, try to find the channel in question */
	if (parc > 1)
	    {
		chptr = find_channel(parv[1], NullChn);
		if (chptr == NullChn)
			return m_umode(cptr, sptr, parc, parv);
	    }
	else
	    {
		sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
			   me.name, parv[0], "MODE");
 	 	return 0;
	    }

	clean_channelname(parv[1]);
	if (check_channelmask(sptr, cptr, parv[1]))
		return 0;
	chanop = is_chan_op(sptr, chptr) || IsServer(sptr);

	if (parc < 3)
	    {
		*modebuf = *parabuf = '\0';
		modebuf[1] = '\0';
		channel_modes(modebuf, parabuf, chptr);
		sendto_one(sptr, rpl_str(RPL_CHANNELMODEIS), me.name, parv[0],
			   chptr->chname, modebuf, parabuf);
		return 0;
	    }
	mcount = set_mode(sptr, chptr, parc - 2, parv + 2,
			  modebuf, parabuf);

	if (strlen(modebuf) > 1)
	    {
		if (MyConnect(sptr) && !IsServer(sptr) && !chanop)
		    {
			sendto_one(sptr, err_str(ERR_CHANOPRIVSNEEDED),
				   me.name, parv[0], chptr->chname);
			return 0;
		    }
		if ((IsServer(cptr) && !IsServer(sptr) && !chanop) ||
		    mcount == -1)
		    {
			sendto_ops("Fake: %s MODE %s %s %s",
				   parv[0], parv[1], modebuf, parabuf);
			ircstp->is_fake++;
		    }
		else
			sendto_channel_butserv(chptr, sptr,
						":%s MODE %s %s %s", parv[0],
						chptr->chname, modebuf,
						parabuf);
		sendto_match_servs(chptr, cptr, ":%s MODE %s %s %s",
				   parv[0], chptr->chname, modebuf, parabuf);
	    }
	return 0;
}

/*
 * Check and try to apply the channel modes passed in the parv array for
 * the client ccptr to channel chptr.  The resultant changes are printed
 * into mbuf and pbuf (if any) and applied to the channel.
 */
static	int	set_mode(cptr, chptr, parc, parv, mbuf, pbuf)
Reg2	aClient *cptr;
aChannel *chptr;
int	parc;
char	*parv[], *mbuf, *pbuf;
{
	static	Link	chops[MAXMODEPARAMS];
	static	int	flags[] = {
				MODE_PRIVATE,    'p', MODE_SECRET,     's',
				MODE_MODERATED,  'm', MODE_NOPRIVMSGS, 'n',
				MODE_TOPICLIMIT, 't', MODE_INVITEONLY, 'i',
				MODE_VOICE,      'v', MODE_KEY,        'k',
				0x0, 0x0 };

	Reg1	Link	*lp;
	Reg2	char	*curr = parv[0], *cp;
	Reg3	int	*ip;
	int	whatt = MODE_ADD, limitset = 0, count = 0, chasing = 0;
	int	nusers, ischop, new, len, keychange = 0, opcnt = 0;
	aClient *who;
	Mode	*mode, oldm;

	*mbuf = *pbuf = '\0';
	if (!chptr)
		return 0;
	if (parc < 1)
		return 0;

	mode = &(chptr->mode);
	bcopy((char *)mode, (char *)&oldm, sizeof(Mode));
	ischop = is_chan_op(cptr, chptr) || IsServer(cptr);
	new = mode->mode;

	while (curr && *curr && count >= 0)
	    {
		switch (*curr)
		{
		case '+':
			whatt = MODE_ADD;
			break;
		case '-':
			whatt = MODE_DEL;
			break;
		case 'o' :
		case 'v' :
			if (--parc <= 0)
				break;
			parv++;
			*parv = check_string(*parv);
			if (opcnt >= MAXMODEPARAMS || BadPtr(*parv))
				break;
			/*
			 * Check for nickname changes and try to follow these
			 * to make sure the right client is affected by the
			 * mode change.
			 */
			if (!(who = find_chasing(cptr, parv[0], &chasing)))
			    {
	  			sendto_one(cptr, err_str(ERR_NOSUCHNICK),
					   me.name, cptr->name, parv[0]);
				break;
			    }
	  		if (!IsMember(who, chptr))
			    {
	    			sendto_one(cptr, err_str(ERR_NOTONCHANNEL),
					   me.name, cptr->name,
					   chptr->chname, parv[0]);
				break;
			    }
			/*
			** If this server noticed the nick change, the
			** information must be propagated back upstream.
			** This is a bit early, but at most this will generate
			** just some extra messages if nick appeared more than
			** once in the MODE message... --msa
			*/
			if (chasing && ischop)
			    {
				sendto_one(cptr, ":%s MODE %s %c%c %s",
					   me.name, chptr->chname,
					   whatt == MODE_ADD ? '+' : '-',
					   *curr, who->name);
			    }
			if (who == cptr && whatt == MODE_ADD && *curr == 'o')
				break;
			if (whatt == MODE_ADD)
			    {
				lp = &chops[opcnt++];
				lp->value.cptr = who;
				lp->flags = (*curr == 'o') ? MODE_CHANOP:
								  MODE_VOICE;
				lp->flags |= MODE_ADD;
			    }
			else if (whatt == MODE_DEL)
			    {
				lp = &chops[opcnt++];
				lp->value.cptr = who;
				lp->flags = (*curr == 'o') ? MODE_CHANOP:
								  MODE_VOICE;
				lp->flags |= MODE_DEL;
			    }
			count++;
			break;
		case 'k':
			if (--parc <= 0)
				break;
			parv++;
			/* check now so we eat the parameter if present */
			if (keychange)
				break;
			*parv = check_string(*parv);
			if (opcnt >= MAXMODEPARAMS || BadPtr(*parv))
				break;
			if (whatt == MODE_ADD)
			    {
				if (*mode->key && !IsServer(cptr))
					sendto_one(cptr, err_str(ERR_KEYSET),
						   me.name, cptr->name,
						   chptr->chname);
				else if (ischop &&
				    (!*mode->key || IsServer(cptr)))
				    {
					lp = &chops[opcnt++];
					lp->value.cp = *parv;
					lp->flags = MODE_KEY|MODE_ADD;
					keychange = 1;
				    }
			    }
			else if (whatt == MODE_DEL)
			    {
				if (ischop && (mycmp(mode->key, *parv) == 0 ||
				     IsServer(cptr)))
				    {
					lp = &chops[opcnt++];
					lp->value.cp = mode->key;
					lp->flags = MODE_KEY|MODE_DEL;
					keychange = 1;
				    }
			    }
			break;
		case 'b':
			if (--parc <= 0)
			    {
				for (lp = chptr->banlist; lp; lp = lp->next)
					sendto_one(cptr, rpl_str(RPL_BANLIST),
					     me.name, cptr->name,
					     chptr->chname, lp->value.cp);
				sendto_one(cptr, rpl_str(RPL_ENDOFBANLIST),
					   me.name, cptr->name, chptr->chname);
				break;
			    }
			parv++;
			if (opcnt >= MAXMODEPARAMS || BadPtr(*parv))
				break;
			if (whatt == MODE_ADD)
			    {
				lp = &chops[opcnt++];
				lp->value.cp = *parv;
				lp->flags = MODE_ADD|MODE_BAN;
			    }
			else if (whatt == MODE_DEL)
			    {
				lp = &chops[opcnt++];
				lp->value.cp = *parv;
				lp->flags = MODE_DEL|MODE_BAN;
			    }
			count++;
			break;
		case 'l':
			/*
			 * limit 'l' to only *1* change per mode command but
			 * eat up others.
			 */
			if (limitset)
			    {
				if (whatt == MODE_ADD && --parc > 0)
					parv++;
				break;
			    }
			if (whatt == MODE_DEL)
			    {
				limitset = 1;
				nusers = 0;
				break;
			    }
			if (--parc > 0)
			    {
				lp = &chops[opcnt++];
				lp->flags = MODE_ADD|MODE_LIMIT;
				nusers = atoi(*++parv);
				limitset = 1;
				count++;
				break;
			    }
			sendto_one(cptr, err_str(ERR_NEEDMOREPARAMS),
					   me.name, cptr->name, "MODE +l");
			break;
		case 'i' : /* falls through for default case */
			if (whatt == MODE_DEL)
			    {
				Link	**lpp;

				if (!ischop)
				    {
					count = -1;
					break;
				    }
				for (lpp = &(chptr->invites); *lpp; )
				    {
					lp = *lpp;
					*lpp = lp->next;
					free_link(lp);
				    }
			    }
		default:
			for (ip = flags; *ip; ip += 2)
				if (*(ip+1) == *curr)
					break;

			if (*ip)
			    {
				if (whatt == MODE_ADD)
				    {
					if (*ip == MODE_PRIVATE)
						new &= ~MODE_SECRET;
					else if (*ip == MODE_SECRET)
						new &= ~MODE_PRIVATE;
					new |= *ip;
				    }
				else
					new &= ~*ip;
				count++;
			    }
			else
				sendto_one(cptr, err_str(ERR_UNKNOWNMODE),
					    me.name, cptr->name, *curr);
			break;
		}
		curr++;
		/*
		 * Make sure modes strings such as "+m +t +p +i" are parsed
		 * fully.
		 */
		if (!*curr && parc > 0)
		    {
			curr = *++parv;
			parc--;
		    }
	    } /* end of while loop for MODE processing */

	whatt = 0;

	for (ip = flags; *ip; ip += 2)
		if ((*ip & new) && !(*ip & oldm.mode))
		    {
			if (whatt == 0)
			    {
				*mbuf++ = '+';
				whatt = 1;
			    }
			if (ischop)
				mode->mode |= *ip;
			*mbuf++ = *(ip+1);
		    }

	for (ip = flags; *ip; ip += 2)
		if ((*ip & oldm.mode) && !(*ip & new))
		    {
			if (whatt != -1)
			    {
				*mbuf++ = '-';
				whatt = -1;
			    }
			if (ischop)
				mode->mode &= ~*ip;
			*mbuf++ = *(ip+1);
		    }

	if (limitset && !nusers && mode->limit)
	    {
		if (whatt != -1)
		    {
			*mbuf++ = '-';
			whatt = -1;
		    }
		*mbuf++ = 'l';
	    }

	/*
	 * Reconstruct "+bkov" chain.
	 */
	if (opcnt)
	    {
		Reg1	int	i = 0;
		Reg2	char	c;
		char	*user, *host, numeric[16];

		if (opcnt > 3)
			opcnt = 3;	/* restriction for 2.7 servers */

		for (; i < opcnt; i++)
		    {
			lp = &chops[i];
			/*
			 * make sure we have correct mode change sign
			 */
			if (whatt != (lp->flags & (MODE_ADD|MODE_DEL)))
				if (lp->flags & MODE_ADD)
				    {
					*mbuf++ = '+';
					whatt = MODE_ADD;
				    }
				else
				    {
					*mbuf++ = '-';
					whatt = MODE_DEL;
				    }
			len = strlen(pbuf);
			/*
			 * get c as the mode char and tmp as a pointer to
			 * the paramter for this mode change.
			 */
			switch(lp->flags & MODE_WPARAS)
			{
			case MODE_CHANOP :
				c = 'o';
				cp = lp->value.cptr->name;
				break;
			case MODE_VOICE :
				c = 'v';
				cp = lp->value.cptr->name;
				break;
			case MODE_BAN :
				c = 'b';
				cp = lp->value.cp;
				user = index(cp, '!');
				if (host = index(user ? user : cp, '@'))
					*host++ = '\0';
				if (user)
					*user++ = '\0';
				cp = make_nick_user_host(cp, user, host);
				break;
			case MODE_KEY :
				c = 'k';
				cp = lp->value.cp;
				break;
			case MODE_LIMIT :
				c = 'l';
				(void)sprintf(numeric, "%-15d", nusers);
				if (cp = index(numeric, ' '))
					*cp = '\0';
				cp = numeric;
				break;
			}

			if (len + strlen(cp) + 2 > MODEBUFLEN)
				break;
			/*
			 * pass on +/-o/v regardless of whether they are
			 * redundant or effective but check +b's to see if
			 * it existed before we created it.
			 */
			switch(lp->flags & MODE_WPARAS)
			{
			case MODE_KEY :
				*mbuf++ = c;
				(void)strcat(pbuf, cp);
				len += strlen(cp);
				(void)strcat(pbuf, " ");
				len++;
				if (!ischop)
					break;
				if (whatt == MODE_ADD)
					strncpyzt(mode->key, cp,
						  sizeof(mode->key));
				else
					*mode->key = '\0';
				break;
			case MODE_LIMIT :
				*mbuf++ = c;
				(void)strcat(pbuf, cp);
				len += strlen(cp);
				(void)strcat(pbuf, " ");
				len++;
				if (!ischop)
					break;
				mode->limit = nusers;
				break;
			case MODE_CHANOP :
			case MODE_VOICE :
				*mbuf++ = c;
				(void)strcat(pbuf, cp);
				len += strlen(cp);
				(void)strcat(pbuf, " ");
				len++;
				if (ischop)
					change_chan_flag(lp, chptr);
				break;
			case MODE_BAN :
				if (ischop && whatt & MODE_ADD &&
				    !add_banid(chptr, cp) ||
				    whatt & MODE_DEL && !del_banid(chptr, cp))
				    {
					*mbuf++ = c;
					(void)strcat(pbuf, cp);
					len += strlen(cp);
					(void)strcat(pbuf, " ");
					len++;
				    }
				break;
			}
		    } /* for (; i < opcnt; i++) */
	    } /* if (opcnt) */

	*mbuf++ = '\0';

	return ischop ? count : -1;
}

static	int	can_join(sptr, chptr, key)
aClient	*sptr;
Reg2	aChannel *chptr;
char	*key;
{
	Reg1	Link	*lp;

	if (is_banned(sptr, chptr))
		return (ERR_BANNEDFROMCHAN);
	if (chptr->mode.mode & MODE_INVITEONLY)
	    {
		for (lp = sptr->user->invited; lp; lp = lp->next)
			if (lp->value.chptr == chptr)
				break;
		if (!lp)
			return (ERR_INVITEONLYCHAN);
	    }

	if (!BadPtr(chptr->mode.key) &&
	    (BadPtr(key) || mycmp(chptr->mode.key, key)))
		return (ERR_BADCHANNELKEY);

	if (chptr->mode.limit && chptr->users >= chptr->mode.limit)
		return (ERR_CHANNELISFULL);

	return 0;
}

/*
** Remove bells and commas from channel name
*/

void	clean_channelname(cn)
Reg1	char *cn;
{
	for (; *cn; cn++)
		if (*cn == '\007' || *cn == ' ' || *cn == ',')
		    {
			*cn = '\0';
			return;
		    }
}

/*
** Return -1 if mask is present and doesnt match our server name.
*/
static	int	check_channelmask(sptr, cptr, chname)
aClient	*sptr, *cptr;
char	*chname;
{
	Reg1	char	*s;

	s = rindex(chname, ':');
	if (!s)
		return 0;

	s++;
	if (matches(s, me.name) || (IsServer(cptr) && matches(s, cptr->name)))
	    {
		if (MyClient(sptr))
			sendto_one(sptr, err_str(ERR_BADCHANMASK),
				   me.name, sptr->name, chname);
		return -1;
	    }
	return 0;
}

/*
**  Get Channel block for i (and allocate a new channel
**  block, if it didn't exists before).
*/
static	aChannel *get_channel(cptr, chname, flag)
aClient *cptr;
char	*chname;
int	flag;
    {
	Reg1	aChannel *chptr;
	int	len;

	if (BadPtr(chname))
		return NULL;

	len = strlen(chname);
	if (MyClient(cptr) && len > CHANNELLEN)
	    {
		len = CHANNELLEN;
		*(chname+CHANNELLEN) = '\0';
	    }
	if (chptr = find_channel(chname, (aChannel *)NULL))
		return (chptr);
	if (flag == CREATE)
	    {
		chptr = (aChannel *)MyMalloc(sizeof(aChannel) + len);
		bzero((char *)chptr, sizeof(aChannel));
		strncpyzt(chptr->chname, chname, len+1);
		if (channel)
			channel->prevch = chptr;
		chptr->prevch = NULL;
		chptr->nextch = channel;
		channel = chptr;
		(void)add_to_channel_hash_table(chname, chptr);
	    }
	return chptr;
    }

static	void	add_invite(cptr, chptr)
aClient *cptr;
aChannel *chptr;
{
	Reg1	Link	*inv, **tmp;

	del_invite(cptr,chptr);
	/*
	 * delete last link in chain if the list is max length
	 */
	if (list_length(cptr->user->invited) >= MAXCHANNELSPERUSER)
	    {
		inv = cptr->user->invited;
		cptr->user->invited = inv->next;
		free_link(inv);
	    }
	/*
	 * add client to channel invite list
	 */
	inv = make_link();
	inv->value.cptr = cptr;
	inv->next = chptr->invites;
	chptr->invites = inv;
	/*
	 * add channel to the end of the client invite list
	 */
	for (tmp = &(cptr->user->invited); *tmp; tmp = &((*tmp)->next))
		;
	inv = make_link();
	inv->value.chptr = chptr;
	inv->next = NULL;
	(*tmp) = inv;
}

/*
 * Delete Invite block from channel invite list and client invite list
 */
void	del_invite(cptr, chptr)
aClient *cptr;
aChannel *chptr;
{
	Reg1	Link	**inv, *tmp;

	for (inv = &(chptr->invites); *inv; inv = &((*inv)->next))
		if ((*inv)->value.cptr == cptr)
		    {
			tmp = *inv;
			*inv = tmp->next;
			free_link(tmp);
			break;
		    }

	for (inv = &(cptr->user->invited); *inv; inv = &((*inv)->next))
		if ((*inv)->value.chptr == chptr)
		    {
			tmp = *inv;
			*inv = tmp->next;
			free_link(tmp);
			break;
		    }
}

/*
**  Subtract one user from channel i (and free channel
**  block, if channel became empty).
*/
static	void	sub1_from_channel(xchptr)
aChannel *xchptr;
{
	Reg1	aChannel *chptr = xchptr;
	Reg2	Link *tmp;
	Link	*obtmp;

	if (--chptr->users <= 0)
	    {
		/*
		 * Now, find all invite links from channel structure
		 */
		while (tmp = chptr->invites)
			del_invite(tmp->value.cptr, xchptr);

		tmp = chptr->banlist;
		while (tmp)
		    {
			obtmp = tmp;
			tmp = tmp->next;
			(void)free(obtmp->value.cp);
			free_link(obtmp);
		    }
		if (chptr->prevch)
			chptr->prevch->nextch = chptr->nextch;
		else
			channel = chptr->nextch;
		if (chptr->nextch)
			chptr->nextch->prevch = chptr->prevch;
		(void)del_from_channel_hash_table(chptr->chname, chptr);
		(void)free((char *)chptr);
	    }
}

/*
** m_join
**	parv[0] = sender prefix
**	parv[1] = channel
**	parv[2] = channel password (key)
*/
int	m_join(cptr, sptr, parc, parv)
Reg2	aClient *cptr, *sptr;
int	parc;
char	*parv[];
{
	Reg1	Link	*lp;
	Reg3	aChannel *chptr;
	Reg4	char	*name, *key = NULL;
	int	i, flags = 0;
	char	*p = NULL, *p2 = NULL;

	if (check_registered(sptr))
		return 0;

	if (parc < 2 || *parv[1] == '\0')
	    {
		sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
			   me.name, parv[0], "JOIN");
		return 0;
	    }

	if (parv[2])
		key = strtoken(&p2, parv[2], ",");
	*buf = '\0';
	/*
	** Rebuild list of channels joined to be the actual result of the
	** JOIN.  Note that "JOIN 0" is the destructive problem.
	*/
	for (name = strtoken(&p, parv[1], ","); name;
	     name = strtoken(&p, NULL, ","))
	    {
		clean_channelname(name);
		if (check_channelmask(sptr, cptr, name)==-1)
			continue;
		if (*name == '0')
			*buf = '\0';
		else if (!IsChannelName(name))
		    {
			if (MyClient(sptr))
				sendto_one(sptr, err_str(ERR_NOSUCHCHANNEL),
					   me.name, parv[0], name);
			continue;
		    }
		if (*buf)
			(void)strcat(buf, ",");
		(void)strncat(buf, name, sizeof(buf)-strlen(buf)-1);
	    }
	(void)strcpy(parv[1], buf);

	for (name = strtoken(&p, buf, ","); name;
	     key = (key) ? strtoken(&p2, NULL, ",") : NULL,
	     name = strtoken(&p, NULL, ","))
	    {
		/*
		** JOIN 0 sends out a part for all channels a user
		** has joined.
		*/
		if (*name == '0' && !atoi(name))
		    {
			while (lp = sptr->user->channel)
			    {
				chptr = lp->value.chptr;
				sendto_channel_butserv(chptr, sptr, PartFmt,
						parv[0], chptr->chname);
				remove_user_from_channel(sptr, chptr);
			    }
			sendto_match_servs(NULL, cptr, ":%s JOIN 0", parv[0]);
			continue;
		    }

		if (MyConnect(sptr))
		    {
			/*
			** local client is first to enter prviously nonexistant
			** channel so make them (rightfully) the Channel
			** Operator.
			*/
			flags = (ChannelExists(name)) ? 0 : CHFL_CHANOP;

			if (list_length(sptr->user->channel) >=
			    MAXCHANNELSPERUSER)
			    {
				sendto_one(sptr, err_str(ERR_TOOMANYCHANNELS),
					   me.name, parv[0], name);
				return 0;
			    }
		    }

		chptr = get_channel(sptr, name, CREATE);
		if (IsMember(sptr, chptr))
			continue;

		if (!chptr ||
		    (MyConnect(sptr) && (i = can_join(sptr, chptr, key))))
		    {
			sendto_one(sptr,
				   ":%s %d %s %s :Sorry, cannot join channel.",
				   me.name, i, parv[0], name);
			continue;
		    }

		del_invite(sptr, chptr);
		/*
		**  Complete user entry to the new channel (if any)
		*/
		add_user_to_channel(chptr, sptr, flags);
		/*
		** notify all other users on the new channel
		*/
		sendto_channel_butserv(chptr, sptr, ":%s JOIN :%s",
					parv[0], name);
		sendto_match_servs(chptr, cptr, ":%s JOIN :%s", parv[0], name);

		if (MyClient(sptr))
		    {
			if (flags == CHFL_CHANOP)
				sendto_match_servs(chptr, cptr,
						   ":%s MODE %s +o %s",
						   me.name, name, parv[0]);
			if (chptr->topic[0] != '\0')
				sendto_one(sptr, rpl_str(RPL_TOPIC), me.name,
					   parv[0], name, chptr->topic);
			(void)m_names(cptr, sptr, 2, parv);
		    }
	    }
	return 0;
}

/*
** m_part
**	parv[0] = sender prefix
**	parv[1] = channel
*/
int	m_part(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int	parc;
char	*parv[];
    {
	Reg1	aChannel *chptr;
	char	*p = NULL, *name;

	if (check_registered(sptr))
		return 0;

	if (parc < 2 || parv[1][0] == '\0')
	    {
		sendto_one(sptr, rpl_str(ERR_NEEDMOREPARAMS),
			   me.name, parv[0], "PART");
		return 0;
	    }

#ifdef	V28PlusOnly
	*buf = '\0';
#endif

	for (; name = strtoken(&p, parv[1], ","); parv[1] = NULL)
	    {
		chptr = get_channel(sptr, name, 0);
		if (!chptr)
		    {
			sendto_one(sptr, err_str(ERR_NOSUCHCHANNEL),
				   me.name, parv[0], name);
			return 0;
		    }
		if (check_channelmask(sptr, cptr, name))
			continue;
		if (!IsMember(sptr, chptr))
		    {
			sendto_one(sptr, err_str(ERR_NOTONCHANNEL),
		    		   me.name, parv[0], name);
			return 0;
		    }
		/*
		**  Remove user from the old channel (if any)
		*/
#ifdef	V28PlusOnly
		if (*buf)
			(void)strcat(buf, ",");
		(void)strcat(buf, name);
#else
		sendto_match_servs(chptr, cptr, PartFmt, parv[0], name);
#endif
		sendto_channel_butserv(chptr, sptr, PartFmt, parv[0], name);
		remove_user_from_channel(sptr, chptr);
	    }
#ifdef	V28PlusOnly
	if (*buf)
		sendto_serv_butone(cptr, PartFmt, parv[0], buf);
#endif
	return 0;
    }

/*
** m_kick
**	parv[0] = sender prefix
**	parv[1] = channel
**	parv[2] = client to kick
**	parv[3] = kick comment
*/
int	m_kick(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int	parc;
char	*parv[];
    {
	aClient *who;
	aChannel *chptr;
	int	chasing = 0;
	char	*comment, *name, *p = NULL, *user, *p2 = NULL;

	if (check_registered(sptr))
		return 0;

	if (parc < 3 || *parv[1] == '\0')
	    {
		sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
			   me.name, parv[0], "KICK");
		return 0;
	    }

	*nickbuf = *buf = '\0';

	for (; name = strtoken(&p, parv[1], ","); parv[1] = NULL)
	    {
		chptr = get_channel(sptr, name, !CREATE);
		if (!chptr)
		    {
			sendto_one(sptr, err_str(ERR_NOSUCHCHANNEL),
				   me.name, parv[0], name);
			continue;
		    }
		if (check_channelmask(sptr, cptr, name))
			continue;
		if (!IsServer(sptr) && !is_chan_op(sptr, chptr))
		    {
			sendto_one(sptr, err_str(ERR_CHANOPRIVSNEEDED),
				   me.name, parv[0], chptr->chname);
			continue;
		    }
#ifdef	V28PlusOnly
		if (*buf)
			(void)strcat(buf, ",");
		(void)strcat(buf, chptr->name);
#endif

		for (; user = strtoken(&p2, parv[2], ","); parv[2] = NULL)
		    {
			if (!(who = find_chasing(sptr, user, &chasing)))
				continue; /* No such user left! */
			if (chasing)
				sendto_one(sptr,
				   ":%s NOTICE %s :KICK changed from %s to %s",
					   me.name, parv[0], user, who->name);
			comment = (BadPtr(parv[3])) ? parv[0] : parv[3];
			if (strlen(comment) > TOPICLEN)
				comment[TOPICLEN] = '\0';
			if (IsMember(who, chptr))
			    {
				sendto_channel_butserv(chptr, sptr,
						":%s KICK %s %s :%s", parv[0],
						name, who->name, comment);
#ifdef	V28PlusOnly
				if (*nickbuf)
					(void)strcat(nickbuf, ",");
				(void)strcat(nickbuf, who->name);
#else
				sendto_match_servs(chptr, cptr,
						   ":%s KICK %s %s :%s",
						   parv[0], name,
						   who->name, comment);
#endif
				remove_user_from_channel(who, chptr);
			    }
			else
				sendto_one(sptr, err_str(ERR_NOTONCHANNEL),
					   me.name, parv[0], who->name, name);
#ifndef	V28PlusOnly
			if (!IsServer(cptr))
				break;
#endif
		    } /* loop on parv[2] */
#ifndef	V28PlusOnly
		if (!IsServer(cptr))
			break;
#endif
	    } /* loop on parv[1] */

#ifdef	V28PlusOnly
	if (*buf && *nickbuf)
		sendto_serv_butone(cptr, ":%s KICK %s %s",
				   parv[0], buf, nickbuf);
#endif
	return (0);
 }
	
int	count_channels(sptr)
aClient	*sptr;
{
Reg1	aChannel	*chptr;
	Reg2	int	count = 0;

	for (chptr = channel; chptr; chptr = chptr->nextch)
#ifdef	SHOW_INVISIBLE_LUSERS
		if (SecretChannel(chptr))
		    {
			if (IsAnOper(sptr))
				count++;
		    }
		else
#endif
			count++;
	return (count);
}

/*
** m_topic
**	parv[0] = sender prefix
**	parv[1] = topic text
*/
int	m_topic(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int	parc;
char	*parv[];
    {
	aChannel *chptr = NullChn;
	char	*topic = NULL, *name, *p = NULL;
	
	if (check_registered(sptr))
		return 0;

	if (parc < 2)
	    {
		sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
			   me.name, parv[0], "TOPIC");
		return 0;
	    }

	for (; name = strtoken(&p, parv[1], ","); parv[1] = NULL)
	    {
		if (parc > 1 && IsChannelName(name))
		    {
			chptr = find_channel(name, NullChn);
			if (!chptr || !IsMember(sptr, chptr))
			    {
				sendto_one(sptr, err_str(ERR_NOTONCHANNEL),
					   me.name, parv[0], name);
				continue;
			    }
			if (parc > 2)
				topic = parv[2];
		    }

		if (!chptr)
		    {
			sendto_one(sptr, rpl_str(RPL_NOTOPIC),
				   me.name, parv[0], name);
			return 0;
		    }

		if (check_channelmask(sptr, cptr, name))
			continue;
	
		if (!topic)  /* only asking  for topic  */
		    {
			if (chptr->topic[0] == '\0')
			sendto_one(sptr, rpl_str(RPL_NOTOPIC),
				   me.name, parv[0], chptr->chname);
			else
				sendto_one(sptr, rpl_str(RPL_TOPIC),
					   me.name, parv[0],
					   chptr->chname, chptr->topic);
		    } 
		else if (((chptr->mode.mode & MODE_TOPICLIMIT) == 0 ||
			  is_chan_op(sptr, chptr)) && topic)
		    {
			/* setting a topic */
			strncpyzt(chptr->topic, topic, sizeof(chptr->topic));
			sendto_match_servs(chptr, cptr,":%s TOPIC %s :%s",
					   parv[0], chptr->chname,
					   chptr->topic);
			sendto_channel_butserv(chptr, sptr, ":%s TOPIC %s :%s",
					       parv[0],
					       chptr->chname, chptr->topic);
		    }
		else
		      sendto_one(sptr, err_str(ERR_CHANOPRIVSNEEDED),
				 me.name, parv[0], chptr->chname);
	    }
	return 0;
    }

/*
** m_invite
**	parv[0] - sender prefix
**	parv[1] - user to invite
**	parv[2] - channel number
*/
int	m_invite(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int	parc;
char	*parv[];
    {
	aClient *acptr;
	aChannel *chptr;

	if (check_registered(sptr))
		return 0;

	if (parc < 3 || *parv[1] == '\0')
	    {
		sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
			   me.name, parv[0], "INVITE");
		return -1;
	    }

	if (!(acptr = find_person(parv[1], (aClient *)NULL)))
	    {
		sendto_one(sptr, err_str(ERR_NOSUCHNICK),
			   me.name, parv[0], parv[1]);
		return 0;
	    }
	clean_channelname(parv[2]);
	if (check_channelmask(sptr, cptr, parv[2]))
		return 0;
	if (!(chptr = find_channel(parv[2], NullChn)))
	    {

		sendto_prefix_one(acptr, sptr, ":%s INVITE %s :%s",
				  parv[0], parv[1], parv[2]);
		return 0;
	    }

	if (chptr && !IsMember(sptr, chptr))
	    {
		sendto_one(sptr, err_str(ERR_NOTONCHANNEL),
			   me.name, parv[0], chptr->chname);
		return -1;
	    }

	if (IsMember(acptr, chptr))
	    {
		sendto_one(sptr, err_str(ERR_USERONCHANNEL),
			   me.name, parv[0], parv[1], parv[2]);
		return 0;
	    }
	if (chptr && (chptr->mode.mode & MODE_INVITEONLY))
	    {
		if (!is_chan_op(sptr, chptr))
		    {
			sendto_one(sptr, err_str(ERR_CHANOPRIVSNEEDED),
				   me.name, parv[0], chptr->chname);
			return -1;
		    }
		else if (!IsMember(sptr, chptr))
		    {
			sendto_one(sptr, err_str(ERR_CHANOPRIVSNEEDED),
				   me.name, parv[0],
				   ((chptr) ? (chptr->chname) : parv[2]));
			return -1;
		    }
	    }

	if (MyConnect(sptr))
	    {
		sendto_one(sptr, rpl_str(RPL_INVITING), me.name, parv[0],
			   acptr->name, ((chptr) ? (chptr->chname) : parv[2]));
		if (acptr->user->away)
			sendto_one(sptr, rpl_str(RPL_AWAY), me.name, parv[0],
				   acptr->name, acptr->user->away);
	    }
	if (MyConnect(acptr))
		if (chptr && (chptr->mode.mode & MODE_INVITEONLY) &&
		    sptr->user && is_chan_op(sptr, chptr))
			add_invite(acptr, chptr);
	sendto_prefix_one(acptr, sptr, ":%s INVITE %s :%s",parv[0],
			  acptr->name, ((chptr) ? (chptr->chname) : parv[2]));
	return 0;
    }


/*
** m_list
**      parv[0] = sender prefix
**      parv[1] = channel
*/
int	m_list(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int	parc;
char	*parv[];
    {
	aChannel *chptr;
	char	*name, *p = NULL;

	sendto_one(sptr, rpl_str(RPL_LISTSTART), me.name, parv[0]);

	if (parc < 2 || BadPtr(parv[1]))
	    {
		for (chptr = channel; chptr; chptr = chptr->nextch)
		    {
			if (!sptr->user ||
			    (SecretChannel(chptr) && !IsMember(sptr, chptr)))
				continue;
			sendto_one(sptr, rpl_str(RPL_LIST), me.name, parv[0],
				   ShowChannel(sptr, chptr)?chptr->chname:"*",
				   chptr->users,
				   ShowChannel(sptr, chptr)?chptr->topic:"");
		    }
		sendto_one(sptr, rpl_str(RPL_LISTEND), me.name, parv[0]);
		return 0;
	    }

	if (hunt_server(cptr, sptr, ":%s LIST %s %s", 2, parc, parv))
		return 0;

	for (; name = strtoken(&p, parv[1], ","); parv[1] = NULL)
	    {
		chptr = find_channel(name, NullChn);
		if (chptr && ShowChannel(sptr, chptr) && sptr->user)
			sendto_one(sptr, rpl_str(RPL_LIST), me.name, parv[0],
				   ShowChannel(sptr,chptr) ? name : "*",
				   chptr->users,
				   chptr->topic ? chptr->topic : "*");
	     }
	sendto_one(sptr, rpl_str(RPL_LISTEND), me.name, parv[0]);
	return 0;
    }


/************************************************************************
 * m_names() - Added by Jto 27 Apr 1989
 ************************************************************************/

/*
** m_names
**	parv[0] = sender prefix
**	parv[1] = channel
*/
int	m_names(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int	parc;
char	*parv[];
{ 
	Reg1	aChannel *chptr;
	Reg2	aClient *c2ptr;
	Reg3	Link	*lp;
	aChannel *ch2ptr = NULL;
	int	idx, flag, len;
	char	*s, *para = parc > 1 ? parv[1] : NULL;

	if (hunt_server(cptr, sptr, ":%s NAMES %s %s", 2, parc, parv))
		return 0;

	if (!BadPtr(para))
	    {
		s = index(para, ',');
		if (s)
		    {
			parv[1] = ++s;
			(void)m_names(cptr, sptr, parc, parv);
		    }
		clean_channelname(para);
		ch2ptr = find_channel(para, (aChannel *)NULL);
	    }

	*buf = '\0';

	/* Allow NAMES without registering
	 *
	 * First, do all visible channels (public and the one user self is)
	 */

	for (chptr = channel; chptr; chptr = chptr->nextch)
	    {
		if (!BadPtr(para) && ((ch2ptr != chptr) || !MyConnect(sptr)))
			continue; /* -- wanted a specific channel */
		if (!ShowChannel(sptr, chptr))
			continue; /* -- users on this are not listed */

		/* Find users on same channel (defined by chptr) */

		(void)strncpy(buf, "* ", 3);
		len = strlen(chptr->chname);
		(void)strcat(strncpy(buf + 2, chptr->chname, len+1), " :");

		if (PubChannel(chptr))
			*buf = '=';
		else if (SecretChannel(chptr))
			*buf = '@';
		idx = len + 4;
		flag = 1;
		for (lp = chptr->members; lp; lp = lp->next)
		    {
			c2ptr = lp->value.cptr;
			if (IsInvisible(c2ptr) && !IsMember(sptr,chptr))
				continue;
		        if (lp->flags & CHFL_CHANOP)
				(void)strcat(buf, "@");
			else if (lp->flags & CHFL_VOICE)
				(void)strcat(buf, "+");
			(void)strncat(buf, c2ptr->name, NICKLEN);
			idx += len + 1;
			flag = 1;
			(void)strcat(buf," ");
			if (idx + NICKLEN > BUFSIZE - 2)
			    {
				sendto_one(sptr, rpl_str(RPL_NAMREPLY),
					   me.name, parv[0], buf);
				(void)strncpy(buf, "* ", 3);
				(void)strncpy(buf+2, chptr->chname, len + 1);
				(void)strcat(buf, " :");
				if (PubChannel(chptr))
					*buf = '=';
				else if (SecretChannel(chptr))
					*buf = '@';
				idx = len + 4;
				flag = 0;
			    }
		    }
		if (flag)
			sendto_one(sptr, rpl_str(RPL_NAMREPLY),
				   me.name, parv[0], buf);
	    }
	if (!BadPtr(para))
	    {
		sendto_one(sptr, rpl_str(RPL_ENDOFNAMES), me.name, parv[0],
			   para);
		return(1);
	    }

	/* Second, do all non-public, non-secret channels in one big sweep */

	(void)strncpy(buf, "* * :", 6);
	idx = 5;
	flag = 0;
	for (c2ptr = client; c2ptr; c2ptr = c2ptr->next)
	    {
  	        aChannel *ch3ptr;
		int	showflag = 0, secret = 0;

		if (!IsPerson(c2ptr) || IsInvisible(c2ptr))
			continue;
		lp = c2ptr->user->channel;
		/*
		 * dont show a client if they are on a secret channel or
		 * they are on a channel sptr is on since they have already
		 * been show earlier. -avalon
		 */
		while (lp)
		    {
			ch3ptr = lp->value.chptr;
			if (PubChannel(ch3ptr) || IsMember(sptr, ch3ptr))
				showflag = 1;
			if (SecretChannel(ch3ptr))
				secret = 1;
			lp = lp->next;
		    }
		if (showflag) /* have we already shown them ? */
			continue;
		if (secret) /* on any secret channels ? */
			continue;
		(void)strncat(buf, c2ptr->name, NICKLEN);
		idx += strlen(c2ptr->name) + 1;
		(void)strcat(buf," ");
		flag = 1;
		if (idx + NICKLEN > BUFSIZE - 2)
		    {
			sendto_one(sptr, rpl_str(RPL_NAMREPLY),
				   me.name, parv[0], buf);
			(void)strncpy(buf, "* * :", 6);
			idx = 5;
			flag = 0;
		    }
	    }
	if (flag)
		sendto_one(sptr, rpl_str(RPL_NAMREPLY), me.name, parv[0], buf);
	sendto_one(sptr, rpl_str(RPL_ENDOFNAMES), me.name, parv[0], "*");
	return(1);
    }


void	send_user_joins(cptr, user)
aClient	*cptr, *user;
{
	Reg1	Link	*lp;
	Reg2	aChannel *chptr;
	Reg3	int	cnt = 0;
	char	 *mask;

	*buf = ':';
	(void)strcpy(buf+1, user->name);
	(void)strcat(buf, " JOIN ");

	for (lp = user->user->channel; lp; lp = lp->next)
	    {
		chptr = lp->value.chptr;
		if (mask = index(chptr->chname, ':'))
			if (matches(++mask, cptr->name))
				continue;
		if (strlen(chptr->chname) > BUFSIZE - 2 - strlen(buf))
		    {
			if (cnt)
				sendto_one(cptr, buf);
			*buf = ':';
			(void)strcpy(buf+1, user->name);
			(void)strcat(buf, " JOIN ");
			cnt = 0;
		    }
		(void)strncat(buf, chptr->chname, BUFSIZE - 2 - strlen(buf));
		cnt++;
		if (lp->next)
			(void)strcat(buf, ",");
	    }
	if (*buf && cnt)
		sendto_one(cptr, buf);

	return;
}