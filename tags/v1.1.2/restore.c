/*-------------------------------------------------------------------------
 *
 * restore.c: restore DB cluster and archived WAL.
 *
 * Copyright (c) 2009-2010, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_rman.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "catalog/pg_control.h"

static void backup_online_files(bool re_recovery);
static void restore_online_files(void);
static void restore_database(pgBackup *backup);
static void restore_archive_logs(pgBackup *backup);
static void create_recovery_conf(const char *target_time,
								 const char *target_xid,
								 const char *target_inclusive,
								 TimeLineID target_tli);
static parray * readTimeLineHistory(TimeLineID targetTLI);
static bool satisfy_timeline(const parray *timelines, const pgBackup *backup);
static TimeLineID get_current_timeline(void);
static TimeLineID get_fullbackup_timeline(parray *backups);
static void print_backup_id(const pgBackup *backup);
static void search_next_wal(const char *path, uint32 *needId, uint32 *needSeg, parray *timelines);

int
do_restore(const char *target_time,
		   const char *target_xid,
		   const char *target_inclusive,
		   TimeLineID target_tli)
{
	int i;
	int base_index;				/* index of base (full) backup */
	int last_restored_index;	/* index of last restored database backup */
	int ret;
	TimeLineID	cur_tli;
	TimeLineID	backup_tli;
	parray *backups;
	pgBackup *base_backup = NULL;
	parray *files;
	parray *timelines;
	char timeline_dir[MAXPGPATH];
	uint32 needId = 0;
	uint32 needSeg = 0;

	/* PGDATA and ARCLOG_PATH are always required */
	if (pgdata == NULL)
		elog(ERROR_ARGS,
			_("required parameter not specified: PGDATA (-D, --pgdata)"));
	if (arclog_path == NULL)
		elog(ERROR_ARGS,
			_("required parameter not specified: ARCLOG_PATH (-A, --arclog-path)"));
	if (srvlog_path == NULL)
		elog(ERROR_ARGS,
			_("required parameter not specified: SRVLOG_PATH (-S, --srvlog-path)"));

	if (verbose)
	{
		printf(_("========================================\n"));
		printf(_("restore start\n"));
	}

	/* get exclusive lock of backup catalog */
	ret = catalog_lock();
	if (ret == -1)
		elog(ERROR_SYSTEM, _("can't lock backup catalog."));
	else if (ret == 1)
		elog(ERROR_ALREADY_RUNNING,
			_("another pg_rman is running, stop restore."));

	/* confirm the PostgreSQL server is not running */
	if (is_pg_running())
		elog(ERROR_PG_RUNNING, _("PostgreSQL server is running"));

	/* get list of backups. (index == 0) is the last backup */
	backups = catalog_get_backup_list(NULL);

	cur_tli = get_current_timeline();
	backup_tli = get_fullbackup_timeline(backups);

	/* determine target timeline */
	if (target_tli == 0)
		target_tli = cur_tli != 0 ? cur_tli : backup_tli;

	if (verbose)
	{
		printf(_("current timeline ID = %u\n"), cur_tli);
		printf(_("latest full backup timeline ID = %u\n"), backup_tli);
		printf(_("target timeline ID = %u\n"), target_tli);
	}

	/* backup online WAL and serverlog */
	backup_online_files(cur_tli != 0 && cur_tli != backup_tli);

	/*
	 * Clear restore destination, but don't remove $PGDATA.
	 * To remove symbolic link, get file list with "omit_symlink = false".
	 */
	if (!check)
	{
		if (verbose)
		{
			printf(_("----------------------------------------\n"));
			printf(_("clearing restore destination\n"));
		}
		files = parray_new();
		dir_list_file(files, pgdata, NULL, false, false);
		parray_qsort(files, pgFileComparePathDesc);	/* delete from leaf */

		for (i = 0; i < parray_num(files); i++)
		{
			pgFile *file = (pgFile *) parray_get(files, i);
			pgFileDelete(file);
		}
		parray_walk(files, pgFileFree);
		parray_free(files);
	}

	/*
	 * restore timeline history files and get timeline branches can reach
	 * recovery target point.
	 */
	join_path_components(timeline_dir, backup_path, TIMELINE_HISTORY_DIR);
	if (verbose && !check)
		printf(_("restoring timeline history files\n"));
	dir_copy_files(timeline_dir, arclog_path);
	timelines = readTimeLineHistory(target_tli);

	/* find last full backup which can be used as base backup. */
	if (verbose)
		printf(_("searching recent full backup\n"));
	for (i = 0; i < parray_num(backups); i++)
	{
		base_backup = (pgBackup *) parray_get(backups, i);

		if (base_backup->backup_mode < BACKUP_MODE_FULL ||
			base_backup->status != BACKUP_STATUS_OK)
			continue;

#ifndef HAVE_LIBZ
		/* Make sure we won't need decompression we haven't got */
		if (base_backup->compress_data &&
			(HAVE_DATABASE(base_backup) || HAVE_ARCLOG(base_backup)))
		{
			elog(EXIT_NOT_SUPPORTED,
				_("can't restore from compressed backup (compression not supported in this installation)"));
		}
#endif
		if (satisfy_timeline(timelines, base_backup))
			goto base_backup_found;
	}
	/* no full backup found, can't restore */
	elog(ERROR_NO_BACKUP, _("no full backup found, can't restore."));

