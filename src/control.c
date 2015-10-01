/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU Affero General Public License in all respects
 * for all of the code used other than OpenSSL.
 */

#include "config.h"

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "monit.h"
#include "net.h"
#include "socket.h"
#include "event.h"
#include "system/Time.h"
#include "exceptions/AssertException.h"


/**
 *  Methods for controlling services managed by monit.
 *
 *  @file
 */


/* ------------------------------------------------------------- Definitions */


typedef enum {
        Process_Stopped = 0,
        Process_Started
} __attribute__((__packed__)) Process_Status;


#define RETRY_INTERVAL 100000 // 100ms


/* ----------------------------------------------------------------- Private */


static int _getOutput(InputStream_T in, char *buf, int buflen) {
        InputStream_setTimeout(in, 0);
        return InputStream_readBytes(in, buf, buflen - 1);
}


static int _commandExecute(Service_T S, command_t c, char *msg, int msglen, long *timeout) {
        ASSERT(S);
        ASSERT(c);
        ASSERT(msg);
        msg[0] = 0;
        int status = -1;
        Command_T C = NULL;
        TRY
        {
                // May throw exception if the program doesn't exist (was removed while Monit was up)
                C = Command_new(c->arg[0], NULL);
        }
        ELSE
        {
                snprintf(msg, msglen, "Program %s failed: %s", c->arg[0], Exception_frame.message);
        }
        END_TRY;
        if (C) {
                for (int i = 1; i < c->length; i++)
                        Command_appendArgument(C, c->arg[i]);
                if (c->has_uid)
                        Command_setUid(C, c->uid);
                if (c->has_gid)
                        Command_setGid(C, c->gid);
                Command_setEnv(C, "MONIT_DATE", Time_string(Time_now(), (char[26]){}));
                Command_setEnv(C, "MONIT_SERVICE", S->name);
                Command_setEnv(C, "MONIT_HOST", Run.system->name);
                Command_setEnv(C, "MONIT_EVENT", c == S->start ? "Started" : c == S->stop ? "Stopped" : "Restarted");
                Command_setEnv(C, "MONIT_DESCRIPTION", c == S->start ? "Started" : c == S->stop ? "Stopped" : "Restarted");
                if (S->type == Service_Process) {
                        Command_vSetEnv(C, "MONIT_PROCESS_PID", "%d", Util_isProcessRunning(S, false));
                        Command_vSetEnv(C, "MONIT_PROCESS_MEMORY", "%ld", S->inf->priv.process.mem_kbyte);
                        Command_vSetEnv(C, "MONIT_PROCESS_CHILDREN", "%d", S->inf->priv.process.children);
                        Command_vSetEnv(C, "MONIT_PROCESS_CPU_PERCENT", "%d", S->inf->priv.process.cpu_percent);
                }
                Process_T P = Command_execute(C);
                Command_free(&C);
                if (P) {
                        do {
                                Time_usleep(RETRY_INTERVAL);
                                *timeout -= RETRY_INTERVAL;
                        } while ((status = Process_exitStatus(P)) < 0 && *timeout > 0 && ! (Run.flags & Run_Stopped));
                        if (*timeout <= 0)
                                snprintf(msg, msglen, "Program %s timed out", c->arg[0]);
                        int n, total = 0;
                        char buf[STRLEN];
                        do {
                                if ((n = _getOutput(Process_getErrorStream(P), buf, sizeof(buf))) <= 0)
                                        n = _getOutput(Process_getInputStream(P), buf, sizeof(buf));
                                if (n > 0) {
                                        buf[n] = 0;
                                        DEBUG("%s", buf);
                                        // Report the first message (override existing plain timeout message if some program output is available)
                                        if (! total)
                                                snprintf(msg, msglen, "%s: %s%s", c->arg[0], *timeout <= 0 ? "Program timed out -- " : "", buf);
                                        total += n;
                                }
                        } while (n > 0 && Run.debug && total < 2048); // Limit the debug output (if the program will have endless output, such as 'yes' utility, we have to stop at some point to not spin here forever)
                        Process_free(&P); // Will kill the program if still running
                }
        }
        return status;
}


