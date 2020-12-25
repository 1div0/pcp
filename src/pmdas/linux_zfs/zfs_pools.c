#include <stdio.h>
#include <stdlib.h>
#include <regex.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/limits.h>

#include "pmapi.h"
#include "libpcp.h"
#include "pmda.h"

#include "zfs_utils.h"
#include "zfs_pools.h"

void
zfs_pools_init(zfs_poolstats_t **poolstats, pmdaInstid **pools, pmdaIndom *poolsindom)
{
    DIR *zfs_dp;
    struct dirent *ep;
    int pool_num = 0;
    size_t size;
    zfs_poolstats_t *poolstats_tmp;
    static int seen_err = 0;

    // Discover the pools by looking for directories in /proc/spl/kstat/zfs
    if ((zfs_dp = opendir(ZFS_PATH)) != NULL) {
        while ((ep = readdir(zfs_dp))) {
            if (ep->d_type == DT_DIR) {
                if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0)
                    continue;
                else {
                    pmdaInstid    *pools_tmp;
                    size = (pool_num + 1) * sizeof(pmdaInstid);
                    if ((pools_tmp = (pmdaInstid *)realloc(*pools, size)) == NULL)
                        pmNoMem("pools", size, PM_FATAL_ERR);
                    *pools = pools_tmp;
                    (*pools)[pool_num].i_name = (char *) malloc(strlen(ep->d_name) + 1);
                    strcpy((*pools)[pool_num].i_name, ep->d_name);
                    (*pools)[pool_num].i_name[strlen(ep->d_name)] = '\0';
                    (*pools)[pool_num].i_inst = pool_num;
                    pool_num++;
                }
            }
        }
        closedir(zfs_dp);
    }
    else {
        pmNotifyErr(LOG_WARNING, "Failed to open ZFS pools dir \"%s\": %s\n", ZFS_PATH, pmErrStr(-errno));
    }
    if (*pools == NULL) {
        if (! seen_err) {
            pmNotifyErr(LOG_WARNING, "no ZFS pools found, instance domain is empty.");
            seen_err = 1;
        }
    }
    else if (seen_err) {
        pmNotifyErr(LOG_INFO, "%d ZFS pools found.", pool_num);
        seen_err = 0;
    }
    (*poolsindom).it_set = *pools;
    (*poolsindom).it_numinst = pool_num;
    if (pool_num > 0) {
        if ((poolstats_tmp = (zfs_poolstats_t *)realloc(*poolstats, pool_num * sizeof(zfs_poolstats_t))) == NULL)
            pmNoMem("poolstats init", pool_num * sizeof(zfs_poolstats_t), PM_FATAL_ERR);
        *poolstats = poolstats_tmp;
    }
}

void
zfs_pools_clear(zfs_poolstats_t **poolstats, pmdaInstid **pools, pmdaIndom *poolsindom)
{
    int i;

    for (i = 0; i < (*poolsindom).it_numinst; i++) {
        free((*pools)[i].i_name);
        (*pools)[i].i_name = NULL;
    }
    if (*pools)
        free(*pools);
    if (*poolstats)
        free(*poolstats);
    *poolstats = NULL;
    (*poolsindom).it_set = *pools = NULL;
    (*poolsindom).it_numinst = 0;
}

void
zfs_poolstats_refresh(zfs_poolstats_t **poolstats, pmdaInstid **pools, pmdaIndom *poolsindom)
{
    int i;
    char pool_dir[MAXPATHLEN], fname[MAXPATHLEN];
    char *line = NULL, *token, delim[] = " ";
    FILE *fp;
    struct stat sstat;
    regex_t rgx_io;
    size_t nmatch = 1, len = 0;
    regmatch_t pmatch[1];

    zfs_pools_init(poolstats, pools, poolsindom);
    if (poolsindom->it_numinst == 0) {
        /* no pools, nothing to do */
        return;
    }

    regcomp(&rgx_io, "([0-9]+[ ]+){11}[0-9]+", REG_EXTENDED|REG_NOSUB);
    if ((*poolstats = realloc(*poolstats, (*poolsindom).it_numinst * sizeof(zfs_poolstats_t))) == NULL)
        pmNoMem("poolstats refresh", (*poolsindom).it_numinst * sizeof(zfs_poolstats_t), PM_FATAL_ERR);
    for (i = 0; i < (*poolsindom).it_numinst; i++) {
	sprintf(pool_dir, "%s%c%s", ZFS_PATH, pmPathSeparator(), (*poolsindom).it_set[i].i_name);
        if (stat(pool_dir, &sstat) != 0) {
            continue;
        }
        // Read the state if exists
        (*poolstats)[i].state = 13; // UNKNOWN
        strcpy(fname, pool_dir);
        strcat(fname, "/state");
        fp = fopen(fname, "r");
        if (fp != NULL) {
            if (getline(&line, &len, fp) != -1) {
                if (strncmp(line, "OFFLINE", 7) == 0) (*poolstats)[i].state = 0;
                else if (strncmp(line, "ONLINE", 6) == 0) (*poolstats)[i].state = 1;
                else if (strncmp(line, "DEGRADED", 8) == 0) (*poolstats)[i].state = 2;
                else if (strncmp(line, "FAULTED", 7) == 0) (*poolstats)[i].state = 3;
                else if (strncmp(line, "REMOVED", 7) == 0) (*poolstats)[i].state = 4;
                else if (strncmp(line, "UNAVAIL", 7) == 0) (*poolstats)[i].state = 5;
            }
            fclose(fp);
            free(line);
            line = NULL;
            len = 0;
        }
        // Read the IO stats
        strcpy(fname, pool_dir);
        strcat(fname, "/io");
        fp = fopen(fname, "r");
        if (fp != NULL) {
            while (getline(&line, &len, fp) != -1) {
                if (regexec(&rgx_io, line, nmatch, pmatch, 0) == 0) {
                    // Tokenize the line to extract the metrics
                    token = strtok(line, delim);
                    (*poolstats)[i].nread = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                     (*poolstats)[i].nwritten = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                     (*poolstats)[i].reads = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                     (*poolstats)[i].writes = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                     (*poolstats)[i].wtime = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                     (*poolstats)[i].wlentime = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                     (*poolstats)[i].wupdate = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                     (*poolstats)[i].rtime = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                     (*poolstats)[i].rlentime = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                     (*poolstats)[i].rupdate = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                     (*poolstats)[i].wcnt = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                     (*poolstats)[i].rcnt = strtoul(token, NULL, 0);
                    token = strtok(NULL, delim);
                }
            }
            fclose(fp);
        }
    }
    regfree(&rgx_io);
}