base_backup_found:
	base_index = i;

	if (verbose)
		print_backup_id(base_backup);

	/* restore base backup */
	restore_database(base_backup);
	last_restored_index = base_index;

	/* restore following incremental backup */
	if (verbose)
		printf(_("searching incremental backup...\n"));
	for (i = base_index - 1; i >= 0; i--)
	{
		pgBackup *backup = (pgBackup *) parray_get(backups, i);

		/* don't use incomplete nor different timeline backup */
		if (backup->status != BACKUP_STATUS_OK ||
					backup->tli != base_backup->tli)
			continue;

		/* use database backup only */
		if (backup->backup_mode < BACKUP_MODE_INCREMENTAL)
			continue;

		/* is the backup is necessary for restore to target timeline ? */
		if (!satisfy_timeline(timelines, backup))
			continue;

		if (verbose)
			print_backup_id(backup);

		restore_database(backup);
		last_restored_index = i;
	}

	/*
	 * Restore archived WAL which backed up with or after last restored backup.
	 * We don't check the backup->tli because a backup of arhived WAL
	 * can contain WALs which were archived in multiple timeline.
	 */
	if (verbose)
		printf(_("searching backed-up WAL...\n"));

	if (check)
	{
		pgBackup *backup = (pgBackup *) parray_get(backups, last_restored_index);
		/* XLByteToSeg(xlrp, logId, logSeg) */
		needId = backup->start_lsn.xlogid;
		needSeg = backup->start_lsn.xrecoff / XLogSegSize;
	}

	for (i = last_restored_index; i >= 0; i--)
	{
		pgBackup *backup = (pgBackup *) parray_get(backups, i);

		/* don't use incomplete backup */
		if (backup->status != BACKUP_STATUS_OK)
			continue;

		if (!HAVE_ARCLOG(backup))
			continue;

		/* care timeline junction */
		if (!satisfy_timeline(timelines, backup))
			continue;

		restore_archive_logs(backup);

		if (check)
		{
			char	xlogpath[MAXPGPATH];

			pgBackupGetPath(backup, xlogpath, lengthof(xlogpath), ARCLOG_DIR);
			search_next_wal(xlogpath, &needId, &needSeg, timelines);
		}
	}

	/* copy online WAL backup to $PGDATA/pg_xlog */
	restore_online_files();

	if (check)
	{
		char	xlogpath[MAXPGPATH];
		if (verbose)
			printf(_("searching archived WAL...\n"));

		search_next_wal(arclog_path, &needId, &needSeg, timelines);

		if (verbose)
			printf(_("searching online WAL...\n"));

		join_path_components(xlogpath, pgdata, PG_XLOG_DIR);
		search_next_wal(xlogpath, &needId, &needSeg, timelines);

		if (verbose)
			printf(_("all necessary files are found.\n"));
	}

	/* create recovery.conf */
	create_recovery_conf(target_time, target_xid, target_inclusive, target_tli);

	/* release catalog lock */
	catalog_unlock();

	/* cleanup */
	parray_walk(backups, pgBackupFree);
	parray_free(backups);

	/* print restore complete message */
	if (verbose && !check)
	{
		printf(_("all restore completed\n"));
		printf(_("========================================\n"));
	}
	if (!check)
		elog(INFO, _("restore complete. Recovery starts automatically when the PostgreSQL server is started."));

	return 0;
}	