static Process_Status _waitProcessStart(Service_T s, long *timeout) {
        long wait = 50000;
        do {
                if (Util_isProcessRunning(s, true))
                        return Process_Started;
                Time_usleep(wait);
                *timeout -= wait;
                wait = wait < 1000000 ? wait * 2 : 1000000; // double the wait during each cycle until 1s is reached (Util_isProcessRunning can be heavy and we don't want to drain power every 100ms on mobile devices)
        } while (*timeout > 0 && ! (Run.flags & Run_Stopped));
        return Process_Stopped;
}


static Process_Status _waitProcessStop(int pid, long *timeout) {
        do {
                if (! pid || (getpgid(pid) == -1 && errno != EPERM))
                        return Process_Stopped;
                Time_usleep(RETRY_INTERVAL);
                *timeout -= RETRY_INTERVAL;
        } while (*timeout > 0 && ! (Run.flags & Run_Stopped));
        return Process_Started;
}


static boolean_t _check(Service_T s) {
        ASSERT(s);
        boolean_t rv = true;
        // The check is performed in passive mode - we want to just check, nested start/stop/restart action is unwanted (alerts are allowed so the user will get feedback what's wrong)
        Monitor_Mode original = s->mode;
        s->mode = Monitor_Passive;
        rv = s->check(s);
        if (s->type == Service_Program) {
                // check program executes the program and needs to be called again to collect the exit value and evaluate the status
                int64_t timeout = s->program->timeout * 1000000;
                do {
                        Time_usleep(RETRY_INTERVAL);
                        timeout -= RETRY_INTERVAL;
                } while (Process_exitStatus(s->program->P) < 0 && timeout > 0LL && ! (Run.flags & Run_Stopped));
                rv = s->check(s);
        }
        s->mode = original;
        return rv;
}


/*
 * This is a post-fix recursive function for starting every service
 * that s depends on before starting s.
 * @param s A Service_T object
 * @return true if the service was stopped otherwise false
 */
static boolean_t _doStart(Service_T s) {
        ASSERT(s);
        boolean_t rv = true;
        StringBuffer_T sb = StringBuffer_create(64);
        if (s->dependantlist) {
                for (Dependant_T d = s->dependantlist; d; d = d->next ) {
                        Service_T parent = Util_getService(d->dependant);
                        ASSERT(parent);
                        if (parent->monitor != Monitor_Yes || parent->error) {
                                if (! _doStart(parent) || ! _check(parent)) {
                                        rv = false;
                                        StringBuffer_append(sb, "%s%s", StringBuffer_length(sb) ? ", " : "", parent->name);
                                }
                        }
                }
        }
        if (rv) {
                if (s->start) {
                        if (s->type != Service_Process || ! Util_isProcessRunning(s, false)) {
                                LogInfo("'%s' start: %s\n", s->name, s->start->arg[0]);
                                char msg[STRLEN];
                                long timeout = s->start->timeout * USEC_PER_SEC;
                                int status = _commandExecute(s, s->start, msg, sizeof(msg), &timeout);
                                if ((s->type == Service_Process && _waitProcessStart(s, &timeout) != Process_Started) || status < 0) {
                                        Event_post(s, Event_Exec, State_Failed, s->action_EXEC, "failed to start (exit status %d) -- %s", status, *msg ? msg : "no output");
                                        rv = false;
                                } else {
                                        Event_post(s, Event_Exec, State_Succeeded, s->action_EXEC, "started");
                                }
                        }
                } else {
                        LogDebug("'%s' start ignored -- method not defined\n", s->name);
                }
                Util_monitorSet(s);
        } else {
                Event_post(s, Event_Exec, State_Failed, s->action_EXEC, "'%s' failed to start -- could not start required services: '%s' (services %s depends on)", s->name, StringBuffer_toString(sb), s->name);
                s->doaction = Action_Start;
        }
        StringBuffer_free(&sb);
        return rv;
}


