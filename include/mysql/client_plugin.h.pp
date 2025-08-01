struct st_mysql_client_plugin
{
  int type; unsigned int interface_version; const char *name; const char *author; const char *desc; unsigned int version[3]; const char *license; void *mysql_api; int (*init)(char *, size_t, int, va_list); int (*deinit)(); int (*options)(const char *option, const void *);
};
struct st_mysql;
typedef struct st_plugin_vio_info
{
  enum { MYSQL_VIO_INVALID, MYSQL_VIO_TCP, MYSQL_VIO_SOCKET,
         MYSQL_VIO_PIPE, MYSQL_VIO_MEMORY } protocol;
  int socket;
  int tls;
} MYSQL_PLUGIN_VIO_INFO;
typedef struct st_plugin_vio
{
  int (*read_packet)(struct st_plugin_vio *vio,
                     unsigned char **buf);
  int (*write_packet)(struct st_plugin_vio *vio,
                      const unsigned char *packet,
                      int packet_len);
  void (*info)(struct st_plugin_vio *vio, struct st_plugin_vio_info *info);
} MYSQL_PLUGIN_VIO;
struct st_mysql_client_plugin_AUTHENTICATION
{
  int type; unsigned int interface_version; const char *name; const char *author; const char *desc; unsigned int version[3]; const char *license; void *mysql_api; int (*init)(char *, size_t, int, va_list); int (*deinit)(); int (*options)(const char *option, const void *);
  int (*authenticate_user)(MYSQL_PLUGIN_VIO *vio, struct st_mysql *mysql);
  int (*hash_password_bin)(struct st_mysql *mysql, unsigned char *hash, size_t *hash_length);
};
struct st_mysql;
typedef char *(*mysql_authentication_dialog_ask_t)(struct st_mysql *mysql,
                      int type, const char *prompt, char *buf, int buf_len);
struct st_mysql_client_plugin *
mysql_load_plugin(struct st_mysql *mysql, const char *name, int type,
                  int argc, ...);
struct st_mysql_client_plugin *
mysql_load_plugin_v(struct st_mysql *mysql, const char *name, int type,
                    int argc, va_list args);
struct st_mysql_client_plugin *
mysql_client_find_plugin(struct st_mysql *mysql, const char *name, int type);
struct st_mysql_client_plugin *
mysql_client_register_plugin(struct st_mysql *mysql,
                             struct st_mysql_client_plugin *plugin);
int mysql_plugin_options(struct st_mysql_client_plugin *plugin,
                         const char *option, const void *value);
