/* Tracing functionality for remote targets in custom GDB protocol
   Copyright 1997 Free Software Foundation, Inc.

This file is part of GDB.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include "defs.h"
#include "symtab.h"
#include "frame.h"
#include "tracepoint.h"
#include "gdbtypes.h"
#include "expression.h"
#include "gdbcmd.h"
#include "value.h"
#include "target.h"
#include "language.h"
#include "gdb_string.h"

/* readline include files */
#include "readline.h"
#include "history.h"

/* readline defines this.  */
#undef savestring

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

extern int info_verbose;

/* If this definition isn't overridden by the header files, assume
   that isatty and fileno exist on this system.  */
#ifndef ISATTY
#define ISATTY(FP)	(isatty (fileno (FP)))
#endif

/* Chain of all tracepoints defined.  */
struct tracepoint *tracepoint_chain;

/* Number of last tracepoint made.  */
static int tracepoint_count;

/* Number of last traceframe collected.  */
static int traceframe_number;

/* Tracepoint for last traceframe collected.  */
static int tracepoint_number;

/* Symbol for function for last traceframe collected */
static struct symbol *traceframe_fun;

/* Symtab and line for last traceframe collected */
static struct symtab_and_line traceframe_sal;

/* Utility: returns true if "target remote" */
static int
target_is_remote ()
{
  if (current_target.to_shortname &&
      strcmp (current_target.to_shortname, "remote") == 0)
    return 1;
  else
    return 0;
}

/* Utility: generate error from an incoming stub packet.  */
static void 
trace_error (buf)
     char *buf;
{
  if (*buf++ != 'E')
    return;			/* not an error msg */
  switch (*buf) 
    {
    case '1':			/* malformed packet error */
      if (*++buf == '0')	/*   general case: */
	error ("tracepoint.c: error in outgoing packet.");
      else
	error ("tracepoint.c: error in outgoing packet at field #%d.", 
	       strtol (buf, NULL, 16));
    case '2':
      error ("trace API error 0x%s.", ++buf);
    default:
      error ("Target returns error code '%s'.", buf);
    }
}

/* Obsolete: collect regs from a trace frame */
static void
trace_receive_regs (buf)
     char *buf;
{
  long regno, i;
  char regbuf[MAX_REGISTER_RAW_SIZE], *tmp, *p = buf;

  while (*p)
    {
      regno = strtol (p, &tmp, 16);
      if (p == tmp || *tmp++ != ':')
	error ("tracepoint.c: malformed 'R' packet");
      else p = tmp;

      for (i = 0; i < REGISTER_RAW_SIZE (regno); i++)
	{
	  if (p[0] == 0 || p[1] == 0)
	    warning ("Remote reply is too short: %s", buf);
	  regbuf[i] = fromhex (p[0]) * 16 + fromhex (p[1]);
	  p += 2;
	}

      if (*p++ != ';')
	error ("tracepoint.c: malformed 'R' packet");

      supply_register (regno, regbuf);
    }
}

/* Utility: wait for reply from stub, while accepting "O" packets */
static char *
remote_get_noisy_reply (buf)
     char *buf;
{
  do	/* loop on reply from remote stub */
    {
      getpkt (buf, 0);
      if (buf[0] == 0)
	error ("Target does not support this command.");
      else if (buf[0] == 'E')
	trace_error (buf);
      else if (buf[0] == 'R')
	{
	  flush_cached_frames ();
	  registers_changed ();
	  select_frame (get_current_frame (), 0);
	  trace_receive_regs (buf);
	}
      else if (buf[0] == 'O' &&
	       buf[1] != 'K')
	remote_console_output (buf + 1);	/* 'O' message from stub */
      else
	return buf;				/* here's the actual reply */
    } while (1);
}

/* Set tracepoint count to NUM.  */
static void
set_tracepoint_count (num)
     int num;
{
  tracepoint_count = num;
  set_internalvar (lookup_internalvar ("tpnum"),
		   value_from_longest (builtin_type_int, (LONGEST) num));
}

/* Set traceframe number to NUM.  */
static void
set_traceframe_num (num)
     int num;
{
  traceframe_number = num;
  set_internalvar (lookup_internalvar ("trace_frame"),
		   value_from_longest (builtin_type_int, (LONGEST) num));
}

/* Set tracepoint number to NUM.  */
static void
set_tracepoint_num (num)
     int num;
{
  tracepoint_number = num;
  set_internalvar (lookup_internalvar ("tracepoint"),
		   value_from_longest (builtin_type_int, (LONGEST) num));
}

/* Set externally visible debug variables for querying/printing
   the traceframe context (line, function, file) */

static void
set_traceframe_context (trace_pc)
     CORE_ADDR trace_pc;
{
  static struct type *func_string, *file_string;
  static struct type *func_range,  *file_range;
  static value_ptr    func_val,     file_val;
  static struct type *charstar;
  int len;

  if (charstar == (struct type *) NULL)
    charstar = lookup_pointer_type (builtin_type_char);

  if (trace_pc == -1)	/* cease debugging any trace buffers */
    {
      traceframe_fun = 0;
      traceframe_sal.pc = traceframe_sal.line = 0;
      traceframe_sal.symtab = NULL;
      set_internalvar (lookup_internalvar ("trace_func"), 
		       value_from_longest (charstar, (LONGEST) 0));
      set_internalvar (lookup_internalvar ("trace_file"), 
		       value_from_longest (charstar, (LONGEST) 0));
      set_internalvar (lookup_internalvar ("trace_line"),
		       value_from_longest (builtin_type_int, (LONGEST) -1));
      return;
    }

  /* save as globals for internal use */
  traceframe_sal = find_pc_line (trace_pc, 0);
  traceframe_fun = find_pc_function (trace_pc);

  /* save linenumber as "$trace_line", a debugger variable visible to users */
  set_internalvar (lookup_internalvar ("trace_line"),
		   value_from_longest (builtin_type_int, 
				       (LONGEST) traceframe_sal.line));

  /* save func name as "$trace_func", a debugger variable visible to users */
  if (traceframe_fun == NULL || 
      SYMBOL_NAME (traceframe_fun) == NULL)
    set_internalvar (lookup_internalvar ("trace_func"), 
		     value_from_longest (charstar, (LONGEST) 0));
  else
    {
      len = strlen (SYMBOL_NAME (traceframe_fun));
      func_range  = create_range_type (func_range,  
				       builtin_type_int, 0, len - 1);
      func_string = create_array_type (func_string, 
				       builtin_type_char, func_range);
      func_val = allocate_value (func_string);
      VALUE_TYPE (func_val) = func_string;
      memcpy (VALUE_CONTENTS_RAW (func_val), 
	      SYMBOL_NAME (traceframe_fun), 
	      len);
      func_val->modifiable = 0;
      set_internalvar (lookup_internalvar ("trace_func"), func_val);
    }

  /* save file name as "$trace_file", a debugger variable visible to users */
  if (traceframe_sal.symtab == NULL || 
      traceframe_sal.symtab->filename == NULL)
    set_internalvar (lookup_internalvar ("trace_file"), 
		     value_from_longest (charstar, (LONGEST) 0));
  else
    {
      len = strlen (traceframe_sal.symtab->filename);
      file_range  = create_range_type (file_range,  
				       builtin_type_int, 0, len - 1);
      file_string = create_array_type (file_string, 
				       builtin_type_char, file_range);
      file_val = allocate_value (file_string);
      VALUE_TYPE (file_val) = file_string;
      memcpy (VALUE_CONTENTS_RAW (file_val), 
	      traceframe_sal.symtab->filename, 
	      len);
      file_val->modifiable = 0;
      set_internalvar (lookup_internalvar ("trace_file"), file_val);
    }
}

/* Low level routine to set a tracepoint.
   Returns the tracepoint object so caller can set other things.
   Does not set the tracepoint number!
   Does not print anything.

   ==> This routine should not be called if there is a chance of later
   error(); otherwise it leaves a bogus tracepoint on the chain.  Validate
   your arguments BEFORE calling this routine!  */

static struct tracepoint *
set_raw_tracepoint (sal)
     struct symtab_and_line sal;
{
  register struct tracepoint *t, *tc;
  struct cleanup *old_chain;

  t = (struct tracepoint *) xmalloc (sizeof (struct tracepoint));
  old_chain = make_cleanup (free, t);
  memset (t, 0, sizeof (*t));
  t->address = sal.pc;
  if (sal.symtab == NULL)
    t->source_file = NULL;
  else
    {
      char *p;

      t->source_file = (char *) xmalloc (strlen (sal.symtab->filename) +
                                         strlen (sal.symtab->dirname) + 2);

      strcpy (t->source_file, sal.symtab->dirname);
      p = t->source_file;
      while (*p++) ;
      if (*p != '/')            /* Will this work on Windows? */
        strcat (t->source_file, "/");
      strcat (t->source_file, sal.symtab->filename);
    }

  t->language = current_language->la_language;
  t->input_radix = input_radix;
  t->line_number = sal.line;
  t->enabled = enabled;
  t->next = 0;
  t->step_count = 0;
  t->pass_count = 0;

