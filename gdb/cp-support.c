/* Helper routines for C++ support in GDB.
   Copyright 2002, 2003, 2004 Free Software Foundation, Inc.

   Contributed by MontaVista Software.

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
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

#include "defs.h"
#include <ctype.h>
#include "cp-support.h"
#include "gdb_string.h"
#include "demangle.h"
#include "gdb_assert.h"
#include "gdbcmd.h"
#include "dictionary.h"
#include "objfiles.h"
#include "frame.h"
#include "symtab.h"
#include "block.h"
#include "complaints.h"
#include "gdbtypes.h"

#define d_left(dc) (dc)->u.s_binary.left
#define d_right(dc) (dc)->u.s_binary.right

/* Functions related to demangled name parsing.  */

static const char *find_last_component (const char *name);

static unsigned int cp_find_first_component_aux (const char *name,
						 int permissive);

static void demangled_name_complaint (const char *name);

/* Functions/variables related to overload resolution.  */

static int sym_return_val_size;
static int sym_return_val_index;
static struct symbol **sym_return_val;

static char *remove_params (const char *demangled_name);

static void overload_list_add_symbol (struct symbol *sym, char *oload_name);

/* The list of "maint cplus" commands.  */

struct cmd_list_element *maint_cplus_cmd_list = NULL;

/* The actual commands.  */

static void maint_cplus_command (char *arg, int from_tty);
static void first_component_command (char *arg, int from_tty);

/* Return the canonicalized form of STRING, or NULL if STRING can not be
   parsed.  */

/* FIXME: Should we also return NULL for things that trivially do not require
   any change?  i.e. alphanumeric strings.  */

char *
cp_canonicalize_string (const char *string)
{
  void *storage;
  struct demangle_component *ret_comp;
  char *ret;
  int len = strlen (string);

  len = len + len / 8;

  ret_comp = demangled_name_to_comp (string, &storage);
  if (ret_comp == NULL)
    return NULL;

  ret = cp_comp_to_string (ret_comp, len);

  xfree (storage);

  return ret;
}

/* Return the name of the class containing method PHYSNAME.  */

char *
class_name_from_physname (const char *physname)
{
  void *storage;
  char *demangled_name = NULL, *ret;
  struct demangle_component *ret_comp, *prev_comp;
  int done;

  ret_comp = mangled_name_to_comp (physname, DMGL_ANSI, &storage,
				   &demangled_name);
  if (ret_comp == NULL)
    return NULL;

  done = 0;
  prev_comp = NULL;
  while (!done)
    switch (ret_comp->type)
      {
      case DEMANGLE_COMPONENT_TYPED_NAME:
	prev_comp = NULL;
        ret_comp = d_right (ret_comp);
        break;
      case DEMANGLE_COMPONENT_QUAL_NAME:
      case DEMANGLE_COMPONENT_LOCAL_NAME:
	prev_comp = ret_comp;
        ret_comp = d_right (ret_comp);
        break;
      case DEMANGLE_COMPONENT_CONST:
      case DEMANGLE_COMPONENT_RESTRICT:
      case DEMANGLE_COMPONENT_VOLATILE:
      case DEMANGLE_COMPONENT_CONST_THIS:
      case DEMANGLE_COMPONENT_RESTRICT_THIS:
      case DEMANGLE_COMPONENT_VOLATILE_THIS:
      case DEMANGLE_COMPONENT_VENDOR_TYPE_QUAL:
	prev_comp = NULL;
        ret_comp = d_left (ret_comp);
        break;
      case DEMANGLE_COMPONENT_NAME:
      case DEMANGLE_COMPONENT_TEMPLATE:
      case DEMANGLE_COMPONENT_CTOR:
      case DEMANGLE_COMPONENT_DTOR:
      case DEMANGLE_COMPONENT_OPERATOR:
      case DEMANGLE_COMPONENT_EXTENDED_OPERATOR:
	done = 1;
	break;
      default:
	done = 1;
	prev_comp = NULL;
	ret_comp = NULL;
	break;
      }

  ret = NULL;
  if (prev_comp != NULL)
    {
      *prev_comp = *d_left (prev_comp);
      /* The ten is completely arbitrary; we don't have a good estimate.  */
      ret = cp_comp_to_string (prev_comp, 10);
    }

  xfree (storage);
  if (demangled_name)
    xfree (demangled_name);
  return ret;
}

