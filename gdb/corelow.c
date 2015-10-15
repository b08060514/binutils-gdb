/* Core dump and executable file functions below target vector, for GDB.

   Copyright (C) 1986-2015 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "arch-utils.h"
#include <signal.h>
#include <fcntl.h>
#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>		/* needed for F_OK and friends */
#endif
#include "frame.h"		/* required by inferior.h */
#include "inferior.h"
#include "infrun.h"
#include "symtab.h"
#include "command.h"
#include "bfd.h"
#include "target.h"
#include "gdbcore.h"
#include "gdbthread.h"
#include "regcache.h"
#include "regset.h"
#include "symfile.h"
#include "exec.h"
#include "readline/readline.h"
#include "solib.h"
#include "filenames.h"
#include "progspace.h"
#include "objfiles.h"
#include "gdb_bfd.h"
#include "completer.h"
#include "filestuff.h"

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

/* List of all available core_fns.  On gdb startup, each core file
   register reader calls deprecated_add_core_fns() to register
   information on each core format it is prepared to read.  */

static struct core_fns *core_file_fns = NULL;

/* A subclass of target_ops that also holds data for a core
   target.  */

struct core_target_ops_with_data
{
  /* The base class.  */

  struct target_ops base;

  /* The core_fns for a core file handler that is prepared to read the
     core file currently open on core_bfd.  */

  struct core_fns *core_vec;

  /* FIXME: kettenis/20031023: Eventually this variable should
     disappear.  */

  struct gdbarch *core_gdbarch;

  /* The section table.  Note that these target sections are *not*
     mapped in the current address spaces' set of target sections ---
     those should come only from pure executable or shared library
     bfds.  The core bfd sections are an implementation detail of the
     core target, just like ptrace is for unix child targets.  */
  struct target_section_table core_data;
};

static void core_files_info (struct target_ops *);

static int gdb_check_format (bfd *);

static void core_close_cleanup (void *ignore);

static void add_to_thread_list (bfd *, asection *, void *);

static void init_core_ops (void);

void _initialize_corelow (void);

static struct target_ops core_ops;

/* An arbitrary identifier for the core inferior.  */
#define CORELOW_PID 1

/* Link a new core_fns into the global core_file_fns list.  Called on
   gdb startup by the _initialize routine in each core file register
   reader, to register information about each format the reader is
   prepared to handle.  */

void
deprecated_add_core_fns (struct core_fns *cf)
{
  cf->next = core_file_fns;
  core_file_fns = cf;
}

/* The default function that core file handlers can use to examine a
   core file BFD and decide whether or not to accept the job of
   reading the core file.  */

int
default_core_sniffer (struct core_fns *our_fns, bfd *abfd)
{
  int result;

  result = (bfd_get_flavour (abfd) == our_fns -> core_flavour);
  return (result);
}

/* Walk through the list of core functions to find a set that can
   handle the core file open on ABFD.  Returns pointer to set that is
   selected.  */

static struct core_fns *
sniff_core_bfd (bfd *abfd, struct gdbarch *core_gdbarch)
{
  struct core_fns *cf;
  struct core_fns *yummy = NULL;
  int matches = 0;;

  /* Don't sniff if we have support for register sets in
     CORE_GDBARCH.  */
  if (core_gdbarch && gdbarch_iterate_over_regset_sections_p (core_gdbarch))
    return NULL;

  for (cf = core_file_fns; cf != NULL; cf = cf->next)
    {
      if (cf->core_sniffer (cf, abfd))
	{
	  yummy = cf;
	  matches++;
	}
    }
  if (matches > 1)
    {
      warning (_("\"%s\": ambiguous core format, %d handlers match"),
	       bfd_get_filename (abfd), matches);
    }
  else if (matches == 0)
    error (_("\"%s\": no core file handler recognizes format"),
	   bfd_get_filename (abfd));

  return (yummy);
}

/* The default is to reject every core file format we see.  Either
   BFD has to recognize it, or we have to provide a function in the
   core file handler that recognizes it.  */

