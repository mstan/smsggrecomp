#ifndef SMSGG_SERVICES_H
#define SMSGG_SERVICES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "recomp_input.h"
void sms_settings_load(void);
void sms_settings_player(ri_player *);
int sms_settings_volume(void);
const char *sms_settings_audio_device(void);
bool sms_settings_audio_enabled(void);
int sms_settings_window_scale(void);
int sms_settings_fullscreen(void);
bool sms_recording_enabled(void);
bool sms_recording_set(bool);
int sms_launcher(const char *,char *,size_t);
int sms_services_init(const char *,unsigned,uint64_t,bool,bool,const char *);
int sms_services_frame(void);
bool sms_services_close(void);
void sms_services_pad(uint8_t);
bool sms_services_input_override(void);
bool sms_services_fast_prepare(void);
#endif
