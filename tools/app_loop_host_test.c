/* SPDX-License-Identifier: Apache-2.0
 * 中文：运行真实主循环，用确定性触摸和任务延时退出验证调度。菜单现在是“全部”页（一个普通页面），这里用替身页代替。
 * English: Run the real loop with deterministic touch and task-delay exit. The menu is now the All page (an ordinary
 * page), stood in for here by a stub page.
 */
#include "app_loop.h"
#include "ui_gesture.h"
#include "ui_menu.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf done;
typedef struct { int x, y, count, error; } sample_t;
static sample_t samples[32];
static int sample_count, step, ticks, enters, exits, touch_calls, keys[3], events[8];
static int fulls, mode, tick_consumed[32];
static int long_keys, locks, on_locks;
static app_redraw_t long_key(app_ctx_t* c, int k) { assert(k==UI_KEY_1); ++long_keys; c->request_menu=true; return APP_REDRAW_NONE; }
static app_redraw_t response;
static bool request_on_touch, request_on_tick, menu_on_tick, menu_on_touch, cancel_clobber;
static bool home_on_tick, start_second;
static int home_enters, home_renders, rendered_leaf;
static app_desc_t first, second, all_page;
static int all_enters, all_exits;
static const app_desc_t* all_from;
static bool all_returns;
static bool full_tick, lock_due, font_due, powers_down;
static int64_t time_offset, time_step;
static bool media_test, sd_font, saved_sd_font;
static bool mounted_steps[32];
static int media_lost, builtin_opens, font_opens, probes, loss_step;
int E0470_WAVEFORM;
int ui_scale_pct = 100;
void ui_set_scale(int pct) { ui_scale_pct = pct; }
int cst836u_read(void* h, cst836u_touch_t* t) {
    (void)h;
    sample_t s = samples[step];
    *t = (cst836u_touch_t){ .touched=s.count>0, .count=s.count, .x=s.x, .y=s.y };
    t->points[0].active=s.count>0;
    return s.error;
}
int cst836u_get_info(void* h,cst836u_info_t* i) {(void)h;(void)i;return 0;}
const char* esp_err_to_name(int e) {(void)e;return "test";}
int64_t esp_timer_get_time(void) {return time_offset+(int64_t)(step+1)*time_step;}
void vTaskDelay(int ms) {(void)ms;if (++step>=sample_count) longjmp(done,1);}
bool continuous_du_init(void) {return true;}
void guard_draw_result(EpdiyHighlevelState* h,enum EpdDrawError e) {(void)h;assert(e==0);}
enum EpdDrawError update_display_area_with(EpdiyHighlevelState*h,const void*w,int m,EpdRect a) {(void)h;(void)w;(void)m;(void)a;return 0;}
enum EpdDrawError update_display_full(EpdiyHighlevelState*h) {(void)h;fulls++;return 0;}
enum EpdDrawError update_display_mode(EpdiyHighlevelState*h,int m) {(void)h;(void)m;mode++;return 0;}
enum EpdDrawError update_display_white(EpdiyHighlevelState*h) {(void)h;return 0;}
bool display_take_white_exit(void) {return false;}
void rails_idle_check(int64_t n) {(void)n;}
bool read_pico_pmu_ready(void) {return lock_due;}
bool read_pico_pmu_take_key_short(void) {return true;}
void enter_lock_and_sleep(EpdiyHighlevelState*h,int64_t*t,void*a) {(void)h;(void)t;(void)a;lock_due=false;locks++;time_offset+=1000000;}
bool app_lock_powers_down(void) {return powers_down;}
const char* app_settings_font_path(void) {return "builtin";}
int app_settings_display_pct(void) {return 100;}
int64_t app_settings_lock_idle_ms(void) {return 0;}
bool ttf_font_path_is_builtin(const char*p) {(void)p;return !font_due&&!saved_sd_font;}
bool ttf_font_ready(void) {return true;}
bool ttf_font_is_builtin(void) {return !font_due&&!sd_font;}
bool ttf_font_sd_suspended(void) {return false;}
const char* ttf_font_path(void) {return "other";}
int ttf_font_open(const char*p) {(void)p;font_opens++;sd_font=true;return 0;}
int ttf_font_open_builtin(void) {assert(media_lost>0);builtin_opens++;sd_font=font_due=false;return 0;}
int read_pico_sd_get_info(read_pico_sd_info_t*i) {
    i->mounted=media_test?mounted_steps[step]:font_due;i->present=i->mounted;
    return media_test&&!i->mounted?ESP_ERR_INVALID_STATE:0;
}
void read_pico_sd_start_probe(void) {probes++;}
esp_err_t read_pico_sd_check(void) {return ESP_OK;}
esp_err_t read_pico_sd_remount(void) {return ESP_OK;}
void book_cover_release(void) {}
void pinyin_ime_unload(void) {}
bool tomato_sync_busy(void) {return false;}
void tomato_sync_cancel(void) {}
bool tomato_sync_stop(int t) {(void)t;return true;}
void tr_pin_gate(EpdiyHighlevelState* h, cst836u_handle_t tp, sc7a20h_handle_t a, int64_t* i) {(void)h;(void)tp;(void)a;(void)i;}
void tr_update_boot_cleanup(void) {}
void tr_time_restore(void) {}
bool auto_turn_active(void) {return false;}
static void lost(app_ctx_t*c) {(void)c;media_lost++;loss_step=step;}
EpdRect ui_content_refresh_area(void) {return (EpdRect){0,0,684,1000};}
const app_desc_t* app_home_page(void) {return &first;}
int ui_key_hit_test(uint16_t x,uint16_t y) {return y>=1300 && x<480?x/160:-1;}
bool ui_menu_handle_hit_test(uint16_t x,uint16_t y) {return x>600 && y>1100 && y<1300;}
// “全部”页替身：记下从哪来；all_returns 时下一轮请求返回来处。/ All page stand-in: records its origin; with
// all_returns it asks to go back on its next tick.
void tr_all_open_from(const app_desc_t* from) { if (from != &all_page) all_from = from; }
const app_desc_t* tr_nav_tab_app(int tab) { (void)tab; return &all_page; }
static void all_enter(app_ctx_t*c) {(void)c;all_enters++;}
static void all_leave(app_ctx_t*c) {(void)c;all_exits++;}
static app_redraw_t all_tick(app_ctx_t*c) { if (all_returns) c->request_return=true; return APP_REDRAW_NONE; }
static void enter(app_ctx_t*c) {(void)c;enters++;}
static void leave(app_ctx_t*c) {(void)c;exits++;}
static void lock_hook(app_ctx_t*c) {(void)c;on_locks++;}
static void render(app_ctx_t*c,uint8_t*f) {(void)c;(void)f;}
static void home_render(app_ctx_t*c,uint8_t*f) {(void)f;rendered_leaf=c->leaf;home_renders++;}
static void home_enter(app_ctx_t*c) {c->leaf=0;enters++;home_enters++;}
static app_redraw_t touch(app_ctx_t*c,const cst836u_touch_t*t) {
    (void)t;touch_calls++;assert(c->pressed&&c->consumed);
    if(request_on_touch)c->request_app=&second;
    if(menu_on_touch)c->request_menu=true;
    return response;
}
static app_redraw_t gesture(app_ctx_t*c,const ui_gesture_event_t*e) {
    events[e->type]++;
    if(e->type==UI_GESTURE_CANCEL&&cancel_clobber) {c->request_app=NULL;c->request_menu=true;c->request_return=false;}
    if(e->type==UI_GESTURE_PRESS&&request_on_touch)c->request_app=&second;
    return response;
}
static app_redraw_t key(app_ctx_t*c,int k) {(void)c;keys[k]++;return APP_REDRAW_NONE;}
static app_redraw_t tick(app_ctx_t*c) {
    ticks++;tick_consumed[step]=c->consumed;
    if(request_on_tick)c->request_app=&second;
    if(menu_on_tick)c->request_menu=true;
    if(home_on_tick){c->request_return=true;home_on_tick=false;}
    return full_tick ? APP_REDRAW_FULL : APP_REDRAW_NONE;
}
static void reset(void) {
    media_test=sd_font=saved_sd_font=false;media_lost=builtin_opens=font_opens=probes=0;loss_step=-1;
    memset(mounted_steps,0,sizeof(mounted_steps));
    long_keys=locks=on_locks=0;
    home_on_tick=start_second=false;home_enters=home_renders=0;rendered_leaf=-1;
    full_tick=lock_due=font_due=powers_down=false;time_offset=0;time_step=10000;
    memset(samples,0,sizeof(samples));memset(keys,0,sizeof(keys));memset(events,0,sizeof(events));memset(tick_consumed,0,sizeof(tick_consumed));
    sample_count=step=ticks=enters=exits=touch_calls=fulls=mode=0;
    all_enters=all_exits=0;all_from=NULL;all_returns=false;
    request_on_touch=request_on_tick=menu_on_tick=menu_on_touch=cancel_clobber=false;response=APP_REDRAW_NONE;
    first=(app_desc_t){.title="first",.render=render,.on_enter=enter,.on_exit=leave,.on_touch=touch,.on_tick=tick,.on_key=key};
    second=(app_desc_t){.title="second",.render=render,.on_enter=enter};
    all_page=(app_desc_t){.title="all",.render=render,.on_enter=all_enter,.on_exit=all_leave,.on_tick=all_tick,.tab_nav=true};
}
static void add(int x,int y,int count,int error) {samples[sample_count++]=(sample_t){x,y,count,error};}
static void run(void) {app_loop_config_t cfg={.first_app=start_second?&second:&first};if(!setjmp(done))app_loop_run(&cfg);}
static app_redraw_t return_tick(app_ctx_t*c) { c->request_return=true; return APP_REDRAW_NONE; }
static app_redraw_t origin_tick(app_ctx_t*c) { c->leaf=7; if(step==0 && request_on_tick)c->request_app=&second; return APP_REDRAW_NONE; }
int main(void) {
    int n = 0;
    // 触摸一次只送一次，轮询照常。/ One touch is delivered once; polling continues.
    reset();add(100,400,1,0);add(110,400,1,0);add(0,0,0,0);run();
    assert(touch_calls==1&&ticks==3&&!tick_consumed[0]);++n;
    reset();response=APP_REDRAW_AREA;add(100,400,1,0);run();assert(touch_calls==1&&tick_consumed[0]);++n;
    reset();first.on_gesture=gesture;response=APP_REDRAW_AREA;add(100,400,1,0);add(0,0,0,0);run();
    assert(!touch_calls&&events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_TAP]==1&&tick_consumed[0]);++n;
    reset();first.on_gesture=gesture;add(100,400,1,0);add(0,0,0,9);add(100,400,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);++n;
    // 切页请求优先于打开“全部”页。/ A page request wins over opening the All page.
    reset();request_on_touch=true;menu_on_touch=true;add(100,400,1,0);run();
    assert(enters==2&&exits==1&&ticks==0&&!all_enters);++n;
    reset();request_on_tick=true;menu_on_tick=true;add(0,0,0,0);run();assert(enters==2&&exits==1&&ticks==1&&!all_enters);++n;
    reset();first.on_gesture=gesture;request_on_touch=true;cancel_clobber=true;add(100,400,1,0);run();
    assert(enters==2&&events[UI_GESTURE_CANCEL]==1&&!all_enters);++n;
    // request_menu 打开“全部”页并记下来处（以前这里会被提前清掉，KEY3 和“菜单”都没反应）。
    // request_menu opens the All page and records the origin (it used to be cleared first, so KEY3 and Menu did
    // nothing).
    reset();menu_on_tick=true;add(0,0,0,0);add(0,0,0,0);run();
    assert(all_enters==1&&exits==1&&all_from==&first&&ticks==1);++n;
    // 接管三键的页自己收 KEY1/2/3。/ A key-owning page receives KEY1/2/3 itself.
    reset();first.owns_keys=true;for(int k=0;k<3;k++){add(k*160+80,1500,1,0);add(0,0,0,0);}run();
    assert(keys[0]==1&&keys[1]==1&&keys[2]==1&&!all_enters&&fulls==1);++n;
    // 普通页：中键短按在主页上整屏刷新，KEY3 打开“全部”页。/ Ordinary page: a short middle press on home refreshes the
    // full screen, KEY3 opens the All page.
    reset();add(240,1500,1,0);add(0,0,0,0);add(400,1500,1,0);add(0,0,0,0);run();
    assert(!keys[1]&&!keys[2]&&all_enters==1&&all_from==&first&&fulls==2);++n;
    // 老页面右下角把手打开“全部”页；带标签栏的页把这块让给标签栏。/ The legacy corner handle opens the All page; a
    // tab-bar page yields that area to its bar.
    reset();add(650,1150,1,0);add(0,0,0,0);run();assert(all_enters==1&&all_from==&first);++n;
    reset();first.tab_nav=true;add(650,1150,1,0);add(0,0,0,0);run();assert(!all_enters&&touch_calls==1);++n;
    // 在“全部”页请求返回：回到打开前的页面。/ Asking to return from the All page goes back to where it was opened.
    reset();menu_on_tick=true;all_returns=true;first.on_tick=NULL;add(400,1500,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(all_enters==1&&all_exits==1&&enters==2&&exits==1);++n;
    reset();first.on_gesture=gesture;full_tick=true;add(100,400,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);++n;
    reset();first.on_gesture=gesture;font_due=true;time_offset=4000000;add(100,400,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]&&tick_consumed[0]);++n;
    // 锁屏取消手势；页面的 on_lock 每次锁屏都调（浅睡不断电也调），on_exit 只在断电时调。
    // Locking cancels gestures; the page's on_lock runs on every lock (light sleep without power loss too), on_exit
    // only when power is cut.
    reset();first.on_gesture=gesture;first.on_lock=lock_hook;time_step=1000000;lock_due=true;
    add(100,400,1,0);add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);run();
    assert(locks==1&&on_locks==1&&!exits&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]&&tick_consumed[2]);++n;
    // 接管三键的页上 KEY1 按住 0.5 秒：先按下动作，再触发一次长按；滑出、读错、锁屏都取消。
    // KEY1 held 0.5 s on a key-owning page: the press action first, then one long-press; leaving, read errors and
    // locks cancel it.
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=200000;
    add(80,1500,1,0);add(80,1500,1,0);add(80,1500,1,0);add(80,1500,1,0);add(80,1500,1,0);add(0,0,0,0);run();
    assert(keys[0]==1&&long_keys==1&&all_enters==1);++n;
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=200000;
    add(80,1500,1,0);add(0,0,0,0);run();assert(keys[0]==1&&!long_keys&&!all_enters);++n;
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=300000;
    add(80,1500,1,0);add(240,1500,1,0);add(80,1500,1,0);add(80,1500,1,0);run();assert(!long_keys);++n;
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=300000;
    add(80,1500,1,0);add(0,0,0,9);add(80,1500,1,0);add(80,1500,1,0);run();assert(!long_keys);++n;
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=1000000;lock_due=true;
    add(0,0,0,0);add(80,1500,1,0);add(80,1500,1,0);add(80,1500,1,0);add(0,0,0,0);run();assert(!lock_due&&!long_keys);++n;
    // 中键按满 0.5 秒是整屏刷新（主循环处理，不交给页面）；短按在接管三键的页松手时交给页面。
    // Holding the middle key 0.5 s is a full refresh (handled by the loop, not the page); a short press on a
    // key-owning page goes to the page on release.
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=200000;
    add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(0,0,0,0);run();
    assert(!keys[1]&&!long_keys&&fulls==2);++n;
    reset();first.owns_keys=true;time_step=200000;add(240,1500,1,0);add(0,0,0,0);run();assert(keys[1]==1&&fulls==1);++n;
    // 没有来处时请求返回：打开“全部”页（记下当前页），不跳主页。/ Return with no origin opens the All page (recording
    // the current page) instead of jumping home.
    reset();start_second=true;second.on_tick=tick;second.on_exit=leave;first.on_enter=home_enter;home_on_tick=true;
    add(0,0,0,0);add(0,0,0,0);run();
    assert(all_enters==1&&all_from==&second&&!home_enters&&exits==1);++n;
    // 有来处时返回原页并恢复它的通用页码。/ With an origin, return restores the page and its shared leaf.
    reset();request_on_tick=true;first.render=home_render;first.on_tick=origin_tick;second.on_tick=return_tick;second.on_exit=leave;
    add(0,0,0,0);add(0,0,0,0);run();
    assert(exits==2&&enters==3&&!all_enters&&home_renders==2&&rendered_leaf==7);++n;
    // 拔卡先关资源再回退字体，取消本轮按键，持续无卡不重复通知。
    // Removal closes consumers before fallback, drops the current key and notifies only once.
    reset();media_test=sd_font=true;mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;
    add(0,0,0,0);add(80,1500,1,0);add(0,0,0,0);run();
    assert(media_lost==1&&loss_step==1&&builtin_opens==1&&!keys[0]&&fulls==2);++n;
    // 延迟挂载后拔卡同样被捕获，使用内置字体无需再打开一次。
    // Removal after a late mount is detected without reopening an already built-in font.
    reset();media_test=true;mounted_steps[1]=true;time_step=600000;first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==1&&loss_step==2&&!builtin_opens);++n;
    // 手势和长按在拔卡边界取消，不能把旧按住状态变成新的动作。
    // Removal cancels gestures and holds without turning an old contact into another action.
    reset();media_test=true;mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;first.on_gesture=gesture;
    add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);run();
    assert(media_lost==1&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);++n;
    reset();media_test=true;mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;first.owns_keys=true;first.on_key_long=long_key;
    add(80,1500,1,0);add(80,1500,1,0);add(80,1500,1,0);run();
    assert(media_lost==1&&keys[0]==1&&!long_keys);++n;
    // 失效后不自动探测；显式恢复的挂载可重新加载保存字体并再次检测拔卡。
    // No automatic probing after invalidation; an explicit remount restores saved-font loading and loss detection.
    reset();media_test=sd_font=saved_sd_font=true;mounted_steps[0]=true;time_step=4000000;first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==1&&builtin_opens==1&&!probes&&font_opens==1);++n;
    reset();media_test=sd_font=saved_sd_font=true;mounted_steps[0]=mounted_steps[2]=true;time_step=4000000;first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==2&&builtin_opens==2&&!probes&&font_opens==2);++n;
    // 要断电的锁屏：先 on_lock 存进度，再 on_exit 收尾，醒来 on_enter。/ A power-down lock: on_lock saves first, then
    // on_exit winds down, and on_enter runs after waking.
    reset();first.on_lock=lock_hook;powers_down=true;time_step=1000000;lock_due=true;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(locks==1&&on_locks==1&&exits==1&&enters==2);++n;
    printf("app_loop: %d scheduler scenarios passed\n", n);
    return 0;
}
