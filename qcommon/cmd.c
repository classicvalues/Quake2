/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// cmd.c -- Quake script command processing module

#include "qcommon.h"

void Cmd_ForwardToServer (void);


static int cmdWait;

#define	ALIAS_LOOP_COUNT	64
#define MACRO_LOOP_COUNT	64
static int	aliasCount;		// for detecting runaway loops


//=============================================================================

/*
============
Cmd_Wait_f

Causes execution of the remainder of the command buffer to be delayed until
next frame.  This allows commands like:
bind g "impulse 5 ; +attack ; wait ; -attack ; impulse 2"
============
*/
static void Cmd_Wait_f (void)
{
	switch (Cmd_Argc ())
	{
	case 1:
		cmdWait = 1;
		break;
	case 2:
		cmdWait = atoi (Cmd_Argv(1));
		break;
	default:
		Com_Printf ("Usage: wait [<num_frames>]\n");
	}
}


/*
=============================================================================

						COMMAND BUFFER

=============================================================================
*/

static sizebuf_t cmd_text;
static byte cmd_text_buf[8192];

static byte defer_text_buf[8192];

/*
============
Cbuf_Init
============
*/
void Cbuf_Init (void)
{
	SZ_Init (&cmd_text, cmd_text_buf, sizeof(cmd_text_buf));
}

/*
============
Cbuf_AddText

Adds command text at the end of the buffer
============
*/
void Cbuf_AddText (char *text)
{
	int		l;

	l = strlen (text);

	if (cmd_text.cursize + l >= cmd_text.maxsize)
	{
		Com_Printf ("Cbuf_AddText: overflow\n");
		return;
	}
	if (l > 256)
		Com_DPrintf ("Cbuf_AddText: %d chars\n", l);
	else
		Com_DPrintf ("Cbuf_AddText: \"%s\"\n", text);
	SZ_Write (&cmd_text, text, strlen (text));
}


/*
============
Cbuf_InsertText

Adds command text immediately after the current command
Adds a \n to the text
FIXME: actually change the command buffer to do less copying
============
*/
void Cbuf_InsertText (char *text)
{
	char	*temp;
	int		templen;

	// copy off any commands still remaining in the exec buffer
	templen = cmd_text.cursize;
	if (templen)
	{
		temp = Z_Malloc (templen);
		memcpy (temp, cmd_text.data, templen);
		SZ_Clear (&cmd_text);
	}
	else
		temp = NULL;	// shut up compiler

	// add the entire text of the file
	Cbuf_AddText (text);

	// add the copied off data
	if (templen)
	{
		SZ_Write (&cmd_text, temp, templen);
		Z_Free (temp);
	}
}


/*
============
Cbuf_CopyToDefer
============
*/
void Cbuf_CopyToDefer (void)
{
	memcpy(defer_text_buf, cmd_text_buf, cmd_text.cursize);
	defer_text_buf[cmd_text.cursize] = 0;
	cmd_text.cursize = 0;
}

/*
============
Cbuf_InsertFromDefer
============
*/
void Cbuf_InsertFromDefer (void)
{
	Cbuf_InsertText (defer_text_buf);
	defer_text_buf[0] = 0;
}


/*
============
Cbuf_Execute
============
*/
void Cbuf_Execute (void)
{
	int		i, len;
	char	*text;
	char	line[1024];
	int		quotes;

	aliasCount = 0;		// don't allow infinite alias loops

	while (cmd_text.cursize)
	{
		if (cmdWait)
		{
			// skip out while text still remains in buffer, leaving it
			// for next frame
			cmdWait--;
			break;
		}

		// find a \n or ; line break
		text = (char*)cmd_text.data;

		quotes = 0;
		len = cmd_text.cursize;
		for (i = 0; i < cmd_text.cursize; i++)
		{
			if (text[i] == '"')
				quotes ^= 1;

			if (!quotes && text[i] == '/' && i < cmd_text.cursize-1 && text[i+1] == '/')
			{	// remove "//" comments
				len = i;
				while (i < cmd_text.cursize && text[i] != '\n') i++;
				break;
			}
			if (!quotes && text[i] == ';')
			{
				len = i;
				break;	// don't break if inside a quoted string
			}
			if (text[i] == '\n')
			{
				len = i;
				break;
			}
		}

		memcpy (line, text, len);
		line[len] = 0;

		// delete the text from the command buffer and move remaining commands down
		// this is necessary because commands (exec, alias) can insert data at the
		// beginning of the text buffer

		if (i == cmd_text.cursize)
			cmd_text.cursize = 0;
		else
		{
			i++;
			cmd_text.cursize -= i;
			memmove (text, text+i, cmd_text.cursize);
		}

		// execute the command line
		Cmd_ExecuteString (line);
	}
}