/*
 * Validate and restore backup.
 */
void
restore_database(pgBackup *backup)
{
	char	timestamp[100];
	char	path[MAXPGPATH];
	char	list_path[MAXPGPATH];
	int		ret;
	parray *files;
	int		i;

	/* confirm block size compatibility */
	if (backup->block_size != BLCKSZ)
		elog(ERROR_PG_INCOMPATIBLE,
			_("BLCKSZ(%d) is not compatible(%d expected)"),
			backup->block_size, BLCKSZ);
	if (backup->wal_block_size != XLOG_BLCKSZ)
		elog(ERROR_PG_INCOMPATIBLE,
			_("XLOG_BLCKSZ(%d) is not compatible(%d expected)"),
			backup->wal_block_size, XLOG_BLCKSZ);

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	if (verbose && !check)
	{
		printf(_("----------------------------------------\n"));
		printf(_("restoring database from backup %s.\n"), timestamp);
	}

	/*
	 * Validate backup files with its size, because load of CRC calculation is
	 * not light.
	 */
	pgBackupValidate(backup, true);

	/* make direcotries and symbolic links */
	pgBackupGetPath(backup, path, lengthof(path), MKDIRS_SH_FILE);
	if (!check)
	{
		char pwd[MAXPGPATH];

		/* keep orginal directory */
		if (getcwd(pwd, sizeof(pwd)) == NULL)
			elog(ERROR_SYSTEM, _("can't get current working directory: %s"),
				strerror(errno));

		/* create pgdata directory */
		dir_create_dir(pgdata, DIR_PERMISSION);

		/* change directory to pgdata */
		if (chdir(pgdata))
			elog(ERROR_SYSTEM, _("can't change directory: %s"),
				strerror(errno));

		/* Execute mkdirs.sh */
		ret = system(path);
		if (ret != 0)
			elog(ERROR_SYSTEM, _("can't execute mkdirs.sh: %s"),
				strerror(errno));

		/* go back to original directory */
		if (chdir(pwd))
			elog(ERROR_SYSTEM, _("can't change directory: %s"),
				strerror(errno));
	}

	/*
	 * get list of files which need to be restored.
	 */
	pgBackupGetPath(backup, path, lengthof(path), DATABASE_DIR);
	pgBackupGetPath(backup, list_path, lengthof(list_path), DATABASE_FILE_LIST);
	files = dir_read_file_list(path, list_path);
	for (i = parray_num(files) - 1; i >= 0; i--)
	{
		pgFile *file = (pgFile *) parray_get(files, i);

		/* remove files which are not backed up */
		if (file->write_size == BYTES_INVALID)
			pgFileFree(parray_remove(files, i));
	}

	/* restore files into $PGDATA */
	for (i = 0; i < parray_num(files); i++)
	{
		char from_root[MAXPGPATH];
		pgFile *file = (pgFile *) parray_get(files, i);

		pgBackupGetPath(backup, from_root, lengthof(from_root), DATABASE_DIR);

		/* check for interrupt */
		if (interrupted)
			elog(ERROR_INTERRUPTED, _("interrupted during restore database"));

		/* print progress */
		if (verbose && !check)
			printf(_("(%d/%lu) %s "), i + 1, (unsigned long) parray_num(files),
				file->path + strlen(from_root) + 1);

		/* directories are created with mkdirs.sh */
		if (S_ISDIR(file->mode))
		{
			if (verbose && !check)
				printf(_("directory, skip\n"));
			continue;
		}

		/* not backed up */
		if (file->write_size == BYTES_INVALID)
		{
			if (verbose && !check)
				printf(_("not backed up, skip\n"));
			continue;
		}

		/* restore file */
		if (!check)
			restore_data_file(from_root, pgdata, file, backup->compress_data);

		/* print size of restored file */
		if (verbose && !check)
			printf(_("restored %lu\n"), (unsigned long) file->write_size);
	}

	/* Delete files which are not in file list. */
	if (!check)
	{
		parray *files_now;

		parray_walk(files, pgFileFree);
		parray_free(files);

		/* re-read file list to change base path to $PGDATA */
		files = dir_read_file_list(pgdata, list_path);
		parray_qsort(files, pgFileComparePathDesc);

		/* get list of files restored to pgdata */
		files_now = parray_new();
		dir_list_file(files_now, pgdata, pgdata_exclude, true, false);
		/* to delete from leaf, sort in reversed order */
		parray_qsort(files_now, pgFileComparePathDesc);

		for (i = 0; i < parray_num(files_now); i++)
		{
			pgFile *file = (pgFile *) parray_get(files_now, i);

			/* If the file is not in the file list, delete it */
			if (parray_bsearch(files, file, pgFileComparePathDesc) == NULL)
			{
				if (verbose)
					printf(_("  delete %s\n"), file->path + strlen(pgdata) + 1);
				pgFileDelete(file);
			}
		}

		parray_walk(files_now, pgFileFree);
		parray_free(files_now);
	}

	/* remove postmaster.pid */
	snprintf(path, lengthof(path), "%s/postmaster.pid", pgdata);
	if (remove(path) == -1 && errno != ENOENT)
		elog(ERROR_SYSTEM, _("can't remove postmaster.pid: %s"),
			strerror(errno));

	/* cleanup */
	parray_walk(files, pgFileFree);
	parray_free(files);

	if (verbose && !check)
		printf(_("resotre backup completed\n"));
}

