/* Opt-in deterministic QA controls. Writes reach the ordinary live WRAM bus;
 * guest code still performs all loading, graphics, collision and progression. */
#include "coverage_route.h"
#include "glue.h"
#include "include/sms_runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { WRITE, PAD, CAPTURE, EXPECT, BIT_OR } Op;
typedef struct {
    uint64_t frame;
    Op op;
    unsigned addr, value;
    char *path;
} Event;
static Event *events;
static size_t count, next_event;
static bool failed;

void coverage_route_close(void){
    for (size_t i=0;i<count;i++) free(events[i].path);
    free(events);events=NULL;count=next_event=0;failed=false;
}
static bool number(const char *s, int base, uint64_t max, uint64_t *out){
    char *end;
    if (!s || !*s || *s=='-' || *s=='+') return false;
    errno=0;unsigned long long n=strtoull(s,&end,base);
    if (errno || *end || n>max) return false;
    *out=(uint64_t)n;return true;
}
static bool pad(const char *s, unsigned *out){
    *out=0;
    if (!strcmp(s,"-")) return true;
    for (;*s;s++) switch (*s){
        case 'U':*out|=SMS_PAD_UP;break;case 'D':*out|=SMS_PAD_DOWN;break;
        case 'L':*out|=SMS_PAD_LEFT;break;case 'R':*out|=SMS_PAD_RIGHT;break;
        case 'A':*out|=SMS_PAD_B1;break;case 'B':*out|=SMS_PAD_B2;break;
        case 'S':*out|=SMS_PAD_START;break;default:return false;
    }
    return true;
}
bool coverage_route_load(const char *path,uint32_t rom_crc){
    coverage_route_close();FILE *f=fopen(path,"r");if (!f) return false;
    char line[1200];unsigned line_no=0;bool identity=false,ok=true;
    while (ok && fgets(line,sizeof line,f)){
        line_no++;
        size_t len=strlen(line);
        if (len==sizeof line-1 && line[len-1]!='\n'){ok=false;break;}
        while (len && (line[len-1]=='\n'||line[len-1]=='\r'||line[len-1]==' '||line[len-1]=='\t')) line[--len]=0;
        char *p=line;while (*p==' '||*p=='\t')p++;
        if (!*p||*p=='#')continue;
        char first[32],op[32],a[32],b[32],extra[2];int used=0;
        if (!identity){
            uint64_t crc;
            if (sscanf(p,"%31s %31s %1s",first,a,extra)!=2 || strcmp(first,"rom") ||
                !number(a,16,UINT32_MAX,&crc) || crc!=rom_crc){ok=false;break;}
            identity=true;continue;
        }
        Event e={0};uint64_t addr,value;
        if (sscanf(p,"%31s %31s %n",first,op,&used)!=2 || !used ||
            !number(first,10,UINT64_MAX-1,&e.frame) ||
            (count && e.frame<events[count-1].frame) || count>=65536){ok=false;break;}
        char *args=p+used;
        if (!strcmp(op,"capture")){
            if (!*args){ok=false;break;}
            e.op=CAPTURE;
            /* Relative capture names follow the route file, including spaces. */
            const char *slash=strrchr(path,'/'),*back=strrchr(path,'\\');
            if (back && (!slash||back>slash))slash=back;
            size_t dir=slash?(size_t)(slash-path+1):0;
            bool absolute=args[0]=='/'||args[0]=='\\'||(strlen(args)>1 && args[1]==':');
            if (absolute)dir=0;
            e.path=malloc(dir+strlen(args)+1);
            if (!e.path){ok=false;break;}
            memcpy(e.path,path,dir);strcpy(e.path+dir,args);
        }else if (!strcmp(op,"pad")){
            e.op=PAD;
            if (sscanf(args,"%31s %1s",a,extra)!=1 || !pad(a,&e.value)){ok=false;break;}
        }else if (!strcmp(op,"write") || !strcmp(op,"expect") || !strcmp(op,"or")){
            e.op=!strcmp(op,"write")?WRITE:!strcmp(op,"or")?BIT_OR:EXPECT;
            if (sscanf(args,"%31s %31s %1s",a,b,extra)!=2 ||
                !number(a,16,0xDFFF,&addr) || addr<0xC000 || !number(b,16,255,&value)){ok=false;break;}
            e.addr=(unsigned)addr;e.value=(unsigned)value;
        }else {ok=false;break;}
        Event *grown=realloc(events,(count+1)*sizeof(*events));
        if (!grown){free(e.path);ok=false;break;}
        events=grown;events[count++]=e;
    }
    if (ferror(f))ok=false;
    fclose(f);
    if (!identity||!count)ok=false;
    if (!ok){fprintf(stderr,"[route] invalid ROM identity or command at line %u\n",line_no);coverage_route_close();}
    return ok;
}
uint64_t coverage_route_last_frame(void){return count?events[count-1].frame:0;}
bool coverage_route_failed(void){return failed;}
int coverage_route_frame(uint64_t frame){
    while (next_event<count && events[next_event].frame<=frame){
        Event *e=events+next_event++;
        switch (e->op){
            case WRITE:sms_write8((uint16_t)e->addr,(uint8_t)e->value);break;
            case BIT_OR:sms_write8((uint16_t)e->addr,(uint8_t)(sms_read8((uint16_t)e->addr)|e->value));break;
            case PAD:glue_set_pad1((uint8_t)e->value);break;
            case CAPTURE:if (!glue_capture(e->path))failed=true;break;
            case EXPECT:if (sms_read8((uint16_t)e->addr)!=e->value)failed=true;break;
        }
        if (failed){fprintf(stderr,"[route] command %llu failed at frame %llu\n",
            (unsigned long long)next_event,(unsigned long long)frame);return 1;}
    }
    return 0;
}
