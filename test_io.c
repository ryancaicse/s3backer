
/*
 * s3backer - FUSE-based single file backing store via Amazon S3
 * 
 * Copyright 2008 Archie L. Cobbs <archie@dellroad.org>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * $Id$
 */

#include "s3backer.h"
#include "http_io.h"
#include "test_io.h"

/* Do we want random errors? */
#define RANDOM_ERROR_PERCENT    0

/* Internal state */
struct test_io_private {
    struct http_io_conf         *config;
    u_char                      zero_block[0];
};

/* s3backer_store functions */
static int test_io_read_block(struct s3backer_store *s3b, s3b_block_t block_num, void *dest, const u_char *expect_md5);
static int test_io_write_block(struct s3backer_store *s3b, s3b_block_t block_num, const void *src, const u_char *md5);
static void test_io_destroy(struct s3backer_store *s3b);

/*
 * Constructor
 *
 * On error, returns NULL and sets `errno'.
 */
struct s3backer_store *
test_io_create(struct http_io_conf *config)
{
    struct s3backer_store *s3b;
    struct test_io_private *priv;

    /* Initialize structures */
    if ((s3b = calloc(1, sizeof(*s3b))) == NULL)
        return NULL;
    s3b->read_block = test_io_read_block;
    s3b->write_block = test_io_write_block;
    s3b->destroy = test_io_destroy;
    if ((priv = calloc(1, sizeof(*priv) + config->block_size)) == NULL) {
        free(s3b);
        errno = ENOMEM;
        return NULL;
    }
    priv->config = config;
    s3b->data = priv;

    /* Random initialization */
    srandom((u_int)time(NULL));

    /* Done */
    return s3b;
}

/*
 * Destructor
 */
static void
test_io_destroy(struct s3backer_store *const s3b)
{
    struct test_io_private *const priv = s3b->data;

    free(priv);
    free(s3b);
}

static int
test_io_read_block(struct s3backer_store *const s3b, s3b_block_t block_num, void *dest, const u_char *expect_md5)
{
    struct test_io_private *const priv = s3b->data;
    struct http_io_conf *const config = priv->config;
    u_char md5[MD5_DIGEST_LENGTH];
    char path[PATH_MAX];
    MD5_CTX ctx;
    int total;
    int fd;
    int r;

    /* Logging */
    (*config->log)(LOG_INFO, "test_io: read %0*x started", S3B_BLOCK_NUM_DIGITS, (u_int)block_num);

    /* Random delay */
    usleep((random() % 200) * 1000);

    /* Random error */
    if ((random() % 100) < RANDOM_ERROR_PERCENT) {
        (*config->log)(LOG_ERR, "test_io: random failure reading %0*x", S3B_BLOCK_NUM_DIGITS, (u_int)block_num);
        return EAGAIN;
    }

    /* Generate path */
    snprintf(path, sizeof(path), "%s/%s%0*x", config->bucket, config->prefix, S3B_BLOCK_NUM_DIGITS, block_num);

    /* Read block */
    if ((fd = open(path, O_RDONLY)) == -1) {
        if (errno == ENOENT) {
            memset(dest, 0, config->block_size);
            return 0;
        }
        return errno;
    }
    for (total = 0; total < config->block_size; total += r) {
        if ((r = read(fd, (char *)dest + total, config->block_size - total)) == -1) {
            r = errno;
            (*config->log)(LOG_ERR, "can't read %s: %s", path, strerror(r));
            close(fd);
            return r;
        }
        if (r == 0)
            break;
    }
    close(fd);

    /* Check for short read */
    if (total != config->block_size) {
        (*config->log)(LOG_ERR, "%s: file is truncated (only read %d out of %u bytes)", path, total, config->block_size);
        return EINVAL;
    }

    /* Compare MD5 checksum */
    if (expect_md5 != NULL) {
        MD5_Init(&ctx);
        MD5_Update(&ctx, dest, config->block_size);
        MD5_Final(md5, &ctx);
        if (memcmp(md5, expect_md5, MD5_DIGEST_LENGTH) != 0) {
            (*config->log)(LOG_ERR, "%s: wrong MD5 checksum", path);
            return EBADF;               /* or, what? */
        }
    }

    /* Logging */
    (*config->log)(LOG_INFO, "test_io: read %0*x complete", S3B_BLOCK_NUM_DIGITS, (u_int)block_num);

    /* Done */
    return 0;
}

static int
test_io_write_block(struct s3backer_store *const s3b, s3b_block_t block_num, const void *src, const u_char *md5)
{
    struct test_io_private *const priv = s3b->data;
    struct http_io_conf *const config = priv->config;
    char temp[PATH_MAX];
    char path[PATH_MAX];
    int total;
    int fd;
    int r;

    /* Check for zero block */
    if (src != NULL && memcmp(src, priv->zero_block, config->block_size) == 0)
        src = NULL;

    /* Logging */
    (*config->log)(LOG_INFO, "test_io: write %0*x started%s",
      S3B_BLOCK_NUM_DIGITS, (u_int)block_num, src == NULL ? " (zero block)" : "");

    /* Random delay */
    usleep((random() % 200) * 1000);

    /* Random error */
    if ((random() % 100) < RANDOM_ERROR_PERCENT) {
        (*config->log)(LOG_ERR, "test_io: random failure writing %0*x", S3B_BLOCK_NUM_DIGITS, (u_int)block_num);
        return EAGAIN;
    }

    /* Generate path */
    snprintf(path, sizeof(path), "%s/%s%0*x", config->bucket, config->prefix, S3B_BLOCK_NUM_DIGITS, block_num);

    /* Delete zero blocks */
    if (src == NULL) {
        if (unlink(path) == -1 && errno != ENOENT) {
            r = errno;
            (*config->log)(LOG_ERR, "can't unlink %s: %s", path, strerror(r));
            return r;
        }
        return 0;
    }

    /* Write into temporary file */
    snprintf(temp, sizeof(temp), "%s.XXXXXX", path);
    if ((fd = mkstemp(temp)) == -1) {
        r = errno;
        (*config->log)(LOG_ERR, "%s: %s", temp, strerror(r));
        return r;
    }
    for (total = 0; total < config->block_size; total += r) {
        if ((r = write(fd, (const char *)src + total, config->block_size - total)) == -1) {
            r = errno;
            (*config->log)(LOG_ERR, "can't write %s: %s", temp, strerror(r));
            close(fd);
            (void)unlink(temp);
            return r;
        }
    }
    close(fd);

    /* Rename file */
    if (rename(temp, path) == -1) {
        r = errno;
        (*config->log)(LOG_ERR, "can't rename %s: %s", temp, strerror(r));
        (void)unlink(temp);
        return r;
    }

    /* Logging */
    (*config->log)(LOG_INFO, "test_io: write %0*x complete", S3B_BLOCK_NUM_DIGITS, (u_int)block_num);

    /* Done */
    return 0;
}