/*
 * Restore archived WAL by creating symbolic link which linked to backup WAL in
 * archive directory.
 */
void
restore_archive_logs(pgBackup *backup)
{
	int i;
	char timestamp[100];
	parray *files;
	char path[MAXPGPATH];
	char list_path[MAXPGPATH];
	char base_path[MAXPGPATH];

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	if (verbose && !check)
	{
		printf(_("----------------------------------------\n"));
		printf(_("restoring WAL from backup %s.\n"), timestamp);
	}

	pgBackupGetPath(backup, list_path, lengthof(list_path), ARCLOG_FILE_LIST);
	pgBackupGetPath(backup, base_path, lengthof(list_path), ARCLOG_DIR);
	files = dir_read_file_list(base_path, list_path);
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);

		/* check for interrupt */
		if (interrupted)
			elog(ERROR_INTERRUPTED, _("interrupted during restore WAL"));

		/* print progress */
		join_path_components(path, arclog_path, file->path + strlen(base_path) + 1);
		if (verbose && !check)
			printf(_("(%d/%lu) %s "), i + 1, (unsigned long) parray_num(files),
				file->path + strlen(base_path) + 1);

		/* skip files which are not in backup */
		if (file->write_size == BYTES_INVALID)
		{
			if (verbose && !check)
				printf(_("skip(not backed up)\n"));
			continue;
		}

		/*
		 * skip timeline history files because timeline history files will be
		 * restored from $BACKUP_PATH/timeline_history.
		 */
		if (strstr(file->path, ".history") ==
				file->path + strlen(file->path) - strlen(".history"))
		{
			if (verbose && !check)
				printf(_("skip(timeline history)\n"));
			continue;
		}

		if (!check)
		{
			if (backup->compress_data)
			{
				copy_file(base_path, arclog_path, file, DECOMPRESSION);
				if (verbose)
					printf(_("decompressed\n"));

				continue;
			}

			/* even same file exist, use backup file */
			if ((remove(path) == -1) && errno != ENOENT)
				elog(ERROR_SYSTEM, _("can't remove file \"%s\": %s"), path,
					strerror(errno));

			if ((symlink(file->path, path) == -1))
				elog(ERROR_SYSTEM, _("can't create link to \"%s\": %s"),
					file->path, strerror(errno));

			if (verbose)
				printf(_("linked\n"));
		}
	}

	parray_walk(files, pgFileFree);
	parray_free(files);
}

