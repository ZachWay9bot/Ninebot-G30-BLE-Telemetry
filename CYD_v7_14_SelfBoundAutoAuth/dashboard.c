#include "dashboard.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 4
#error "This CYD portrait release is tested with LVGL 9.4.x."
#endif

static const uint32_t BG=0x050910, WHITE=0xf3f8ff, MUTED=0x9aaebe;
static const uint32_t TRACK=0x172638, EDGE=0x2a3b4f, GREEN=0x56e7ac, WARN=0xffb561;
enum { CX=120, CY=114 };
static lv_obj_t *root;
static dash_data_t sample;
static uint32_t clock_now,last_sample,last_frame,last_values;
static bool have_sample,was_live,needle_visible,light_on,light_valid,battery_valid;
static bool delta_active;
static bool load_valid,load_regen;
static int load_percent=-1,load_fill_px=-1;
static float shown_speed,draw_angle=135;
static int speed_number=-1,battery_steps=-1,battery_fill=-1;
static uint32_t battery_color;
static unsigned speed_max=60;
static int scale_label_count=7;
static dash_theme_t theme=DASH_RED;

typedef struct { float x1,y1,x2,y2; int width; } segment_t;
static segment_t ticks[41],needle;
#define MAX_SCALE_LABELS 9
static struct { int x,y; char text[5]; } scale_labels[MAX_SCALE_LABELS];
typedef struct { int x,y,w,h; const lv_font_t *font; lv_text_align_t align;
                 char text[32]; uint32_t color; } field_t;
enum { LINK, MODE, SPEED, HINT, SOC, VOLT, POWER, CURRENT, BMS_TEMP, TRIP, ODO, LOAD, FIELD_COUNT };
static field_t fields[FIELD_COUNT]={
    {172, 7, 50,18,&lv_font_montserrat_14,LV_TEXT_ALIGN_CENTER,"",0},
    { 87,73, 66,18,&lv_font_montserrat_14,LV_TEXT_ALIGN_CENTER,"",0},
    { 52,84,136,58,&lv_font_montserrat_14,LV_TEXT_ALIGN_CENTER,"",0},
    { 52,153,136,18,&lv_font_montserrat_14,LV_TEXT_ALIGN_CENTER,"",0},
    {114,168, 46,20,&lv_font_montserrat_16,LV_TEXT_ALIGN_LEFT,"",0},
    { 12,239,106,24,&lv_font_montserrat_20,LV_TEXT_ALIGN_LEFT,"",0},
    {128,239,100,24,&lv_font_montserrat_20,LV_TEXT_ALIGN_LEFT,"",0},
    { 12,259,106,18,&lv_font_montserrat_14,LV_TEXT_ALIGN_LEFT,"",0},
    {128,259,100,18,&lv_font_montserrat_14,LV_TEXT_ALIGN_LEFT,"",0},
    { 12,277,109,18,&lv_font_montserrat_14,LV_TEXT_ALIGN_LEFT,"",0},
    {123,277,105,18,&lv_font_montserrat_14,LV_TEXT_ALIGN_RIGHT,"",0},
    { 75,199, 90,18,&lv_font_montserrat_14,LV_TEXT_ALIGN_CENTER,"",0}
};
typedef struct { lv_layer_t *layer; int ox,oy; lv_area_t clip; } painter_t;

