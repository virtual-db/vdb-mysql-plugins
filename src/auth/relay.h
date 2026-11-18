/* relay.h: the login relay shared by vdb_mysql_auth and
   vdb_mysql_auth_native (implementation plan doc 08 §4). */
#ifndef VDB_AUTH_RELAY_H
#define VDB_AUTH_RELAY_H

#include "mysql/plugin_auth.h"

/* Run one login: method is the client plug-in this server plug-in asks for. */
int vdb_relay_authenticate(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info, const char *method);

int vdb_auth_generate(char *outbuf, unsigned int *outbuflen, const char *inbuf, unsigned int inbuflen);
int vdb_auth_validate(char *const inbuf, unsigned int buflen);
int vdb_auth_set_salt(const char *password, unsigned int password_len, unsigned char *salt,
                      unsigned char *salt_len);

#endif