  /* Add this tracepoint to the end of the chain
     so that a list of tracepoints will come out in order
     of increasing numbers.  */

  tc = tracepoint_chain;
  if (tc == 0)
    tracepoint_chain = t;
  else
    {
      while (tc->next)
	tc = tc->next;
      tc->next = t;
    }
  discard_cleanups (old_chain);
  return t;
}

static void
trace_command (arg, from_tty)
     char *arg;
     int from_tty;
{
  char **canonical = (char **)NULL;
  struct symtabs_and_lines sals;
  struct symtab_and_line sal;
  struct tracepoint *t;
  char *addr_start = 0, *addr_end = 0, *cond_start = 0, *cond_end = 0;
  int i;

  if (!arg || !*arg)
    error ("trace command requires an argument");

  if (from_tty && info_verbose)
    printf_filtered ("TRACE %s\n", arg);

  if (arg[0] == '/')
    {
      return;
    }

  addr_start = arg;
  sals = decode_line_1 (&arg, 1, (struct symtab *)NULL, 0, &canonical);
  addr_end   = arg;
  if (! sals.nelts) 
    return;	/* ??? Presumably decode_line_1 has already warned? */

  /* Resolve all line numbers to PC's */
  for (i = 0; i < sals.nelts; i++)
    resolve_sal_pc (&sals.sals[i]);

  /* Now set all the tracepoints.  */
  for (i = 0; i < sals.nelts; i++)
    {
      sal = sals.sals[i];

      t = set_raw_tracepoint (sal);
      set_tracepoint_count (tracepoint_count + 1);
      t->number = tracepoint_count;

      /* If a canonical line spec is needed use that instead of the
	 command string.  */
      if (canonical != (char **)NULL && canonical[i] != NULL)
	t->addr_string = canonical[i];
      else if (addr_start)
	t->addr_string = savestring (addr_start, addr_end - addr_start);
      if (cond_start)
	t->cond_string = savestring (cond_start, cond_end - cond_start);

      /* Let the UI know of any additions */
      if (create_tracepoint_hook)
	create_tracepoint_hook (t);
    }

  if (sals.nelts > 1)
    {
      printf_filtered ("Multiple tracepoints were set.\n");
      printf_filtered ("Use the \"delete\" command to delete unwanted tracepoints.\n");
    }
}

static void
tracepoints_info (tpnum_exp, from_tty)
     char *tpnum_exp;
     int from_tty;
{
  struct tracepoint *t;
  struct action_line *action;
  int found_a_tracepoint = 0;
  char wrap_indent[80];
  struct symbol *sym;
  int tpnum = -1;
#if 0
  char *i1 = "\t", *i2 = "\t  ";
  char *indent, *actionline;;
#endif

  if (tpnum_exp)
    tpnum = parse_and_eval_address (tpnum_exp);

  ALL_TRACEPOINTS (t)
    if (tpnum == -1 || tpnum == t->number)
      {
	extern int addressprint;	/* print machine addresses? */

	if (!found_a_tracepoint++)
	  {
	    printf_filtered ("Num Enb ");
	    if (addressprint)
	      printf_filtered ("Address    ");
	    printf_filtered ("PassC StepC What\n");
	  }
	strcpy (wrap_indent, "                           ");
	if (addressprint)
	  strcat (wrap_indent, "           ");

	printf_filtered ("%-3d %-3s ", t->number, 
			 t->enabled == enabled ? "y" : "n");
	if (addressprint)
	  printf_filtered ("%s ", 
			   local_hex_string_custom ((unsigned long) t->address, 
						    "08l"));
	printf_filtered ("%-5d %-5d ", t->pass_count, t->step_count);

	if (t->source_file)
	  {
	    sym = find_pc_function (t->address);
	    if (sym)
	      {
		fputs_filtered ("in ", gdb_stdout);
		fputs_filtered (SYMBOL_SOURCE_NAME (sym), gdb_stdout);
		wrap_here (wrap_indent);
		fputs_filtered (" at ", gdb_stdout);
	      }
	    fputs_filtered (t->source_file, gdb_stdout);
	    printf_filtered (":%d", t->line_number);
	  }
	else
	  print_address_symbolic (t->address, gdb_stdout, demangle, " ");

	printf_filtered ("\n");
	if (t->actions)
	  {
	    printf_filtered ("  Actions for tracepoint %d: \n", t->number);
/*	    indent = i1; */
	    for (action = t->actions; action; action = action->next)
	      {
#if 0
		actionline = action->action;
		while (isspace(*actionline))
		  actionline++;

		printf_filtered ("%s%s\n", indent, actionline);
		if (0 == strncasecmp (actionline, "while-stepping", 14))
		  indent = i2;
		else if (0 == strncasecmp (actionline, "end", 3))
		  indent = i1;
#else
		printf_filtered ("\t%s\n", action->action);
#endif
	      }
	  }
      }
  if (!found_a_tracepoint)
    {
      if (tpnum == -1)
        printf_filtered ("No tracepoints.\n");
      else
        printf_filtered ("No tracepoint number %d.\n", tpnum);
    }
}

/* Optimization: the code to parse an enable, disable, or delete TP command
   is virtually identical except for whether it performs an enable, disable,
   or delete.  Therefore I've combined them into one function with an opcode.
   */
enum tracepoint_opcode 
{
  enable, 
  disable,
  delete
};

/* This function implements enable, disable and delete. */
static void
tracepoint_operation (t, from_tty, opcode)
     struct tracepoint *t;
     int from_tty;
     enum tracepoint_opcode opcode;
{
  struct tracepoint *t2;
  struct action_line *action, *next;

  switch (opcode) {
  case enable:
    t->enabled = enabled;
    break;
  case disable:
    t->enabled = disabled;
    break;
  case delete:
    if (tracepoint_chain == t)
      tracepoint_chain = t->next;

    ALL_TRACEPOINTS (t2)
      if (t2->next == t)
	{
	  t2->next = t->next;
	  break;
	}

    /* Let the UI know of any deletions */
    if (delete_tracepoint_hook)
      delete_tracepoint_hook (t);

    if (t->cond_string)
      free (t->cond_string);
    if (t->addr_string)
      free (t->addr_string);
    if (t->source_file)
      free (t->source_file);
    for (action = t->actions; action; action = next)
      {
	next = action->next;
	if (action->action) 
	  free (action->action);
	free (action);
      }
    free (t);
    break;
  }
}

/* Utility: parse a tracepoint number and look it up in the list.  */
struct tracepoint *
get_tracepoint_by_number (arg)
     char **arg;
{
  struct tracepoint *t;
  char *end, *copy;
  value_ptr val;
  int tpnum;

  if (arg == 0)
    error ("Bad tracepoint argument");

  if (*arg == 0 || **arg == 0)	/* empty arg means refer to last tp */
    tpnum = tracepoint_count;
  else if (**arg == '$')	/* handle convenience variable */
    {
      /* Make a copy of the name, so we can null-terminate it
	 to pass to lookup_internalvar().  */
      end = *arg + 1;
      while (isalnum(*end) || *end == '_')
	end++;
      copy = (char *) alloca (end - *arg);
      strncpy (copy, *arg + 1, (end - *arg - 1));
      copy[end - *arg - 1] = '\0';
      *arg = end;

      val = value_of_internalvar (lookup_internalvar (copy));
      if (TYPE_CODE( VALUE_TYPE (val)) != TYPE_CODE_INT)
	error ("Convenience variable must have integral type.");
      tpnum = (int) value_as_long (val);
    }
  else		/* handle tracepoint number */
    {
      tpnum = strtol (*arg, arg, 10);
    }
  ALL_TRACEPOINTS (t)
    if (t->number == tpnum)
      {
	return t;
      }
  warning ("No tracepoint number %d.\n", tpnum);
  return NULL;
}

/* Utility: parse a list of tracepoint numbers, and call a func for each. */
static void
map_args_over_tracepoints (args, from_tty, opcode)
     char *args;
     int from_tty;
     enum tracepoint_opcode opcode;
{
  struct tracepoint *t;
  int tpnum;
  char *cp;

  if (args == 0 || *args == 0)	/* do them all */
    ALL_TRACEPOINTS (t)
      tracepoint_operation (t, from_tty, opcode);
  else
    while (*args)
      {
	if (t = get_tracepoint_by_number (&args))
	  tracepoint_operation (t, from_tty, opcode);
	while (*args == ' ' || *args == '\t')
	  args++;
      }
}

static void
enable_trace_command (args, from_tty)
     char *args;
     int from_tty;
{
  dont_repeat ();
  map_args_over_tracepoints (args, from_tty, enable);
}

static void
disable_trace_command (args, from_tty)
     char *args;
     int from_tty;
{
  dont_repeat ();
  map_args_over_tracepoints (args, from_tty, disable);
}

static void
delete_trace_command (args, from_tty)
     char *args;
     int from_tty;
{
  dont_repeat ();
  if (!args || !*args)
    if (!query ("Delete all tracepoints? "))
      return;

  map_args_over_tracepoints (args, from_tty, delete);
}