static int _executeStop(Service_T s, char *msg, int msglen, long *timeout) {
        LogInfo("'%s' stop: %s\n", s->name, s->stop->arg[0]);
        return _commandExecute(s, s->stop, msg, msglen, timeout);
}


static void _evaluateStop(Service_T s, boolean_t succeeded, int exitStatus, char *msg) {
        if (succeeded)
                Event_post(s, Event_Exec, State_Succeeded, s->action_EXEC, "stopped");
        else
                Event_post(s, Event_Exec, State_Failed, s->action_EXEC, "failed to stop (exit status %d) -- %s", exitStatus, *msg ? msg : "no output");
}


/*
 * This function simply stops the service s.
 * @param s A Service_T object
 * @param unmonitor true if the monitoring should be disabled or false if monitoring should continue (when stop is part of restart)
 * @return true if the service was stopped otherwise false
 */
static boolean_t _doStop(Service_T s, boolean_t unmonitor) {
        ASSERT(s);
        boolean_t rv = true;
        if (s->stop) {
                if (s->monitor != Monitor_Not) {
                        int exitStatus;
                        char msg[STRLEN];
                        long timeout = s->stop->timeout * USEC_PER_SEC;
                        if (s->type == Service_Process) {
                                int pid = Util_isProcessRunning(s, true);
                                if (pid) {
                                        exitStatus = _executeStop(s, msg, sizeof(msg), &timeout);
                                        rv = _waitProcessStop(pid, &timeout) == Process_Stopped ? true : false;
                                        _evaluateStop(s, rv, exitStatus, msg);
                                }
                        } else {
                                exitStatus = _executeStop(s, msg, sizeof(msg), &timeout);
                                rv = exitStatus >= 0 ? true : false;
                                _evaluateStop(s, rv, exitStatus, msg);
                        }
                }
        } else {
                LogDebug("'%s' stop skipped -- method not defined\n", s->name);
        }
        if (unmonitor)
                Util_monitorUnset(s);
        else
                Util_resetInfo(s);
        return rv;
}


/*
 * This function simply restarts the service s.
 * @param s A Service_T object
 */
static boolean_t _doRestart(Service_T s) {
        ASSERT(s);
        boolean_t rv = true;
        if (s->restart) {
                LogInfo("'%s' restart: %s\n", s->name, s->restart->arg[0]);
                Util_resetInfo(s);
                char msg[STRLEN];
                long timeout = s->restart->timeout * USEC_PER_SEC;
                int status = _commandExecute(s, s->restart, msg, sizeof(msg), &timeout);
                if ((s->type == Service_Process && _waitProcessStart(s, &timeout) != Process_Started) || status < 0) {
                        rv = false;
                        Event_post(s, Event_Exec, State_Failed, s->action_EXEC, "failed to restart (exit status %d) -- %s", status, msg);
                } else {
                        Event_post(s, Event_Exec, State_Succeeded, s->action_EXEC, "restarted");
                }
        } else {
                LogDebug("'%s' restart skipped -- method not defined\n", s->name);
        }
        Util_monitorSet(s);
        return rv;
}


/*
 * This is a post- fix recursive function for enabling monitoring every service
 * that s depends on before monitor s.
 * @param s A Service_T object
 */
static void _doMonitor(Service_T s) {
        ASSERT(s);
        if (s->dependantlist) {
                for (Dependant_T d = s->dependantlist; d; d = d->next ) {
                        Service_T parent = Util_getService(d->dependant);
                        ASSERT(parent);
                        _doMonitor(parent);
                }
        }
        Util_monitorSet(s);
}


/*
 * This is a function for disabling monitoring
 * @param s A Service_T object
 */
static void _doUnmonitor(Service_T s) {
        ASSERT(s);
        Util_monitorUnset(s);
}