/* Return the name of the method whose linkage name is PHYSNAME.  */

char *
method_name_from_physname (const char *physname)
{
  void *storage;
  char *demangled_name = NULL, *ret;
  struct demangle_component *ret_comp;
  int done;

  ret_comp = mangled_name_to_comp (physname, DMGL_ANSI, &storage,
				   &demangled_name);
  if (ret_comp == NULL)
    return NULL;

  done = 0;
  while (!done)
    switch (ret_comp->type)
      {
      case DEMANGLE_COMPONENT_QUAL_NAME:
      case DEMANGLE_COMPONENT_LOCAL_NAME:
      case DEMANGLE_COMPONENT_TYPED_NAME:
        ret_comp = d_right (ret_comp);
        break;
      case DEMANGLE_COMPONENT_CONST:
      case DEMANGLE_COMPONENT_RESTRICT:
      case DEMANGLE_COMPONENT_VOLATILE:
      case DEMANGLE_COMPONENT_CONST_THIS:
      case DEMANGLE_COMPONENT_RESTRICT_THIS:
      case DEMANGLE_COMPONENT_VOLATILE_THIS:
      case DEMANGLE_COMPONENT_VENDOR_TYPE_QUAL:
        ret_comp = d_left (ret_comp);
        break;
      case DEMANGLE_COMPONENT_NAME:
      case DEMANGLE_COMPONENT_TEMPLATE:
      case DEMANGLE_COMPONENT_CTOR:
      case DEMANGLE_COMPONENT_DTOR:
      case DEMANGLE_COMPONENT_OPERATOR:
      case DEMANGLE_COMPONENT_EXTENDED_OPERATOR:
	done = 1;
	break;
      default:
	done = 1;
	ret_comp = NULL;
	break;
      }

  ret = NULL;
  if (ret_comp != NULL)
    /* The ten is completely arbitrary; we don't have a good estimate.  */
    ret = cp_comp_to_string (ret_comp, 10);

  xfree (storage);
  if (demangled_name)
    xfree (demangled_name);
  return ret;
}

/* Here are some random pieces of trivia to keep in mind while trying
   to take apart demangled names:

   - Names can contain function arguments or templates, so the process
     has to be, to some extent recursive: maybe keep track of your
     depth based on encountering <> and ().

   - Parentheses don't just have to happen at the end of a name: they
     can occur even if the name in question isn't a function, because
     a template argument might be a type that's a function.

   - Conversely, even if you're trying to deal with a function, its
     demangled name might not end with ')': it could be a const or
     volatile class method, in which case it ends with "const" or
     "volatile".

   - Parentheses are also used in anonymous namespaces: a variable
     'foo' in an anonymous namespace gets demangled as "(anonymous
     namespace)::foo".

   - And operator names can contain parentheses or angle brackets.  */

/* FIXME: carlton/2003-03-13: We have several functions here with
   overlapping functionality; can we combine them?  Also, do they
   handle all the above considerations correctly?  */


/* This returns the length of first component of NAME, which should be
   the demangled name of a C++ variable/function/method/etc.
   Specifically, it returns the index of the first colon forming the
   boundary of the first component: so, given 'A::foo' or 'A::B::foo'
   it returns the 1, and given 'foo', it returns 0.  */

/* The character in NAME indexed by the return value is guaranteed to
   always be either ':' or '\0'.  */

/* NOTE: carlton/2003-03-13: This function is currently only intended
   for internal use: it's probably not entirely safe when called on
   user-generated input, because some of the 'index += 2' lines in
   cp_find_first_component_aux might go past the end of malformed
   input.  */

unsigned int
cp_find_first_component (const char *name)
{
  return cp_find_first_component_aux (name, 0);
}

/* Helper function for cp_find_first_component.  Like that function,
   it returns the length of the first component of NAME, but to make
   the recursion easier, it also stops if it reaches an unexpected ')'
   or '>' if the value of PERMISSIVE is nonzero.  */

/* Let's optimize away calls to strlen("operator").  */

