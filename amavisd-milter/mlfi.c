/*
 * Copyright (c) 2005, Petr Rehor <rx@rx.cz>. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $Id: mlfi.c,v 1.3 2005/12/04 23:59:52 reho Exp $
 */

#include "amavisd-milter.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/stat.h>


/*
** SMFILTER - Milter description
*/
struct smfiDesc smfilter =
{
    PACKAGE,			/* filter name */
    SMFI_VERSION,		/* version code -- do not change */
    SMFIF_ADDHDRS |
    SMFIF_CHGHDRS |
    SMFIF_ADDRCPT |
    SMFIF_DELRCPT,		/* filter actions */
    mlfi_connect,		/* connection info filter */
    mlfi_helo,			/* SMTP HELO command filter */
    mlfi_envfrom,		/* envelope sender filter */
    mlfi_envrcpt,		/* envelope recipient filter */
    mlfi_header,		/* header filter */
    mlfi_eoh,			/* end of header */
    mlfi_body,			/* body block filter */
    mlfi_eom,			/* end of message */
    mlfi_abort,			/* message aborted */
    mlfi_close			/* connection cleanup */
};


/*
** LOGQIDMSG - Log message with mail queue id
*/
#define LOGQIDMSG(priority, format, args...) \
{ \
    if (mlfi != NULL && mlfi->mlfi_qid != '\0') { \
	logmsg(priority, "%s: " format, mlfi->mlfi_qid , ## args); \
    } else { \
	logmsg(priority, "NOQUEUE: " format , ## args); \
    } \
}


/*
** LOGQIDERR - Log error message with mail queue id
*/
#define LOGQIDERR(priority, format, args...) \
{ \
    if (mlfi != NULL && mlfi->mlfi_qid != '\0') { \
	logmsg(priority, "%s: %s: " format, mlfi->mlfi_qid, __FUNCTION__ , \
	    ## args); \
    } else { \
	logmsg(priority, "NOQUEUE: %s: " format, __FUNCTION__ , ## args); \
    } \
}


/*
** SMFI_SETREPLY - Set SMTP reply
*/
#define SMFI_SETREPLY(rcode, xcode, reason) \
{ \
    if (smfi_setreply(ctx, rcode, xcode, reason) != MI_SUCCESS) { \
	LOGQIDERR(LOG_WARNING, "could not set SMTP reply: %s %s %s", rcode, \
	    xcode, reason); \
    } else { \
	LOGQIDMSG(LOG_DEBUG, "set reply %s %s %s", rcode, xcode, reason); \
    } \
}


/*
** SMFI_SETREPLY_TEMPFAIL - Set SMFIS_TEMPFAIL reply
*/
#define SMFI_SETREPLY_TEMPFAIL() \
{ \
    SMFI_SETREPLY("451", "4.6.0", "Content scanner malfunction"); \
}


/*
** AMAVISD_REQUEST - Sent request line to amavisd
*/
#define AMAVISD_REQUEST(name, value) \
{ \
    if (name != NULL) { \
	LOGQIDMSG(LOG_DEBUG, "%s=%s", name, value); \
    } \
    if (amavisd_request(sd, name, value) == -1) {  \
	LOGQIDERR(LOG_CRIT, "could not write to socket %s: %s", \
	    amavisd_socket, strerror(errno)); \
	SMFI_SETREPLY_TEMPFAIL(); \
	(void) amavisd_close(sd); \
	return SMFIS_TEMPFAIL; \
    } \
}


/*
** AMAVISD_RESPONSE - Parse amavisd response line
*/
#define AMAVISD_RESPONSE(item, value, sep) \
{ \
    item = value; \
    if ((value = strchr(value, sep)) == NULL) { \
	LOGQIDERR(LOG_ERR, "malformed line: %s", name); \
	SMFI_SETREPLY_TEMPFAIL(); \
	(void) amavisd_close(sd); \
	return SMFIS_TEMPFAIL; \
    } \
    *value++ = '\0'; \
}


/*
** MLFI_CHECK_CTX - Check milter private data
*/
#define MLFI_CHECK_CTX() \
{ \
    if (mlfi == NULL) { \
	LOGQIDERR(LOG_CRIT, "context is not set"); \
	SMFI_SETREPLY_TEMPFAIL(); \
	return SMFIS_TEMPFAIL; \
    } \
}


/*
** MLFI_FREE - Free allocated memory
*/
#define MLFI_FREE(p) \
{ \
    free(p); \
    p = NULL; \
}


/*
** MLFI_STRDUP - Duplicate string
*/
#define MLFI_STRDUP(mlfi, str) \
{ \
    if ((str) != NULL && *(str) != '\0') { \
	if ((mlfi = strdup(str)) == NULL) { \
	    LOGQIDERR(LOG_ALERT, "could not allocate memory"); \
	    SMFI_SETREPLY_TEMPFAIL(); \
	    return SMFIS_TEMPFAIL; \
	} \
    } \
}


/*
** MLFI_CLEANUP - Cleanup connection context
*/
#define MLFI_CLEANUP(mlfi) \
{ \
	mlfi_cleanup(mlfi); \
	mlfi = NULL; \
}


/*
** MLFI_CLEANUP_MESSAGE - Cleanup message context
**
** mlfi_cleanup_message() close message file if open, unlink work directory
** and release message context
*/
static void
mlfi_cleanup_message(struct mlfiCtx *mlfi)
{
    struct	mlfiAddress *rcpt;

    /* Check milter private data */
    if (mlfi == NULL) {
	LOGQIDERR(LOG_DEBUG, "context is not set");
	return;
    }

    LOGQIDMSG(LOG_INFO, "CLEANUP");

    /* Close the message file */
    if (mlfi->mlfi_fp != NULL) {
	if (fclose(mlfi->mlfi_fp) != 0 && errno != EBADF) {
	    LOGQIDERR(LOG_WARNING, "could not close message file %s: %s",
		mlfi->mlfi_fname, strerror(errno));
	} else {
	    LOGQIDMSG(LOG_DEBUG, "close message file %s", mlfi->mlfi_fname);
	}
	mlfi->mlfi_fp = NULL;
    }

    /* Remove the message file */
    if (mlfi->mlfi_fname[0] != '\0') {
	if (unlink(mlfi->mlfi_fname) != 0 && errno != ENOENT) {
	    LOGQIDERR(LOG_WARNING, "could not unlink message file %s: %s",
		mlfi->mlfi_fname, strerror(errno));
	} else {
	    LOGQIDMSG(LOG_DEBUG, "unlink message file %s", mlfi->mlfi_fname);
	}
	mlfi->mlfi_fname[0] = '\0';
    }

    /* Unlink work directory */
    if (mlfi->mlfi_wrkdir[0] != '\0') {
	if (rmdir(mlfi->mlfi_wrkdir) == -1 && errno != ENOENT) {
	    LOGQIDERR(LOG_WARNING, "could not remove work dir %s: %s",
		mlfi->mlfi_wrkdir, strerror(errno));
	} else {
	    LOGQIDMSG(LOG_DEBUG, "remove work directory %s", mlfi->mlfi_wrkdir);
	}
	mlfi->mlfi_wrkdir[0] = '\0';
    }

    /* Free memory */
    MLFI_FREE(mlfi->mlfi_qid);
    MLFI_FREE(mlfi->mlfi_from);
    while(mlfi->mlfi_rcpt != NULL) {
	rcpt = mlfi->mlfi_rcpt;
	mlfi->mlfi_rcpt = rcpt->q_next;
	free(rcpt);
    }
}


/*
** MLFI_CLEANUP - Cleanup connection context
**
** mlfi_cleanup() cleanup message context and relese connection context
*/
static void
mlfi_cleanup(struct mlfiCtx *mlfi)
{
    /* Check milter private data */
    if (mlfi == NULL) {
	LOGQIDERR(LOG_DEBUG, "context is not set");
	return;
    }

    /* Cleanup the message context */
    mlfi_cleanup_message(mlfi);

    LOGQIDMSG(LOG_INFO, "cleanup connection context");

    /* Cleanup the connection context */
    MLFI_FREE(mlfi->mlfi_addr);
    MLFI_FREE(mlfi->mlfi_hostname);
    MLFI_FREE(mlfi->mlfi_helo);

    /* Free context */
    free(mlfi);
}


/*
** MLFI_CONNECT - Handle incomming connection
** 
** mlfi_connect() is called once, at the start of each SMTP connection
*/
sfsistat
mlfi_connect(SMFICTX *ctx, char *hostname, _SOCK_ADDR * hostaddr)
{
    struct	mlfiCtx *mlfi = NULL;
    char       *addr;

    LOGQIDMSG(LOG_INFO, "CONNECT: %s", hostname);

    /* Check amavisd socket */
    if (amavisd_init() == -1) {
	SMFI_SETREPLY_TEMPFAIL();
	return SMFIS_TEMPFAIL;
    }

    /* Allocate memory for private data */
    mlfi = malloc(sizeof(*mlfi));
    if (mlfi == NULL) {
        LOGQIDERR(LOG_ALERT, "could not allocate memory");
        SMFI_SETREPLY_TEMPFAIL();
        return SMFIS_TEMPFAIL;
    }
    (void) memset(mlfi, '\0', sizeof(*mlfi));

    /* Save connection informations */
    MLFI_STRDUP(mlfi->mlfi_hostname, hostname);
    if (hostaddr != NULL) {
	addr = inet_ntoa(((struct sockaddr_in *)hostaddr)->sin_addr);
	MLFI_STRDUP(mlfi->mlfi_addr, addr);
    }

    /* Save the private data */
    if (smfi_setpriv(ctx, mlfi) != MI_SUCCESS) {
	LOGQIDERR(LOG_ERR, "could not set milter context");
	SMFI_SETREPLY_TEMPFAIL();
	MLFI_CLEANUP(mlfi);
	return SMFIS_TEMPFAIL;
    }

    /* Continue processing */
    return SMFIS_CONTINUE;
}


/*
** MLFI_HELO - Handle the HELO/EHLO command
**
** mlfi_helo() is called whenever the client sends a HELO/EHLO command.
** It may therefore be called between zero and three times.
*/
sfsistat
mlfi_helo(SMFICTX *ctx, char* helohost)
{
    struct	mlfiCtx *mlfi = MLFICTX(ctx);

    /* Check milter private data */
    MLFI_CHECK_CTX();

    LOGQIDMSG(LOG_DEBUG, "HELO: %s", helohost);

    /* Save helo hostname */
    if (helohost != NULL && *helohost != '\0') {
	MLFI_FREE(mlfi->mlfi_helo);
	MLFI_STRDUP(mlfi->mlfi_helo, helohost);
    }

    /* Continue processing */
    return SMFIS_CONTINUE;
}


/*
** MLFI_ENVFORM - Handle the envelope FROM command
** 
** mlfi_envfrom() is called once at the beginning of each message, before
** mlfi_envrcpt()
*/
sfsistat
mlfi_envfrom(SMFICTX *ctx, char **envfrom)
{
    struct	mlfiCtx *mlfi = MLFICTX(ctx);
    char       *qid, *wrkdir;

    /* Check milter private data */
    MLFI_CHECK_CTX();

    /* Cleanup message data */
    mlfi_cleanup_message(mlfi);

    /* Get message id */
    if ((qid = smfi_getsymval(ctx, "i")) != NULL) {
	MLFI_STRDUP(mlfi->mlfi_qid, qid);
    }

    LOGQIDMSG(LOG_INFO, "MAIL FROM: %s", *envfrom);

    /* Save from mail address */
    MLFI_STRDUP(mlfi->mlfi_from, *envfrom);

    /* Create work directory */
    if (mlfi->mlfi_qid != NULL) {
	(void) snprintf(mlfi->mlfi_wrkdir, sizeof(mlfi->mlfi_wrkdir) - 1,
	    "%s/af%s", work_dir, mlfi->mlfi_qid);
	if (mkdir(mlfi->mlfi_wrkdir, S_IRWXU|S_IRGRP|S_IXGRP) != 0) {
	    mlfi->mlfi_wrkdir[0] = '\0';
	}
    }
    if (mlfi->mlfi_wrkdir[0] == '\0') {
	(void) snprintf(mlfi->mlfi_wrkdir, sizeof(mlfi->mlfi_wrkdir) - 1,
	    "%s/afXXXXXXXXXX", work_dir);
	if ((wrkdir = mkdtemp(mlfi->mlfi_wrkdir)) != NULL) {
	    (void) strlcpy(mlfi->mlfi_wrkdir, wrkdir,
		sizeof(mlfi->mlfi_wrkdir));
	} else {
	    LOGQIDERR(LOG_ERR, "could not create work directory: %s",
		strerror(errno));
	    mlfi->mlfi_wrkdir[0] = '\0';
            SMFI_SETREPLY_TEMPFAIL();
            return SMFIS_TEMPFAIL;
	}
	if (chmod(mlfi->mlfi_wrkdir, S_IRWXU|S_IRGRP|S_IXGRP) == -1) {
	    LOGQIDERR(LOG_ERR, "could not change mode of directory %s: %s",
		mlfi->mlfi_wrkdir, strerror(errno));
	    SMFI_SETREPLY_TEMPFAIL();
	    return SMFIS_TEMPFAIL;
	}
    }
    LOGQIDMSG(LOG_DEBUG, "create work directory %s", mlfi->mlfi_wrkdir);

    /* Open file to store this message */
    (void) snprintf(mlfi->mlfi_fname, sizeof(mlfi->mlfi_fname) - 1,
	"%s/email.txt", mlfi->mlfi_wrkdir);
    if ((mlfi->mlfi_fp = fopen(mlfi->mlfi_fname, "w+")) == NULL) {
	LOGQIDERR(LOG_ERR, "could not create message file %s: %s",
	    mlfi->mlfi_fname, strerror(errno));
	SMFI_SETREPLY_TEMPFAIL();
	return SMFIS_TEMPFAIL;
    }
    if (fchmod(fileno(mlfi->mlfi_fp), S_IRUSR|S_IWUSR|S_IRGRP) == -1) {
	LOGQIDERR(LOG_ERR, "could not change mode of file %s: %s",
	    mlfi->mlfi_fname, strerror(errno));
	SMFI_SETREPLY_TEMPFAIL();
	return SMFIS_TEMPFAIL;
    }
    LOGQIDMSG(LOG_DEBUG, "create message file %s", mlfi->mlfi_fname);

    /* Continue processing */
    return SMFIS_CONTINUE;
}


/*
** MLFI_ENVRCPT - Handle the envelope RCPT command
** 
** mlfi_envrcpt() is called once per recipient, hence one or more times
** per message, immediately after mlfi_envfrom()
*/
sfsistat
mlfi_envrcpt(SMFICTX *ctx, char **envrcpt)
{
    struct	mlfiCtx *mlfi = MLFICTX(ctx);
    struct	mlfiAddress *rcpt, *r;
    int		rcptlen;

    /* Check milter private data */
    MLFI_CHECK_CTX();

    LOGQIDMSG(LOG_INFO, "RCPT TO: %s",  *envrcpt);

    /* Store recipient address */
    rcptlen = strlen(*envrcpt);
    if ((rcpt = malloc(sizeof(*rcpt) + rcptlen)) == NULL) {
	LOGQIDERR(LOG_ALERT, "could not allocate memory");
	SMFI_SETREPLY_TEMPFAIL();
	return SMFIS_TEMPFAIL;
    }
    (void) strlcpy(rcpt->q_paddr, *envrcpt, rcptlen + 1);
    rcpt->q_next = NULL;
    if (mlfi->mlfi_rcpt == NULL) {
	mlfi->mlfi_rcpt = rcpt;
    } else {
	r = mlfi->mlfi_rcpt;
	while (r->q_next != NULL) {
	    r = r->q_next;
	}
	r->q_next = rcpt;
    }

    /* Continue processing */
    return SMFIS_CONTINUE;
}


/*
** MLFI_HEADER - Handle a message header
** 
** mlfi_header() is called zero or more times between mlfi_envrcpt() and
** mlfi_eoh(), once per message header
*/
sfsistat
mlfi_header(SMFICTX *ctx, char *headerf, char *headerv)
{
    struct	mlfiCtx *mlfi = MLFICTX(ctx);

    /* Check milter private data */
    MLFI_CHECK_CTX();

    LOGQIDMSG(LOG_DEBUG, "HEADER: %s: %s", headerf, headerv);

    /* Write the header to the message file */
    /* amavisd_new require \n instead of \r\n at the end of header */
    (void) fprintf(mlfi->mlfi_fp, "%s: %s\n", headerf, headerv);
    if (ferror(mlfi->mlfi_fp)) {
	LOGQIDERR(LOG_ERR, "could not write to message file %s: %s",
	    mlfi->mlfi_fname, strerror(errno));
	SMFI_SETREPLY_TEMPFAIL();
	return SMFIS_TEMPFAIL;
    }

    /* Continue processing */
    return SMFIS_CONTINUE;
}


/*
** MLFI_EOH - Handle the end of message headers
** 
** mlfi_eoh() is called once after all headers have been sent and processed
*/
sfsistat
mlfi_eoh(SMFICTX *ctx)
{
    struct	mlfiCtx *mlfi = MLFICTX(ctx);

    /* Check milter private data */
    MLFI_CHECK_CTX();

    LOGQIDMSG(LOG_DEBUG, "END OF HEADERS");

    /* Write the blank line between the header and the body */
    /* XXX: amavisd_new require \n instead of \r\n at the end of line */
    (void) fprintf(mlfi->mlfi_fp, "\n");
    if (ferror(mlfi->mlfi_fp)) {
	LOGQIDERR(LOG_ERR, "could not write to message file %s: %s",
	    mlfi->mlfi_fname, strerror(errno));
	SMFI_SETREPLY_TEMPFAIL();
	return SMFIS_TEMPFAIL;
    }

    /* Continue processing */
    return SMFIS_CONTINUE;
}


/*
** MLFI_BODY - Handle a piece of a message's body
** 
** mlfi_body() is called zero or more times between mlfi_eoh() and mlfi_eom()
*/
sfsistat
mlfi_body(SMFICTX *ctx, unsigned char * bodyp, size_t bodylen)
{
    struct	mlfiCtx *mlfi = MLFICTX(ctx);

    /* Check milter private data */
    MLFI_CHECK_CTX();

    LOGQIDMSG(LOG_DEBUG, "body chunk: %ld", (long)bodylen);

    /* Write the body chunk to the message file */
    if (fwrite(bodyp, bodylen, 1, mlfi->mlfi_fp) < 1) {
	LOGQIDERR(LOG_ERR, "could not write to message file %s: %s",
	    mlfi->mlfi_fname, strerror(errno));
	SMFI_SETREPLY_TEMPFAIL();
	return SMFIS_TEMPFAIL;
    }

    /* Continue processing */
    return SMFIS_CONTINUE;
}


/*
** MLFI_EOM - Handle the end of a message
** 
** mlfi_eom() is called once after all calls to mlfi_body()
** for a given message
*/
sfsistat
mlfi_eom(SMFICTX *ctx)
{
    int		sd, i;
    char       *idx, *header, *rcode, *xcode, *value;
    char	name[MAXAMABUF];
    sfsistat	rstat;
    struct	mlfiCtx *mlfi = MLFICTX(ctx);
    struct	mlfiAddress *rcpt;
    struct	sockaddr_un amavisd_sock;

    /* Check milter private data */
    MLFI_CHECK_CTX();

    LOGQIDMSG(LOG_INFO, "CONTENT CHECK");

    /* Close the message file */
    if (mlfi->mlfi_fp == NULL) {
	LOGQIDERR(LOG_ERR, "message file %s is not opened", mlfi->mlfi_fname);
	SMFI_SETREPLY_TEMPFAIL();
	return SMFIS_TEMPFAIL;
    }
    if (fclose(mlfi->mlfi_fp) == -1) {
	mlfi->mlfi_fp = NULL;
	LOGQIDERR(LOG_ERR, "could not close message file %s: %s",
	    mlfi->mlfi_fname, strerror(errno));
	SMFI_SETREPLY_TEMPFAIL();
	return SMFIS_TEMPFAIL;
    }
    mlfi->mlfi_fp = NULL;
    LOGQIDMSG(LOG_DEBUG, "close message file %s", mlfi->mlfi_fname);

    /* Connect to amavisd */
    if ((sd = amavisd_connect(&amavisd_sock)) == -1) {
	LOGQIDERR(LOG_CRIT, "could not connect to amavisd socket %s: %s",
	    amavisd_socket, strerror(errno));
	SMFI_SETREPLY_TEMPFAIL();
	return SMFIS_TEMPFAIL;
    }

    LOGQIDMSG(LOG_DEBUG, "AMAVISD REQUEST");

    /* Send email to amavisd */
    AMAVISD_REQUEST("request", "AM.PDP");
    if (mlfi->mlfi_qid != NULL) {
	AMAVISD_REQUEST("queue_id", mlfi->mlfi_qid);
    }
    AMAVISD_REQUEST("sender", mlfi->mlfi_from);
    rcpt = mlfi->mlfi_rcpt;
    while (rcpt != NULL) {
	AMAVISD_REQUEST("recipient", rcpt->q_paddr);
	rcpt = rcpt->q_next;
    }
    AMAVISD_REQUEST("tempdir", mlfi->mlfi_wrkdir);
    AMAVISD_REQUEST("tempdir_removed_by", "server");
    AMAVISD_REQUEST("mail_file", mlfi->mlfi_fname);
    AMAVISD_REQUEST("delivery_care_of", "client");
    AMAVISD_REQUEST("client_address", mlfi->mlfi_addr);
    if (mlfi->mlfi_hostname != NULL) {
	AMAVISD_REQUEST("client_name", mlfi->mlfi_hostname);
    }
    if (mlfi->mlfi_helo != NULL) {
	AMAVISD_REQUEST("helo_name", mlfi->mlfi_helo);
    }
    AMAVISD_REQUEST(NULL, NULL);

    LOGQIDMSG(LOG_DEBUG, "AMAVISD RESPONSE");

    /* Process response from amavisd */
    rstat = SMFIS_TEMPFAIL;
    while (amavisd_response(sd, name, sizeof(name)) != -1) {

	/* End of response */
	if (name[0] == '\0') {
	    (void) amavisd_close(sd);
	    return rstat;
	}

	/* Get name and value */
        value = name;
	AMAVISD_RESPONSE(value, value, '=');

	/* Add recipient */
	if (strcmp(name, "addrcpt") == 0) {
	    LOGQIDMSG(LOG_INFO, "%s=%s", name, value);
	    if (smfi_addrcpt(ctx, value) != MI_SUCCESS) {
		LOGQIDERR(LOG_ERR, "could not add recipient %s", value);
		SMFI_SETREPLY_TEMPFAIL();
		(void) amavisd_close(sd);
		return SMFIS_TEMPFAIL;
	    }

	/* Delete recipient */
	} else if (strcmp(name, "delrcpt") == 0) {
	    LOGQIDMSG(LOG_INFO, "%s=%s", name, value);
	    if (smfi_delrcpt(ctx, value) != MI_SUCCESS) {
		LOGQIDERR(LOG_ERR, "could not delete recipient %s", value);
		SMFI_SETREPLY_TEMPFAIL();
		(void) amavisd_close(sd);
		return SMFIS_TEMPFAIL;
	    }

	/* Add header */
	} else if (strcmp(name, "addheader") == 0) {
	    LOGQIDMSG(LOG_INFO, "%s=%s", name, value);
	    AMAVISD_RESPONSE(header, value, ' ');
	    if (smfi_addheader(ctx, header, value) != MI_SUCCESS) {
		LOGQIDERR(LOG_ERR, "could not add header %s: %s", header,
		    value);
		SMFI_SETREPLY_TEMPFAIL();
		(void) amavisd_close(sd);
		return SMFIS_TEMPFAIL;
	    }

	/* Change header */
	} else if (strcmp(name, "chgheader") == 0) {
	    LOGQIDMSG(LOG_INFO, "%s=%s", name, value);
	    AMAVISD_RESPONSE(idx, value, ' ');
	    i = (int) strtol(idx, &header, 10);
	    if (header != NULL && *header != '\0') {
		LOGQIDERR(LOG_ERR, "malformed line '%s=%s'", name, idx);
		SMFI_SETREPLY_TEMPFAIL();
		(void) amavisd_close(sd);
		return SMFIS_TEMPFAIL;
	    }
	    AMAVISD_RESPONSE(header, value, ' ');
	    if (smfi_chgheader(ctx, header, i, value) != MI_SUCCESS) {
		LOGQIDERR(LOG_ERR, "could not change header %s %s: %s",
		    idx, header, value);
		SMFI_SETREPLY_TEMPFAIL();
		(void) amavisd_close(sd);
		return SMFIS_TEMPFAIL;
	    }

	/* Delete header */
        } else if (strcmp(name, "delheader") == 0) {
	    LOGQIDMSG(LOG_INFO, "%s=%s", name, value);
	    AMAVISD_RESPONSE(idx, value, ' ');
	    i = (int) strtol(idx, &header, 10);
	    if (header != NULL && *header != '\0') {
		LOGQIDERR(LOG_ERR, "malformed line '%s=%s'", name, idx);
		SMFI_SETREPLY_TEMPFAIL();
		(void) amavisd_close(sd);
		return SMFIS_TEMPFAIL;
	    }
	    if (smfi_chgheader(ctx, value, i, NULL) != MI_SUCCESS) {
		LOGQIDERR(LOG_ERR, "could not delete header %s %s:",
		    idx, header);
		SMFI_SETREPLY_TEMPFAIL();
		(void) amavisd_close(sd);
		return SMFIS_TEMPFAIL;
	    }

	/* Set response code */
        } else if (strcmp(name, "return_value") == 0) {
	    LOGQIDMSG(LOG_INFO, "%s=%s", name, value);
	    if (strcmp(value, "continue") == 0) {
		rstat = SMFIS_CONTINUE;
	    } else if (strcmp(value, "accept") == 0) {
		rstat = SMFIS_ACCEPT;
	    } else if (strcmp(value, "reject") == 0) {
		rstat = SMFIS_REJECT;
	    } else if (strcmp(value, "discard") == 0) {
		rstat = SMFIS_DISCARD;
	    } else if (strcmp(value, "tempfail") == 0) {
		rstat = SMFIS_TEMPFAIL;
	    } else {
		LOGQIDERR(LOG_ERR, "unknown return value %s", value);
		SMFI_SETREPLY_TEMPFAIL();
		(void) amavisd_close(sd);
		return SMFIS_TEMPFAIL;
	    }

	/* Set SMTP reply */
        } else if (strcmp(name, "setreply") == 0) {
	    AMAVISD_RESPONSE(rcode, value, ' ');
	    AMAVISD_RESPONSE(xcode, value, ' ');
	    if (*rcode != '4' && *rcode != '5') {
		/* smfi_setreply accept only 4xx and 5XX codes */
		LOGQIDMSG(LOG_DEBUG, "%s=%s %s %s", name, rcode, xcode, value);
	    } else {
		LOGQIDMSG(LOG_NOTICE, "%s=%s %s %s", name, rcode, xcode, value);
		if (smfi_setreply(ctx, rcode, xcode, value) != MI_SUCCESS) {
		    LOGQIDERR(LOG_ERR, "could not set reply %s %s %s",
			rcode, xcode, value);
		    SMFI_SETREPLY_TEMPFAIL();
		    (void) amavisd_close(sd);
		    return SMFIS_TEMPFAIL;
		}
	    }

	/* Exit code */
        } else if (strcmp(name, "exit_code") == 0) {
	    /* ignore legacy exit_code */
	    LOGQIDMSG(LOG_DEBUG, "%s=%s", name, value);

	/* Unknown response */
        } else {
	    LOGQIDERR(LOG_WARNING, "ignore unknown response %s=%s",
		name, value);
	}
    }

    /* Amavisd response fail */
    LOGQIDMSG(LOG_DEBUG, "amavisd response line %s", name);
    LOGQIDERR(LOG_ERR, "could not read from amavisd socket %s: %s",
	amavisd_socket, strerror(errno));
    SMFI_SETREPLY_TEMPFAIL();
    (void) amavisd_close(sd);
    return SMFIS_TEMPFAIL;
}


/*
** MLFI_ABORT - Handle the current message's being aborted
**
** mlfi_abort() must reclaim any resources allocated on a per-message
** basis, and must be tolerant of being called between any two
** message-oriented callbacks
*/
sfsistat
mlfi_abort(SMFICTX *ctx)
{
    struct	mlfiCtx *mlfi = MLFICTX(ctx);

    /* Check milter private data */
    if (mlfi == NULL) {
	LOGQIDERR(LOG_DEBUG, "context is not set");
	return SMFIS_CONTINUE;
    }

    LOGQIDMSG(LOG_NOTICE, "ABORT");

    /* Cleanup message data */
    mlfi_cleanup_message(mlfi);

    /* Continue processing */
    return SMFIS_CONTINUE;
}


/*
** MLFI_CLOSE - The current connection is being closed
** 
** mlfi_close() is always called once at the end of each connection
*/
sfsistat
mlfi_close(SMFICTX *ctx)
{
    struct	mlfiCtx *mlfi = MLFICTX(ctx);

    /* Check milter private data */
    if (mlfi == NULL) {
	LOGQIDERR(LOG_DEBUG, "context is not set");
	return SMFIS_CONTINUE;
    }

    /* Release private data */
    MLFI_CLEANUP(mlfi);
    if (smfi_setpriv(ctx, NULL) != MI_SUCCESS) {
	/* NOTE: smfi_setpriv return MI_FAILURE when ctx is NULL */
	/* LOGQIDERR(LOG_ERR, "could not release milter context"); */
    }

    LOGQIDMSG(LOG_INFO, "CLOSE");

    /* Continue processing */
    return SMFIS_CONTINUE;
}
