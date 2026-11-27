#ifndef SMSGG_CONTENT_MODS_H
#define SMSGG_CONTENT_MODS_H
#include <stddef.h>
#include "recomp_mod.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SmsContentInfo {
 const char *id,*name,*description,*source_path;
 int enabled,has_source,verified;
} SmsContentInfo;
void sms_content_load(void);
size_t sms_content_count(void);
int sms_content_get(size_t,SmsContentInfo *);
int sms_content_install(const char *archive);
int sms_content_enable(const char *id,int enabled);
int sms_content_import(const char *id,const char *original_image);
int sms_content_prepare(const char *original_game_image);
int sms_content_apply(rmod_context *,const rmod_engine *,unsigned char identity[32]);
const char *sms_content_error(void);
#ifdef __cplusplus
}
#endif
#endif