static void
trace_pass_command (args, from_tty)
     char *args;
     int from_tty;
{
  struct tracepoint *t1 = (struct tracepoint *) -1, *t2;
  unsigned long count;

  if (args == 0 || *args == 0)
    error ("PASS command requires an argument (count + optional TP num)");

  count = strtoul (args, &args, 10);	/* count comes first, then TP num */

  while (*args && isspace (*args))
    args++;

  if (*args && strncasecmp (args, "all", 3) == 0)
    args += 3;	/* skip special argument "all" */
  else
    t1 = get_tracepoint_by_number (&args);

  if (t1 == NULL)
    return;	/* error, bad tracepoint number */

  ALL_TRACEPOINTS (t2)
    if (t1 == (struct tracepoint *) -1 || t1 == t2)
      {
	t2->pass_count = count;
	if (from_tty)
	  printf_filtered ("Setting tracepoint %d's passcount to %d\n", 
			   t2->number, count);
      }
}

/* ACTIONS ACTIONS ACTIONS */

static void read_actions PARAMS((struct tracepoint *));
static void free_actions PARAMS((struct tracepoint *));
static int  validate_actionline PARAMS((char *, struct tracepoint *));

static void 
end_pseudocom (args, from_tty)
{
  error ("This command cannot be used at the top level.");
}

static void
while_stepping_pseudocom (args, from_tty)
{
  error ("This command can only be used in a tracepoint actions list.");
}

static void
collect_pseudocom (args, from_tty)
{
  error ("This command can only be used in a tracepoint actions list.");
}

static void
trace_actions_command (args, from_tty)
     char *args;
     int from_tty;
{
  struct tracepoint *t;
  char *actions;

  if (t = get_tracepoint_by_number (&args))
    {
      if (from_tty)
	printf_filtered ("Enter actions for tracepoint %d, one per line.\n", 
			 t->number);
      free_actions (t);
      read_actions (t);
      /* tracepoints_changed () */
    }
  /* else error, just return; */
}

enum actionline_type
{
  BADLINE  = -1, 
  GENERIC  =  0,
  END      =  1,
  STEPPING =  2,
};

static void
read_actions (t)
     struct tracepoint *t;
{
  char *line;
  char *prompt1 = "> ", *prompt2 = "  > ";
  char *prompt = prompt1;
  enum actionline_type linetype;
  extern FILE *instream;
  struct action_line *next = NULL, *temp;
  struct cleanup *old_chain;

  /* Control-C quits instantly if typed while in this loop
     since it should not wait until the user types a newline.  */
  immediate_quit++;
#ifdef STOP_SIGNAL
  if (job_control)
    signal (STOP_SIGNAL, stop_sig);
#endif
  old_chain = make_cleanup (free_actions, (void *) t);
  while (1)
    {
      /* Make sure that all output has been output.  Some machines may let
	 you get away with leaving out some of the gdb_flush, but not all.  */
      wrap_here ("");
      gdb_flush (gdb_stdout);
      gdb_flush (gdb_stderr);
      if (instream == stdin && ISATTY (instream))
	line = readline (prompt);
      else
	line = gdb_readline (0);

      linetype = validate_actionline (line, t);
      if (linetype == BADLINE)
	continue;	/* already warned -- collect another line */

      temp = xmalloc (sizeof (struct action_line));
      temp->next = NULL;
      temp->action = line;

      if (next == NULL)		/* first action for this tracepoint? */
	t->actions = next = temp;
      else
	{
	  next->next = temp;
	  next = temp;
	}

      if (linetype == STEPPING)	/* begin "while-stepping" */
	if (prompt == prompt2)
	  {
	    warning ("Already processing 'while-stepping'");
	    continue;
	  }
	else
	  prompt = prompt2;	/* change prompt for stepping actions */
      else if (linetype == END)
	if (prompt == prompt2)
	  prompt = prompt1;	/* end of single-stepping actions */
	else
	  break;		/* end of actions */
    }
#ifdef STOP_SIGNAL
  if (job_control)
    signal (STOP_SIGNAL, SIG_DFL);
#endif
  immediate_quit = 0;
  discard_cleanups (old_chain);
}

static char *
parse_and_eval_memrange (arg, addr, typecode, offset, size)
     char *arg;
     CORE_ADDR addr;
     long *typecode, *size;
     bfd_signed_vma *offset;
{
  char *start = arg;
  struct expression *exp;

  if (*arg++ != '$' || *arg++ != '(')
    error ("Internal: bad argument to validate_memrange: %s", start);

  if (*arg == '$')	/* register for relative memrange? */
    {
      exp = parse_exp_1 (&arg, block_for_pc (addr), 1);
      if (exp->elts[0].opcode != OP_REGISTER)
	error ("Bad register operand for memrange: %s", start);
      if (*arg++ != ',')
	error ("missing comma for memrange: %s", start);
      *typecode = exp->elts[1].longconst;
    }
  else
    *typecode = 0;

#if 0
  /* While attractive, this fails for a number of reasons:
     1) parse_and_eval_address does not deal with trailing commas,
        close-parens etc.
     2) There is no safeguard against the user trying to use
        an out-of-scope variable in an address expression (for instance).
     2.5) If you are going to allow semi-arbitrary expressions, you 
          would need to explain which expressions are allowed, and 
	  which are not (which would provoke endless questions).
     3) If you are going to allow semi-arbitrary expressions in the
        offset and size fields, then the leading "$" of a register
	name no longer disambiguates the typecode field.
  */

  *offset = parse_and_eval_address (arg);
  if ((arg = strchr (arg, ',')) == NULL)
    error ("missing comma for memrange: %s", start);
  else
    arg++;

  *size = parse_and_eval_address (arg);
  if ((arg = strchr (arg, ')')) == NULL)
    error ("missing close-parenthesis for memrange: %s", start);
  else
    arg++;
#else
#if 0
  /* This, on the other hand, doesn't work because "-1" is an 
     expression, not an OP_LONG!  Fall back to using strtol for now. */

  exp = parse_exp_1 (&arg, block_for_pc (addr), 1);
  if (exp->elts[0].opcode != OP_LONG)
    error ("Bad offset operand for memrange: %s", start);
  *offset = exp->elts[2].longconst;

  if (*arg++ != ',')
    error ("missing comma for memrange: %s", start);

  exp = parse_exp_1 (&arg, block_for_pc (addr), 1);
  if (exp->elts[0].opcode != OP_LONG)
    error ("Bad size operand for memrange: %s", start);
  *size = exp->elts[2].longconst;

  if (*size <= 0)
    error ("invalid size in memrange: %s", start);

  if (*arg++ != ')')
    error ("missing close-parenthesis for memrange: %s", start);
#else
  *offset = strtol (arg, &arg, 0);
  if (*arg++ != ',')
    error ("missing comma for memrange: %s", start);
  *size   = strtol (arg, &arg, 0);
  if (*size <= 0)
    error ("invalid size in memrange: %s", start);
  if (*arg++ != ')')
    error ("missing close-parenthesis for memrange: %s", start);
#endif
#endif
  if (info_verbose)
    printf_filtered ("Collecting memrange: (0x%x,0x%x,0x%x)\n", 
		     *typecode, *offset, *size);

  return arg;
}

static enum actionline_type
validate_actionline (line, t)
     char *line;
     struct tracepoint *t;
{
  char *p;
  struct expression *exp;
  value_ptr temp, temp2;

  for (p = line; isspace (*p); )
    p++;

  /* symbol lookup etc. */
  if (*p == '\0')	/* empty line: just prompt for another line. */
    return BADLINE;
  else if (0 == strncasecmp (p, "collect", 7))
    {
      p += 7;
      do {			/* repeat over a comma-separated list */
	while (isspace (*p))
	  p++;

	if (*p == '$')			/* look for special pseudo-symbols */
	  {
	    long typecode, size;
	    bfd_signed_vma offset;

	    if ((0 == strncasecmp ("reg", p + 1, 3)) ||
		(0 == strncasecmp ("arg", p + 1, 3)) ||
		(0 == strncasecmp ("loc", p + 1, 3)))
	      p = strchr (p, ',');

	    else if (p[1] == '(')	/* literal memrange */
	      p = parse_and_eval_memrange (p, t->address, 
					    &typecode, &offset, &size);
	  }
	else
	  {
	    exp   = parse_exp_1 (&p, block_for_pc (t->address), 1);

	    if (exp->elts[0].opcode != OP_VAR_VALUE &&
	      /*exp->elts[0].opcode != OP_LONG      && */
	      /*exp->elts[0].opcode != UNOP_CAST    && */
		exp->elts[0].opcode != OP_REGISTER)
	      {
		warning ("collect: enter variable name or register.\n");
		return BADLINE;
	      }
	    if (exp->elts[0].opcode == OP_VAR_VALUE)
	      if (SYMBOL_CLASS (exp->elts[2].symbol) == LOC_CONST)
		{
		  warning ("%s is constant (value %d): will not be collected.",
			   SYMBOL_NAME (exp->elts[2].symbol),
			   SYMBOL_VALUE (exp->elts[2].symbol));
		  return BADLINE;
		}
	      else if (SYMBOL_CLASS (exp->elts[2].symbol) == LOC_OPTIMIZED_OUT)
		{
		  warning ("%s is optimized away and cannot be collected.",
			   SYMBOL_NAME (exp->elts[2].symbol));
		  return BADLINE;
		}
	  }
      } while (p && *p++ == ',');
      return GENERIC;
    }
  else if (0 == strncasecmp (p, "while-stepping", 14))
    {
      char *steparg;	/* in case warning is necessary */

      p += 14;
      while (isspace (*p))
	p++;
      steparg = p;

      if (*p)
	{
	  t->step_count = strtol (p, &p, 0);
	  if (t->step_count == 0)
	    {
	      warning ("'%s' evaluates to zero -- command ignored.");
	      return BADLINE;
	    }
	}
      else 
	t->step_count = -1;
      return STEPPING;
    }
  else if (0 == strncasecmp (p, "end", 3))
    return END;
  else
    {
      warning ("'%s' is not a supported tracepoint action.", p);
      return BADLINE;
    }
}