static float bound(float v,float lo,float hi) { return fminf(hi,fmaxf(lo,v)); }
static uint32_t accent(void) { return theme==DASH_RED?0xff593c:0x20caff; }
static uint32_t mix(uint32_t a,uint32_t b,float f) {
    uint32_t c=0;
    for(unsigned s=0;s<=16;s+=8) {
        float x=(float)((a>>s)&255),y=(float)((b>>s)&255);
        c|=(uint32_t)lroundf(x+(y-x)*f)<<s;
    }
    return c;
}
static bool overlaps(const lv_area_t *a,const lv_area_t *b) {
    return a->x1<=b->x2 && a->x2>=b->x1 && a->y1<=b->y2 && a->y2>=b->y1;
}
static bool visible(painter_t *p,int x,int y,int w,int h) {
    lv_area_t a={x,y,x+w-1,y+h-1}; return overlaps(&a,&p->clip);
}
static void damage(lv_area_t a) {
    if(!root) return;
    lv_area_t pos; lv_obj_get_coords(root,&pos);
    a.x1+=pos.x1; a.x2+=pos.x1; a.y1+=pos.y1; a.y2+=pos.y1;
    lv_obj_invalidate_area(root,&a);
}
static void damage_rect(int x,int y,int w,int h) {
    lv_area_t a={x,y,x+w-1,y+h-1}; damage(a);
}
static void field_set(int id,const char *value,uint32_t color) {
    field_t *f=&fields[id];
    if(f->color==color && strcmp(f->text,value)==0) return;
    snprintf(f->text,sizeof(f->text),"%s",value); f->color=color;
    damage_rect(f->x,f->y,f->w,f->h);
}
static void field_number(int id,float value,const char *fmt,uint32_t color) {
    char b[32];
    if(isfinite(value)) snprintf(b,sizeof(b),fmt,(double)value); else strcpy(b,"--");
    field_set(id,b,color);
}
static void paint_rect(painter_t *p,int x,int y,int w,int h,int radius,
                       uint32_t color,uint32_t border,int bw) {
    if(w<=0||h<=0||!visible(p,x,y,w,h)) return;
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color=lv_color_hex(color); d.radius=radius;
    d.border_color=lv_color_hex(border); d.border_width=bw;
    lv_area_t a={p->ox+x,p->oy+y,p->ox+x+w-1,p->oy+y+h-1};
    lv_draw_rect(p->layer,&d,&a);
}
static lv_area_t segment_box(segment_t s,int pad) {
    lv_area_t a={(int)floorf(fminf(s.x1,s.x2))-pad,(int)floorf(fminf(s.y1,s.y2))-pad,
                 (int)ceilf(fmaxf(s.x1,s.x2))+pad,(int)ceilf(fmaxf(s.y1,s.y2))+pad};
    return a;
}
static void paint_line(painter_t *p,segment_t s,int width,uint32_t color) {
    lv_area_t a=segment_box(s,width+2); if(!overlaps(&a,&p->clip)) return;
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.p1.x=p->ox+s.x1; d.p1.y=p->oy+s.y1;
    d.p2.x=p->ox+s.x2; d.p2.y=p->oy+s.y2;
    d.width=width; d.color=lv_color_hex(color);
    /* Match the LVGL 9.4 SW line reject bounds on every partial buffer.
       Otherwise endpoint antialiasing can differ at a buffer boundary. */
    lv_area_t saved=p->layer->_clip_area;
    lv_area_t line_clip={(int)fminf(d.p1.x,d.p2.x)-width/2,
                         (int)fminf(d.p1.y,d.p2.y)-width/2,
                         (int)fmaxf(d.p1.x,d.p2.x)+width/2,
                         (int)fmaxf(d.p1.y,d.p2.y)+width/2};
    if(!lv_area_intersect(&p->layer->_clip_area,&saved,&line_clip)) {
        p->layer->_clip_area=saved;return;
    }
    lv_draw_line(p->layer,&d);p->layer->_clip_area=saved;
}
static void paint_arc(painter_t *p,int cx,int cy,int radius,int width,float start,float end,uint32_t color) {
    if(end-start<0.1f) return;
    /* In particular, the central speed digits must not rebuild all arc tasks. */
    int dx1=abs(p->clip.x1-cx),dx2=abs(p->clip.x2-cx);
    int dy1=abs(p->clip.y1-cy),dy2=abs(p->clip.y2-cy);
    int dx=dx1>dx2?dx1:dx2,dy=dy1>dy2?dy1:dy2;
    int inner=radius-width-3;
    if(inner>0 && dx*dx+dy*dy<inner*inner) return;
    lv_area_t a; lv_draw_arc_get_area(cx,cy,radius,start,end,width,false,&a);
    a.x1-=3;a.x2+=3;a.y1-=3;a.y2+=3;
    if(!overlaps(&a,&p->clip)) return;
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.center.x=p->ox+cx;d.center.y=p->oy+cy;d.radius=radius;d.width=width;
    d.start_angle=fmodf(start,360);d.end_angle=fmodf(end,360);
    d.color=lv_color_hex(color);lv_draw_arc(p->layer,&d);
}
static void paint_text(painter_t *p,int x,int y,int w,const char *s,const lv_font_t *font,
                       uint32_t color,lv_text_align_t align) {
    if(!s[0]||!visible(p,x,y,w,font->line_height+2)) return;
    lv_draw_label_dsc_t d;lv_draw_label_dsc_init(&d);
    /* Every string is a literal or persistent field/scale buffer; no strdup per draw. */
    d.text=s;d.text_local=0;d.font=font;d.color=lv_color_hex(color);d.align=align;d.flag=0;
    lv_area_t a={p->ox+x,p->oy+y,p->ox+x+w-1,p->oy+y+font->line_height-1};
    lv_draw_label(p->layer,&d,&a);
}
static void paint_center_grid(painter_t *p,int cx,int cy,int radius) {
    const uint32_t hcol=0x1b2b3a, vcol=0x142332, mid=0x284056;
    for(int yy=-radius+5; yy<=radius-5; yy+=6) {
        float inside=(float)(radius*radius - yy*yy);
        if(inside <= 0) continue;
        int half=(int)floorf(sqrtf(inside));
        uint32_t col=(yy==0)?mid:hcol;
        paint_line(p,(segment_t){cx-half+2,cy+yy,cx+half-2,cy+yy,1},1,col);
    }
    for(int xx=-radius+5; xx<=radius-5; xx+=6) {
        float inside=(float)(radius*radius - xx*xx);
        if(inside <= 0) continue;
        int half=(int)floorf(sqrtf(inside));
        uint32_t col=(xx==0)?mid:vcol;
        paint_line(p,(segment_t){cx+xx,cy-half+2,cx+xx,cy+half-2,1},1,col);
    }
}
static uint8_t seg_mask(char c) {
    switch(c) {
        case '0': return 0x3f; /* a b c d e f */
        case '1': return 0x06; /* b c */
        case '2': return 0x5b; /* a b d e g */
        case '3': return 0x4f; /* a b c d g */
        case '4': return 0x66; /* b c f g */
        case '5': return 0x6d; /* a c d f g */
        case '6': return 0x7d; /* a c d e f g */
        case '7': return 0x07; /* a b c */
        case '8': return 0x7f; /* all */
        case '9': return 0x6f; /* a b c d f g */
        case '-': return 0x40; /* g */
        default:  return 0x00;
    }
}
static void paint_7seg_digit(painter_t *p,int x,int y,int w,int h,int t,char c,uint32_t on,uint32_t off) {
    uint8_t m=seg_mask(c);
    int r=t/2; if(r<1) r=1;
    int hh=(h-3*t)/2;
    struct seg { int x,y,w,h; uint8_t bit; } segs[7]={
        {x+t,     y,          w-2*t, t,      0x01}, /* a */
        {x+w-t,   y+t,        t,     hh,     0x02}, /* b */
        {x+w-t,   y+2*t+hh,   t,     hh,     0x04}, /* c */
        {x+t,     y+h-t,      w-2*t, t,      0x08}, /* d */
        {x,       y+2*t+hh,   t,     hh,     0x10}, /* e */
        {x,       y+t,        t,     hh,     0x20}, /* f */
        {x+t,     y+t+hh,     w-2*t, t,      0x40}, /* g */
    };
    for(int i=0;i<7;i++) {
        uint32_t col=(m & segs[i].bit)?on:off;
        uint32_t border=(m & segs[i].bit)?mix(on,WHITE,0.20f):off;
        paint_rect(p,segs[i].x,segs[i].y,segs[i].w,segs[i].h,r,col,border,0);
    }
}
static void paint_speed_display_7seg(painter_t *p,const char *text,uint32_t color) {
    if(!text[0]) return;
    int len=(int)strlen(text);
    if(len<1) return;
    if(len>3) len=3;
    const int dw=23, dh=43, th=4, gap=6;
    int total=len*dw + (len-1)*gap;
    int startx=CX - total/2;
    int y=89;
    uint32_t panel=0x0a121b, edge=0x23384b;
    uint32_t off=mix(panel,color,0.18f);
    paint_rect(p,startx-6,y-5,total+12,dh+10,5,panel,edge,1);
    for(int i=0;i<len;i++) paint_7seg_digit(p,startx+i*(dw+gap),y,dw,dh,th,text[i],color,off);
}
static segment_t radial_segment(float angle,float inner,float outer,int width) {
    float a=angle*0.01745329252f,c=cosf(a),s=sinf(a);
    segment_t v={CX+inner*c,CY+inner*s,CX+outer*c,CY+outer*s,width};return v;
}
static void make_scale(void) {
    for(int i=0;i<=40;i++) {
        float a=135+6.75f*i; bool major=i%5==0;
        ticks[i]=radial_segment(a,major?83:86,90,major?2:1);
    }
    if(speed_max == 60) scale_label_count = 7;
    else scale_label_count = 9;
    for(int i=0;i<MAX_SCALE_LABELS;i++) scale_labels[i].text[0]=0;
    for(int i=0;i<scale_label_count;i++) {
        float a=135.0f + 270.0f * ((float)i / (float)(scale_label_count - 1));
        float r=a*0.01745329252f;
        unsigned value=(unsigned)lroundf((float)speed_max * (float)i / (float)(scale_label_count - 1));
        scale_labels[i].x=(int)lroundf(CX+73*cosf(r))-15;
        scale_labels[i].y=(int)lroundf(CY+73*sinf(r))-7;
        snprintf(scale_labels[i].text,5,"%u",value);
    }
}
static void draw(lv_event_t *e) {
    lv_layer_t *l=lv_event_get_layer(e);lv_area_t a;lv_obj_get_coords(root,&a);
    painter_t p={l,a.x1,a.y1,l->_clip_area};
    p.clip.x1-=p.ox;p.clip.x2-=p.ox;p.clip.y1-=p.oy;p.clip.y2-=p.oy;
    uint32_t ac=accent();

    /* Same visual hierarchy as the 320x480 freeze, compressed to 240x320. */
    const uint32_t top_bg=0x0b131e, top_edge=0x263849;
    const uint32_t ble_col=was_live?0x52d6ff:0x536474;
    const uint32_t light_col=light_valid?(light_on?0xffd45a:0x6f8090):0x42515f;
    paint_rect(&p,8,4,225,24,12,top_bg,top_edge,1);
    paint_line(&p,(segment_t){20,7,20,24,2},2,ble_col);
    paint_line(&p,(segment_t){20,7,26,11,2},2,ble_col);
    paint_line(&p,(segment_t){26,11,16,19,2},2,ble_col);
    paint_line(&p,(segment_t){16,11,26,19,2},2,ble_col);
    paint_line(&p,(segment_t){26,19,20,24,2},2,ble_col);
    paint_text(&p,31,7,34,"BLE",&lv_font_montserrat_14,was_live?WHITE:MUTED,LV_TEXT_ALIGN_LEFT);
    paint_rect(&p,66,13,6,6,3,was_live?GREEN:0x42515f,was_live?0xa2f4cf:0x596978,1);
    paint_line(&p,(segment_t){78,7,78,25,1},1,0x1d2b39);
    paint_arc(&p,92,16,5,2,105,255,light_col);
    paint_line(&p,(segment_t){94,11,94,21,2},2,light_col);
    for(int i=-1;i<=1;i++) paint_line(&p,(segment_t){99,13+i*3,104,13+i*3,1},1,light_col);
    paint_text(&p,109,7,40,"LICHT",&lv_font_montserrat_14,light_valid?(light_on?WHITE:MUTED):0x687887,LV_TEXT_ALIGN_LEFT);
    paint_line(&p,(segment_t){155,7,155,25,1},1,0x1d2b39);
    paint_rect(&p,163,13,6,6,3,was_live?GREEN:0x7a3434,was_live?0xa2f4cf:0x9b4c4c,1);

    paint_arc(&p,CX,CY,93,1,135,405,0x2c3d50);
    paint_arc(&p,CX,CY,92,1,135,405,0x111c2a);
    paint_arc(&p,CX,CY,83,20,135,405,0x0d1926);
    paint_arc(&p,CX,CY,83,1,135,405,0x254059);
    if(needle_visible && draw_angle>135.1f) {
        paint_arc(&p,CX,CY,82,19,135,draw_angle,mix(BG,ac,0.30f));
        for(int i=0;i<18;i+=3)
            paint_arc(&p,CX,CY,82-i,4,135,draw_angle,mix(BG,ac,0.28f+0.49f*(18-i)/18));
        paint_arc(&p,CX,CY,83,2,135,draw_angle,ac);
    }
    for(int i=0;i<=40;i++) {
        float tick_value=(speed_max*(float)i)/40.0f;
        bool from30=tick_value>=29.99f;
        uint32_t tick_color=(i%5)?(from30?0xb64a4a:0x455a70):(from30?0xff6b63:0xc7d4e2);
        paint_line(&p,ticks[i],ticks[i].width,tick_color);
    }
    for(int i=0;i<scale_label_count;i++)
        paint_text(&p,scale_labels[i].x,scale_labels[i].y,30,scale_labels[i].text,
                   &lv_font_montserrat_14,0xdce6f1,LV_TEXT_ALIGN_CENTER);

    paint_rect(&p,CX-53,CY-53,106,106,53,0x080f19,0x24364b,1);
    paint_arc(&p,CX,CY,53,1,185,350,0x334759);
    paint_center_grid(&p,CX,CY,47);
    if(needle_visible) {
        paint_line(&p,needle,5,mix(BG,ac,0.28f));
        paint_line(&p,needle,2,WHITE);
    }
    paint_rect(&p,87,70,66,19,5,0x102333,0x294052,1);
    paint_speed_display_7seg(&p,fields[SPEED].text,fields[SPEED].color?fields[SPEED].color:WHITE);
    paint_text(&p,90,140,60,"km/h",&lv_font_montserrat_14,MUTED,LV_TEXT_ALIGN_CENTER);

    /* Original v6.15 battery treatment: 20 segmented arc + small battery icon. */
    for(int i=0;i<20;i++) {
        float start=52+i*3.8f;
        paint_arc(&p,CX,CY,84,5,start,start+2.9f,
                  battery_valid && (20-i)<=battery_steps?battery_color:TRACK);
    }
    uint32_t bc=battery_valid?battery_color:MUTED;
    paint_rect(&p,94,174,14,7,2,BG,bc,1);
    paint_rect(&p,108,176,2,3,0,bc,bc,0);
    if(battery_valid)paint_rect(&p,96,176,battery_fill,3,0,bc,bc,0);

    const uint32_t delta_red=0xff3b30,delta_red_dim=0x5a1717;
    const uint32_t delta_shell=delta_active?0x211014:0x101820;
    const uint32_t delta_edge=delta_active?0x6f2426:0x2b3b4a;
    paint_rect(&p,81,151,84,22,11,delta_shell,delta_edge,1);
    paint_rect(&p,88,156,10,10,5,delta_active?delta_red_dim:0x1f2a34,
               delta_active?0x7d2525:0x344554,1);
    paint_rect(&p,91,159,5,5,3,delta_active?delta_red:0x465664,
               delta_active?0xff867d:0x5d6d7a,1);
    paint_text(&p,103,154,53,"DELTA",&lv_font_montserrat_14,
               delta_active?WHITE:MUTED,LV_TEXT_ALIGN_LEFT);

    paint_rect(&p,30,209,180,5,2,TRACK,TRACK,0);
    paint_rect(&p,119,207,2,9,0,0x52677a,0x52677a,0);
    if(load_valid && load_fill_px>0) {
        if(load_regen) paint_rect(&p,120-load_fill_px,209,load_fill_px,5,2,GREEN,GREEN,0);
        else paint_rect(&p,120,209,load_fill_px,5,2,ac,ac,0);
    }

    paint_line(&p,(segment_t){12,216,228,216,1},1,EDGE);
    paint_line(&p,(segment_t){120,225,120,267,1},1,0x1a293a);
    paint_text(&p,12,226,106,"SPANNUNG",&lv_font_montserrat_14,MUTED,LV_TEXT_ALIGN_LEFT);
    paint_text(&p,128,226,100,"LEISTUNG",&lv_font_montserrat_14,MUTED,LV_TEXT_ALIGN_LEFT);
    paint_line(&p,(segment_t){12,272,228,272,1},1,EDGE);

    for(int i=0;i<FIELD_COUNT;i++) {
        if(i==SPEED) continue;
        field_t *f=&fields[i];
        paint_text(&p,f->x,f->y,f->w,f->text,f->font,f->color,f->align);
    }
}
bool dashboard_is_live(uint32_t now) {
    return root && have_sample && sample.connected && (uint32_t)(now-last_sample)<=DASH_STALE_MS;
}
static void update_metadata(bool live) {
    uint32_t ac=accent();char b[32];
    if(live!=was_live) damage_rect(8,4,225,24);
    field_set(LINK,sample.demo?"DEMO":live?"ONLINE":"OFFLINE",sample.demo?MUTED:live?GREEN:WARN);
    field_set(MODE,sample.mode==DASH_ECO?"ECO":sample.mode==DASH_DRIVE?"DRIVE":"SPORT",
              sample.mode==DASH_ECO?GREEN:ac);
    if(light_on!=sample.headlight || light_valid!=sample.headlight_valid){
        light_on=sample.headlight;light_valid=sample.headlight_valid;damage_rect(8,4,225,24);
    }
    /* MAX km/h display removed. Keep HINT only for real warnings; normal live
       operation is represented by the DELTA LED drawn in this area. */
    if(live && sample.fault_code)snprintf(b,sizeof(b),"FEHLER %u",sample.fault_code);
    else if(!live)strcpy(b,"KEINE DATEN");
    else if(!isfinite(sample.speed_kmh))strcpy(b,"SPEED FEHLT");
    else b[0]=0;
    field_set(HINT,b,(!live||sample.fault_code)?WARN:MUTED);
    bool new_delta=live && sample.delta_active;
    if(new_delta!=delta_active) { delta_active=new_delta; damage_rect(80,150,86,24); }
}
static void update_values(bool live) {
    char b[32],value[16];
    bool bv=live&&isfinite(sample.battery_percent);
    int steps=bv?(int)lroundf(sample.battery_percent/5):-1;
    uint32_t bc=sample.battery_percent<=15?0xff7151:accent();
    if(bv!=battery_valid || steps!=battery_steps || bc!=battery_color)damage_rect(30,160,180,34);
    battery_valid=bv;battery_steps=steps;battery_color=bc;
    field_number(SOC,bv?sample.battery_percent:NAN,"%.0f%%",WHITE);
    /* Fill inside the battery icon can change even within the same 5% segment. */
    int fill=bv?(int)lroundf(10*sample.battery_percent/100):-1;
    if(fill!=battery_fill){damage_rect(92,172,20,11);battery_fill=fill;}
    field_number(VOLT,live?sample.voltage_v:NAN,"%.1f V",WHITE);

    const float current=live?sample.battery_current_a:NAN;
    const float power=(live&&isfinite(sample.voltage_v)&&isfinite(sample.battery_current_a))
                        ? sample.voltage_v*sample.battery_current_a : NAN;
    if(isfinite(power)) {
        if(fabsf(power)>=10000.0f) snprintf(b,sizeof(b),"%.1f kW",(double)(power/1000.0f));
        else snprintf(b,sizeof(b),"%.0f W",(double)power);
    } else strcpy(b,"--");
    field_set(POWER,b,isfinite(power)&&power<0?GREEN:WHITE);

    if(isfinite(current)) snprintf(b,sizeof(b),"STROM %+.1f A",(double)current);
    else strcpy(b,"STROM -- A");
    field_set(CURRENT,b,isfinite(current)&&current<0?GREEN:MUTED);

    bool new_load_valid=isfinite(current);
    bool new_load_regen=new_load_valid && current < -0.15f;
    float full_a=DASH_LOAD_FULL_CURRENT_A>0.1f?DASH_LOAD_FULL_CURRENT_A:30.0f;
    int new_load_percent=new_load_valid?(int)lroundf(bound(fabsf(current)/full_a*100.0f,0.0f,100.0f)):-1;
    int new_load_fill=new_load_valid?(int)lroundf(89.0f*new_load_percent/100.0f):0;
    if(new_load_valid && fabsf(current)<0.15f) { new_load_percent=0; new_load_fill=0; new_load_regen=false; }
    if(new_load_valid!=load_valid || new_load_regen!=load_regen ||
       new_load_percent!=load_percent || new_load_fill!=load_fill_px) {
        damage_rect(28,197,184,20);
    }
    load_valid=new_load_valid; load_regen=new_load_regen;
    load_percent=new_load_percent; load_fill_px=new_load_fill;
    if(!load_valid) field_set(LOAD,"LOAD --",MUTED);
    else if(load_regen) { snprintf(b,sizeof(b),"REGEN %d%%",load_percent); field_set(LOAD,b,GREEN); }
    else { snprintf(b,sizeof(b),"LOAD %d%%",load_percent); field_set(LOAD,b,load_percent?accent():MUTED); }

    if(live&&isfinite(sample.bms_temp_c))snprintf(value,sizeof(value),"%.0f",(double)sample.bms_temp_c);else strcpy(value,"--");
    snprintf(b,sizeof(b),"BMS %s\xc2\xb0" "C",value);field_set(BMS_TEMP,b,MUTED);
    if(isfinite(sample.trip_km))snprintf(value,sizeof(value),"%.1f",(double)sample.trip_km);else strcpy(value,"--");
    snprintf(b,sizeof(b),"TRIP %s km",value);field_set(TRIP,b,WHITE);
    if(isfinite(sample.odometer_km))snprintf(value,sizeof(value),"%.0f",(double)sample.odometer_km);else strcpy(value,"--");
    snprintf(b,sizeof(b),"ODO %s km",value);field_set(ODO,b,MUTED);
}
static void damage_sweep(float from,float to) {
    if(fabsf(to-from)<0.1f)return;
    lv_area_t a;lv_draw_arc_get_area(CX,CY,84,fminf(from,to),fmaxf(from,to),23,false,&a);
    a.x1-=3;a.x2+=3;a.y1-=3;a.y2+=3;damage(a);
}
static void update_speed(bool live,uint32_t dt) {
    bool valid=live&&isfinite(sample.speed_kmh);
    if(valid) {
        if(!needle_visible)shown_speed=sample.speed_kmh;
        else shown_speed+=(sample.speed_kmh-shown_speed)*((float)dt/(110.0f+(float)dt));
        if(fabsf(sample.speed_kmh-shown_speed)<0.05f)shown_speed=sample.speed_kmh;
    }
    int number=valid?(int)lroundf(shown_speed):-1;
    /* Speed digits: white below 30 km/h, red from 30 km/h upward.
       Use the smoothed displayed speed, so the colour change follows exactly
       what the rider sees instead of jumping ahead of the needle/digits. */
    const uint32_t speed_color=(valid && shown_speed>=30.0f)?0xff3b30:WHITE;
    {
        char b[16];
        if(valid) {
            if(number>=0 && number<10) snprintf(b,sizeof(b),"%02d",number);
            else snprintf(b,sizeof(b),"%d",number);
        } else strcpy(b,"--");
        field_set(SPEED,b,speed_color);speed_number=number;
    }
    float angle=valid?roundf((135+270*bound(shown_speed,0,(float)speed_max)/speed_max)*2)/2:135;
    if(valid!=needle_visible || fabsf(angle-draw_angle)>0.1f) {
        if(needle_visible)damage(segment_box(needle,6));
        damage_sweep(needle_visible?draw_angle:135,valid?angle:135);
        needle_visible=valid;draw_angle=angle;
        needle=radial_segment(angle,55,87,2);
        if(valid)damage(segment_box(needle,6));
    }
}
dash_data_t dashboard_empty_data(void) {
    dash_data_t d;memset(&d,0,sizeof(d));
    d.speed_kmh=d.battery_percent=d.voltage_v=d.battery_current_a=NAN;
    d.esc_temp_c=d.bms_temp_c=d.trip_km=d.odometer_km=d.max_speed_kmh=NAN;
    d.mode=DASH_DRIVE;return d;
}
static void deleted(lv_event_t *e){(void)e;root=NULL;have_sample=false;}
bool dashboard_create(lv_obj_t *parent) {
    if(root||!parent)return false;
    sample=dashboard_empty_data();have_sample=was_live=needle_visible=light_on=light_valid=battery_valid=false;
    clock_now=last_sample=last_frame=last_values=0;shown_speed=0;draw_angle=135;
    speed_number=-2;battery_steps=-2;battery_color=0;battery_fill=-1;
    load_valid=load_regen=delta_active=false;load_percent=-1;load_fill_px=-1;
    for(int i=0;i<FIELD_COUNT;i++){fields[i].text[0]=0;fields[i].color=0;}
    make_scale();root=lv_obj_create(parent);lv_obj_remove_style_all(root);
    lv_obj_set_size(root,DASH_WIDTH,DASH_HEIGHT);lv_obj_center(root);
    lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(root,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);
    lv_obj_add_event_cb(root,draw,LV_EVENT_DRAW_MAIN,NULL);
    lv_obj_add_event_cb(root,deleted,LV_EVENT_DELETE,NULL);
    update_metadata(false);update_values(false);update_speed(false,0);return true;
}
void dashboard_destroy(void){if(root)lv_obj_delete(root);}
void dashboard_feed(const dash_data_t *data,uint32_t now) {
    if(!root||!data)return;sample=*data;
    if(!isfinite(sample.speed_kmh))sample.speed_kmh=NAN;else sample.speed_kmh=bound(sample.speed_kmh,0,999);
    if(isfinite(sample.battery_percent))sample.battery_percent=bound(sample.battery_percent,0,100);
    if(!isfinite(sample.voltage_v)||sample.voltage_v<0||sample.voltage_v>200)sample.voltage_v=NAN;
    if(!isfinite(sample.battery_current_a)||fabsf(sample.battery_current_a)>1000)sample.battery_current_a=NAN;
    if(!isfinite(sample.esc_temp_c)||fabsf(sample.esc_temp_c)>300)sample.esc_temp_c=NAN;
    if(!isfinite(sample.bms_temp_c)||fabsf(sample.bms_temp_c)>300)sample.bms_temp_c=NAN;
    if(!isfinite(sample.trip_km)||sample.trip_km<0||sample.trip_km>9999)sample.trip_km=NAN;
    if(!isfinite(sample.odometer_km)||sample.odometer_km<0||sample.odometer_km>999999)sample.odometer_km=NAN;
    if(!isfinite(sample.max_speed_kmh)||sample.max_speed_kmh<0||sample.max_speed_kmh>999)sample.max_speed_kmh=NAN;
    if((unsigned)sample.mode>DASH_SPORT)sample.mode=DASH_DRIVE;
    have_sample=true;last_sample=now;
}
void dashboard_tick(uint32_t now) {
    if(!root)return;clock_now=now;bool live=dashboard_is_live(now),changed=live!=was_live;
    uint32_t dt=now-last_frame;
    if(dt>=DASH_FRAME_MS||changed){last_frame=now;update_speed(live,dt);update_metadata(live);}
    if((uint32_t)(now-last_values)>=200||changed){last_values=now;update_values(live);}
    was_live=live;
}
void dashboard_set_theme(dash_theme_t t) {
    if(t!=DASH_BLUE&&t!=DASH_RED)return;if(theme==t)return;theme=t;
    if(root){update_metadata(dashboard_is_live(clock_now));update_values(dashboard_is_live(clock_now));lv_obj_invalidate(root);}
}
void dashboard_set_speed_max(unsigned kmh) {
    if(kmh<20||kmh>160||kmh%10||kmh==speed_max)return;
    speed_max=kmh;make_scale();if(root){update_speed(dashboard_is_live(clock_now),0);damage_rect(0,29,240,188);}
}
dash_data_t dashboard_demo_sample(uint32_t ms) {
    dash_data_t d=dashboard_empty_data();float t=ms/1000.0f,phase=fmodf(t,24);
    d.speed_kmh=phase<4?phase*8:phase<13?32+14*sinf((phase-4)/9*3.14159265f):phase<19?32*(19-phase)/6:0;
    d.battery_percent=73;d.voltage_v=54.6f-0.9f*sinf(t/8);
    d.battery_current_a=phase<13?4+d.speed_kmh*0.65f:phase<19?-5.5f:0;
    d.esc_temp_c=42;d.bms_temp_c=36;d.trip_km=18.4f;d.odometer_km=1234;
    d.max_speed_kmh=NAN;d.delta_active=d.speed_kmh>30.0f;d.mode=DASH_SPORT;d.connected=true;d.demo=true;d.headlight_valid=true;d.headlight=((ms/2500)%2)!=0;return d;
}