int
default_check_format (bfd *abfd)
{
  return (0);
}

/* Attempt to recognize core file formats that BFD rejects.  */

static int
gdb_check_format (bfd *abfd)
{
  struct core_fns *cf;

  for (cf = core_file_fns; cf != NULL; cf = cf->next)
    {
      if (cf->check_format (abfd))
	{
	  return (1);
	}
    }
  return (0);
}



/* Return the core_target_ops_with_data for the current target stack,
   if any.  */

static struct core_target_ops_with_data *
get_core_target_ops (void)
{
  struct target_ops *targ = find_target_at (process_stratum);

  if (targ == NULL || targ->to_identity != &core_ops)
    return NULL;

  /* Downcast.  */
  return (struct core_target_ops_with_data *) targ;
}

/* Discard all vestiges of any previous core file and mark data and
   stack spaces as empty.  */

static void
core_xclose (struct target_ops *self)
{
  /* Downcast.  */
  struct core_target_ops_with_data *cops
    = (struct core_target_ops_with_data *) self;

  gdb_assert (self->to_identity == &core_ops);

  if (core_bfd)
    {
      int pid = ptid_get_pid (inferior_ptid);
      inferior_ptid = null_ptid;    /* Avoid confusion from thread
				       stuff.  */
      if (pid != 0)
	exit_inferior_silent (pid);

      /* Clear out solib state while the bfd is still open.  See
         comments in clear_solib in solib.c.  */
      clear_solib ();

      gdb_bfd_unref (core_bfd);
      core_bfd = NULL;
    }

  xfree (cops->core_data.sections);
  xfree (cops);
}

static void
core_close_cleanup (void *arg)
{
  core_xclose (arg);
}

/* Look for sections whose names start with `.reg/' so that we can
   extract the list of threads in a core file.  */

static void
add_to_thread_list (bfd *abfd, asection *asect, void *reg_sect_arg)
{
  ptid_t ptid;
  int core_tid;
  int pid, lwpid;
  asection *reg_sect = (asection *) reg_sect_arg;
  int fake_pid_p = 0;
  struct inferior *inf;

  if (!startswith (bfd_section_name (abfd, asect), ".reg/"))
    return;

  core_tid = atoi (bfd_section_name (abfd, asect) + 5);

  pid = bfd_core_file_pid (core_bfd);
  if (pid == 0)
    {
      fake_pid_p = 1;
      pid = CORELOW_PID;
    }

  lwpid = core_tid;

  inf = current_inferior ();
  if (inf->pid == 0)
    {
      inferior_appeared (inf, pid);
      inf->fake_pid_p = fake_pid_p;
    }

  ptid = ptid_build (pid, lwpid, 0);

  add_thread (ptid);

/* Warning, Will Robinson, looking at BFD private data! */

  if (reg_sect != NULL
      && asect->filepos == reg_sect->filepos)	/* Did we find .reg?  */
    inferior_ptid = ptid;			/* Yes, make it current.  */
}

/* This routine opens and sets up the core file bfd.  */