#define LENGTH_OF_OPERATOR 8

static unsigned int
cp_find_first_component_aux (const char *name, int permissive)
{
  unsigned int index = 0;
  /* Operator names can show up in unexpected places.  Since these can
     contain parentheses or angle brackets, they can screw up the
     recursion.  But not every string 'operator' is part of an
     operater name: e.g. you could have a variable 'cooperator'.  So
     this variable tells us whether or not we should treat the string
     'operator' as starting an operator.  */
  int operator_possible = 1;

  for (;; ++index)
    {
      switch (name[index])
	{
	case '<':
	  /* Template; eat it up.  The calls to cp_first_component
	     should only return (I hope!) when they reach the '>'
	     terminating the component or a '::' between two
	     components.  (Hence the '+ 2'.)  */
	  index += 1;
	  for (index += cp_find_first_component_aux (name + index, 1);
	       name[index] != '>';
	       index += cp_find_first_component_aux (name + index, 1))
	    {
	      if (name[index] != ':')
		{
		  demangled_name_complaint (name);
		  return strlen (name);
		}
	      index += 2;
	    }
	  operator_possible = 1;
	  break;
	case '(':
	  /* Similar comment as to '<'.  */
	  index += 1;
	  for (index += cp_find_first_component_aux (name + index, 1);
	       name[index] != ')';
	       index += cp_find_first_component_aux (name + index, 1))
	    {
	      if (name[index] != ':')
		{
		  demangled_name_complaint (name);
		  return strlen (name);
		}
	      index += 2;
	    }
	  operator_possible = 1;
	  break;
	case '>':
	case ')':
	  if (permissive)
	    return index;
	  else
	    {
	      demangled_name_complaint (name);
	      return strlen (name);
	    }
	case '\0':
	case ':':
	  return index;
	case 'o':
	  /* Operator names can screw up the recursion.  */
	  if (operator_possible
	      && strncmp (name + index, "operator", LENGTH_OF_OPERATOR) == 0)
	    {
	      index += LENGTH_OF_OPERATOR;
	      while (isspace(name[index]))
		++index;
	      switch (name[index])
		{
		  /* Skip over one less than the appropriate number of
		     characters: the for loop will skip over the last
		     one.  */
		case '<':
		  if (name[index + 1] == '<')
		    index += 1;
		  else
		    index += 0;
		  break;
		case '>':
		case '-':
		  if (name[index + 1] == '>')
		    index += 1;
		  else
		    index += 0;
		  break;
		case '(':
		  index += 1;
		  break;
		default:
		  index += 0;
		  break;
		}
	    }
	  operator_possible = 0;
	  break;
	case ' ':
	case ',':
	case '.':
	case '&':
	case '*':
	  /* NOTE: carlton/2003-04-18: I'm not sure what the precise
	     set of relevant characters are here: it's necessary to
	     include any character that can show up before 'operator'
	     in a demangled name, and it's safe to include any
	     character that can't be part of an identifier's name.  */
	  operator_possible = 1;
	  break;
	default:
	  operator_possible = 0;
	  break;
	}
    }
}

/* Complain about a demangled name that we don't know how to parse.
   NAME is the demangled name in question.  */

static void
demangled_name_complaint (const char *name)
{
  complaint (&symfile_complaints,
	     "unexpected demangled name '%s'", name);
}

/* If NAME is the fully-qualified name of a C++
   function/variable/method/etc., this returns the length of its
   entire prefix: all of the namespaces and classes that make up its
   name.  Given 'A::foo', it returns 1, given 'A::B::foo', it returns
   4, given 'foo', it returns 0.  */

unsigned int
cp_entire_prefix_len (const char *name)
{
  unsigned int current_len = cp_find_first_component (name);
  unsigned int previous_len = 0;

  while (name[current_len] != '\0')
    {
      gdb_assert (name[current_len] == ':');
      previous_len = current_len;
      /* Skip the '::'.  */
      current_len += 2;
      current_len += cp_find_first_component (name + current_len);
    }

  return previous_len;
}

/* Overload resolution functions.  */

