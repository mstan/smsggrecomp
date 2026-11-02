/* Resident immutable tile-row jobs, integer-exact at every output density.
 * OpenGL is optional: the SDL/CPU path has no GL link dependency. */
#include "host_gl.h"
#include "png_write.h"
#include <string.h>
#include <stdlib.h>
#ifdef SMS_GPU
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>
#define GL_API(X) \
 X(PFNGLCREATESHADERPROC,CreateShader) X(PFNGLSHADERSOURCEPROC,ShaderSource) \
 X(PFNGLCOMPILESHADERPROC,CompileShader) X(PFNGLGETSHADERIVPROC,GetShaderiv) \
 X(PFNGLGETSHADERINFOLOGPROC,GetShaderInfoLog) X(PFNGLDELETESHADERPROC,DeleteShader) \
 X(PFNGLCREATEPROGRAMPROC,CreateProgram) X(PFNGLATTACHSHADERPROC,AttachShader) \
 X(PFNGLLINKPROGRAMPROC,LinkProgram) X(PFNGLGETPROGRAMIVPROC,GetProgramiv) \
 X(PFNGLGETPROGRAMINFOLOGPROC,GetProgramInfoLog) X(PFNGLDELETEPROGRAMPROC,DeleteProgram) \
 X(PFNGLUSEPROGRAMPROC,UseProgram) X(PFNGLGETUNIFORMLOCATIONPROC,GetUniformLocation) \
 X(PFNGLUNIFORM1IPROC,Uniform1i) X(PFNGLUNIFORM2IPROC,Uniform2i) \
 X(PFNGLGENVERTEXARRAYSPROC,GenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC,BindVertexArray) \
 X(PFNGLDELETEVERTEXARRAYSPROC,DeleteVertexArrays) X(PFNGLGENFRAMEBUFFERSPROC,GenFramebuffers) \
 X(PFNGLBINDFRAMEBUFFERPROC,BindFramebuffer) X(PFNGLFRAMEBUFFERTEXTURE2DPROC,FramebufferTexture2D) \
 X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,CheckFramebufferStatus) X(PFNGLDELETEFRAMEBUFFERSPROC,DeleteFramebuffers) \
 X(PFNGLGENQUERIESPROC,GenQueries) X(PFNGLDELETEQUERIESPROC,DeleteQueries) \
 X(PFNGLBEGINQUERYPROC,BeginQuery) X(PFNGLENDQUERYPROC,EndQuery) \
 X(PFNGLGETQUERYOBJECTIVPROC,GetQueryObjectiv) X(PFNGLGETQUERYOBJECTUI64VPROC,GetQueryObjectui64v)
#define DECL(t,n) static t q##n;
GL_API(DECL)
#undef DECL
/* GL 1.1 functions also resolve through SDL, keeping Linux/Windows linking identical. */
static void (APIENTRY *qGenTextures)(GLsizei,GLuint*);
static void (APIENTRY *qDeleteTextures)(GLsizei,const GLuint*);
static void (APIENTRY *qBindTexture)(GLenum,GLuint);
static void (APIENTRY *qTexParameteri)(GLenum,GLenum,GLint);
static void (APIENTRY *qTexImage2D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
static void (APIENTRY *qTexSubImage2D)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*);
static void (APIENTRY *qViewport)(GLint,GLint,GLsizei,GLsizei);
static void (APIENTRY *qDrawArrays)(GLenum,GLint,GLsizei);
static void (APIENTRY *qGetIntegerv)(GLenum,GLint*);
static const GLubyte *(APIENTRY *qGetString)(GLenum);
static void (APIENTRY *qReadPixels)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
static void (APIENTRY *qClearColor)(GLfloat,GLfloat,GLfloat,GLfloat);
static void (APIENTRY *qClear)(GLbitfield);
static GLenum (APIENTRY *qGetError)(void);
static SDL_GLContext context;
static SDL_Window *window;
static GLuint jobs,target,fbo,vao,render_program,present_program,query[24];
static bool pending[8],resident;
static VdpFrameJob uploaded;
static int cx,cy,cw,ch,density;
static uint64_t frame_count,upload_bytes,gpu_ns[8192];
static uint64_t upload_ns[8192],render_ns[8192],present_ns[8192];
static unsigned gpu_count;
static const char *vertex=
 "#version 330 core\n"
 "out vec2 uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
 "uv=p;gl_Position=vec4(p*2.-1.,0.,1.);}";