static void
core_open (const char *arg, int from_tty)
{
  const char *p;
  int siggy;
  struct cleanup *old_chain;
  char *temp;
  bfd *temp_bfd;
  int scratch_chan;
  int flags;
  volatile struct gdb_exception except;
  char *filename;
  struct core_target_ops_with_data *cops;

  target_preopen (from_tty);
  if (!arg)
    {
      if (core_bfd)
	error (_("No core file specified.  (Use `detach' "
		 "to stop debugging a core file.)"));
      else
	error (_("No core file specified."));
    }

  filename = tilde_expand (arg);
  if (!IS_ABSOLUTE_PATH (filename))
    {
      temp = concat (current_directory, "/",
		     filename, (char *) NULL);
      xfree (filename);
      filename = temp;
    }

  old_chain = make_cleanup (xfree, filename);

  flags = O_BINARY | O_LARGEFILE;
  if (write_files)
    flags |= O_RDWR;
  else
    flags |= O_RDONLY;
  scratch_chan = gdb_open_cloexec (filename, flags, 0);
  if (scratch_chan < 0)
    perror_with_name (filename);

  temp_bfd = gdb_bfd_fopen (filename, gnutarget, 
			    write_files ? FOPEN_RUB : FOPEN_RB,
			    scratch_chan);
  if (temp_bfd == NULL)
    perror_with_name (filename);

  if (!bfd_check_format (temp_bfd, bfd_core)
      && !gdb_check_format (temp_bfd))
    {
      /* Do it after the err msg */
      /* FIXME: should be checking for errors from bfd_close (for one
         thing, on error it does not free all the storage associated
         with the bfd).  */
      make_cleanup_bfd_unref (temp_bfd);
      error (_("\"%s\" is not a core dump: %s"),
	     filename, bfd_errmsg (bfd_get_error ()));
    }

  /* Looks semi-reasonable.  Toss the old core file and work on the
     new.  */

  do_cleanups (old_chain);
  unpush_target (&core_ops);
  core_bfd = temp_bfd;

  cops = TARGET_NEW (struct core_target_ops_with_data, &core_ops);
  old_chain = make_cleanup (core_close_cleanup, cops);

  cops->core_gdbarch = gdbarch_from_bfd (core_bfd);

  /* Find a suitable core file handler to munch on core_bfd */
  cops->core_vec = sniff_core_bfd (core_bfd, cops->core_gdbarch);

  validate_files ();

  /* Find the data section */
  if (build_section_table (core_bfd,
			   &cops->core_data.sections,
			   &cops->core_data.sections_end))
    error (_("\"%s\": Can't find sections: %s"),
	   bfd_get_filename (core_bfd), bfd_errmsg (bfd_get_error ()));

  /* If we have no exec file, try to set the architecture from the
     core file.  We don't do this unconditionally since an exec file
     typically contains more information that helps us determine the
     architecture than a core file.  */
  if (!exec_bfd)
    set_gdbarch_from_file (core_bfd);

  push_target (&cops->base);
  discard_cleanups (old_chain);

  /* Do this before acknowledging the inferior, so if
     post_create_inferior throws (can happen easilly if you're loading
     a core file with the wrong exec), we aren't left with threads
     from the previous inferior.  */
  init_thread_list ();

  inferior_ptid = null_ptid;

  /* Need to flush the register cache (and the frame cache) from a
     previous debug session.  If inferior_ptid ends up the same as the
     last debug session --- e.g., b foo; run; gcore core1; step; gcore
     core2; core core1; core core2 --- then there's potential for
     get_current_regcache to return the cached regcache of the
     previous session, and the frame cache being stale.  */
  registers_changed ();

  /* Build up thread list from BFD sections, and possibly set the
     current thread to the .reg/NN section matching the .reg
     section.  */
  bfd_map_over_sections (core_bfd, add_to_thread_list,
			 bfd_get_section_by_name (core_bfd, ".reg"));

  if (ptid_equal (inferior_ptid, null_ptid))
    {
      /* Either we found no .reg/NN section, and hence we have a
	 non-threaded core (single-threaded, from gdb's perspective),
	 or for some reason add_to_thread_list couldn't determine
	 which was the "main" thread.  The latter case shouldn't
	 usually happen, but we're dealing with input here, which can
	 always be broken in different ways.  */
      struct thread_info *thread = first_thread_of_process (-1);

      if (thread == NULL)
	{
	  inferior_appeared (current_inferior (), CORELOW_PID);
	  inferior_ptid = pid_to_ptid (CORELOW_PID);
	  add_thread_silent (inferior_ptid);
	}
      else
	switch_to_thread (thread->ptid);
    }

  post_create_inferior (&cops->base, from_tty);

  /* Now go through the target stack looking for threads since there
     may be a thread_stratum target loaded on top of target core by
     now.  The layer above should claim threads found in the BFD
     sections.  */
  TRY
    {
      target_update_thread_list ();
    }

  CATCH (except, RETURN_MASK_ERROR)
    {
      exception_print (gdb_stderr, except);
    }
  END_CATCH

  p = bfd_core_file_failing_command (core_bfd);
  if (p)
    printf_filtered (_("Core was generated by `%s'.\n"), p);

  /* Clearing any previous state of convenience variables.  */
  clear_exit_convenience_vars ();

  siggy = bfd_core_file_failing_signal (core_bfd);
  if (siggy > 0)
    {
      /* If we don't have a CORE_GDBARCH to work with, assume a native
	 core (map gdb_signal from host signals).  If we do have
	 CORE_GDBARCH to work with, but no gdb_signal_from_target
	 implementation for that gdbarch, as a fallback measure,
	 assume the host signal mapping.  It'll be correct for native
	 cores, but most likely incorrect for cross-cores.  */
      enum gdb_signal sig
	= (cops->core_gdbarch != NULL
	   && gdbarch_gdb_signal_from_target_p (cops->core_gdbarch)
	   ? gdbarch_gdb_signal_from_target (cops->core_gdbarch, siggy)
	   : gdb_signal_from_host (siggy));

      printf_filtered (_("Program terminated with signal %s, %s.\n"),
		       gdb_signal_to_name (sig), gdb_signal_to_string (sig));

      /* Set the value of the internal variable $_exitsignal,
	 which holds the signal uncaught by the inferior.  */
      set_internalvar_integer (lookup_internalvar ("_exitsignal"),
			       siggy);
    }

  /* Fetch all registers from core file.  */
  target_fetch_registers (get_current_regcache (), -1);

  /* Now, set up the frame cache, and print the top of stack.  */
  reinit_frame_cache ();
  print_stack_frame (get_selected_frame (NULL), 1, SRC_AND_LOC, 1);

  /* Current thread should be NUM 1 but the user does not know that.
     If a program is single threaded gdb in general does not mention
     anything about threads.  That is why the test is >= 2.  */
  if (thread_count () >= 2)
    {
      TRY
	{
	  thread_command (NULL, from_tty);
	}
      CATCH (except, RETURN_MASK_ERROR)
	{
	  exception_print (gdb_stderr, except);
	}
      END_CATCH
    }
}