static char *
remove_params (const char *demangled_name)
{
  const char *argp;
  char *new_name;
  int depth;

  if (demangled_name == NULL)
    return NULL;

  /* First find the end of the arg list.  */
  argp = strrchr (demangled_name, ')');
  if (argp == NULL)
    return NULL;

  /* Back up to the beginning.  */
  depth = 1;

  while (argp-- > demangled_name)
    {
      if (*argp == ')')
	depth ++;
      else if (*argp == '(')
	{
	  depth --;

	  if (depth == 0)
	    break;
	}
    }
  if (depth != 0)
    internal_error (__FILE__, __LINE__,
		    "bad demangled name %s\n", demangled_name);
  while (argp[-1] == ' ' && argp > demangled_name)
    argp --;

  new_name = xmalloc (argp - demangled_name + 1);
  memcpy (new_name, demangled_name, argp - demangled_name);
  new_name[argp - demangled_name] = '\0';
  return new_name;
}

/*  Test to see if the symbol specified by SYMNAME (which is already
   demangled for C++ symbols) matches SYM_TEXT in the first SYM_TEXT_LEN
   characters.  If so, add it to the current completion list. */

static void
overload_list_add_symbol (struct symbol *sym, char *oload_name)
{
  int newsize;
  int i;
  char *sym_name;

  /* If there is no type information, we can't do anything, so skip */
  if (SYMBOL_TYPE (sym) == NULL)
    return;

  /* skip any symbols that we've already considered. */
  for (i = 0; i < sym_return_val_index; ++i)
    if (!strcmp (DEPRECATED_SYMBOL_NAME (sym), DEPRECATED_SYMBOL_NAME (sym_return_val[i])))
      return;

  /* Get the demangled name without parameters */
  sym_name = remove_params (SYMBOL_DEMANGLED_NAME (sym));
  if (!sym_name)
    return;

  /* skip symbols that cannot match */
  if (strcmp (sym_name, oload_name) != 0)
    {
      xfree (sym_name);
      return;
    }

  xfree (sym_name);

  /* We have a match for an overload instance, so add SYM to the current list
   * of overload instances */
  if (sym_return_val_index + 3 > sym_return_val_size)
    {
      newsize = (sym_return_val_size *= 2) * sizeof (struct symbol *);
      sym_return_val = (struct symbol **) xrealloc ((char *) sym_return_val, newsize);
    }
  sym_return_val[sym_return_val_index++] = sym;
  sym_return_val[sym_return_val_index] = NULL;
}

/* Return a null-terminated list of pointers to function symbols that
 * match name of the supplied symbol FSYM.
 * This is used in finding all overloaded instances of a function name.
 * This has been modified from make_symbol_completion_list.  */


struct symbol **
make_symbol_overload_list (struct symbol *fsym)
{
  struct symbol *sym;
  struct symtab *s;
  struct partial_symtab *ps;
  struct objfile *objfile;
  struct block *b, *surrounding_static_block = 0;
  struct dict_iterator iter;
  /* The name we are completing on. */
  char *oload_name = NULL;
  /* Length of name.  */
  int oload_name_len = 0;

  /* Look for the symbol we are supposed to complete on.  */

  oload_name = remove_params (SYMBOL_DEMANGLED_NAME (fsym));
  if (!oload_name)
    {
      sym_return_val_size = 1;
      sym_return_val = (struct symbol **) xmalloc (2 * sizeof (struct symbol *));
      sym_return_val[0] = fsym;
      sym_return_val[1] = NULL;

      return sym_return_val;
    }
  oload_name_len = strlen (oload_name);

  sym_return_val_size = 100;
  sym_return_val_index = 0;
  sym_return_val = (struct symbol **) xmalloc ((sym_return_val_size + 1) * sizeof (struct symbol *));
  sym_return_val[0] = NULL;

  /* Read in all partial symtabs containing a partial symbol named
     OLOAD_NAME.  */

  ALL_PSYMTABS (objfile, ps)
  {
    struct partial_symbol **psym;

    /* If the psymtab's been read in we'll get it when we search
       through the blockvector.  */
    if (ps->readin)
      continue;

    if ((lookup_partial_symbol (ps, oload_name, NULL, 1, VAR_DOMAIN)
	 != NULL)
	|| (lookup_partial_symbol (ps, oload_name, NULL, 0, VAR_DOMAIN)
	    != NULL))
      PSYMTAB_TO_SYMTAB (ps);
  }

  /* Search upwards from currently selected frame (so that we can
     complete on local vars.  */

  for (b = get_selected_block (0); b != NULL; b = BLOCK_SUPERBLOCK (b))
    {
      if (!BLOCK_SUPERBLOCK (b))
	{
	  surrounding_static_block = b;		/* For elimination of dups */
	}

      /* Also catch fields of types defined in this places which match our
         text string.  Only complete on types visible from current context. */

      ALL_BLOCK_SYMBOLS (b, iter, sym)
	{
	  overload_list_add_symbol (sym, oload_name);
	}
    }

  /* Go through the symtabs and check the externs and statics for
     symbols which match.  */

  ALL_SYMTABS (objfile, s)
  {
    QUIT;
    b = BLOCKVECTOR_BLOCK (BLOCKVECTOR (s), GLOBAL_BLOCK);
    ALL_BLOCK_SYMBOLS (b, iter, sym)
      {
	overload_list_add_symbol (sym, oload_name);
      }
  }

  ALL_SYMTABS (objfile, s)
  {
    QUIT;
    b = BLOCKVECTOR_BLOCK (BLOCKVECTOR (s), STATIC_BLOCK);
    /* Don't do this block twice.  */
    if (b == surrounding_static_block)
      continue;
    ALL_BLOCK_SYMBOLS (b, iter, sym)
      {
	overload_list_add_symbol (sym, oload_name);
      }
  }

  xfree (oload_name);

  return (sym_return_val);
}

