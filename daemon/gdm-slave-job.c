/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>

#ifdef WITH_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-object.h>

#include "gdm-common.h"

#include "gdm-slave-job.h"

#define GDM_SLAVE_JOB_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GDM_TYPE_SLAVE_JOB, GdmSlaveJobPrivate))

#define MAX_LOGS 5

struct GdmSlaveJobPrivate
{
        char    *command;
        char    *log_path;
        GPid     pid;
        guint    child_watch_id;
};

enum {
        PROP_0,
        PROP_COMMAND,
        PROP_LOG_PATH,
};

enum {
        EXITED,
        DIED,
        LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0, };

static void     gdm_slave_job_class_init      (GdmSlaveJobClass *klass);
static void     gdm_slave_job_init            (GdmSlaveJob      *slave);
static void     gdm_slave_job_finalize        (GObject            *object);

G_DEFINE_TYPE (GdmSlaveJob, gdm_slave_job, G_TYPE_OBJECT)

static void
child_watch (GPid           pid,
             int            status,
             GdmSlaveJob *slave)
{
        g_debug ("GdmSlaveJob: slave (pid:%d) done (%s:%d)",
                 (int) pid,
                 WIFEXITED (status) ? "status"
                 : WIFSIGNALED (status) ? "signal"
                 : "unknown",
                 WIFEXITED (status) ? WEXITSTATUS (status)
                 : WIFSIGNALED (status) ? WTERMSIG (status)
                 : -1);

        g_spawn_close_pid (slave->priv->pid);
        slave->priv->pid = -1;
        slave->priv->child_watch_id = 0;

        if (WIFEXITED (status)) {
                int code = WEXITSTATUS (status);
                g_signal_emit (slave, signals [EXITED], 0, code);
        } else if (WIFSIGNALED (status)) {
                int num = WTERMSIG (status);
                g_signal_emit (slave, signals [DIED], 0, num);
        }
}

static void
rotate_logs (const char *path,
             guint       n_copies)
{
        int i;

        for (i = n_copies - 1; i > 0; i--) {
                char *name_n;
                char *name_n1;

                name_n = g_strdup_printf ("%s.%d", path, i);
                if (i > 1) {
                        name_n1 = g_strdup_printf ("%s.%d", path, i - 1);
                } else {
                        name_n1 = g_strdup (path);
                }

                VE_IGNORE_EINTR (g_unlink (name_n));
                VE_IGNORE_EINTR (g_rename (name_n1, name_n));

                g_free (name_n1);
                g_free (name_n);
        }

        VE_IGNORE_EINTR (g_unlink (path));
}

typedef struct {
        const char *identifier;
        const char *log_file;
} SpawnChildData;

static void
spawn_child_setup (SpawnChildData *data)
{
#ifdef WITH_SYSTEMD
        if (sd_booted () > 0) {
                return;
        }
#endif

        if (data->log_file != NULL) {
                int logfd;

                rotate_logs (data->log_file, MAX_LOGS);

                VE_IGNORE_EINTR (g_unlink (data->log_file));
                VE_IGNORE_EINTR (logfd = open (data->log_file, O_CREAT|O_APPEND|O_TRUNC|O_WRONLY|O_EXCL, 0644));

                if (logfd != -1) {
                        VE_IGNORE_EINTR (dup2 (logfd, 1));
                        VE_IGNORE_EINTR (dup2 (logfd, 2));
                        close (logfd);
                }
        }
}

static gboolean
spawn_command_line_async (const char *command_line,
                          const char *log_file,
                          char      **env,
                          GPid       *child_pid,
                          GError    **error)
{
        char           **argv;
        GError          *local_error;
        gboolean         ret;
        gboolean         res;
        SpawnChildData   data;
        gboolean         has_journald = FALSE;

        ret = FALSE;

        argv = NULL;
        local_error = NULL;
        if (! g_shell_parse_argv (command_line, NULL, &argv, &local_error)) {
                g_warning ("Could not parse command: %s", local_error->message);
                g_propagate_error (error, local_error);
                goto out;
        }

        data.identifier = argv[0];

#ifdef WITH_SYSTEMD
        if (sd_booted () > 0) {
                has_journald = TRUE;
        }
#endif

        if (has_journald) {
                data.log_file = NULL;
        } else {
                data.log_file = log_file;
        }

        local_error = NULL;
        res = g_spawn_async (NULL,
                             argv,
                             env,
                             G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                             (GSpawnChildSetupFunc)spawn_child_setup,
                             &data,
                             child_pid,
                             &local_error);

        if (! res) {
                g_warning ("Could not spawn command: %s", local_error->message);
                g_propagate_error (error, local_error);
                goto out;
        }

        ret = TRUE;
 out:
        g_strfreev (argv);

        return ret;
}

static void
clear_child_watch (GdmSlaveJob *slave)
{
        slave->priv->child_watch_id = 0;
        g_object_unref (slave);
}