static void
core_detach (struct target_ops *ops, const char *args, int from_tty)
{
  if (args)
    error (_("Too many arguments"));
  unpush_target (ops);
  reinit_frame_cache ();
  if (from_tty)
    printf_filtered (_("No core file now.\n"));
}

/* Try to retrieve registers from a section in core_bfd, and supply
   them to core_vec->core_read_registers, as the register set numbered
   WHICH.

   If inferior_ptid's lwp member is zero, do the single-threaded
   thing: look for a section named NAME.  If inferior_ptid's lwp
   member is non-zero, do the multi-threaded thing: look for a section
   named "NAME/LWP", where LWP is the shortest ASCII decimal
   representation of inferior_ptid's lwp member.

   HUMAN_NAME is a human-readable name for the kind of registers the
   NAME section contains, for use in error messages.

   If REQUIRED is non-zero, print an error if the core file doesn't
   have a section by the appropriate name.  Otherwise, just do
   nothing.  */

static void
get_core_register_section (struct regcache *regcache,
			   const struct regset *regset,
			   const char *name,
			   int min_size,
			   int which,
			   const char *human_name,
			   int required)
{
  static char *section_name = NULL;
  struct bfd_section *section;
  bfd_size_type size;
  char *contents;
  struct core_target_ops_with_data *cops = get_core_target_ops ();

  xfree (section_name);

  if (ptid_get_lwp (inferior_ptid))
    section_name = xstrprintf ("%s/%ld", name,
			       ptid_get_lwp (inferior_ptid));
  else
    section_name = xstrdup (name);

  section = bfd_get_section_by_name (core_bfd, section_name);
  if (! section)
    {
      if (required)
	warning (_("Couldn't find %s registers in core file."),
		 human_name);
      return;
    }

  size = bfd_section_size (core_bfd, section);
  if (size < min_size)
    {
      warning (_("Section `%s' in core file too small."), section_name);
      return;
    }
  if (size != min_size && !(regset->flags & REGSET_VARIABLE_SIZE))
    {
      warning (_("Unexpected size of section `%s' in core file."),
 	       section_name);
    }

  contents = (char *) alloca (size);

  contents = alloca (size);
  if (! bfd_get_section_contents (core_bfd, section, contents,
				  (file_ptr) 0, size))
    {
      warning (_("Couldn't read %s registers from `%s' section in core file."),
	       human_name, name);
      return;
    }

  if (regset != NULL)
    {
      regset->supply_regset (regset, regcache, -1, contents, size);
      return;
    }

  gdb_assert (cops->core_vec);
  cops->core_vec->core_read_registers (regcache, contents, size, which,
				       ((CORE_ADDR)
					bfd_section_vma (core_bfd, section)));
}