/*
 * This is an in-fix recursive function for control of services that depend on s
 * @param s A Service_T object
 * @param action An action for the dependant services
 * @param unmonitor Disable service monitoring: used for stop action only to differentiate hard/soft stop - see _doStop()
 * @return true if all depending services were started/stopped otherwise false
 */
static boolean_t _doDepend(Service_T s, Action_Type action, boolean_t unmonitor) {
        ASSERT(s);
        boolean_t rv = true;
        for (Service_T child = servicelist; child; child = child->next) {
                for (Dependant_T d = child->dependantlist; d; d = d->next) {
                        if (IS(d->dependant, s->name)) {
                                if (action == Action_Start) {
                                        // (re)start children only if it's monitoring is enabled (we keep monitoring flag during restart, allowing to restore original pre-restart configuration)
                                        if (child->monitor != Monitor_Not && ! _doStart(child))
                                                rv = false;
                                } else if (action == Action_Monitor) {
                                        _doMonitor(child);
                                }
                                // We can start children of current child (2nd+ dependency level) only if the child itself started
                                if (rv) {
                                        if (! _doDepend(child, action, unmonitor)) {
                                                rv = false;
                                        } else {
                                                // Stop this service only if all children stopped
                                                if (action == Action_Stop && child->monitor != Monitor_Not) {
                                                        if (! _doStop(child, unmonitor))
                                                                rv = false;
                                                } else if (action == Action_Unmonitor) {
                                                        _doUnmonitor(child);
                                                }
                                        }
                                }
                                break;
                        }
                }
        }
        return rv;
}




/* ------------------------------------------------------------------ Public */


/**
 * Pass on to methods in http/cervlet.c to start/stop services
 * @param S A service name as stated in the config file
 * @param action A string describing the action to execute
 * @return false for error, otherwise true
 */
boolean_t control_service_daemon(const char *S, const char *action) {
        ASSERT(S);
        ASSERT(action);
        if (Util_getAction(action) == Action_Ignored) {
                LogError("Cannot %s service '%s' -- invalid action %s\n", action, S, action);
                return false;
        }
        Socket_T socket = NULL;
        boolean_t rv = false;
        if (Run.httpd.flags & Httpd_Net)
                // FIXME: Monit HTTP support IPv4 only currently ... when IPv6 is implemented change the family to Socket_Ip
                socket = Socket_create(Run.httpd.socket.net.address ? Run.httpd.socket.net.address : "localhost", Run.httpd.socket.net.port, Socket_Tcp, Socket_Ip4, (SslOptions_T){.use_ssl = Run.httpd.flags & Httpd_Ssl, .clientpemfile = Run.httpd.socket.net.ssl.clientpem, .allowSelfSigned = Run.httpd.flags & Httpd_AllowSelfSignedCertificates}, NET_TIMEOUT);
        else if (Run.httpd.flags & Httpd_Unix)
                socket = Socket_createUnix(Run.httpd.socket.unix.path, Socket_Tcp, NET_TIMEOUT);
        else
                LogError("Action %s not possible - monit http interface is not enabled, please add the 'set httpd' statement\n", action);
        if (! socket)
                return false;

        /* Send request */
        char *auth = Util_getBasicAuthHeaderMonit();
        if (Socket_print(socket,
                         "POST /%s HTTP/1.0\r\n"
                         "Content-Type: application/x-www-form-urlencoded\r\n"
                         "Content-Length: %lu\r\n"
                         "%s"
                         "\r\n"
                         "action=%s",
                         S,
                         (unsigned long)(strlen("action=") + strlen(action)),
                         auth ? auth : "",
                         action) < 0)
        {
                LogError("Cannot send the command '%s' to the monit daemon -- %s\n", action ? action : "null", STRERROR);
                goto err1;
        }

        /* Process response */
        char buf[STRLEN];
        if (! Socket_readLine(socket, buf, STRLEN)) {
                LogError("Error receiving data -- %s\n", STRERROR);
                goto err1;
        }
        Str_chomp(buf);
        int status;
        if (! sscanf(buf, "%*s %d", &status)) {
                LogError("Cannot parse status in response: %s\n", buf);
                goto err1;
        }
        if (status >= 300) {
                char *message = NULL;
                int content_length = 0;

                /* Skip headers */
                while (Socket_readLine(socket, buf, STRLEN)) {
                        if (! strncmp(buf, "\r\n", sizeof(buf)))
                                break;
                        if (Str_startsWith(buf, "Content-Length") && ! sscanf(buf, "%*s%*[: ]%d", &content_length))
                                goto err1;
                }
                if (content_length > 0 && content_length < 1024 && Socket_readLine(socket, buf, STRLEN)) {
                        char token[] = "</h2>";
                        char *p = strstr(buf, token);
                        if (strlen(p) <= strlen(token))
                                goto err2;
                        p += strlen(token);
                        message = CALLOC(1, content_length + 1);
                        snprintf(message, content_length + 1, "%s", p);
                        if ((p = strstr(message, "<p>")) || (p = strstr(message, "<hr>")))
                                *p = 0;
                }
        err2:
                LogError("Action failed -- %s\n", message ? message : "unable to parse response");
                FREE(message);
        } else
                rv = true;
err1:
        FREE(auth);
        Socket_free(&socket);
        return rv;
}