/*
===============
Cbuf_AddEarlyCommands

Adds command line parameters as script statements
Commands lead with a +, and continue until another +

Set commands are added early, so they are guaranteed to be set before
the client and server initialize for the first time.

Other commands are added late, after all initialization is complete.
===============
*/
void Cbuf_AddEarlyCommands (qboolean clear)
{
	int		i;
	char	*s;

	for (i = 0; i < COM_Argc(); i++)
	{
		s = COM_Argv(i);
		if (strcmp (s, "+set"))
			continue;
		Cbuf_AddText (va("set %s %s\n", COM_Argv(i+1), COM_Argv(i+2)));
		if (clear)
		{
			COM_ClearArgv(i);
			COM_ClearArgv(i+1);
			COM_ClearArgv(i+2);
		}
		i+=2;
	}
}

/*
=================
Cbuf_AddLateCommands

Adds command line parameters as script statements
Commands lead with a '+' and continue until another '+'
quake2 +vid_ref gl +map amlev1

Returns true if any late commands were added, which
will keep the demoloop from immediately starting
=================
*/
qboolean Cbuf_AddLateCommands (void)
{
	int		i, s, argc;
	char	*text, *build;
	qboolean ret;

	// build the combined string to parse from
	s = 0;
	argc = COM_Argc();
	for (i = 1; i < argc; i++)
	{
		s += strlen (COM_Argv(i)) + 1;
	}
	if (!s)
		return false;

	text = Z_Malloc (s+1);
//	text[0] = 0; -- zero initialized
	for (i = 1; i < argc; i++)
	{
		strcat (text, COM_Argv(i));
		if (i != argc-1)
			strcat (text, " ");
	}

	// pull out the commands
	build = Z_Malloc (s+1);
	build[0] = 0;

	for (i = 0; i < s-1; i++)
	{
		if (text[i] == '+')
		{
			char	c;
			int		quote, j;

			i++;				// skip '+'
			quote = 0;
			for (j = i; (c = text[j]) != 0; j++)
			{
				if (c == '"')
					quote ^= 1;
				if (!quote && c == '+')
					break;		// found unquoted '+' -- next command
			}

			c = text[j];
			text[j] = 0;

			strcat (build, text+i);
			strcat (build, "\n");
			text[j] = c;
			i = j-1;
		}
	}

	ret = (build[0] != 0);
	if (ret)
		Cbuf_AddText (build);

	Z_Free (text);
	Z_Free (build);

	return ret;
}


/*
==============================================================================

						SCRIPT COMMANDS

==============================================================================
*/


/*
===============
Cmd_Exec_f
===============
*/
void Cmd_Exec_f (void)
{
	char	*f;
	int		len;

	if (Cmd_Argc () != 2)
	{
		Com_Printf ("Usage: exec <filename>\n");
		return;
	}

	len = FS_LoadFile (Cmd_Argv(1), (void **)&f);
	if (!f)
	{
		Com_WPrintf ("Couldn't exec %s\n", Cmd_Argv(1));
		return;
	}
	Com_Printf ("Execing %s\n", Cmd_Argv(1));

	Cbuf_InsertText (f);

	FS_FreeFile (f);
}


/*
===============
Cmd_Echo_f

Just prints the rest of the line to the console
===============
*/
void Cmd_Echo_f (void)
{
	int		i;

	for (i = 1; i < Cmd_Argc (); i++)
		Com_Printf ("%s ", Cmd_Argv(i));
	Com_Printf ("\n");
}

/*
===============
Cmd_Alias_f

Creates a new command that executes a command string (possibly ";"-separated)
===============
*/
void Cmd_Alias_f (void)
{
	cmdAlias_t	*a;
	char	cmd[1024];
	int		i, c;
	char	*name;

	if (Cmd_Argc() == 2)
	{
		Com_Printf ("Usage: alias <name> <value>\n");
		return;
	}

	if (Cmd_Argc() == 1)
	{
		Com_Printf ("Current alias commands:\n");
		for (a = cmdAlias; a; a = a->next)
			Com_Printf ("%s \"%s\"\n", a->name, a->value);
		return;
	}

	name = Cmd_Argv(1);

	// if the alias already exists, reuse it
	for (a = cmdAlias; a; a = a->next)
	{
		if (!strcmp (name, a->name))
		{
			Z_Free (a->value);
			break;
		}
	}

	if (!a)
	{
		a = (cmdAlias_t*) AllocNamedStruc (sizeof(cmdAlias_t), name);
		a->next = cmdAlias;
		cmdAlias = a;
	}

	// copy the rest of the command line
	cmd[0] = 0;		// start out with a null string
	c = Cmd_Argc();
	for (i = 2; i < c; i++)
	{
		strcat (cmd, Cmd_Argv(i));
		if (i != (c - 1))
			strcat (cmd, " ");
	}

	a->value = CopyString (cmd);
}