static gboolean
spawn_slave (GdmSlaveJob *slave)
{
        gboolean    result;
        GError     *error;

        g_debug ("GdmSlaveJob: Running command: %s", slave->priv->command);

        error = NULL;
        result = spawn_command_line_async (slave->priv->command,
                                           slave->priv->log_path,
                                           NULL,
                                           &slave->priv->pid,
                                           &error);
        if (! result) {
                g_warning ("Could not start command '%s': %s", slave->priv->command, error->message);
                g_error_free (error);
                goto out;
        }

        g_debug ("GdmSlaveJob: Started slave with pid %d", slave->priv->pid);

        slave->priv->child_watch_id = g_child_watch_add_full (G_PRIORITY_DEFAULT,
                                                              slave->priv->pid,
                                                              (GChildWatchFunc)child_watch,
                                                              g_object_ref (slave),
                                                              (GDestroyNotify)
                                                              clear_child_watch);

        result = TRUE;

 out:

        return result;
}

static void
kill_slave (GdmSlaveJob *slave)
{
        int res;

        if (slave->priv->pid <= 1) {
                return;
        }

        res = gdm_signal_pid (slave->priv->pid, SIGTERM);
        if (res < 0) {
                g_warning ("Unable to kill slave process");
        } else {
                int exit_status;

                exit_status = gdm_wait_on_pid (slave->priv->pid);

                g_debug ("GdmSlaveJob: slave died with exit status %d", exit_status);

                g_spawn_close_pid (slave->priv->pid);
                slave->priv->pid = 0;
        }
}

gboolean
gdm_slave_job_start (GdmSlaveJob *slave)
{
        gdm_slave_job_stop (slave);

        spawn_slave (slave);

        return TRUE;
}

gboolean
gdm_slave_job_stop (GdmSlaveJob *slave)
{
        g_debug ("GdmSlaveJob: Killing slave");

        if (slave->priv->child_watch_id > 0) {
                g_source_remove (slave->priv->child_watch_id);
                slave->priv->child_watch_id = 0;
        }

        kill_slave (slave);

        return TRUE;
}

void
gdm_slave_job_set_command (GdmSlaveJob *slave,
                             const char    *command)
{
        g_free (slave->priv->command);
        slave->priv->command = g_strdup (command);
}

void
gdm_slave_job_set_log_path (GdmSlaveJob *slave,
                              const char    *path)
{
        g_free (slave->priv->log_path);
        slave->priv->log_path = g_strdup (path);
}

static void
gdm_slave_job_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
        GdmSlaveJob *self;

        self = GDM_SLAVE_JOB (object);

        switch (prop_id) {
        case PROP_COMMAND:
                gdm_slave_job_set_command (self, g_value_get_string (value));
                break;
        case PROP_LOG_PATH:
                gdm_slave_job_set_log_path (self, g_value_get_string (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gdm_slave_job_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
        GdmSlaveJob *self;

        self = GDM_SLAVE_JOB (object);

        switch (prop_id) {
        case PROP_COMMAND:
                g_value_set_string (value, self->priv->command);
                break;
        case PROP_LOG_PATH:
                g_value_set_string (value, self->priv->log_path);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gdm_slave_job_dispose (GObject *object)
{
        GdmSlaveJob *slave;

        slave = GDM_SLAVE_JOB (object);

        g_debug ("GdmSlaveJob: Disposing slave job");
        gdm_slave_job_stop (slave);

        G_OBJECT_CLASS (gdm_slave_job_parent_class)->dispose (object);
}

static void
gdm_slave_job_class_init (GdmSlaveJobClass *klass)
{
        GObjectClass    *object_class = G_OBJECT_CLASS (klass);

        object_class->get_property = gdm_slave_job_get_property;
        object_class->set_property = gdm_slave_job_set_property;
        object_class->dispose = gdm_slave_job_dispose;
        object_class->finalize = gdm_slave_job_finalize;

        g_type_class_add_private (klass, sizeof (GdmSlaveJobPrivate));

        g_object_class_install_property (object_class,
                                         PROP_COMMAND,
                                         g_param_spec_string ("command",
                                                              "command",
                                                              "command",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_LOG_PATH,
                                         g_param_spec_string ("log-path",
                                                              "log path",
                                                              "log path",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

        signals [EXITED] =
                g_signal_new ("exited",
                              G_OBJECT_CLASS_TYPE (object_class),
                              G_SIGNAL_RUN_FIRST,
                              G_STRUCT_OFFSET (GdmSlaveJobClass, exited),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__INT,
                              G_TYPE_NONE,
                              1,
                              G_TYPE_INT);

        signals [DIED] =
                g_signal_new ("died",
                              G_OBJECT_CLASS_TYPE (object_class),
                              G_SIGNAL_RUN_FIRST,
                              G_STRUCT_OFFSET (GdmSlaveJobClass, died),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__INT,
                              G_TYPE_NONE,
                              1,
                              G_TYPE_INT);
}

static void
gdm_slave_job_init (GdmSlaveJob *slave)
{

        slave->priv = GDM_SLAVE_JOB_GET_PRIVATE (slave);

        slave->priv->pid = -1;
}

static void
gdm_slave_job_finalize (GObject *object)
{
        GdmSlaveJob *slave;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GDM_IS_SLAVE_JOB (object));

        slave = GDM_SLAVE_JOB (object);

        g_return_if_fail (slave->priv != NULL);

        g_free (slave->priv->command);
        g_free (slave->priv->log_path);

        G_OBJECT_CLASS (gdm_slave_job_parent_class)->finalize (object);
}

GdmSlaveJob *
gdm_slave_job_new (void)
{
        GObject *object;

        object = g_object_new (GDM_TYPE_SLAVE_JOB,
                               NULL);

        return GDM_SLAVE_JOB (object);
}