/* Lookup the rtti type for a class name. */

struct type *
cp_lookup_rtti_type (const char *name, struct block *block)
{
  struct symbol * rtti_sym;
  struct type * rtti_type;

  rtti_sym = lookup_symbol (name, block, STRUCT_DOMAIN, NULL, NULL);

  if (rtti_sym == NULL)
    {
      warning ("RTTI symbol not found for class '%s'", name);
      return NULL;
    }

  if (SYMBOL_CLASS (rtti_sym) != LOC_TYPEDEF)
    {
      warning ("RTTI symbol for class '%s' is not a type", name);
      return NULL;
    }

  rtti_type = SYMBOL_TYPE (rtti_sym);

  switch (TYPE_CODE (rtti_type))
    {
    case TYPE_CODE_CLASS:
      break;
    case TYPE_CODE_NAMESPACE:
      /* chastain/2003-11-26: the symbol tables often contain fake
	 symbols for namespaces with the same name as the struct.
	 This warning is an indication of a bug in the lookup order
	 or a bug in the way that the symbol tables are populated.  */
      warning ("RTTI symbol for class '%s' is a namespace", name);
      return NULL;
    default:
      warning ("RTTI symbol for class '%s' has bad type", name);
      return NULL;
    }

  return rtti_type;
}

/* Don't allow just "maintenance cplus".  */

static  void
maint_cplus_command (char *arg, int from_tty)
{
  printf_unfiltered ("\"maintenance cplus\" must be followed by the name of a command.\n");
  help_list (maint_cplus_cmd_list, "maintenance cplus ", -1, gdb_stdout);
}

/* This is a front end for cp_find_first_component, for unit testing.
   Be careful when using it: see the NOTE above
   cp_find_first_component.  */

static void
first_component_command (char *arg, int from_tty)
{
  int len = cp_find_first_component (arg);
  char *prefix = alloca (len + 1);

  memcpy (prefix, arg, len);
  prefix[len] = '\0';

  printf_unfiltered ("%s\n", prefix);
}

extern initialize_file_ftype _initialize_cp_support; /* -Wmissing-prototypes */

void
_initialize_cp_support (void)
{
  add_prefix_cmd ("cplus", class_maintenance, maint_cplus_command,
		  "C++ maintenance commands.", &maint_cplus_cmd_list,
		  "maintenance cplus ", 0, &maintenancelist);
  add_alias_cmd ("cp", "cplus", class_maintenance, 1, &maintenancelist);

  add_cmd ("first_component", class_maintenance, first_component_command,
	   "Print the first class/namespace component of NAME.",
	   &maint_cplus_cmd_list);
		  
}