static void
create_recovery_conf(const char *target_time,
					 const char *target_xid,
					 const char *target_inclusive,
					 TimeLineID target_tli)
{
	char path[MAXPGPATH];
	FILE *fp;

	if (verbose && !check)
	{
		printf(_("----------------------------------------\n"));
		printf(_("creating recovery.conf\n"));
	}

	if (!check)
	{
		snprintf(path, lengthof(path), "%s/recovery.conf", pgdata);
		fp = fopen(path, "wt");
		if (fp == NULL)
			elog(ERROR_SYSTEM, _("can't open recovery.conf \"%s\": %s"), path,
				strerror(errno));

		fprintf(fp, "# recovery.conf generated by pg_rman %s\n",
			PROGRAM_VERSION);
		fprintf(fp, "restore_command = 'cp %s/%%f %%p'\n", arclog_path);
		if (target_time)
			fprintf(fp, "recovery_target_time = '%s'\n", target_time);
		if (target_xid)
			fprintf(fp, "recovery_target_xid = '%s'\n", target_xid);
		if (target_inclusive)
			fprintf(fp, "recovery_target_inclusive = '%s'\n", target_inclusive);
		fprintf(fp, "recovery_target_timeline = '%u'\n", target_tli);

		fclose(fp);
	}
}

static void
backup_online_files(bool re_recovery)
{
	char work_path[MAXPGPATH];
	char pg_xlog_path[MAXPGPATH];
	bool files_exist;
	parray *files;

	if (verbose && !check)
	{
		printf(_("----------------------------------------\n"));
		printf(_("backup online WAL and serverlog start\n"));
	}

	/* get list of files in $BACKUP_PATH/backup/pg_xlog */
	files = parray_new();
	snprintf(work_path, lengthof(work_path), "%s/%s/%s", backup_path,
		RESTORE_WORK_DIR, PG_XLOG_DIR);
	dir_list_file(files, work_path, NULL, true, false);

	files_exist = parray_num(files) > 0;

	parray_walk(files, pgFileFree);
	parray_free(files);

	/* If files exist in RESTORE_WORK_DIR and not re-recovery, use them. */
	if (files_exist && !re_recovery)
	{
		if (verbose)
			printf(_("online WALs have been already backed up, use them.\n"));

		return;
	}

	/* backup online WAL */
	snprintf(pg_xlog_path, lengthof(pg_xlog_path), "%s/pg_xlog", pgdata);
	snprintf(work_path, lengthof(work_path), "%s/%s/%s", backup_path,
		RESTORE_WORK_DIR, PG_XLOG_DIR);
	dir_create_dir(work_path, DIR_PERMISSION);
	dir_copy_files(pg_xlog_path, work_path);

	/* backup serverlog */
	snprintf(work_path, lengthof(work_path), "%s/%s/%s", backup_path,
		RESTORE_WORK_DIR, SRVLOG_DIR);
	dir_create_dir(work_path, DIR_PERMISSION);
	dir_copy_files(srvlog_path, work_path);
}

static void
restore_online_files(void)
{
	int		i;
	char	root_backup[MAXPGPATH];
	parray *files_backup;

	/* get list of files in $BACKUP_PATH/backup/pg_xlog */
	files_backup = parray_new();
	snprintf(root_backup, lengthof(root_backup), "%s/%s/%s", backup_path,
		RESTORE_WORK_DIR, PG_XLOG_DIR);
	dir_list_file(files_backup, root_backup, NULL, true, false);

	if (verbose && !check)
	{
		printf(_("----------------------------------------\n"));
		printf(_("restoring online WAL\n"));
	}

	/* restore online WAL */
	for (i = 0; i < parray_num(files_backup); i++)
	{
		pgFile *file = (pgFile *) parray_get(files_backup, i);

		if (S_ISDIR(file->mode))
		{
			char to_path[MAXPGPATH];
			snprintf(to_path, lengthof(to_path), "%s/%s/%s", pgdata,
				PG_XLOG_DIR, file->path + strlen(root_backup) + 1);
			if (verbose && !check)
				printf(_("create directory \"%s\"\n"),
					file->path + strlen(root_backup) + 1);
			if (!check)
				dir_create_dir(to_path, DIR_PERMISSION);
			continue;
		}
		else if(S_ISREG(file->mode))
		{
			char to_root[MAXPGPATH];
			join_path_components(to_root, pgdata, PG_XLOG_DIR);
			if (verbose && !check)
				printf(_("restore \"%s\"\n"),
					file->path + strlen(root_backup) + 1);
			if (!check)
				copy_file(root_backup, to_root, file, NO_COMPRESSION);
		}
	}

	/* cleanup */
	parray_walk(files_backup, pgFileFree);
	parray_free(files_backup);
}