void Cmd_Unalias_f (void)
{
	cmdAlias_t *alias, *prev, *next;
	int		n;

	if (Cmd_Argc() != 2)
	{
		Com_Printf ("Usage: unalias <mask>\n");
		return;
	}

	prev = NULL;
	n = 0;
	for (alias = cmdAlias; alias; alias = next)
	{
		next = alias->next;
		if (MatchWildcard2 (alias->name, Cmd_Argv(1), true))
		{
			if (prev)
				prev->next = alias->next;
			else
				cmdAlias = alias->next;
			Z_Free (alias->value);
			FreeNamedStruc (alias);
			n++;
		}
		else
			prev = alias;
	}
	Com_Printf ("%d aliases removed\n", n);
}


void Cmd_WriteAliases (FILE *f)
{
	cmdAlias_t *alias;

	for (alias = cmdAlias; alias; alias = alias->next)
		fprintf (f, "alias %s \"%s\"\n", alias->name, alias->value);
}


/*
=============================================================================

					COMMAND EXECUTION

=============================================================================
*/

static int	cmd_argc;
static char	*cmd_argv[MAX_STRING_TOKENS];
static char	cmd_args[MAX_STRING_CHARS];

/*
============
Cmd_Argc
============
*/
int Cmd_Argc (void)
{
	return cmd_argc;
}

/*
============
Cmd_Argv
============
*/
char *Cmd_Argv (int arg)
{
	if (arg >= cmd_argc)
		return "";
	return cmd_argv[arg];
}

/*
============
Cmd_Args

Returns a single string containing argv(1) to argv(argc()-1)
============
*/
char *Cmd_Args (void)
{
	return cmd_args;
}


/*
======================
Cmd_MacroExpandString
======================
*/
static char *Cmd_MacroExpandString (char *text)
{
	int		i, j, count, len;
	int		quotes;
	char	*scan;
	static	char	expanded[MAX_STRING_CHARS];
	char	temporary[MAX_STRING_CHARS];
	char	*token, *start;

	quotes = 0;
	scan = text;

	len = strlen (scan);
	if (len >= MAX_STRING_CHARS)
	{
		Com_WPrintf ("Line exceeded %d chars, discarded\n", MAX_STRING_CHARS);
		return NULL;
	}

	count = 0;

	for (i = 0; i < len; i++)
	{
		if (scan[i] == '"')
			quotes ^= 1;
		if (quotes)
			continue;	// don't expand inside quotes
		if (scan[i] != '$')
			continue;
		// scan out the complete macro
		start = scan+i+1;
		token = COM_Parse (&start);
		if (!start)
			continue;

		token = Cvar_VariableString (token);

		j = strlen (token);
		len += j;
		if (len >= MAX_STRING_CHARS)
		{
			Com_WPrintf ("Expanded line exceeded %d chars, discarded\n", MAX_STRING_CHARS);
			return NULL;
		}

		strncpy (temporary, scan, i);
		strcpy (temporary+i, token);
		strcpy (temporary+i+j, start);

		strcpy (expanded, temporary);
		scan = expanded;
		i--;

		if (++count == MACRO_LOOP_COUNT)
		{
			Com_WPrintf ("Macro expansion loop, discarded\n");
			return NULL;
		}
	}

	if (quotes)
	{
		Com_WPrintf ("Line has unmatched quote, discarded.\n");
		return NULL;
	}

	return scan;
}


/*
============
Cmd_TokenizeString

Parses the given string into command line tokens.
$Cvars will be expanded unless they are in a quoted token
============
*/
void Cmd_TokenizeString (char *text, qboolean macroExpand)
{
	int		i;
	char	*com_token;

	// clear the args from the last string
	for (i = 0; i < cmd_argc; i++)
		Z_Free (cmd_argv[i]);

	cmd_argc = 0;
	cmd_args[0] = 0;

	// macro expand the text
	if (macroExpand)
		text = Cmd_MacroExpandString (text);
	if (!text) return;

	while (1)
	{
		// skip whitespace up to a /n
		while (*text && *text <= ' ' && *text != '\n') text++;

		if (!text[0] || text[0] == '\n')
			return;

		// set cmd_args to everything after the first arg
		if (cmd_argc == 1)
		{
			int		l;

			strcpy (cmd_args, text);

			// strip off any trailing whitespace
			l = strlen (cmd_args) - 1;
			while (l >= 0 && cmd_args[l] <= ' ')
				cmd_args[l--] = 0;
		}

		com_token = COM_Parse (&text);
		if (!text) return;

		if (cmd_argc < MAX_STRING_TOKENS)
		{
			cmd_argv[cmd_argc] = Z_Malloc (strlen(com_token)+1);
			strcpy (cmd_argv[cmd_argc], com_token);
			cmd_argc++;
		}
	}
}