/* Callback for get_core_registers that handles a single core file
   register note section. */

static void
get_core_registers_cb (const char *sect_name, int size,
		       const struct regset *regset,
		       const char *human_name, void *cb_data)
{
  struct regcache *regcache = (struct regcache *) cb_data;
  int required = 0;

  if (strcmp (sect_name, ".reg") == 0)
    {
      required = 1;
      if (human_name == NULL)
	human_name = "general-purpose";
    }
  else if (strcmp (sect_name, ".reg2") == 0)
    {
      if (human_name == NULL)
	human_name = "floating-point";
    }

  /* The 'which' parameter is only used when no regset is provided.
     Thus we just set it to -1. */
  get_core_register_section (regcache, regset, sect_name,
			     size, -1, human_name, required);
}

/* Get the registers out of a core file.  This is the machine-
   independent part.  Fetch_core_registers is the machine-dependent
   part, typically implemented in the xm-file for each
   architecture.  */

/* We just get all the registers, so we don't use regno.  */

static void
get_core_registers (struct target_ops *ops,
		    struct regcache *regcache, int regno)
{
  struct core_regset_section *sect_list;
  int i;
  struct core_target_ops_with_data *cops = get_core_target_ops ();

  if (!(cops->core_gdbarch
	&& gdbarch_iterate_over_regset_sections_p (cops->core_gdbarch))
      && (cops->core_vec == NULL
	  || cops->core_vec->core_read_registers == NULL))
    {
      fprintf_filtered (gdb_stderr,
		     "Can't fetch registers from this type of core file\n");
      return;
    }

  if (gdbarch_iterate_over_regset_sections_p (cops->core_gdbarch))
    gdbarch_iterate_over_regset_sections (cops->core_gdbarch,
					  get_core_registers_cb,
					  (void *) regcache, NULL);
  else
    {
      get_core_register_section (regcache, NULL,
				 ".reg", 0, 0, "general-purpose", 1);
      get_core_register_section (regcache, NULL,
				 ".reg2", 0, 2, "floating-point", 0);
    }

  /* Mark all registers not found in the core as unavailable.  */
  for (i = 0; i < gdbarch_num_regs (get_regcache_arch (regcache)); i++)
    if (regcache_register_status (regcache, i) == REG_UNKNOWN)
      regcache_raw_supply (regcache, i, NULL);
}

static void
core_files_info (struct target_ops *t)
{
  struct core_target_ops_with_data *cops = get_core_target_ops ();

  print_section_info (&cops->core_data, core_bfd);
}

struct spuid_list
{
  gdb_byte *buf;
  ULONGEST offset;
  LONGEST len;
  ULONGEST pos;
  ULONGEST written;
};

static void
add_to_spuid_list (bfd *abfd, asection *asect, void *list_p)
{
  struct spuid_list *list = (struct spuid_list *) list_p;
  enum bfd_endian byte_order
    = bfd_big_endian (abfd) ? BFD_ENDIAN_BIG : BFD_ENDIAN_LITTLE;
  int fd, pos = 0;

  sscanf (bfd_section_name (abfd, asect), "SPU/%d/regs%n", &fd, &pos);
  if (pos == 0)
    return;

  if (list->pos >= list->offset && list->pos + 4 <= list->offset + list->len)
    {
      store_unsigned_integer (list->buf + list->pos - list->offset,
			      4, byte_order, fd);
      list->written += 4;
    }
  list->pos += 4;
}