static void 
free_actions (t)
     struct tracepoint *t;
{
  struct action_line *line, *next;

  for (line = t->actions; line; line = next)
    {
      next = line->next;
      free (line);
    }
  t->actions = NULL;
}

struct memrange {
  int type;		/* 0 for absolute memory range, else basereg number */
  bfd_signed_vma start;
  bfd_signed_vma end;
};

struct collection_list {
  unsigned char regs_mask[8];	/* room for up to 256 regs */
  long listsize;
  long next_memrange;
  struct memrange *list;
} tracepoint_list, stepping_list;

static int
memrange_cmp (a, b)
     struct memrange *a, *b;
{
  if (a->type < b->type) return -1;
  if (a->type > b->type) return  1;
  if (a->type == 0)
    {
      if ((bfd_vma) a->start  < (bfd_vma) b->start)  return -1;
      if ((bfd_vma) a->start  > (bfd_vma) b->start)  return  1;
    }
  else
    {
      if (a->start  < b->start)  return -1;
      if (a->start  > b->start)  return  1;
    }
  return 0;
}

static void
memrange_sortmerge (memranges)
     struct collection_list *memranges;
{
  int a, b;

  qsort (memranges->list, memranges->next_memrange, 
	 sizeof (struct memrange), memrange_cmp);
  if (memranges->next_memrange > 0)
    {
      for (a = 0, b = 1; b < memranges->next_memrange; b++)
	{
	  if (memranges->list[a].type == memranges->list[b].type &&
	      memranges->list[b].start - memranges->list[a].end <= 
	      MAX_REGISTER_VIRTUAL_SIZE)
	    {
	      memranges->list[a].end = memranges->list[b].end;
	      continue;		/* next b, same a */
	    }
	  a++;			/* next a */
	  if (a != b)
	    memcpy (&memranges->list[a], &memranges->list[b], 
		    sizeof (struct memrange));
	}
      memranges->next_memrange = a + 1;
    }
}

void
add_register (collection, regno)
     struct collection_list *collection;
     unsigned long regno;
{
  if (info_verbose)
    printf_filtered ("collect register %d\n", regno);
  if (regno > (8 * sizeof (collection->regs_mask)))
    error ("Internal: register number %d too large for tracepoint",
	   regno);
  collection->regs_mask [regno / 8] |= 1 << (regno  % 8);
}

static void
add_memrange (memranges, type, base, len)
     struct collection_list *memranges;
     int type;
     bfd_signed_vma base;
     unsigned long len;
{
  if (info_verbose)
    printf_filtered ("(%d,0x%x,%d)\n", type, base, len);
  /* type: 0 == memory, n == basereg */
  memranges->list[memranges->next_memrange].type  = type;
  /* base: addr if memory, offset if reg relative. */
  memranges->list[memranges->next_memrange].start = base;
  /* len: we actually save end (base + len) for convenience */
  memranges->list[memranges->next_memrange].end   = base + len;
  memranges->next_memrange++;
  if (memranges->next_memrange >= memranges->listsize)
    {
      memranges->listsize *= 2;
      memranges->list = xrealloc (memranges->list, 
				  memranges->listsize);
    }

  if (type != 0)	/* better collect the base register! */
    add_register (memranges, type);
}

static void
collect_symbol (collect, sym)
     struct collection_list *collect;
     struct symbol *sym;
{
  unsigned long  len;
  unsigned long  reg;
  bfd_signed_vma offset;

  len  = TYPE_LENGTH (check_typedef (SYMBOL_TYPE (sym)));
  switch (SYMBOL_CLASS (sym)) {
  default:
    printf_filtered ("%s: don't know symbol class %d\n",
		     SYMBOL_NAME (sym), SYMBOL_CLASS (sym));
    break;
  case LOC_CONST:
    printf_filtered ("%s is constant, value is %d: will not be collected.\n",
		     SYMBOL_NAME (sym), SYMBOL_VALUE (sym));
    break;
  case LOC_STATIC:
    offset = SYMBOL_VALUE_ADDRESS (sym); 
    if (info_verbose)
      printf_filtered ("LOC_STATIC %s: collect %d bytes "
		       "at 0x%08x\n",
		       SYMBOL_NAME (sym), len, offset);
    add_memrange (collect, 0, offset, len);	/* 0 == memory */
    break;
  case LOC_REGISTER:
  case LOC_REGPARM:
    reg = SYMBOL_VALUE (sym); 
    if (info_verbose)
      printf_filtered ("LOC_REG[parm] %s: ", SYMBOL_NAME (sym));
    add_register (collect, reg);
    break;
  case LOC_ARG:
  case LOC_REF_ARG:
    printf_filtered ("Sorry, don't know how to do LOC_ARGs yet.\n");
    printf_filtered ("       (will not collect %s)\n", 
		     SYMBOL_NAME (sym));
    break;
  case LOC_REGPARM_ADDR:
    reg = SYMBOL_VALUE (sym);
    offset = 0;
    if (info_verbose)
      {
	printf_filtered ("LOC_REGPARM_ADDR %s: Collect %d bytes at offset %d from reg %d\n", 
			 SYMBOL_NAME (sym), len, offset, reg);
      }
    add_memrange (collect, reg, offset, len);
    break;
  case LOC_LOCAL:
  case LOC_LOCAL_ARG:
    offset = SYMBOL_VALUE (sym);
    reg = FP_REGNUM;
    if (info_verbose)
      {
	printf_filtered ("LOC_LOCAL %s: Collect %d bytes at offset %d from frame ptr reg %d\n", 
			 SYMBOL_NAME (sym), len, offset, reg);
      }
    add_memrange (collect, reg, offset, len);
    break;
  case LOC_BASEREG:
  case LOC_BASEREG_ARG:
    reg = SYMBOL_BASEREG (sym);
    offset  = SYMBOL_VALUE (sym);
    if (info_verbose)
      {
	printf_filtered ("LOC_BASEREG %s: collect %d bytes at offset %d from basereg %d\n", 
			 SYMBOL_NAME (sym), len, offset, reg);
      }
    add_memrange (collect, reg, offset, len);
    break;
  case LOC_UNRESOLVED:
    printf_filtered ("Don't know LOC_UNRESOLVED %s\n", SYMBOL_NAME (sym));
    break;
  case LOC_OPTIMIZED_OUT:
    printf_filtered ("%s has been optimized out of existance.\n",
		     SYMBOL_NAME (sym));
    break;
  }
}

static void
add_local_symbols (collect, pc, type)
     struct collection_list *collect;
     CORE_ADDR pc;
     char type;
{
  struct symbol *sym;
  struct block  *block;
  int i, nsyms, count = 0;

  block = block_for_pc (pc);
  while (block != 0)
    {
      nsyms = BLOCK_NSYMS (block);
      for (i = 0; i < nsyms; i++)
	{
	  sym = BLOCK_SYM (block, i);
	  switch (SYMBOL_CLASS (sym)) {
	  case LOC_LOCAL:
	  case LOC_STATIC:
	  case LOC_REGISTER:
	  case LOC_BASEREG:
	    if (type == 'L')	/* collecting Locals */
	      {
		count++;
		collect_symbol (collect, sym);
	      }
	    break;
	  case LOC_ARG:
	  case LOC_LOCAL_ARG:
	  case LOC_REF_ARG:
	  case LOC_REGPARM:
	  case LOC_REGPARM_ADDR:
	  case LOC_BASEREG_ARG:
	    if (type == 'A')	/* collecting Arguments */
	      {
		count++;
		collect_symbol (collect, sym);
	      }
	  }
	}
      if (BLOCK_FUNCTION (block))
	break;
      else
	block = BLOCK_SUPERBLOCK (block);
    }
  if (count == 0)
    warning ("No %s found in scope.", type == 'L' ? "locals" : "args");
}

static void
clear_collection_list (list)
     struct collection_list *list;
{
  list->next_memrange = 0;
  memset (list->regs_mask, 0, sizeof (list->regs_mask));
}

