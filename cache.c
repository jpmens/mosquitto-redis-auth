/*
 * Copyright (c) 2014 Jan-Piet Mens <jpmens()gmail.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of mosquitto nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <mosquitto.h>
#include "userdata.h"
#include "cache.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include "uthash.h"
#include "log.h"

static unsigned int sha_hash(const char *data, size_t size, unsigned char *out)
{
	unsigned int md_len = -1;
	const EVP_MD *md = EVP_get_digestbyname("SHA1");

	if (md != NULL) {
		EVP_MD_CTX mdctx;
		EVP_MD_CTX_init(&mdctx);
		EVP_DigestInit_ex(&mdctx, md, NULL);
		EVP_DigestUpdate(&mdctx, data, size);
		EVP_DigestFinal_ex(&mdctx, out, &md_len);
		EVP_MD_CTX_cleanup(&mdctx);
	}
	return md_len;
}

static void hexify(const char *username, const char *topic, int access, char *hex)
{
	char *data;
	unsigned char hashdata[SHA_DIGEST_LENGTH];
	int mdlen, i;

	data = malloc(strlen(username) + strlen(topic) + 20);
	sprintf(data, "%s:%s:%d", username, topic, access);

	mdlen = sha_hash(data, strlen(data), hashdata);
	if (mdlen != SHA_DIGEST_LENGTH) {
		free(data);
		return;
	}

	// printf("mdlen=%d, string=%s\n\thash=", mdlen, data);
	for (i = 0, *hex = 0; i < sizeof(hashdata) / sizeof(hashdata[0]); i++) {
		sprintf(hex + (i*2), "%02X", hashdata[i]);
	}
	// printf("%s\n", hex);


	free(data);
}

static void create_update(struct maincache **cached, char *hex, int granted, int cacheseconds)
{
	struct maincache *a, *tmp;
	time_t now;

	now = time(NULL);

	HASH_FIND_STR(*cached, hex, a);
	if (a) {
		a->granted = granted;
		a->seconds = now;
		_log(DEBUG, "Updated  [%s] = %d", hex, granted);
	} else {
		a = (struct maincache *)malloc(sizeof(struct maincache));
		strcpy(a->hex, hex);
		a->granted = granted;
		a->seconds = now;
		HASH_ADD_STR(*cached, hex, a);
		_log(DEBUG, " Cached  [%s] = %d", hex, granted);
	}

	/*
	 * Check whole cache for items which need deleting. Important with
	 * clients who show up once only (mosquitto_[sp]ub with variable clientIDs
	 */

	HASH_ITER(hh, *cached, a, tmp) {
		if (now > (a->seconds + cacheseconds)) {
			_log(DEBUG, " Cleanup [%s]", a->hex);
			HASH_DEL(*cached, a);
			free(a);
		}
	}
}

static int find_and_expire(struct maincache **cached, char *hex, int cacheseconds)
{
	struct maincache *a;
	int granted = MOSQ_ERR_UNKNOWN;

	HASH_FIND_STR(*cached, hex, a);
	if (a) {
		// printf("---> CACHED! %d\n", a->granted);

		if (time(NULL) > (a->seconds + cacheseconds)) {
			_log(DEBUG, " Expired [%s]", hex);
			HASH_DEL(*cached, a);
			free(a);
		} else {
			granted = a->granted;
		}
	}
	return granted;
}

/* access is desired read/write access
 * granted is what Mosquitto auth-plug actually granted
 */

void acl_cache(const char *username, const char *topic, int access, int granted, void *userdata)
{
	char hex[SHA_DIGEST_LENGTH * 2 + 1];
	struct userdata *ud = (struct userdata *)userdata;

	if (ud->cacheseconds <= 0) {
		return;
	}

	if (!username || !topic) {
		return;
	}

	hexify(username, topic, access, hex);

	create_update(&ud->aclcache, hex, granted, ud->cacheseconds);
}

int acl_cache_q(const char *username, const char *topic, int access, void *userdata)
{
	char hex[SHA_DIGEST_LENGTH * 2 + 1];
	struct userdata *ud = (struct userdata *)userdata;

	if (ud->cacheseconds <= 0) {
		return (MOSQ_ERR_UNKNOWN);
	}

	if (!username || !topic) {
		return (MOSQ_ERR_UNKNOWN);
	}

	hexify(username, topic, access, hex);

	return find_and_expire(&ud->aclcache, hex, ud->cacheseconds);
}

void auth_cache(const char *username, const char *password, int granted, void *userdata)
{
	char hex[SHA_DIGEST_LENGTH * 2 + 1];
	struct userdata *ud = (struct userdata *)userdata;

	if (ud->cacheseconds <= 0) {
		return;
	}

	if (!username || !password) {
		return;
	}

	hexify(username, password, 0, hex);

	create_update(&ud->authcache, hex, granted, ud->cacheseconds);
}

int auth_cache_q(const char *username, const char *password, void *userdata)
{
	char hex[SHA_DIGEST_LENGTH * 2 + 1];
	struct userdata *ud = (struct userdata *)userdata;
	
	if (ud->cacheseconds <= 0) {
		return (MOSQ_ERR_UNKNOWN);
	}

	if (!username || !password) {
		return (MOSQ_ERR_UNKNOWN);
	}

	hexify(username, password, 0, hex);

	return find_and_expire(&ud->authcache, hex, ud->cacheseconds);
}