/* Read siginfo data from the core, if possible.  Returns -1 on
   failure.  Otherwise, returns the number of bytes read.  ABFD is the
   core file's BFD; READBUF, OFFSET, and LEN are all as specified by
   the to_xfer_partial interface.  */

static LONGEST
get_core_siginfo (bfd *abfd, gdb_byte *readbuf, ULONGEST offset, ULONGEST len)
{
  asection *section;
  char *section_name;
  const char *name = ".note.linuxcore.siginfo";

  if (ptid_get_lwp (inferior_ptid))
    section_name = xstrprintf ("%s/%ld", name,
			       ptid_get_lwp (inferior_ptid));
  else
    section_name = xstrdup (name);

  section = bfd_get_section_by_name (abfd, section_name);
  xfree (section_name);
  if (section == NULL)
    return -1;

  if (!bfd_get_section_contents (abfd, section, readbuf, offset, len))
    return -1;

  return len;
}

static enum target_xfer_status
core_xfer_partial (struct target_ops *ops, enum target_object object,
		   const char *annex, gdb_byte *readbuf,
		   const gdb_byte *writebuf, ULONGEST offset,
		   ULONGEST len, ULONGEST *xfered_len)
{
  struct core_target_ops_with_data *cops = get_core_target_ops ();

  switch (object)
    {
    case TARGET_OBJECT_MEMORY:
      return section_table_xfer_memory_partial (readbuf, writebuf,
						offset, len, xfered_len,
						cops->core_data.sections,
						cops->core_data.sections_end,
						NULL);

    case TARGET_OBJECT_AUXV:
      if (readbuf)
	{
	  /* When the aux vector is stored in core file, BFD
	     represents this with a fake section called ".auxv".  */

	  struct bfd_section *section;
	  bfd_size_type size;

	  section = bfd_get_section_by_name (core_bfd, ".auxv");
	  if (section == NULL)
	    return TARGET_XFER_E_IO;

	  size = bfd_section_size (core_bfd, section);
	  if (offset >= size)
	    return TARGET_XFER_EOF;
	  size -= offset;
	  if (size > len)
	    size = len;

	  if (size == 0)
	    return TARGET_XFER_EOF;
	  if (!bfd_get_section_contents (core_bfd, section, readbuf,
					 (file_ptr) offset, size))
	    {
	      warning (_("Couldn't read NT_AUXV note in core file."));
	      return TARGET_XFER_E_IO;
	    }

	  *xfered_len = (ULONGEST) size;
	  return TARGET_XFER_OK;
	}
      return TARGET_XFER_E_IO;

    case TARGET_OBJECT_WCOOKIE:
      if (readbuf)
	{
	  /* When the StackGhost cookie is stored in core file, BFD
	     represents this with a fake section called
	     ".wcookie".  */

	  struct bfd_section *section;
	  bfd_size_type size;

	  section = bfd_get_section_by_name (core_bfd, ".wcookie");
	  if (section == NULL)
	    return TARGET_XFER_E_IO;

	  size = bfd_section_size (core_bfd, section);
	  if (offset >= size)
	    return TARGET_XFER_EOF;
	  size -= offset;
	  if (size > len)
	    size = len;

	  if (size == 0)
	    return TARGET_XFER_EOF;
	  if (!bfd_get_section_contents (core_bfd, section, readbuf,
					 (file_ptr) offset, size))
	    {
	      warning (_("Couldn't read StackGhost cookie in core file."));
	      return TARGET_XFER_E_IO;
	    }

	  *xfered_len = (ULONGEST) size;
	  return TARGET_XFER_OK;

	}
      return TARGET_XFER_E_IO;

    case TARGET_OBJECT_LIBRARIES:
      if (cops->core_gdbarch
	  && gdbarch_core_xfer_shared_libraries_p (cops->core_gdbarch))
	{
	  if (writebuf)
	    return TARGET_XFER_E_IO;
	  else
	    {
	      *xfered_len
		= gdbarch_core_xfer_shared_libraries (cops->core_gdbarch,
						      readbuf,
						      offset, len);

	      if (*xfered_len == 0)
		return TARGET_XFER_EOF;
	      else
		return TARGET_XFER_OK;
	    }
	}
      /* FALL THROUGH */

    case TARGET_OBJECT_LIBRARIES_AIX:
      if (cops->core_gdbarch
	  && gdbarch_core_xfer_shared_libraries_aix_p (cops->core_gdbarch))
	{
	  if (writebuf)
	    return TARGET_XFER_E_IO;
	  else
	    {
	      *xfered_len
		= gdbarch_core_xfer_shared_libraries_aix (cops->core_gdbarch,
							  readbuf, offset,
							  len);

	      if (*xfered_len == 0)
		return TARGET_XFER_EOF;
	      else
		return TARGET_XFER_OK;
	    }
	}
      /* FALL THROUGH */

    case TARGET_OBJECT_SPU:
      if (readbuf && annex)
	{
	  /* When the SPU contexts are stored in a core file, BFD
	     represents this with a fake section called
	     "SPU/<annex>".  */

	  struct bfd_section *section;
	  bfd_size_type size;
	  char sectionstr[100];

	  xsnprintf (sectionstr, sizeof sectionstr, "SPU/%s", annex);

	  section = bfd_get_section_by_name (core_bfd, sectionstr);
	  if (section == NULL)
	    return TARGET_XFER_E_IO;

	  size = bfd_section_size (core_bfd, section);
	  if (offset >= size)
	    return TARGET_XFER_EOF;
	  size -= offset;
	  if (size > len)
	    size = len;

	  if (size == 0)
	    return TARGET_XFER_EOF;
	  if (!bfd_get_section_contents (core_bfd, section, readbuf,
					 (file_ptr) offset, size))
	    {
	      warning (_("Couldn't read SPU section in core file."));
	      return TARGET_XFER_E_IO;
	    }

	  *xfered_len = (ULONGEST) size;
	  return TARGET_XFER_OK;
	}
      else if (readbuf)
	{
	  /* NULL annex requests list of all present spuids.  */
	  struct spuid_list list;

	  list.buf = readbuf;
	  list.offset = offset;
	  list.len = len;
	  list.pos = 0;
	  list.written = 0;
	  bfd_map_over_sections (core_bfd, add_to_spuid_list, &list);

	  if (list.written == 0)
	    return TARGET_XFER_EOF;
	  else
	    {
	      *xfered_len = (ULONGEST) list.written;
	      return TARGET_XFER_OK;
	    }
	}
      return TARGET_XFER_E_IO;

    case TARGET_OBJECT_SIGNAL_INFO:
      if (readbuf)
	{
	  LONGEST l = get_core_siginfo (core_bfd, readbuf, offset, len);

	  if (l > 0)
	    {
	      *xfered_len = len;
	      return TARGET_XFER_OK;
	    }
	}
      return TARGET_XFER_E_IO;

    default:
      return ops->beneath->to_xfer_partial (ops->beneath, object,
					    annex, readbuf,
					    writebuf, offset, len,
					    xfered_len);
    }
}