static char *
stringify_collection_list (list, string)
     struct collection_list *list;
     char *string;
{
  char *end = string;
  long  i;

  for (i = sizeof (list->regs_mask) - 1; i > 0; i--)
    if (list->regs_mask[i] != 0)	/* skip leading zeroes in regs_mask */
      break;
  if (list->regs_mask[i] != 0)	/* prepare to send regs_mask to the stub */
    {
      if (info_verbose)
	printf_filtered ("\nCollecting registers (mask): 0x");
      *end++='R';
      for (; i >= 0; i--)
	{
	  if (info_verbose)
	    printf_filtered ("%02X", list->regs_mask[i]);
	  sprintf (end,  "%02X", list->regs_mask[i]);
	  end += 2;
	}
    }
  if (info_verbose)
    printf_filtered ("\n");
  if (list->next_memrange > 0 && info_verbose)
    printf_filtered ("Collecting memranges: \n");
  for (i = 0; i < list->next_memrange; i++)
    {
      if (info_verbose)
	printf_filtered ("(%d, 0x%x, %d)\n", 
			 list->list[i].type, 
			 list->list[i].start, 
			 list->list[i].end - list->list[i].start);
      sprintf (end, "M%X,%X,%X", 
	       list->list[i].type, 
	       list->list[i].start, 
	       list->list[i].end - list->list[i].start);
      end += strlen (end);
    }
  if (end == string)
    return NULL;
  else
    return string;
}

static void
encode_actions (t, tdp_actions, step_count, stepping_actions)
     struct tracepoint  *t;
     char              **tdp_actions;
     unsigned long      *step_count;
     char              **stepping_actions;
{
  struct expression  *exp;
  static char        tdp_buff[2048], step_buff[2048];
  struct action_line *action;
  char               *action_exp;
  bfd_signed_vma      offset;
  long                i;
  struct collection_list *collect;

  clear_collection_list (&tracepoint_list);
  clear_collection_list (&stepping_list);
  collect = &tracepoint_list;

  *tdp_actions = NULL;
  *stepping_actions = NULL;

  for (action = t->actions; action; action = action->next)
    {
      action_exp = action->action;
      while (isspace (*action_exp))
	action_exp++;

      if (0 == strncasecmp (action_exp, "collect", 7))
	{
	  action_exp = action_exp + 7;
	  do {	/* repeat over a comma-separated list */
	    while (isspace (*action_exp))
	      action_exp++;

	    if (0 == strncasecmp ("$reg", action_exp, 4))
	      {
		for (i = 0; i < NUM_REGS; i++)
		  add_register (collect, i);
		action_exp = strchr (action_exp, ','); /* more? */
	      }
	    else if (0 == strncasecmp ("$arg", action_exp, 4))
	      {
		add_local_symbols (collect, t->address, 'A');
		action_exp = strchr (action_exp, ','); /* more? */
	      }
	    else if (0 == strncasecmp ("$loc", action_exp, 4))
	      {
		add_local_symbols (collect, t->address, 'L');
		action_exp = strchr (action_exp, ','); /* more? */
	      }
	    else if (action_exp[0] == '$' &&
		     action_exp[1] == '(')	/* literal memrange */
	      {
		long typecode, size;
		bfd_signed_vma offset;

		action_exp = parse_and_eval_memrange (action_exp,
						      t->address,
						      &typecode,
						      &offset,
						      &size);
		add_memrange (collect, typecode, offset, size);
	      }
	    else
	      {
		unsigned long addr, len;

		exp = parse_exp_1 (&action_exp, block_for_pc (t->address), 1);
		switch (exp->elts[0].opcode) {
		case OP_REGISTER:
		  i = exp->elts[1].longconst; 
		  if (info_verbose)
		    printf_filtered ("OP_REGISTER: ");
		  add_register (collect, i);
		  break;
		case OP_VAR_VALUE:
		  collect_symbol (collect, exp->elts[2].symbol);
		  break;
#if 0
		case OP_LONG:
		  addr = exp->elts[2].longconst;
		  if (*action_exp == ':')
		    {
		      exp = parse_exp_1 (&action_exp, 
					 block_for_pc (t->address), 
					 1);
		      if (exp->elts[0].opcode == OP_LONG)
			len = exp->elts[2].longconst;
		      else
			error ("length field requires a literal long const");
		    }
		  else 
		    len = 4;

		  add_memrange (collect, 0, addr, len);
		  break;
#endif
		}
	      }
	  } while (action_exp && *action_exp++ == ',');
	}
      else if (0 == strncasecmp (action_exp, "while-stepping", 14))
	{
	  collect = &stepping_list;
	}
      else if (0 == strncasecmp (action_exp, "end", 3))
	{
	  if (collect == &stepping_list)	/* end stepping actions */
	    collect = &tracepoint_list;
	  else
	    break;			/* end tracepoint actions */
	}
    }
  memrange_sortmerge (&tracepoint_list); 
  memrange_sortmerge (&stepping_list); 

  *tdp_actions      = stringify_collection_list (&tracepoint_list, &tdp_buff);
  *stepping_actions = stringify_collection_list (&stepping_list,   &step_buff);
}

static char target_buf[2048];

static void
trace_start_command (args, from_tty)
     char *args;
     int from_tty;
{ /* STUB_COMM MOSTLY_IMPLEMENTED */
  struct tracepoint *t;
  char buf[2048];
  char *tdp_actions;
  char *stepping_actions;
  unsigned long step_count;

  dont_repeat ();	/* like "run", dangerous to repeat accidentally */
  
  if (target_is_remote ())
    {
      putpkt ("QTinit");
      remote_get_noisy_reply (target_buf);
      if (strcmp (target_buf, "OK"))
	error ("Target does not support this command.");

      ALL_TRACEPOINTS (t)
	{
	  int ss_count;		/* if actions include singlestepping */
	  int disable_mask;	/* ??? */
	  int enable_mask;	/* ??? */

	  sprintf (buf, "QTDP:%x:%x:%c:%x:%x", t->number, t->address, 
		   t->enabled == enabled ? 'E' : 'D', 
		   t->step_count, t->pass_count);
	  if (t->actions)
	    {
	      encode_actions (t, &tdp_actions, &step_count, &stepping_actions);
	      /* do_single_steps (t); */
	      if (tdp_actions)
		{
		  if (strlen (buf) + strlen (tdp_actions) >= sizeof (buf))
		    error ("Actions for tracepoint %d too complex; "
			   "please simplify.", t->number);
		  strcat (buf, tdp_actions);
		}
	      if (stepping_actions)
		{
		  strcat (buf, "S");
		  if (strlen (buf) + strlen (stepping_actions) >= sizeof (buf))
		    error ("Actions for tracepoint %d too complex; "
			   "please simplify.", t->number);
		  strcat (buf, stepping_actions);
		}
	    }
	  putpkt (buf);
	  remote_get_noisy_reply (target_buf);
	  if (strcmp (target_buf, "OK"))
	    error ("Target does not support tracepoints.");
	}
      putpkt ("QTStart");
      remote_get_noisy_reply (target_buf);
      if (strcmp (target_buf, "OK"))
	error ("Bogus reply from target: %s", target_buf);
      set_traceframe_num (-1);	/* all old traceframes invalidated */
      set_tracepoint_num (-1);
      set_traceframe_context(-1);
    }
  else
    printf_filtered ("Trace can only be run on remote targets.\n");
}

static void
trace_stop_command (args, from_tty)
     char *args;
     int from_tty;
{ /* STUB_COMM IS_IMPLEMENTED */
  if (target_is_remote ())
    {
      putpkt ("QTStop");
      remote_get_noisy_reply (target_buf);
      if (strcmp (target_buf, "OK"))
	error ("Bogus reply from target: %s", target_buf);
    }
  else
    error ("Trace can only be run on remote targets.");
}

static void
trace_status_command (args, from_tty)
     char *args;
     int from_tty;
{ /* STUB_COMM IS_IMPLEMENTED */
  if (target_is_remote ())
    {
      putpkt ("qTStatus");
      remote_get_noisy_reply (target_buf);
      if (strcmp (target_buf, "OK"))
	error ("Bogus reply from target: %s", target_buf);
    }
  else
    error ("Trace can only be run on remote targets.");
}

static void
trace_buff_command (args, from_tty)
     char *args;
     int from_tty;
{ /* STUB_COMM NOT_IMPLEMENTED */
  if (args == 0 || *args == 0)
    printf_filtered ("TBUFFER command requires argument (on or off)\n");
  else if (strcasecmp (args, "on") == 0)
    printf_filtered ("tbuffer overflow on.\n");
  else if (strcasecmp (args, "off") == 0)
    printf_filtered ("tbuffer overflow off.\n");
  else
    printf_filtered ("TBUFFER: unknown argument (use on or off)\n");
}

static void
trace_limit_command (args, from_tty)
     char *args;
     int from_tty;
{ /* STUB_COMM NOT_IMPLEMENTED */
  printf_filtered ("Limit it to what?\n");
}