static const char *fragment=
 "#version 330 core\n"
 "uniform usampler2D source;uniform int scale;uniform ivec2 crop;out vec4 color;"
 "uint pixel(uint w,int x){uint b=uint(7-x);return ((w>>b)&1u)|(((w>>(b+8u))&1u)<<1u)|(((w>>(b+16u))&1u)<<2u)|(((w>>(b+24u))&1u)<<3u);}"
 "void main(){ivec2 p=ivec2(gl_FragCoord.xy)/scale+crop;int x=p.x,y=p.y;"
 "uvec4 info=texelFetch(source,ivec2(104,y),0);uint c=info.x;"
 "if(info.y!=0u){uvec4 bg=texelFetch(source,ivec2(x/8,y),0);"
 "int u=(x&7)+int(bg.z>>16u),side=u/8;uint flags=bg[2+side];"
 "uint index=pixel(bg[side],(u&7)^((flags&512u)!=0u?7:0));"
 "uint pal=(flags&2048u)!=0u?16u:0u;uint k=pal+index;"
 "c=texelFetch(source,ivec2(96+int(k/4u),y),0)[int(k&3u)];"
 "if(info.w!=0u&&x<8)c=info.x;"
 "if((flags&4096u)==0u||index==0u){for(int s=int(info.z)-1;s>=0;s--){"
 "uvec4 sp=texelFetch(source,ivec2(32+s,y),0);int dx=x-int(sp.y);"
 "if(dx<0||dx>=8)continue;uint v=pixel(sp.x,dx);if(v!=0u){"
 "c=texelFetch(source,ivec2(100+int(v/4u),y),0)[int(v&3u)];break;}}}}"
 "color=vec4(float((c>>16u)&255u),float((c>>8u)&255u),float(c&255u),255.)/255.;}";
static const char *present_fragment=
 "#version 330 core\n"
 "uniform sampler2D source;in vec2 uv;out vec4 color;"
 "void main(){color=texture(source,vec2(uv.x,1.-uv.y));}";