/* If mourn is being called in all the right places, this could be say
   `gdb internal error' (since generic_mourn calls
   breakpoint_init_inferior).  */

static int
ignore (struct target_ops *ops, struct gdbarch *gdbarch,
	struct bp_target_info *bp_tgt)
{
  return 0;
}


/* Okay, let's be honest: threads gleaned from a core file aren't
   exactly lively, are they?  On the other hand, if we don't claim
   that each & every one is alive, then we don't get any of them
   to appear in an "info thread" command, which is quite a useful
   behaviour.
 */
static int
core_thread_alive (struct target_ops *ops, ptid_t ptid)
{
  return 1;
}

/* Ask the current architecture what it knows about this core file.
   That will be used, in turn, to pick a better architecture.  This
   wrapper could be avoided if targets got a chance to specialize
   core_ops.  */

static const struct target_desc *
core_read_description (struct target_ops *target)
{
  struct core_target_ops_with_data *cops = get_core_target_ops ();

  if (cops->core_gdbarch
      && gdbarch_core_read_description_p (cops->core_gdbarch))
    {
      const struct target_desc *result;

      result = gdbarch_core_read_description (cops->core_gdbarch, 
					      target, core_bfd);
      if (result != NULL)
	return result;
    }

  return target->beneath->to_read_description (target->beneath);
}