/**
 * Check to see if we should try to start/stop service
 * @param S A service name as stated in the config file
 * @param A A string describing the action to execute
 * @return false for error, otherwise true
 */
boolean_t control_service_string(const char *S, const char *A) {
        Action_Type a;
        ASSERT(S);
        ASSERT(A);
        if ((a = Util_getAction(A)) == Action_Ignored) {
                LogError("Service '%s' -- invalid action %s\n", S, A);
                return false;
        }
        return control_service(S, a);
}


/**
 * Check to see if we should try to start/stop service
 * @param S A service name as stated in the config file
 * @param A An action id describing the action to execute
 * @return false for error, otherwise true
 */
boolean_t control_service(const char *S, Action_Type A) {
        Service_T s = NULL;
        boolean_t rv = false;
        ASSERT(S);
        if (! (s = Util_getService(S))) {
                LogError("Service '%s' -- doesn't exist\n", S);
                return rv;
        }
        if (s->doaction == A) {
                s->doaction = Action_Ignored;
        }
        switch (A) {
                case Action_Start:
                        rv = _doStart(s);
                        break;

                case Action_Stop:
                        // Stop this service only if all children which depend on it were stopped
                        if (_doDepend(s, Action_Stop, true))
                                rv = _doStop(s, true);
                        break;

                case Action_Restart:
                        LogInfo("'%s' trying to restart\n", s->name);
                        // Restart this service only if all children that depend on it were stopped
                        if (_doDepend(s, Action_Stop, false)) {
                                if (s->restart) {
                                        if ((rv = _doRestart(s)))
                                                _doDepend(s, Action_Start, false); // Start children only if we successfully restarted
                                } else {
                                        if (_doStop(s, false)) {
                                                if ((rv = _doStart(s))) // Only start if we successfully stopped
                                                        _doDepend(s, Action_Start, false); // Start children only if we successfully started
                                        } else {
                                                /* enable monitoring of this service again to allow the restart retry in the next cycle up to timeout limit */
                                                Util_monitorSet(s);
                                        }
                                }
                        }
                        break;

                case Action_Monitor:
                        /* We only enable monitoring of this service and all prerequisite services. Chain of services which depends on this service keeps its state */
                        _doMonitor(s);
                        break;

                case Action_Unmonitor:
                        /* We disable monitoring of this service and all services which depends on it */
                        _doDepend(s, Action_Unmonitor, false);
                        _doUnmonitor(s);
                        break;

                default:
                        LogError("Service '%s' -- invalid action %d\n", S, A);
                        return false;
        }
        return rv;
}