static void
finish_tfind_command (reply, from_tty)
     char *reply;
     int from_tty;
{
  int target_frameno = -1, target_tracept = -1;

  while (reply && *reply)
    switch (*reply) {
    case 'F':
      if ((target_frameno = strtol (++reply, &reply, 16)) == -1)
	error ("Target failed to find requested trace frame.");
      break;
    case 'T':
      if ((target_tracept = strtol (++reply, &reply, 16)) == -1)
	error ("Target failed to find requested trace frame.");
      break;
    case 'O':	/* "OK"? */
      if (reply[1] == 'K' && reply[2] == '\0')
	reply += 2;
      else
	error ("Bogus reply from target: %s", reply);
      break;
    default:
      error ("Bogus reply from target: %s", reply);
    }

  flush_cached_frames ();
  registers_changed ();
  select_frame (get_current_frame (), 0);
  set_traceframe_num (target_frameno);
  set_tracepoint_num (target_tracept);
  set_traceframe_context ((get_current_frame ())->pc);

  if (from_tty)
    print_stack_frame (selected_frame, selected_frame_level, 1);
}

/* trace_find_command takes a trace frame number n, 
   sends "QTFrame:<n>" to the target, 
   and accepts a reply that may contain several optional pieces
   of information: a frame number, a tracepoint number, and an
   indication of whether this is a trap frame or a stepping frame.

   The minimal response is just "OK" (which indicates that the 
   target does not give us a frame number or a tracepoint number).
   Instead of that, the target may send us a string containing
   any combination of:
	F<hexnum>	(gives the selected frame number)
	T<hexnum>	(gives the selected tracepoint number)
   */

static void
trace_find_command (args, from_tty)
     char *args;
     int from_tty;
{ /* STUB_COMM PART_IMPLEMENTED */
  /* this should only be called with a numeric argument */
  int frameno = -1;
  int target_frameno = -1, target_tracept = -1, target_stepfrm = 0;
  char *tmp;

  if (target_is_remote ())
    {
      if (args == 0 || *args == 0)
	{ /* TFIND with no args means find NEXT trace frame. */
	  if (traceframe_number == -1)
	    frameno = 0;	/* "next" is first one */
	  else
	    frameno = traceframe_number + 1;
	}
      else if (0 == strcmp (args, "-"))
	{
	  if (traceframe_number == -1)
	    error ("not debugging trace buffer");
	  else if (traceframe_number == 0)
	    error ("already at start of trace buffer");

	  frameno = traceframe_number - 1;
	}
#if 0
      else if (0 == strcasecmp (args, "start"))
	frameno = 0;
      else if (0 == strcasecmp (args, "none") ||
	       0 == strcasecmp (args, "end"))
	frameno = -1;
#endif
      else
	frameno = parse_and_eval_address (args);

      sprintf (target_buf, "QTFrame:%x", frameno);
      putpkt  (target_buf);
      tmp = remote_get_noisy_reply (target_buf);

      if (frameno == -1)	/* end trace debugging */
	{			/* hopefully the stub has complied! */
	  if (0 != strcmp (tmp, "F-1"))
	    error ("Bogus response from target: %s", tmp);

	  flush_cached_frames ();
	  registers_changed ();
	  select_frame (get_current_frame (), 0);
	  set_traceframe_num (-1);
	  set_tracepoint_num (-1);
	  set_traceframe_context (-1);

	  if (from_tty)
	    print_stack_frame (selected_frame, selected_frame_level, 1);
	}
      else
	finish_tfind_command (tmp, from_tty);
    }
  else
    error ("Trace can only be run on remote targets.");
}

static void
trace_find_end_command (args, from_tty)
     char *args;
     int from_tty;
{
  trace_find_command ("-1", from_tty);
}

static void
trace_find_none_command (args, from_tty)
     char *args;
     int from_tty;
{
  trace_find_command ("-1", from_tty);
}

static void
trace_find_start_command (args, from_tty)
     char *args;
     int from_tty;
{
  trace_find_command ("0", from_tty);
}

static void
trace_find_pc_command (args, from_tty)
     char *args;
     int from_tty;
{ /* STUB_COMM PART_IMPLEMENTED */
  CORE_ADDR pc;
  int target_frameno;
  char *tmp;

  if (target_is_remote ())
    {
      if (args == 0 || *args == 0)
	pc = read_pc ();	/* default is current pc */
      else
	pc = parse_and_eval_address (args);

      sprintf (target_buf, "QTFrame:pc:%x", pc);
      putpkt (target_buf);
      tmp = remote_get_noisy_reply (target_buf);

      finish_tfind_command (tmp, from_tty);
    }
  else
    error ("Trace can only be run on remote targets.");
}

static void
trace_find_tracepoint_command (args, from_tty)
     char *args;
     int from_tty;
{ /* STUB_COMM PART_IMPLEMENTED */
  int target_frameno, tdp;
  char buf[40], *tmp;

  if (target_is_remote ())
    {
      if (args == 0 || *args == 0)
	if (tracepoint_number == -1)
	  error ("No current tracepoint -- please supply an argument.");
	else
	  tdp = tracepoint_number;	/* default is current TDP */
      else
	tdp = parse_and_eval_address (args);

      sprintf (target_buf, "QTFrame:tdp:%x", tdp);
      putpkt (target_buf);
      tmp = remote_get_noisy_reply (target_buf);

      finish_tfind_command (tmp, from_tty);
    }
  else
    error ("Trace can only be run on remote targets.");
}

/* TFIND LINE command:
 *
 * This command will take a sourceline for argument, just like BREAK
 * or TRACE (ie. anything that "decode_line_1" can handle).  
 * 
 * With no argument, this command will find the next trace frame 
 * corresponding to a source line OTHER THAN THE CURRENT ONE.
 */

static void
trace_find_line_command (args, from_tty)
     char *args;
     int from_tty;
{ /* STUB_COMM PART_IMPLEMENTED */
  static CORE_ADDR start_pc, end_pc;
  struct symtabs_and_lines sals;
  struct symtab_and_line sal;
  int target_frameno;
  char *tmp;
  struct cleanup *old_chain;

  if (target_is_remote ())
    {
      if (args == 0 || *args == 0)
	{
	  sal = find_pc_line ((get_current_frame ())->pc, 0);
	  sals.nelts = 1;
	  sals.sals = (struct symtab_and_line *)
	    xmalloc (sizeof (struct symtab_and_line));
	  sals.sals[0] = sal;
	}
      else
	{
	  sals = decode_line_spec (args, 1);
	  sal  = sals.sals[0];
	}

      old_chain = make_cleanup (free, sals.sals);
      if (sal.symtab == 0)
	{
	  printf_filtered ("TFIND: No line number information available");
	  if (sal.pc != 0)
	    {
	      /* This is useful for "info line *0x7f34".  If we can't tell the
		 user about a source line, at least let them have the symbolic
		 address.  */
	      printf_filtered (" for address ");
	      wrap_here ("  ");
	      print_address (sal.pc, gdb_stdout);
	      printf_filtered (";\n -- will attempt to find by PC. \n");
	    }
	  else
	    {
	      printf_filtered (".\n");
	      return;	/* no line, no PC; what can we do? */
	    }
	}
      else if (sal.line > 0
	       && find_line_pc_range (sal, &start_pc, &end_pc))
	{
	  if (start_pc == end_pc)
	    {
	      printf_filtered ("Line %d of \"%s\"",
			       sal.line, sal.symtab->filename);
	      wrap_here ("  ");
	      printf_filtered (" is at address ");
	      print_address (start_pc, gdb_stdout);
	      wrap_here ("  ");
	      printf_filtered (" but contains no code.\n");
	      sal = find_pc_line (start_pc, 0);
	      if (sal.line > 0 &&
		  find_line_pc_range (sal, &start_pc, &end_pc) &&
		  start_pc != end_pc)
		printf_filtered ("Attempting to find line %d instead.\n",
				 sal.line);
	      else
		error ("Cannot find a good line.");
	    }
	}
      else
	/* Is there any case in which we get here, and have an address
	   which the user would want to see?  If we have debugging symbols
	   and no line numbers?  */
	error ("Line number %d is out of range for \"%s\".\n",
	       sal.line, sal.symtab->filename);

      if (args && *args)	/* find within range of stated line */
	sprintf (target_buf, "QTFrame:range:%x:%x", start_pc, end_pc - 1);
      else			/* find OUTSIDE OF range of CURRENT line */
	sprintf (target_buf, "QTFrame:outside:%x:%x", start_pc, end_pc - 1);
      putpkt (target_buf);
      tmp = remote_get_noisy_reply (target_buf);

      finish_tfind_command (tmp, from_tty);
      do_cleanups (old_chain);
    }
  else
      error ("Trace can only be run on remote targets.");
}

static void
trace_find_range_command (args, from_tty)
     char *args;
     int from_tty;
{ /* STUB_COMM PART_IMPLEMENTED */
  static CORE_ADDR start, stop;
  int target_frameno;
  char *tmp;

  if (target_is_remote ())
    {
      if (args == 0 || *args == 0)
	{ /* XXX FIXME: what should default behavior be? */
	  printf_filtered ("Usage: tfind range <startaddr>,<endaddr>\n");
	  return;
	}

      if (0 != (tmp = strchr (args, ',' )))
	{
	  *tmp++ = '\0';	/* terminate start address */
	  while (isspace (*tmp))
	    tmp++;
	  start = parse_and_eval_address (args);
	  stop  = parse_and_eval_address (tmp);
	}
      else
	{ /* no explicit end address? */
	  start = parse_and_eval_address (args);
	  stop  = start + 1; /* ??? */
	}

      sprintf (target_buf, "QTFrame:range:%x:%x", start, stop);
      putpkt (target_buf);
      tmp = remote_get_noisy_reply (target_buf);

      finish_tfind_command (tmp, from_tty);
    }
  else
      error ("Trace can only be run on remote targets.");
}