static char *
core_pid_to_str (struct target_ops *ops, ptid_t ptid)
{
  static char buf[64];
  struct inferior *inf;
  int pid;
  struct core_target_ops_with_data *cops = get_core_target_ops ();

  /* The preferred way is to have a gdbarch/OS specific
     implementation.  */
  if (cops->core_gdbarch
      && gdbarch_core_pid_to_str_p (cops->core_gdbarch))
    return gdbarch_core_pid_to_str (cops->core_gdbarch, ptid);

  /* Otherwise, if we don't have one, we'll just fallback to
     "process", with normal_pid_to_str.  */

  /* Try the LWPID field first.  */
  pid = ptid_get_lwp (ptid);
  if (pid != 0)
    return normal_pid_to_str (pid_to_ptid (pid));

  /* Otherwise, this isn't a "threaded" core -- use the PID field, but
     only if it isn't a fake PID.  */
  inf = find_inferior_pid (ptid_get_pid (ptid));
  if (inf != NULL && !inf->fake_pid_p)
    return normal_pid_to_str (ptid);

  /* No luck.  We simply don't have a valid PID to print.  */
  xsnprintf (buf, sizeof buf, "<main task>");
  return buf;
}

static int
core_has_memory (struct target_ops *ops)
{
  return (core_bfd != NULL);
}

static int
core_has_stack (struct target_ops *ops)
{
  return (core_bfd != NULL);
}

static int
core_has_registers (struct target_ops *ops)
{
  return (core_bfd != NULL);
}

/* Implement the to_info_proc method.  */

static void
core_info_proc (struct target_ops *ops, const char *args,
		enum info_proc_what request)
{
  struct gdbarch *gdbarch = get_current_arch ();

  /* Since this is the core file target, call the 'core_info_proc'
     method on gdbarch, not 'info_proc'.  */
  if (gdbarch_core_info_proc_p (gdbarch))
    gdbarch_core_info_proc (gdbarch, args, request);
}

/* Fill in core_ops with its defined operations and properties.  */

static void
init_core_ops (void)
{
  core_ops.to_shortname = "core";
  core_ops.to_longname = "Local core dump file";
  core_ops.to_doc =
    "Use a core file as a target.  Specify the filename of the core file.";
  core_ops.to_open = core_open;
  core_ops.to_xclose = core_xclose;
  core_ops.to_detach = core_detach;
  core_ops.to_fetch_registers = get_core_registers;
  core_ops.to_xfer_partial = core_xfer_partial;
  core_ops.to_files_info = core_files_info;
  core_ops.to_insert_breakpoint = ignore;
  core_ops.to_remove_breakpoint = ignore;
  core_ops.to_thread_alive = core_thread_alive;
  core_ops.to_read_description = core_read_description;
  core_ops.to_pid_to_str = core_pid_to_str;
  core_ops.to_stratum = process_stratum;
  core_ops.to_has_memory = core_has_memory;
  core_ops.to_has_stack = core_has_stack;
  core_ops.to_has_registers = core_has_registers;
  core_ops.to_info_proc = core_info_proc;
  core_ops.to_magic = OPS_MAGIC;

  if (core_target)
    internal_error (__FILE__, __LINE__, 
		    _("init_core_ops: core target already exists (\"%s\")."),
		    core_target->to_longname);
  core_target = &core_ops;
}

void
_initialize_corelow (void)
{
  init_core_ops ();

  add_target_with_completer (&core_ops, filename_completer);
}
