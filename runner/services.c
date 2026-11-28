/* Owner-thread service adapter. TCP never touches desktop windows or pickers. */
#include "services.h"
#include "glue.h"
#include "game_spec.h"
#include "content_mods.h"
#include "recomp_mod.h"
#include "diagnostics_server.h"
#include "recomp_diagnostics/replay_c.h"
#include "recomp_persistence/atomic_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SMS_HAVE_SDL
#include "host_sdl.h"
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif
static DiagServer *server;
static RecompReplayWriter *writer;
static rmod_context *mods;
static char original_sha[65],normalized_sha[65],plan_sha[65],status_path[1024];
static bool paused,quitting,override,show_ready,failed;
static bool has_window;
static uint8_t input;
static uint64_t pause_at,step_until;
static void hex(const uint8_t *bytes,size_t n,char *out){const char digits[]="0123456789abcdef";for(size_t i=0;i<n;i++){out[2*i]=digits[bytes[i]>>4];out[2*i+1]=digits[bytes[i]&15];}out[2*n]=0;}
static int nibble(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static int bytes(const char *s,uint8_t *out,size_t cap){size_t n=strlen(s);if(!n||(n&1)||n/2>cap)return -1;for(size_t i=0;i<n/2;i++){int a=nibble(s[2*i]),b=nibble(s[2*i+1]);if(a<0||b<0)return -1;out[i]=(uint8_t)((a<<4)|b);}return (int)(n/2);}
static void reply(const char *s){if(server)diag_server_reply(server,s);}
static void status(char *out,size_t cap){
 unsigned long pid;
#ifdef _WIN32
 pid=GetCurrentProcessId();
#else
 pid=(unsigned long)getpid();
#endif
 const char *controller="Scripted input";
#ifdef SMS_HAVE_SDL
 if(has_window&&!override)controller=host_controller_label();
#endif
 /* Controller names are logged separately to avoid unsafe JSON injection. */
 (void)controller;
 snprintf(out,cap,"{\"ok\":true,\"pid\":%lu,\"frame\":%llu,\"nmi_count\":%llu,\"paused\":%s,\"pad\":%u,\"recording\":%s,\"port\":%u,\"platform\":\"%s\",\"original_sha256\":\"%s\",\"normalized_sha256\":\"%s\",\"plan_sha256\":\"%s\",\"dispatch_misses\":%d,\"providers\":[\"z80-recomp-core\",\"sega-psg-core\",\"recomp-audio-core\",\"recomp-input-core\",\"recomp-presentation-core\",\"recomp-persistence-core\",\"recomp-mod-core\",\"recomp-diagnostics-core\"]}",
 pid,(unsigned long long)glue_frame_count(),(unsigned long long)glue_nmi_count(),paused?"true":"false",input,sms_recording_enabled()?"true":"false",server?diag_server_port(server):0,g_game_spec.platform==GAME_PLATFORM_GG?"Game Gear":"Master System",original_sha,normalized_sha,plan_sha,glue_dispatch_miss_count());
}
static void save_status(void){if(status_path[0]){char text[2048];status(text,sizeof text);if(!recomp_atomic_file_store(status_path,text,strlen(text)))failed=true;}}
static void pause_runtime(bool value){paused=value;
#ifdef SMS_HAVE_SDL
 if(has_window)host_audio_pause(value);
#endif
 save_status();
}
static int begin(void *unused){(void)unused;return glue_mod_begin();}
static int hook(void *unused,const char *id){(void)unused;return glue_mod_hook(id);}
static int read_bytes(void *unused,const char *space,uint64_t at,uint8_t *out,size_t n){(void)unused;return glue_mod_read(space,at,out,n);}
static int write_bytes(void *unused,const char *space,uint64_t at,const uint8_t *data,size_t n){(void)unused;return glue_mod_write(space,at,data,n);}
static void finish(void *unused,int committed){(void)unused;glue_mod_finish(committed);}
static void command(void *unused,DiagServer *s,const char *line){
 (void)unused;(void)s;
 if(!strcmp(line,"status")){char text[2048];status(text,sizeof text);reply(text);}
 else if(!strcmp(line,"pause")){step_until=0;pause_runtime(true);reply("{\"ok\":true}");}
 else if(!strcmp(line,"resume")){pause_at=0;step_until=0;pause_runtime(false);reply("{\"ok\":true}");}
 else if(!strcmp(line,"quit")){quitting=true;reply("{\"ok\":true}");}
 else if(!strncmp(line,"step ",5)){
  char *end;unsigned long long n=strtoull(line+5,&end,10);
  if(*end||!n||n>1000000||!paused)reply("{\"ok\":false,\"error\":\"step requires paused runtime and 1..1000000 frames\"}");
  else{step_until=glue_frame_count()+n;pause_runtime(false);reply("{\"ok\":true}");}
 }
 else if(!strncmp(line,"capture ",8)){reply(glue_capture(line+8)?"{\"ok\":true}":"{\"ok\":false,\"error\":\"capture failed\"}");}
 else if(!strncmp(line,"pad ",4)){
  if(!strcmp(line+4,"live")){override=false;input=0;glue_set_pad1(0);reply("{\"ok\":true}");}
  else{char *end;unsigned long value=strtoul(line+4,&end,0);if(*end||value>127)reply("{\"ok\":false}");else{override=true;input=(uint8_t)value;glue_set_pad1(input);reply("{\"ok\":true}");}}
 }
 else if(!strncmp(line,"record ",7)){
  if(strcmp(line+7,"on")&&strcmp(line+7,"off"))reply("{\"ok\":false}");
  else reply(sms_recording_set(!strcmp(line+7,"on"))?"{\"ok\":true}":"{\"ok\":false}");
 }
 else if(!strncmp(line,"patch ",6)){
  char space[32],expected[513],replacement[513],extra[2];unsigned long long at;uint8_t old[256],data[256],identity[32];
  int a,b;
  if(sscanf(line+6,"%31s %llx %512s %512s %1s",space,&at,expected,replacement,extra)!=4||(a=bytes(expected,old,sizeof old))<0||(b=bytes(replacement,data,sizeof data))!=a)reply("{\"ok\":false,\"error\":\"patch space offset expected_hex replacement_hex\"}");
  else{
   rmod_patch patch={space,(uint64_t)at,old,data,(size_t)a};
   rmod_engine engine={NULL,g_game_spec.short_name,original_sha,begin,read_bytes,write_bytes,hook,finish};
   int result=rmod_apply(mods,&patch,1,NULL,0,&engine,identity);
   if(result==1){hex(identity,32,plan_sha);save_status();reply("{\"ok\":true}");}
   else{if(result<0){failed=true;quitting=true;}reply("{\"ok\":false,\"error\":\"guard, conflict, identity or transaction rejected\"}");}
  }
 }
 else if(!strncmp(line,"read ",5)){
  char space[32],extra[2],text[600];unsigned long long at;unsigned n;uint8_t data[256];
  if(sscanf(line+5,"%31s %llx %u %1s",space,&at,&n,extra)!=3||!n||n>256||!glue_mod_read(space,at,data,n))reply("{\"ok\":false}");
  else{char encoded[513];hex(data,n,encoded);snprintf(text,sizeof text,"{\"ok\":true,\"hex\":\"%s\"}",encoded);reply(text);}
 }
 else if(!strncmp(line,"hook ",5)){
  char first[64],second[64],extra[2];int count=sscanf(line+5,"%63s %63s %1s",first,second,extra);
  if(count<1||count>2)reply("{\"ok\":false}");else{
   rmod_hook hooks[2]={{first},{second}};uint8_t identity[32];rmod_engine engine={NULL,g_game_spec.short_name,original_sha,begin,read_bytes,write_bytes,hook,finish};
   int result=rmod_apply(mods,NULL,0,hooks,(size_t)count,&engine,identity);if(result==1){hex(identity,32,plan_sha);reply("{\"ok\":true}");}else reply("{\"ok\":false,\"error\":\"unknown, conflicting or rolled-back hook\"}");
  }
 }
 else reply("{\"ok\":false,\"error\":\"unknown command\"}");
}
int sms_services_init(const char *rom,unsigned port,uint64_t stop,bool show,bool window,const char *path){
 (void)rom;pause_at=stop;show_ready=show;has_window=window;if(path)snprintf(status_path,sizeof status_path,"%s",path);
 size_t size;const uint8_t *image=glue_original_image(&size);uint8_t sha[32],plan[32];
 if(!rmod_sha256_bytes(image,size,sha)||!rmod_sha256_bytes("smsgg-runtime-plan-v1",21,plan))return 0;
 hex(sha,32,original_sha);image=glue_normalized_image(&size);
 if(!rmod_sha256_bytes(image,size,sha))return 0;hex(sha,32,normalized_sha);
 hex(plan,32,plan_sha);mods=rmod_create();
 if(!mods||!rmod_bind_plan(mods,g_game_spec.short_name,original_sha,plan_sha))return 0;
 if(!sms_content_prepare(rom)){fprintf(stderr,"[mods] %s\n",sms_content_error());return 0;}
 rmod_engine engine={NULL,g_game_spec.short_name,original_sha,begin,read_bytes,write_bytes,hook,finish};
 int content=sms_content_apply(mods,&engine,plan);if(content!=1){fprintf(stderr,"[mods] %s\n",sms_content_error());return 0;}
 if(sms_content_count())hex(plan,32,plan_sha);
 if(port<=65535){server=diag_server_create((uint16_t)port);if(!server||!diag_server_raw_handler(server,command,NULL))return 0;fprintf(stderr,"[diagnostics] loopback TCP %u\n",diag_server_port(server));}
 writer=recomp_replay_create_profile(RECOMP_REPLAY_HASHED64,2*1024*1024);
 char header[512];snprintf(header,sizeof header,"# SMSGG_INPUT_V1 platform=%s original_sha256=%s\n",g_game_spec.platform==GAME_PLATFORM_GG?"gg":"sms",original_sha);
 if(!writer||!recomp_replay_begin_with_sidecars(writer,"session.replay",header,NULL,0,NULL,0))return 0;
 save_status();return !failed;
}
void sms_services_pad(uint8_t mask){if(!override)input=mask;}
bool sms_services_input_override(void){return override;}
bool sms_services_fast_prepare(void){return pause_at&&glue_frame_count()<pause_at;}
int sms_services_frame(void){
 uint64_t frame=glue_frame_count();
 if((pause_at&&frame>=pause_at)||(step_until&&frame>=step_until)){
  pause_at=0;step_until=0;pause_runtime(true);
#ifdef SMS_HAVE_SDL
  if(show_ready&&has_window){host_show();show_ready=false;}
#endif
 }
 if(writer&&sms_recording_enabled()){
  char line[128];snprintf(line,sizeof line,"I %llu %u\n",(unsigned long long)frame,input);
  if(!recomp_replay_line(writer,line)){fprintf(stderr,"[diagnostics] %s\n",recomp_replay_error(writer));failed=true;quitting=true;}
 }
 do{
  if(server&&!diag_server_poll(server)){failed=true;quitting=true;}
  if(quitting){if(!server||!diag_server_pending_output(server))break;}
  if(!paused&&!quitting)break;
#ifdef SMS_HAVE_SDL
  if(has_window&&!host_poll()){quitting=true;break;}
#endif
#ifdef _WIN32
  Sleep(5);
#else
  struct timespec ts={0,5000000};nanosleep(&ts,NULL);
#endif
 }while(paused||quitting);
 return quitting?1:0;
}
bool sms_services_close(void){
 if(writer){if(!recomp_replay_end(writer))failed=true;const char *error=recomp_replay_destroy(writer);writer=NULL;if(error){fprintf(stderr,"[diagnostics] %s\n",error);failed=true;}}
 diag_server_destroy(server);server=NULL;rmod_destroy(mods);mods=NULL;
 return !failed;
}