static void
trace_find_outside_command (args, from_tty)
     char *args;
     int from_tty;
{ /* STUB_COMM PART_IMPLEMENTED */
  CORE_ADDR start, stop;
  int target_frameno;
  char *tmp;

  if (target_is_remote ())
    {
      if (args == 0 || *args == 0)
	{ /* XXX FIXME: what should default behavior be? */
	  printf_filtered ("Usage: tfind outside <startaddr>,<endaddr>\n");
	  return;
	}

      if (0 != (tmp = strchr (args, ',' )))
	{
	  *tmp++ = '\0';	/* terminate start address */
	  while (isspace (*tmp))
	    tmp++;
	  start = parse_and_eval_address (args);
	  stop  = parse_and_eval_address (tmp);
	}
      else
	{ /* no explicit end address? */
	  start = parse_and_eval_address (args);
	  stop  = start + 1; /* ??? */
	}

      sprintf (target_buf, "QTFrame:outside:%x:%x", start, stop);
      putpkt (target_buf);
      tmp = remote_get_noisy_reply (target_buf);

      finish_tfind_command (tmp, from_tty);
    }
  else
      error ("Trace can only be run on remote targets.");
}

static void
tracepoint_save_command (args, from_tty)
     char *args;
     int from_tty;
{
  struct tracepoint  *tp;
  struct action_line *line;
  FILE *fp;
  char *i1 = "    ", *i2 = "      ";
  char *indent, *actionline;

  if (args == 0 || *args == 0)
    error ("Argument required (file name in which to save tracepoints");

  if (tracepoint_chain == 0)
    {
      warning ("save-tracepoints: no tracepoints to save.\n");
      return;
    }

  if (!(fp = fopen (args, "w")))
    error ("Unable to open file '%s' for saving tracepoints");

  ALL_TRACEPOINTS (tp)
    {
      if (tp->addr_string)
	fprintf (fp, "trace %s\n", tp->addr_string);
      else
	fprintf (fp, "trace *0x%x\n", tp->address);

      if (tp->pass_count)
	fprintf (fp, "  passcount %d\n", tp->pass_count);

      if (tp->actions)
	{
	  fprintf (fp, "  actions\n");
	  indent = i1;
	  for (line = tp->actions; line; line = line->next)
	    {
	      actionline = line->action;
	      while (isspace(*actionline))
		actionline++;

	      fprintf (fp, "%s%s\n", indent, actionline);
	      if (0 == strncasecmp (actionline, "while-stepping", 14))
		indent = i2;
	      else if (0 == strncasecmp (actionline, "end", 3))
		indent = i1;
	    }
	}
    }
  fclose (fp);
  if (from_tty)
    printf_filtered ("Tracepoints saved to file '%s'.\n", args);
  return;
}

static void
scope_info (args, from_tty)
     char *args;
     int from_tty;
{
  struct symtab_and_line sal;
  struct symtabs_and_lines sals;
  struct symbol *sym;
  struct minimal_symbol *msym;
  struct block *block;
  char **canonical, *symname, *save_args = args;
  int i, nsyms, count = 0;

  if (args == 0 || *args == 0)
    error ("requires an argument (function, line or *addr) to define a scope");

  sals = decode_line_1 (&args, 1, NULL, 0, &canonical);
  if (sals.nelts == 0)
    return;		/* presumably decode_line_1 has already warned */

  /* Resolve line numbers to PC */
  resolve_sal_pc (&sals.sals[0]);
  block = block_for_pc (sals.sals[0].pc);

  while (block != 0)
    {
      nsyms = BLOCK_NSYMS (block);
      for (i = 0; i < nsyms; i++)
	{
	  if (count == 0)
	    printf_filtered ("Scope for %s:\n", save_args);
	  count++;
	  sym = BLOCK_SYM (block, i);
	  symname = SYMBOL_NAME (sym);
	  if (symname == NULL || *symname == '\0')
	    continue;	/* probably botched, certainly useless */

	  printf_filtered ("Symbol %s is ", symname);
	  switch (SYMBOL_CLASS (sym)) {
	  default:
	  case LOC_UNDEF:		/* messed up symbol? */
	    printf_filtered ("a bogus symbol, class %d.\n", 
			     SYMBOL_CLASS (sym));
	    count--;			/* don't count this one */
	    continue;
	  case LOC_CONST:
	    printf_filtered ("a constant with value %d (0x%x)", 
			     SYMBOL_VALUE (sym), SYMBOL_VALUE (sym));
	    break;
	  case LOC_CONST_BYTES:
	    printf_filtered ("constant bytes: ");
	    if (SYMBOL_TYPE (sym))
	      for (i = 0; i < TYPE_LENGTH (SYMBOL_TYPE (sym)); i++)
		fprintf_filtered (gdb_stdout, " %02x",
				  (unsigned) SYMBOL_VALUE_BYTES (sym) [i]);
  	    break;
	  case LOC_STATIC:
	    printf_filtered ("in static storage at address ");
	    print_address_numeric (SYMBOL_VALUE_ADDRESS (sym), 1, gdb_stdout);
	    break;
	  case LOC_REGISTER:
	    printf_filtered ("a local variable in register $%s",
			     reg_names [SYMBOL_VALUE (sym)]);
	    break;
	  case LOC_ARG:
	  case LOC_LOCAL_ARG:
	    printf_filtered ("an argument at stack/frame offset %ld",
			     SYMBOL_VALUE (sym));
	    break;
	  case LOC_LOCAL:
	    printf_filtered ("a local variable at frame offset %ld",
			     SYMBOL_VALUE (sym));
	    break;
	  case LOC_REF_ARG:
	    printf_filtered ("a reference argument at offset %ld",
			     SYMBOL_VALUE (sym));
	    break;
	  case LOC_REGPARM:
	    printf_filtered ("an argument in register $%s",
			     reg_names[SYMBOL_VALUE (sym)]);
	    break;
	  case LOC_REGPARM_ADDR:
	    printf_filtered ("the address of an argument, in register $%s",
			     reg_names[SYMBOL_VALUE (sym)]);
	    break;
	  case LOC_TYPEDEF:
	    printf_filtered ("a typedef.\n");
	    continue;
	  case LOC_LABEL:
	    printf_filtered ("a label at address ");
	    print_address_numeric (SYMBOL_VALUE_ADDRESS (sym), 1, gdb_stdout);
	    break;
	  case LOC_BLOCK:
	    printf_filtered ("a function at address ");
	    print_address_numeric (BLOCK_START (SYMBOL_BLOCK_VALUE (sym)), 1,
				   gdb_stdout);
	    break;
	  case LOC_BASEREG:
	    printf_filtered ("a variable at offset %d from register $%s",
			     SYMBOL_VALUE (sym),
			     reg_names [SYMBOL_BASEREG (sym)]);
	    break;
	  case LOC_BASEREG_ARG:
	    printf_filtered ("an argument at offset %d from register $%s",
			     SYMBOL_VALUE (sym),
			     reg_names [SYMBOL_BASEREG (sym)]);
	    break;
	  case LOC_UNRESOLVED:
	    msym = lookup_minimal_symbol (SYMBOL_NAME (sym), NULL, NULL);
	    if (msym == NULL)
	      printf_filtered ("Unresolved Static");
	    else
	      {
		printf_filtered ("static storage at address ");
		print_address_numeric (SYMBOL_VALUE_ADDRESS (msym), 1, 
				       gdb_stdout);
	      }
	    break;
	  case LOC_OPTIMIZED_OUT:
	    printf_filtered ("optimized out.\n");
	    continue;
	  }
	  if (SYMBOL_TYPE (sym))
	    printf_filtered (", length %d.\n", 
			     TYPE_LENGTH (check_typedef (SYMBOL_TYPE (sym))));
	}
      if (BLOCK_FUNCTION (block))
	break;
      else
	block = BLOCK_SUPERBLOCK (block);
    }
  if (count <= 0)
    printf_filtered ("Scope for %s contains no locals or arguments.\n",
		     save_args);
}

static void
replace_comma (comma)
     char *comma;
{
  *comma = ',';
}