static GLuint shader(GLenum kind,const char *text){
    GLuint s=qCreateShader(kind); GLint ok=0;
    qShaderSource(s,1,&text,NULL);qCompileShader(s);qGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[2048];qGetShaderInfoLog(s,sizeof log,NULL,log);fprintf(stderr,"[gpu] shader: %s\n",log);qDeleteShader(s);return 0;}
    return s;
}
static GLuint program(const char *frag){
    GLuint v=shader(GL_VERTEX_SHADER,vertex),f=shader(GL_FRAGMENT_SHADER,frag),p=0;GLint ok=0;
    if(v&&f){p=qCreateProgram();qAttachShader(p,v);qAttachShader(p,f);qLinkProgram(p);qGetProgramiv(p,GL_LINK_STATUS,&ok);}
    if(v)qDeleteShader(v);if(f)qDeleteShader(f);
    if(p&&!ok){char log[2048];qGetProgramInfoLog(p,sizeof log,NULL,log);fprintf(stderr,"[gpu] link: %s\n",log);qDeleteProgram(p);p=0;}
    return p;
}
static void texture(GLuint *id,GLint format,int w,int h,GLenum external,GLenum type){
    qGenTextures(1,id);qBindTexture(GL_TEXTURE_2D,*id);
    qTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    qTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    qTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    qTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    qTexImage2D(GL_TEXTURE_2D,0,format,w,h,0,external,type,NULL);
}
bool host_gl_init(SDL_Window *win,int x,int y,int w,int h,int scale){
    context=SDL_GL_CreateContext(win);if(!context)return false;window=win;
#define LOAD(t,n) q##n=(t)SDL_GL_GetProcAddress("gl" #n);if(!q##n)goto fail;
    GL_API(LOAD)
#undef LOAD
#define BASE(n) *(void **)(&q##n)=SDL_GL_GetProcAddress("gl" #n);if(!q##n)goto fail;
    BASE(GenTextures) BASE(DeleteTextures) BASE(BindTexture) BASE(TexParameteri)
    BASE(TexImage2D) BASE(TexSubImage2D) BASE(Viewport) BASE(DrawArrays) BASE(GetIntegerv)
    BASE(GetString) BASE(ReadPixels) BASE(ClearColor) BASE(Clear) BASE(GetError)
#undef BASE
    cx=x;cy=y;cw=w;ch=h;density=scale;GLint limit=0;qGetIntegerv(GL_MAX_TEXTURE_SIZE,&limit);
    while(density>1&&(w*density>limit||h*density>limit))density/=2;
    qGenFramebuffers(1,&fbo);qBindFramebuffer(GL_FRAMEBUFFER,fbo);
    for(;;){
        texture(&target,GL_RGBA8,w*density,h*density,GL_RGBA,GL_UNSIGNED_BYTE);
        qFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
        if(qGetError()==GL_NO_ERROR&&qCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE)break;
        qDeleteTextures(1,&target);target=0;if(density==1)goto fail;density/=2;
    }
    render_program=program(fragment);present_program=program(present_fragment);
    if(!render_program||!present_program)goto fail;
    texture(&jobs,GL_RGBA32UI,VDP_JOB_WIDTH,SMS_SCREEN_H,GL_RGBA_INTEGER,GL_UNSIGNED_INT);
    qGenVertexArrays(1,&vao);qBindVertexArray(vao);qGenQueries(24,query);
    GLenum error=qGetError();
    if(!jobs||!vao||!query[0]||!query[23]||error!=GL_NO_ERROR){
        fprintf(stderr,"[gpu] resident resources unavailable (GL error 0x%x)\n",(unsigned)error);
        goto fail;
    }
    SDL_GL_SetSwapInterval(0);resident=false;frame_count=upload_bytes=gpu_count=0;memset(pending,0,sizeof pending);
    fprintf(stderr,"[gpu] %s / %s; requested=%dx effective=%dx target=%dx%d\n",qGetString(GL_VENDOR),qGetString(GL_RENDERER),scale,density,w*density,h*density);
    return true;
fail:
    host_gl_shutdown();return false;
}
static void collect(unsigned slot,bool wait){
    if(!pending[slot])return;GLint ready=0;
    if(!wait){qGetQueryObjectiv(query[slot*3+2],GL_QUERY_RESULT_AVAILABLE,&ready);if(!ready)return;}
    GLuint64 ns[3]={0};for(unsigned i=0;i<3;i++)qGetQueryObjectui64v(query[slot*3+i],GL_QUERY_RESULT,&ns[i]);
    pending[slot]=false;
    if(gpu_count<8192){
        upload_ns[gpu_count]=ns[0];render_ns[gpu_count]=ns[1];present_ns[gpu_count]=ns[2];
        gpu_ns[gpu_count++]=ns[0]+ns[1]+ns[2];
    }
}
void host_gl_present(const VdpFrameJob *frame){
    unsigned slot=(unsigned)(frame_count%8);collect(slot,false);
    bool timed=!pending[slot];if(timed)qBeginQuery(GL_TIME_ELAPSED,query[slot*3]);
    qBindTexture(GL_TEXTURE_2D,jobs);
    for(int y=0;y<SMS_SCREEN_H;){
        if(resident&&!memcmp(&frame->line[y],&uploaded.line[y],sizeof(VdpLineJob))){y++;continue;}
        int start=y++;
        while(y<SMS_SCREEN_H&&(!resident||memcmp(&frame->line[y],&uploaded.line[y],sizeof(VdpLineJob))))y++;
        qTexSubImage2D(GL_TEXTURE_2D,0,0,start,VDP_JOB_WIDTH,y-start,GL_RGBA_INTEGER,GL_UNSIGNED_INT,&frame->line[start]);
        memcpy(&uploaded.line[start],&frame->line[start],(y-start)*sizeof(VdpLineJob));
        upload_bytes+=(y-start)*sizeof(VdpLineJob);
    }
    if(timed){qEndQuery(GL_TIME_ELAPSED);qBeginQuery(GL_TIME_ELAPSED,query[slot*3+1]);}
    resident=true;qBindFramebuffer(GL_FRAMEBUFFER,fbo);qViewport(0,0,cw*density,ch*density);
    qUseProgram(render_program);qUniform1i(qGetUniformLocation(render_program,"source"),0);
    qUniform1i(qGetUniformLocation(render_program,"scale"),density);
    qUniform2i(qGetUniformLocation(render_program,"crop"),cx,cy);qDrawArrays(GL_TRIANGLES,0,3);
    if(timed){qEndQuery(GL_TIME_ELAPSED);qBeginQuery(GL_TIME_ELAPSED,query[slot*3+2]);}
    qBindFramebuffer(GL_FRAMEBUFFER,0);int ww,wh;SDL_GL_GetDrawableSize(window,&ww,&wh);
    qViewport(0,0,ww,wh);qClearColor(0,0,0,1);qClear(GL_COLOR_BUFFER_BIT);
    int vw=ww,vh=(int)((int64_t)ww*ch/cw);if(vh>wh){vh=wh;vw=(int)((int64_t)wh*cw/ch);}
    qViewport((ww-vw)/2,(wh-vh)/2,vw,vh);qBindTexture(GL_TEXTURE_2D,target);
    qUseProgram(present_program);qUniform1i(qGetUniformLocation(present_program,"source"),0);qDrawArrays(GL_TRIANGLES,0,3);
    if(timed){qEndQuery(GL_TIME_ELAPSED);pending[slot]=true;}
    SDL_GL_SwapWindow(window);frame_count++;
}
static int compare_ns(const void *a,const void *b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return(x>y)-(x<y);}
static double pass_p95(const uint64_t *samples){
    uint64_t sorted[8192];if(!gpu_count)return 0;
    memcpy(sorted,samples,gpu_count*sizeof sorted[0]);
    qsort(sorted,gpu_count,sizeof sorted[0],compare_ns);
    return sorted[(gpu_count-1)*95/100]*1e-6;
}
void host_gl_report(FILE *out){
    for(unsigned i=0;i<8;i++)collect(i,true);
    fputs("{\"gpu_pass_ns\":[",out);
    for(unsigned i=0;i<gpu_count;i++)fprintf(out,"%s[%llu,%llu,%llu]",i?",":"",
        (unsigned long long)upload_ns[i],(unsigned long long)render_ns[i],(unsigned long long)present_ns[i]);
    fputs("],",out);
    qsort(gpu_ns,gpu_count,sizeof gpu_ns[0],compare_ns);
    fprintf(out,"\"upload_p95_ms\":%.6f,\"render_p95_ms\":%.6f,\"present_p95_ms\":%.6f,",
        pass_p95(upload_ns),pass_p95(render_ns),pass_p95(present_ns));
    fprintf(out,"\"frames\":%llu,\"effective_scale\":%d,\"upload_bytes\":%llu,\"gpu_samples\":%u,\"gpu_p50_ms\":%.6f,\"gpu_p95_ms\":%.6f,\"gpu_max_ms\":%.6f,\"target_bytes\":%llu}\n",
        (unsigned long long)frame_count,density,(unsigned long long)upload_bytes,gpu_count,
        gpu_count?gpu_ns[gpu_count/2]*1e-6:0,gpu_count?gpu_ns[(gpu_count-1)*95/100]*1e-6:0,
        gpu_count?gpu_ns[gpu_count-1]*1e-6:0,(unsigned long long)cw*ch*density*density*4);
}
bool host_gl_dump(const char *path){
    size_t count=(size_t)cw*ch*density*density;uint32_t *pixels=malloc(count*4);if(!pixels)return false;
    int w,h;if(!host_gl_readback(pixels,count,&w,&h)){free(pixels);return false;}
    bool ok=png_write_argb(path,pixels,cw*density,ch*density,cw*density)==0;free(pixels);return ok;
}
bool host_gl_readback(uint32_t *pixels,size_t capacity,int *width,int *height){
    if(!context||!pixels||!width||!height)return false;
    *width=cw*density;*height=ch*density;
    if(capacity<(size_t)*width * *height)return false;
    qBindFramebuffer(GL_FRAMEBUFFER,fbo);
    qReadPixels(0,0,*width,*height,GL_BGRA,GL_UNSIGNED_BYTE,pixels);
    return qGetError()==GL_NO_ERROR;
}
void host_gl_shutdown(void){
    if(!context)return;
    if(jobs)qDeleteTextures(1,&jobs);if(target)qDeleteTextures(1,&target);
    if(fbo)qDeleteFramebuffers(1,&fbo);if(vao)qDeleteVertexArrays(1,&vao);
    if(render_program)qDeleteProgram(render_program);if(present_program)qDeleteProgram(present_program);
    if(query[0])qDeleteQueries(24,query);
    SDL_GL_DeleteContext(context);context=NULL;jobs=target=fbo=vao=render_program=present_program=0;memset(query,0,sizeof query);
}
#else
bool host_gl_init(SDL_Window *w,int x,int y,int cw,int ch,int s){(void)w;(void)x;(void)y;(void)cw;(void)ch;(void)s;return false;}
void host_gl_present(const VdpFrameJob *f){(void)f;}
void host_gl_shutdown(void){}
void host_gl_report(FILE *f){fputs("{\"gpu\":false}\n",f);}
bool host_gl_dump(const char *p){(void)p;return false;}
bool host_gl_readback(uint32_t *p,size_t c,int *w,int *h){(void)p;(void)c;(void)w;(void)h;return false;}
#endif
