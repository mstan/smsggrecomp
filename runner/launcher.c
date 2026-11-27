/* Host-owned SMS/GG policy; shared UI edits the same bindings consumed by RI. */
#include "services.h"
#include "game_spec.h"
#include "content_mods.h"
#include "recomp_persistence/atomic_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SMS_HAVE_SDL
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "recomp_launcher.h"
#endif
static int recording=1,volume=100,audio=1;
static int window_scale=3,fullscreen;
static ri_player player;
static char device[128];
static char error[160];
static int save_settings(void){
 char text[4096];int n=snprintf(text,sizeof text,"recording=%d\nvolume=%d\naudio=%d\nwindow_scale=%d\nfullscreen=%d\nsources=%u\ndeadzone=%u\nguid=%s\nphysical=%s\nname=%s\naudio_device=%s\n",recording,volume,audio,window_scale,fullscreen,player.sources,player.deadzone_pct,player.guid,player.physical_key,player.name,device);
 for(int i=0;i<7;i++)n+=snprintf(text+n,sizeof text-(size_t)n,"key%d=%u\npad%d=%u\n",i,player.key_binding[i],i,player.pad_binding[i]);
 return n>0&&(size_t)n<sizeof text&&recomp_atomic_file_store("smsgg-settings.ini",text,(size_t)n);
}
void sms_settings_load(void){
 sms_content_load();
 memset(&player,0,sizeof player);player.sources=RI_SOURCE_KEYBOARD;player.deadzone_pct=20;
 /* SDL standard scancodes and controller indices. */
 const uint16_t keys[7]={82,81,80,79,29,27,40};
 const uint32_t pads[7]={RI_BUTTON(11),RI_BUTTON(12),RI_BUTTON(13),RI_BUTTON(14),RI_BUTTON(0),RI_BUTTON(2),RI_BUTTON(6)};
 memcpy(player.key_binding,keys,sizeof keys);memcpy(player.pad_binding,pads,sizeof pads);
 FILE *f=fopen("smsgg-settings.ini","r");if(!f)return;char line[512];
 while(fgets(line,sizeof line,f)){
  line[strcspn(line,"\r\n")]=0;char *value=strchr(line,'=');if(!value)continue;*value++=0;
  if(!strcmp(line,"recording"))recording=atoi(value)!=0;
  else if(!strcmp(line,"volume")){int v=atoi(value);if(v>=0&&v<=100)volume=v;}
  else if(!strcmp(line,"audio"))audio=atoi(value)!=0;
  else if(!strcmp(line,"window_scale")){int v=atoi(value);if(v>=1&&v<=8)window_scale=v;}
  else if(!strcmp(line,"fullscreen")){int v=atoi(value);if(v>=0&&v<=2)fullscreen=v;}
  else if(!strcmp(line,"sources")){unsigned v=(unsigned)strtoul(value,NULL,10);if(v<=3)player.sources=v;}
  else if(!strcmp(line,"deadzone")){unsigned v=(unsigned)strtoul(value,NULL,10);if(v<=100)player.deadzone_pct=v;}
  else if(!strcmp(line,"guid"))snprintf(player.guid,sizeof player.guid,"%s",value);
  else if(!strcmp(line,"physical"))snprintf(player.physical_key,sizeof player.physical_key,"%s",value);
  else if(!strcmp(line,"name"))snprintf(player.name,sizeof player.name,"%s",value);
  else if(!strcmp(line,"audio_device"))snprintf(device,sizeof device,"%s",value);
  else for(int i=0;i<7;i++){char key[16];snprintf(key,sizeof key,"key%d",i);if(!strcmp(line,key)){unsigned v=(unsigned)strtoul(value,NULL,10);if(v<RI_MAX_KEYS)player.key_binding[i]=(uint16_t)v;}
   snprintf(key,sizeof key,"pad%d",i);if(!strcmp(line,key))player.pad_binding[i]=(uint32_t)strtoul(value,NULL,10);}
 }
 fclose(f);
}
void sms_settings_player(ri_player *out){*out=player;}
int sms_settings_volume(void){return volume;}
const char *sms_settings_audio_device(void){return device;}
bool sms_settings_audio_enabled(void){return audio!=0;}
int sms_settings_window_scale(void){return window_scale;}
int sms_settings_fullscreen(void){return fullscreen;}
bool sms_recording_enabled(void){return recording!=0;}
bool sms_recording_set(bool enabled){int previous=recording;recording=enabled?1:0;if(!save_settings()){recording=previous;snprintf(error,sizeof error,"Could not save diagnostics preference");return false;}return true;}
#ifdef SMS_HAVE_SDL
static int feature_count(void *ctx){(void)ctx;return 1+(int)sms_content_count();}
static int feature_get(void *ctx,int index,RecompLauncherCModFeature *out){
 (void)ctx;memset(out,0,sizeof *out);
 if(index){SmsContentInfo info;if(index<1||!sms_content_get((size_t)(index-1),&info))return 0;
  snprintf(out->id,sizeof out->id,"content");snprintf(out->package_id,sizeof out->package_id,"%s",info.id);
  snprintf(out->package_name,sizeof out->package_name,"%s",info.name);snprintf(out->package_version,sizeof out->package_version,"1");
  snprintf(out->name,sizeof out->name,"%s",info.name);snprintf(out->description,sizeof out->description,"%s",info.description);
  snprintf(out->group,sizeof out->group,"Content and runtime hooks");out->enabled=info.enabled;return 1;
 }
 snprintf(out->id,sizeof out->id,"recording");snprintf(out->package_id,sizeof out->package_id,"smsgg.diagnostics");
 snprintf(out->package_name,sizeof out->package_name,"Gameplay diagnostics");snprintf(out->package_version,sizeof out->package_version,"1");
 snprintf(out->name,sizeof out->name,"Gameplay diagnostics and input recording");snprintf(out->group,sizeof out->group,"Diagnostics");
 snprintf(out->description,sizeof out->description,"Record frame-indexed controls and gameplay checkpoints for troubleshooting. Enabled by default; your saved choice is remembered.");
 out->enabled=recording;return 1;
}
static int enable_feature(void *ctx,const char *package,const char *feature,int enabled){(void)ctx;
 if(!strcmp(package,"smsgg.diagnostics")&&!strcmp(feature,"recording"))return sms_recording_set(enabled!=0);
 return !strcmp(feature,"content")&&sms_content_enable(package,enabled);
}
static const char *last_error(void *ctx){(void)ctx;const char *content=sms_content_error();return *content?content:error;}
static int commit(void *ctx,const char *image){(void)ctx;return sms_content_prepare(image)&&save_settings();}
static int package_count(void *ctx){return feature_count(ctx);}
static int package_get(void *ctx,int index,RecompLauncherCModPackage *out){
 RecompLauncherCModFeature feature;if(!feature_get(ctx,index,&feature))return 0;memset(out,0,sizeof *out);
 snprintf(out->id,sizeof out->id,"%s",feature.package_id);snprintf(out->name,sizeof out->name,"%s",feature.package_name);
 snprintf(out->version,sizeof out->version,"1");snprintf(out->description,sizeof out->description,"%s",feature.description);out->enabled=feature.enabled;return 1;
}
static int install_archive(void *ctx,const char *path){(void)ctx;return sms_content_install(path);}
static int content_info(const char *package,SmsContentInfo *out){for(size_t i=0;i<sms_content_count();i++)if(sms_content_get(i,out)&&!strcmp(package,out->id))return 1;return 0;}
static int resource_count(void *ctx,const char *package,const char *feature){(void)ctx;SmsContentInfo info;return !strcmp(feature,"content")&&content_info(package,&info)&&info.has_source?1:0;}
static int resource_get(void *ctx,const char *package,const char *feature,int index,RecompLauncherCModResource *out){
 (void)ctx;SmsContentInfo info;if(index||strcmp(feature,"content")||!content_info(package,&info)||!info.has_source)return 0;memset(out,0,sizeof *out);
 snprintf(out->id,sizeof out->id,"original-image");snprintf(out->label,sizeof out->label,"Original cartridge image");
 snprintf(out->description,sizeof out->description,"Select the supported complete SMS or Game Gear cartridge image. Required content is validated and extracted automatically.");
 snprintf(out->path,sizeof out->path,"%s",info.source_path);snprintf(out->status,sizeof out->status,"%s",info.verified?"Original image verified":"Select the supported original image");
 snprintf(out->file_patterns,sizeof out->file_patterns,"*.sms,*.gg");snprintf(out->file_description,sizeof out->file_description,"Master System / Game Gear ROM");
 snprintf(out->format,sizeof out->format,"file");out->required=1;out->verified=info.verified;out->import_on_select=1;return 1;
}
static int resource_set(void *ctx,const char *package,const char *feature,const char *resource,const char *path){(void)ctx;return !strcmp(feature,"content")&&!strcmp(resource,"original-image")&&sms_content_import(package,path);}
static int resource_import(void *ctx,const char *package,const char *feature,const char *resource,const char *path,const char *image,void (*progress)(void*,float,const char*),void *user,char *why,size_t cap){
 (void)image;if(progress)progress(user,0,"Validating original cartridge image");
 int ok=resource_set(ctx,package,feature,resource,path);if(!ok&&why&&cap)snprintf(why,cap,"%s",sms_content_error());
 if(ok&&progress)progress(user,1,"Original content extracted");return ok;
}
int sms_launcher(const char *initial,char *rom,size_t cap){
 RecompLauncherCSettings settings;memset(&settings,0,sizeof settings);
 settings.window_scale=window_scale;settings.fullscreen=fullscreen;settings.enable_audio=audio;settings.audio_freq=48000;settings.volume=volume;settings.deadzone[0]=(int)player.deadzone_pct;
 settings.player_src[0]=(player.sources&RI_SOURCE_GAMEPAD)?2:(player.sources&RI_SOURCE_KEYBOARD)?1:0;
 snprintf(settings.player_gamepad_guid[0],sizeof settings.player_gamepad_guid[0],"%s",player.guid);
 for(int i=0;i<7;i++){settings.player_key_bind[0][i]=player.key_binding[i];settings.player_pad_bind[0][i]=(int)player.pad_binding[i];}
 snprintf(settings.audio_device,sizeof settings.audio_device,"%s",device);
 RecompLauncherCModProvider mods={0};mods.feature_count=feature_count;mods.feature_get=feature_get;mods.feature_enable=enable_feature;mods.commit=commit;mods.last_error=last_error;
 mods.package_count=package_count;mods.package_get=package_get;mods.install_archive=install_archive;mods.archive_extension=".smsmod";mods.archive_description="SMS / Game Gear content descriptor";
 mods.feature_resource_count=resource_count;mods.feature_resource_get=resource_get;mods.feature_resource_set_path=resource_set;mods.feature_resource_import=resource_import;
 RecompLauncherCGameInfo info={0};info.name=g_game_spec.display_name;info.region="USA / Europe";info.platform=g_game_spec.platform==GAME_PLATFORM_GG?"GAME GEAR":"MASTER SYSTEM";
 info.num_players=1;info.expected_crc=g_game_spec.expected_rom_crc32;info.has_expected_crc=1;info.rom_noun="ROM";info.settings_bindings=1;info.mods=&mods;
 info.config_path="smsgg-ui.ini";info.keybinds_path="smsgg-keybinds.ini";
 const char *patterns[]={g_game_spec.platform==GAME_PLATFORM_GG?"*.gg":"*.sms"};info.rom_patterns=patterns;info.num_rom_patterns=1;
 int result=recomp_launcher_run_window(g_game_spec.display_name,&settings,&info,NULL,initial,rom,cap);
 volume=settings.volume;audio=settings.enable_audio;window_scale=settings.window_scale;fullscreen=settings.fullscreen;player.deadzone_pct=(unsigned)settings.deadzone[0];
 player.sources=settings.player_src[0]==2?RI_SOURCE_GAMEPAD:settings.player_src[0]==1?RI_SOURCE_KEYBOARD:RI_SOURCE_NONE;
 snprintf(player.guid,sizeof player.guid,"%s",settings.player_gamepad_guid[0]);player.physical_key[0]=0;
 for(int i=0;i<7;i++){player.key_binding[i]=(uint16_t)settings.player_key_bind[0][i];player.pad_binding[i]=(uint32_t)settings.player_pad_bind[0][i];}
 snprintf(device,sizeof device,"%s",settings.audio_device);
 if(!save_settings()){fprintf(stderr,"Could not save settings\n");return 1;}
 return result;
}
#else
int sms_launcher(const char *initial,char *rom,size_t cap){(void)initial;(void)rom;(void)cap;return 2;}
#endif