static void
trace_dump_command (args, from_tty)
     char *args;
     int from_tty;
{
  struct tracepoint  *t;
  struct action_line *action;
  char               *action_exp, *next_comma;
  struct cleanup     *old_cleanups;
  int                 stepping_actions = 0;
  int                 stepping_frame   = 0;

  if (tracepoint_number == -1)
    {
      warning ("No current trace frame.");
      return;
    }

  ALL_TRACEPOINTS (t)
    if (t->number == tracepoint_number)
      break;

  if (t == NULL)
    error ("No known tracepoint matches 'current' tracepoint #%d.", 
	   tracepoint_number);

  old_cleanups = make_cleanup (null_cleanup, NULL);

  printf_filtered ("Data collected at tracepoint %d, trace frame %d:\n", 
		   tracepoint_number, traceframe_number);

  /* The current frame is a trap frame if the frame PC is equal
     to the tracepoint PC.  If not, then the current frame was
     collected during single-stepping.  */

  stepping_frame = (t->address != read_pc());

  for (action = t->actions; action; action = action->next)
    {
      action_exp = action->action;
      while (isspace (*action_exp))
	action_exp++;

      /* The collection actions to be done while stepping are
	 bracketed by the commands "while-stepping" and "end".  */

      if (0 == strncasecmp (action_exp, "while-stepping", 14))
	stepping_actions = 1;
      else if (0 == strncasecmp (action_exp, "end", 3))
	stepping_actions = 0;
      else if (0 == strncasecmp (action_exp, "collect", 7))
	{
	  /* Display the collected data.
	     For the trap frame, display only what was collected at the trap.
	     Likewise for stepping frames, display only what was collected
	     while stepping.  This means that the two boolean variables,
	     STEPPING_FRAME and STEPPING_ACTIONS should be equal.  */
	  if (stepping_frame == stepping_actions)
	    {
	      action_exp += 7;
	      do { /* repeat over a comma-separated list */
		QUIT;
		if (*action_exp == ',')
		  action_exp++;
		while (isspace (*action_exp))
		  action_exp++;

		next_comma = strchr (action_exp, ',');
		if (next_comma)
		  {
		    make_cleanup (replace_comma, next_comma);
		    *next_comma = '\0';
		  }

		if      (0 == strncasecmp (action_exp, "$reg", 4))
		  registers_info (NULL, from_tty);
		else if (0 == strncasecmp (action_exp, "$loc", 4))
		  locals_info (NULL, from_tty);
		else if (0 == strncasecmp (action_exp, "$arg", 4))
		  args_info (NULL, from_tty);
		else
		  {
		    printf_filtered ("%s = ", action_exp);
		    output_command (action_exp, from_tty);
		    printf_filtered ("\n");
		  }
		if (next_comma)
		  *next_comma = ',';
		action_exp = next_comma;
	      } while (action_exp && *action_exp == ',');
	    }
	}
    }
  discard_cleanups (old_cleanups);
}



static struct cmd_list_element *tfindlist;
static struct cmd_list_element *tracelist;

void
_initialize_tracepoint ()
{
  tracepoint_chain  = 0;
  tracepoint_count  = 0;
  traceframe_number = -1;
  tracepoint_number = -1;

  set_internalvar (lookup_internalvar ("tpnum"), 
		   value_from_longest (builtin_type_int, (LONGEST) 0));
  set_internalvar (lookup_internalvar ("trace_frame"), 
		   value_from_longest (builtin_type_int, (LONGEST) 0));

  if (tracepoint_list.list == NULL)
    {
      tracepoint_list.listsize = 128;
      tracepoint_list.list = xmalloc 
	(tracepoint_list.listsize * sizeof (struct memrange));
    }
  if (stepping_list.list == NULL)
    {
      stepping_list.listsize = 128;
      stepping_list.list = xmalloc 
	(stepping_list.listsize * sizeof (struct memrange));
    }

  add_info ("scope", scope_info, 
	    "List the variables local to a scope");

  add_cmd ("tracepoints", class_trace, NO_FUNCTION, 
	   "Tracing of program execution without stopping the program.", 
	   &cmdlist);

  add_info ("tracepoints", tracepoints_info,
	    "Status of tracepoints, or tracepoint number NUMBER.\n\
Convenience variable \"$tpnum\" contains the number of the\n\
last tracepoint set.");

  add_info_alias ("tp", "tracepoints", 1);

  add_com ("save-tracepoints", class_trace, tracepoint_save_command, 
	   "Save current tracepoint definitions as a script.\n\
Use the 'source' command in another debug session to restore them.");

  add_com ("tdump", class_trace, trace_dump_command, 
	   "Print everything collected at the current tracepoint.");

  add_prefix_cmd ("tfind",  class_trace, trace_find_command,
		  "Select a trace frame;\n\
No argument means forward by one frame; '-' meand backward by one frame.",
		  &tfindlist, "tfind ", 1, &cmdlist);

  add_cmd ("outside", class_trace, trace_find_outside_command,
	   "Select a trace frame whose PC is outside the given \
range.\nUsage: tfind outside addr1, addr2", 
	   &tfindlist);

  add_cmd ("range", class_trace, trace_find_range_command,
	   "Select a trace frame whose PC is in the given range.\n\
Usage: tfind range addr1,addr2", 
	   &tfindlist);

  add_cmd ("line", class_trace, trace_find_line_command,
	   "Select a trace frame by source line.\n\
Argument can be a line number (with optional source file), \n\
a function name, or '*' followed by an address.\n\
Default argument is 'the next source line that was traced'.",
	   &tfindlist);

  add_cmd ("tracepoint", class_trace, trace_find_tracepoint_command,
	   "Select a trace frame by tracepoint number.\n\
Default is the tracepoint for the current trace frame.",
	   &tfindlist);

  add_cmd ("pc", class_trace, trace_find_pc_command,
	   "Select a trace frame by PC.\n\
Default is the current PC, or the PC of the current trace frame.",
	   &tfindlist);

  add_cmd ("end", class_trace, trace_find_end_command,
	   "Synonym for 'none'.\n\
De-select any trace frame and resume 'live' debugging.",
	   &tfindlist);

  add_cmd ("none", class_trace, trace_find_none_command,
	   "De-select any trace frame and resume 'live' debugging.",
	   &tfindlist);

  add_cmd ("start", class_trace, trace_find_start_command,
	   "Select the first trace frame in the trace buffer.",
	   &tfindlist);

  add_com ("tstatus",  class_trace, trace_status_command,
	   "Display the status of the current trace data collection.");

  add_com ("tstop",  class_trace, trace_stop_command,
	   "Stop trace data collection.");

  add_com ("tstart", class_trace, trace_start_command,
	   "Start trace data collection.");

  add_com ("passcount", class_trace, trace_pass_command, 
	   "Set the passcount for a tracepoint.\n\
The trace will end when the tracepoint has been passed 'count' times.\n\
Usage: passcount COUNT TPNUM, where TPNUM may also be \"all\";\n\
if TPNUM is omitted, passcount refers to the last tracepoint defined.");

  add_com ("end", class_trace, end_pseudocom,
	   "Ends a list of commands or actions.\n\
Several GDB commands allow you to enter a list of commands or actions.\n\
Entering \"end\" on a line by itself is the normal way to terminate\n\
such a list.\n\n\
Note: the \"end\" command cannot be used at the gdb prompt.");

  add_com ("while-stepping", class_trace, while_stepping_pseudocom,
	   "Specify single-stepping behavior at a tracepoint.\n\
Argument is number of instructions to trace in single-step mode\n\
following the tracepoint.  This command is normally followed by\n\
one or more \"collect\" commands, to specify what to collect\n\
while single-stepping.\n\n\
Note: this command can only be used in a tracepoint \"actions\" list.");

  add_com ("collect", class_trace, collect_pseudocom, 
	   "Specify one or more data items to be collected at a tracepoint.\n\
Accepts a comma-separated list of (one or more) arguments.\n\
Things that may be collected include registers, variables, plus\n\
the following special arguments:\n\
    $regs   -- all registers.\n\
    $args   -- all function arguments.\n\
    $locals -- all variables local to the block/function scope.\n\
    $(addr,len) -- a literal memory range.\n\
    $($reg,addr,len) -- a register-relative literal memory range.\n\n\
Note: this command can only be used in a tracepoint \"actions\" list.");

  add_com ("actions", class_trace, trace_actions_command,
	   "Specify the actions to be taken at a tracepoint.\n\
Tracepoint actions may include collecting of specified data, \n\
single-stepping, or enabling/disabling other tracepoints, \n\
depending on target's capabilities.");

  add_cmd ("tracepoints", class_trace, delete_trace_command, 
	   "Delete specified tracepoints.\n\
Arguments are tracepoint numbers, separated by spaces.\n\
No argument means delete all tracepoints.",
	   &deletelist);

  add_cmd ("tracepoints", class_trace, disable_trace_command, 
	   "Disable specified tracepoints.\n\
Arguments are tracepoint numbers, separated by spaces.\n\
No argument means disable all tracepoints.",
	   &disablelist);

  add_cmd ("tracepoints", class_trace, enable_trace_command, 
	   "Enable specified tracepoints.\n\
Arguments are tracepoint numbers, separated by spaces.\n\
No argument means enable all tracepoints.",
	   &enablelist);

  add_com ("trace", class_trace, trace_command,
	   "Set a tracepoint at a specified line or function or address.\n\
Argument may be a line number, function name, or '*' plus an address.\n\
For a line number or function, trace at the start of its code.\n\
If an address is specified, trace at that exact address.\n\n\
Do \"help tracepoints\" for info on other tracepoint commands.");

  add_com_alias ("tp",   "trace", class_alias, 0);
  add_com_alias ("tr",   "trace", class_alias, 1);
  add_com_alias ("tra",  "trace", class_alias, 1);
  add_com_alias ("trac", "trace", class_alias, 1);
}