/*
 * Try to read a timeline's history file.
 *
 * If successful, return the list of component pgTimeLine (the ancestor
 * timelines followed by target timeline).	If we can't find the history file,
 * assume that the timeline has no parents, and return a list of just the
 * specified timeline ID.
 * based on readTimeLineHistory() in xlog.c
 */
static parray *
readTimeLineHistory(TimeLineID targetTLI)
{
	parray	   *result;
	char		path[MAXPGPATH];
	char		fline[MAXPGPATH];
	FILE	   *fd;
	pgTimeLine *timeline;
	pgTimeLine *last_timeline = NULL;

	result = parray_new();

	/* search from arclog_path first */
	snprintf(path, lengthof(path), "%s/%08X.history", arclog_path,
		targetTLI);
	fd = fopen(path, "rt");
	if (fd == NULL)
	{
		if (errno != ENOENT)
			elog(ERROR_SYSTEM, _("could not open file \"%s\": %s"), path,
				strerror(errno));

		/* search from restore work directory next */
		snprintf(path, lengthof(path), "%s/%s/%s/%08X.history", backup_path,
			RESTORE_WORK_DIR, PG_XLOG_DIR, targetTLI);
		fd = fopen(path, "rt");
		if (fd == NULL)
		{
			if (errno != ENOENT)
				elog(ERROR_SYSTEM, _("could not open file \"%s\": %s"), path,
						strerror(errno));
		}
	}

	/*
	 * Parse the file...
	 */
	while (fd && fgets(fline, sizeof(fline), fd) != NULL)
	{
		/* skip leading whitespaces and check for # comment */
		char	   *ptr;
		char	   *endptr;

		for (ptr = fline; *ptr; ptr++)
		{
			if (!IsSpace(*ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			continue;

		timeline = pgut_new(pgTimeLine);
		timeline->tli = 0;
		timeline->end.xlogid = 0;
		timeline->end.xrecoff = 0;

		/* expect a numeric timeline ID as first field of line */
		timeline->tli = (TimeLineID) strtoul(ptr, &endptr, 0);
		if (endptr == ptr)
			elog(ERROR_CORRUPTED,
					_("syntax error(timeline ID) in history file: %s"),
					fline);

		if (last_timeline && timeline->tli <= last_timeline->tli)
			elog(ERROR_CORRUPTED,
				   _("Timeline IDs must be in increasing sequence."));

		/* Build list with newest item first */
		parray_insert(result, 0, timeline);
		last_timeline = timeline;

		/* parse end point(logfname, xid) in the timeline */
		for (ptr = endptr; *ptr; ptr++)
		{
			if (!IsSpace(*ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			elog(ERROR_CORRUPTED,
			   _("End logfile must follow Timeline ID."));

		if (!xlog_logfname2lsn(ptr, &timeline->end))
			elog(ERROR_CORRUPTED,
					_("syntax error(endfname) in history file: %s"), fline);
		/* we ignore the remainder of each line */
	}

	if (fd)
		fclose(fd);

	if (last_timeline && targetTLI <= last_timeline->tli)
		elog(ERROR_CORRUPTED,
			_("Timeline IDs must be less than child timeline's ID."));

	/* append target timeline */
	timeline = pgut_new(pgTimeLine);
	timeline->tli = targetTLI;
	timeline->end.xlogid = (uint32) -1; /* lsn in target timelie is valid */
	timeline->end.xrecoff = (uint32) -1; /* lsn target timelie is valid */
	parray_insert(result, 0, timeline);

	/* dump timeline branches for debug */
	if (debug)
	{
		int i;
		for (i = 0; i < parray_num(result); i++)
		{
			pgTimeLine *timeline = parray_get(result, i);
			elog(LOG, "%s() result[%d]: %08X/%08X/%08X", __FUNCTION__, i,
				timeline->tli, timeline->end.xlogid, timeline->end.xrecoff);
		}
	}

	return result;
}

static bool
satisfy_timeline(const parray *timelines, const pgBackup *backup)
{
	int i;
	for (i = 0; i < parray_num(timelines); i++)
	{
		pgTimeLine *timeline = (pgTimeLine *) parray_get(timelines, i);
		if (backup->tli == timeline->tli &&
				XLByteLT(backup->stop_lsn, timeline->end))
			return true;
	}
	return false;
}

/* get TLI of the current database */
static TimeLineID
get_current_timeline(void)
{
	ControlFileData ControlFile;
	int			fd;
	char		ControlFilePath[MAXPGPATH];
	pg_crc32	crc;
	TimeLineID	ret;

	snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", pgdata);

	if ((fd = open(ControlFilePath, O_RDONLY | PG_BINARY, 0)) == -1)
	{
		elog(WARNING, _("can't open pg_controldata file \"%s\": %s"),
			ControlFilePath, strerror(errno));
		return 0;
	}

	if (read(fd, &ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
	{
		elog(WARNING, _("can't read pg_controldata file \"%s\": %s"),
			ControlFilePath, strerror(errno));
		return 0;
	}
	close(fd);

	/* Check the CRC. */
	INIT_CRC32(crc);
	COMP_CRC32(crc,
		   	(char *) &ControlFile,
		   	offsetof(ControlFileData, crc));
	FIN_CRC32(crc);

	if (!EQ_CRC32(crc, ControlFile.crc))
	{
		elog(WARNING, _("Calculated CRC checksum does not match value stored in file.\n"
			"Either the file is corrupt, or it has a different layout than this program\n"
			"is expecting.  The results below are untrustworthy.\n"));
		return 0;
	}

	if (ControlFile.pg_control_version % 65536 == 0 && ControlFile.pg_control_version / 65536 != 0)
	{
		elog(WARNING, _("possible byte ordering mismatch\n"
			"The byte ordering used to store the pg_control file might not match the one\n"
			"used by this program.  In that case the results below would be incorrect, and\n"
			"the PostgreSQL installation would be incompatible with this data directory.\n"));
		return 0;
	}

	ret = ControlFile.checkPointCopy.ThisTimeLineID;

	return ret;
}

/* get TLI of the latest full backup */
static TimeLineID
get_fullbackup_timeline(parray *backups)
{
	int			i;
	pgBackup   *base_backup = NULL;
	TimeLineID	ret;

	for (i = 0; i < parray_num(backups); i++)
	{
		base_backup = (pgBackup *) parray_get(backups, i);

		if (base_backup->backup_mode >= BACKUP_MODE_FULL)
		{
			/*
			 * Validate backup files with its size, because load of CRC
			 * calculation is not light.
			 */
			if (base_backup->status == BACKUP_STATUS_DONE)
				pgBackupValidate(base_backup, true);

			if (base_backup->status == BACKUP_STATUS_OK)
				break;
		}
	}
	/* no full backup found, can't restore */
	if (i == parray_num(backups))
		elog(ERROR_NO_BACKUP, _("no full backup found, can't restore."));

	ret = base_backup->tli;

	return ret;
}

static void
print_backup_id(const pgBackup *backup)
{
	char timestamp[100];
	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	printf(_("  %s (%X/%08X)\n"), timestamp, backup->stop_lsn.xlogid,
		backup->stop_lsn.xrecoff);
}

static void
search_next_wal(const char *path, uint32 *needId, uint32 *needSeg, parray *timelines)
{
	int		i;
	int		j;
	int		count;
	char	xlogfname[MAXFNAMELEN];
	char	pre_xlogfname[MAXFNAMELEN];
	char	xlogpath[MAXPGPATH];
	struct stat	st;

	count = 0;
	for (;;)
	{
		for (i = 0; i < parray_num(timelines); i++)
		{
			pgTimeLine *timeline = (pgTimeLine *) parray_get(timelines, i);

			XLogFileName(xlogfname, timeline->tli, *needId, *needSeg);
			join_path_components(xlogpath, path, xlogfname);

			if (stat(xlogpath, &st) == 0)
				break;
		}

		/* not found */
		if (i == parray_num(timelines))
		{
			if (count == 1)
				printf(_("\n"));
			else if (count > 1)
				printf(_(" - %s\n"), pre_xlogfname);

			return;
		}

		count++;
		if (count == 1)
			printf(_("%s"), xlogfname);

		strcpy(pre_xlogfname, xlogfname);

		/* delete old TLI */
		for (j = i + 1; j < parray_num(timelines); j++)
			parray_remove(timelines, i + 1);
		/* XXX: should we add a linebreak when we find a timeline? */

		NextLogSeg(*needId, *needSeg);
	}
}