/*
============
Cmd_AddCommand
============
*/
void Cmd_AddCommand (char *cmd_name, void (*func) (void))
{
	cmdFunc_t *cmd;

	// fail if the command is a variable name
	if (Cvar_VariableString (cmd_name)[0])
	{
		Com_WPrintf ("Cmd_AddCommand: %s already defined as a var\n", cmd_name);
		return;
	}

	// fail if the command already exists
	for (cmd = cmdFuncs; cmd; cmd = cmd->next)
	{
		if (!strcmp (cmd_name, cmd->name))
		{
			Com_WPrintf ("Cmd_AddCommand: %s already defined\n", cmd_name);
			return;
		}
	}

	cmd = Z_Malloc (sizeof(cmdFunc_t));
	cmd->name = cmd_name;
	cmd->func = func;
	cmd->next = cmdFuncs;
	cmdFuncs = cmd;
}

/*
============
Cmd_RemoveCommand
============
*/
void Cmd_RemoveCommand (char *cmd_name)
{
	cmdFunc_t *cmd, **back;

	back = &cmdFuncs;
	while (1)
	{
		cmd = *back;
		if (!cmd)
		{
			Com_WPrintf ("Cmd_RemoveCommand: %s not found\n", cmd_name);
			return;
		}
		if (!strcmp (cmd_name, cmd->name))
		{
			*back = cmd->next;
			Z_Free (cmd);
			return;
		}
		back = &cmd->next;
	}
}

/*
============
Cmd_ExecuteString

A complete command line has been parsed, so try to execute it
FIXME: lookupnoadd the token to speed search?
============
*/
void Cmd_ExecuteString (char *text)
{
	cmdFunc_t	*cmd;
	cmdAlias_t	*a;

	Cmd_TokenizeString (text, true);

	// execute the command line
	if (!Cmd_Argc ())
		return;		// no tokens

	Com_DPrintf ("cmd: %s\n", text);

	// check functions
	for (cmd = cmdFuncs; cmd; cmd = cmd->next)
	{
		if (!Q_strcasecmp (cmd_argv[0], cmd->name))
		{
			if (!cmd->func)
			{	// forward to server command
				Cmd_ExecuteString (va("cmd %s", text));
			}
			else
				cmd->func ();
			return;
		}
	}

	// check alias
	for (a = cmdAlias; a; a = a->next)
	{
		if (!Q_strcasecmp (cmd_argv[0], a->name))
		{
			if (++aliasCount == ALIAS_LOOP_COUNT)
			{
				Com_WPrintf ("ALIAS_LOOP_COUNT\n");
				return;
			}
			Cbuf_InsertText (va("%s\n", a->value));
			return;
		}
	}

	// check cvars
	if (Cvar_Command ())
		return;

	// send it as a server command if we are connected
	Cmd_ForwardToServer ();
}

/*
============
Cmd_List_f
============
*/
static void Cmd_List_f (void)
{
	cmdFunc_t *cmd;
	int		i;
	char	*mask;

	if (Cmd_Argc () > 2)
	{
		Com_Printf ("Usage: cmdlist [<mask>]\n");
		return;
	}

	if (Cmd_Argc () == 2)
		mask = Cmd_Argv (1);
	else
		mask = NULL;

	i = 0;
	for (cmd = cmdFuncs; cmd; cmd = cmd->next)
	{
		if (mask && !MatchWildcard2 (cmd->name, mask, true)) continue;
		i++;
		Com_Printf ("%s\n", cmd->name);
	}
	Com_Printf ("%d commands\n", i);
}

/*
============
Cmd_Init
============
*/
void Cmd_Init (void)
{
	Cmd_AddCommand ("cmdlist", Cmd_List_f);
	Cmd_AddCommand ("exec", Cmd_Exec_f);
	Cmd_AddCommand ("echo", Cmd_Echo_f);
	Cmd_AddCommand ("alias", Cmd_Alias_f);
	Cmd_AddCommand ("unalias", Cmd_Unalias_f);
	Cmd_AddCommand ("wait", Cmd_Wait_f);
}
