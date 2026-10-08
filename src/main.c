#include "CH58x_common.h"
#include "CH58x_sys.h"
#include "CH58xBLE_LIB.h"

#include "leddrv.h"
#include "button.h"
#include "bmlist.h"
#include "resource.h"
#include "animation.h"
#include "font.h"
#include "font3x5.h"
#if HW_KEY_COUNT == 4
#include "auxbtn.h"
#endif
#include "game.h"
#include "flappy.h"
#include "pong.h"

#include "power.h"
#include "data.h"
#include "config.h"
#include "debug.h"

#include "ble/setup.h"
#include "ble/profile.h"

#include "usb/usb.h"
#include "legacyctrl.h"

#define NEXT_STATE(v, min, max) \
				(v)++; \
				if ((v) >= (max)) \
					(v) = (min)

enum MODES {
	MENU = 0,
    NORMAL,
    DOWNLOAD,
    CLOCK,
	GAME,
	POWER_OFF,
    MODES_COUNT,
};

static int menu_cursor=0;
#define MENU_ITEMS_COUNT 7
static const char *menu_labels[] = {
	"ANIMATION",
	"BT-PAIRING",
	"CLOCK MODE",
	"GAMES",
	"BRIGHT",
	"SECURITY",
	"OFF"
};
#define MENU_IDX_ANIMATION 0
#define MENU_IDX_BLE       1
#define MENU_IDX_CLOCK     2
#define MENU_IDX_GAMES     3
#define MENU_IDX_BRIGHT    4
#define MENU_IDX_SECURITY  5
#define MENU_IDX_OFF       6

#define ANI_BASE_SPEED_T      (200000) // uS
#define ANI_MARQUE_SPEED_T    (100000) // uS
#define ANI_FLASH_SPEED_T     (500000) // uS
#define SCAN_BOOTLD_BTN_SPEED_T         (200000) // uS
#define ANI_SPEED_STRATEGY(speed_level) \
				(ANI_BASE_SPEED_T - ((speed_level) \
				* ANI_BASE_SPEED_T / 8))

#define ANI_NEXT_STEP       (1 << 0)
#define ANI_MARQUE          (1 << 1)
#define ANI_FLASH           (1 << 2)
#define SCAN_BOOTLD_BTN     (1 << 3)
#define BLE_NEXT_STEP       (1 << 4)
#define CLOCK_TICK          (1 << 5)
#define STOPWATCH_TICK  	(1 << 6)
#define BLE_OFF_DONE 		(1 << 7)

typedef enum {
    SW_STOPPED,
    SW_RUNNING,
} sw_state_t;

static sw_state_t sw_state = SW_STOPPED;
static uint32_t sw_centiseconds = 0;

static tmosTaskID common_taskid = INVALID_TASK_ID ;

// Double Buffering: front buffer for LED display, back buffer for animation
volatile uint16_t fb_front[LED_COLS] = {0};
volatile uint16_t fb_back[LED_COLS] = {0};
volatile uint16_t *fb_write = fb_back;    // Animation writes to back buffer
volatile uint16_t *fb_display = fb_front; // LED interrupt reads from front buffer

// Synchronization: track LED scan completion
volatile uint8_t led_frame_complete = 1;  // Set when one complete frame is displayed
volatile uint8_t fb_swap_pending = 0;     // Flag indicating swap is waiting

volatile int mode, is_play_sequentially = 1;
static int clock_active = 0;

__HIGH_CODE

static void mode_setup_normal();
static void disp_clock();
static void disp_menu();
static void menu_up();
static void menu_down();
static void enter_clock_submenu();
static void disp_stopwatch();
void return_to_menu();
static void enter_games_submenu(void);
static void enter_security_submenu();
static void enter_bright_submenu();
static void bt_pairing_bypass();

__HIGH_CODE
static void bm_transition()
{
	if (is_play_sequentially) {
		is_play_sequentially = 0;
		bmlist_gohead();
		return;
	}

	bmlist_current()->anim_step = 0;
	bmlist_gonext();
	if (bmlist_current() == bmlist_head()) {
		is_play_sequentially = 1;
		return;
	}
}
void play_splash(xbm_t *xbm, int col, int row, int spT)
{
	while (1) {
		fb_begin_update();
		int more = ani_xbm_scrollup_pad(xbm, 11, 11, 11, (uint16_t *)fb_write, 0, 0);
		fb_end_update();
		if (more == 0)
			break;
		fb_swap();
		DelayMs(spT);
	}
}

void load_bmlist()
{
	data_legacy_t header;
	data_get_header(&header);
	if (memcmp(header.header, "wang", 5))
		return; // There is no bitmap stored in flash

	bm_t *curr_bm = bmlist_current();

	for (int i=0; i<8; i++) {
		bm_t *bm = flash2newbm(i);
		if (bm == NULL)
			continue;
		bmlist_append(bm);
	}
	bmlist_gonext();

	bmlist_drop(curr_bm);
}

static uint16_t common_tasks(tmosTaskID task_id, uint16_t events)
{
	static int marque_step, flash_step;

	if(events & SYS_EVENT_MSG) {
		uint8 *pMsg = tmos_msg_receive(common_taskid);
		if(pMsg != NULL)
		{
			tmos_msg_deallocate(pMsg);
		}
		return (events ^ SYS_EVENT_MSG);
	}

	if(events & ANI_NEXT_STEP) {

		static int (*animations[])(bm_t *bm, uint16_t *fb) = {
			ani_scroll_left,
			ani_scroll_right,
			ani_scroll_up,
			ani_scroll_down,
			ani_fixed,
			ani_animation,
			ani_snowflake,
			ani_picture,
			ani_laser
		};

		bm_t *bm = bmlist_current();
		fb_begin_update();
		if (animations[LEGACY_GET_ANIMATION(bm->modes)])
			if (animations[LEGACY_GET_ANIMATION(bm->modes)](bm, (uint16_t *)fb_write) == 0
				&& is_play_sequentially) {
				bm->anim_step = 0;
				bmlist_gonext();
			}
		if (bm->is_flash) {
			ani_flash(bm, (uint16_t *)fb_write, flash_step);
		}
		if (bm->is_marquee) {
			ani_marque(bm, (uint16_t *)fb_write, marque_step);
		}
		fb_end_update();

		// Swap buffers after animation update is complete
		fb_swap();

		uint32_t t = ANI_SPEED_STRATEGY(LEGACY_GET_SPEED(bm->modes));
		tmos_start_task(common_taskid, ANI_NEXT_STEP, t / 625);

		return events ^ ANI_NEXT_STEP;
	}

	if (events & ANI_MARQUE) {
		bm_t *bm = bmlist_current();
		marque_step++;
		if (bm->is_marquee) {
			fb_begin_update();
			ani_marque(bm, (uint16_t *)fb_write, marque_step);
			fb_end_update();
		}

		return events ^ ANI_MARQUE;
	}

	if (events & SCAN_BOOTLD_BTN) {
		static uint32_t hold;
		hold = isPressed(KEY2) ? hold + 1 : 0;
		if (hold > 10) {
			reset_jump();
		}

		return events ^ SCAN_BOOTLD_BTN;
	}

	if (events & ANI_FLASH) {
		bm_t *bm = bmlist_current();
		flash_step++;

		if (bm->is_flash) {
			fb_begin_update();
			ani_flash(bm, (uint16_t *)fb_write, flash_step);
			fb_end_update();
		}
		if (bm->is_marquee) {
			fb_begin_update();
			ani_marque(bm, (uint16_t *)fb_write, marque_step);
			fb_end_update();
		}

		return events ^ ANI_FLASH;
	}

	if (events & BLE_NEXT_STEP) {
		fb_begin_update();
		ani_xbm_next_frame(&bluetooth, (uint16_t *)fb_write, 10, 0);
		fb_end_update();
		// Swap buffers after BLE animation update
		fb_swap();

		return events ^ BLE_NEXT_STEP;
	}
	
	if (events & CLOCK_TICK) {
		disp_clock();
		return events ^ CLOCK_TICK;
	}

	if (events & STOPWATCH_TICK) {
		sw_centiseconds++;
		disp_stopwatch();
		return events ^ STOPWATCH_TICK;
	}

	if (events & BLE_OFF_DONE) {
		ble_disable_advertise();
		return_to_menu();
		return events ^ BLE_OFF_DONE;
	}

	return 0;
}

void ble_setup()
{
	ble_hardwareInit();
	tmos_clockInit();

	peripheral_init();

	if (! badge_cfg.ble_always_on) {
		ble_disable_advertise();
	}

	devInfo_registerService();
	legacy_registerService();
	batt_registerService();
	ng_registerService();
}

static void spawn_tasks()
{
	common_taskid = TMOS_ProcessEventRegister(common_tasks);

	tmos_start_reload_task(common_taskid, ANI_MARQUE, ANI_MARQUE_SPEED_T / 625);
	tmos_start_reload_task(common_taskid, ANI_FLASH, ANI_FLASH_SPEED_T / 625);
	tmos_start_reload_task(common_taskid, SCAN_BOOTLD_BTN,
				SCAN_BOOTLD_BTN_SPEED_T / 625);
	tmos_start_task(common_taskid, ANI_NEXT_STEP, 500000 / 625);
}

static void start_ble_animation()
{
	tmos_stop_task(common_taskid, ANI_NEXT_STEP);
	tmos_stop_task(common_taskid, ANI_MARQUE);
	tmos_stop_task(common_taskid, ANI_FLASH);
	PFIC_DisableIRQ(TMR0_IRQn);
	memset((void *)fb_front, 0, LED_COLS * sizeof(uint16_t));
	memset((void *)fb_back, 0, LED_COLS * sizeof(uint16_t));
	PFIC_EnableIRQ(TMR0_IRQn);

	tmos_start_reload_task(common_taskid, BLE_NEXT_STEP, 500000 / 625);
}

static void start_normal_animation()
{
	tmos_start_reload_task(common_taskid, ANI_MARQUE, ANI_MARQUE_SPEED_T / 625);
	tmos_start_reload_task(common_taskid, ANI_FLASH, ANI_FLASH_SPEED_T / 625);
	tmos_start_task(common_taskid, ANI_NEXT_STEP, 500000 / 625);
	tmos_stop_task(common_taskid, BLE_NEXT_STEP);
}

static void resume_from_streaming()
{
	if (badge_cfg.ble_always_on) {
		start_normal_animation();
	} else {
		start_ble_animation();
	}
}

static void stop_all_animation()
{
	tmos_stop_task(common_taskid, ANI_NEXT_STEP);
	tmos_stop_task(common_taskid, ANI_MARQUE);
	tmos_stop_task(common_taskid, ANI_FLASH);
	tmos_stop_task(common_taskid, BLE_NEXT_STEP);
	PFIC_DisableIRQ(TMR0_IRQn);
	// Games keep a pointer to fb_display, so no swap may happen after this
	fb_swap_pending = 0;
	memset((void *)fb_front, 0, LED_COLS * sizeof(uint16_t));
	memset((void *)fb_back, 0, LED_COLS * sizeof(uint16_t));
	PFIC_EnableIRQ(TMR0_IRQn);
}

int streaming_enabled;

uint8_t streaming_setting(uint8_t *params, uint16_t len)
{
	if (params[0] == 0x00) { // enter streaming mode
		stop_all_animation();
		streaming_enabled = 1;
	} else if (params[0] == 0x01) { // return to normal mode
		resume_from_streaming();
		streaming_enabled = 0;
	}
	return 0;
}

uint8_t stream_bitmap(uint8_t *params, uint16_t len)
{
	if (! streaming_enabled) {
		return -1;
	}

	fb_begin_update();
	tmos_memcpy((void *)fb_write, params, min(LED_COLS * 2, len));
	fb_end_update();
	fb_swap();
	return 0;
}

static void debug_init()
{
	GPIOA_SetBits(GPIO_Pin_9);
	GPIOA_ModeCfg(GPIO_Pin_8, GPIO_ModeIN_PU);
	GPIOA_ModeCfg(GPIO_Pin_9, GPIO_ModeOut_PP_5mA);
	UART1_DefInit();
	UART1_BaudRateCfg(921600);
}

static void disp_bat_stt(int bat_percent, int col, int row)
{
	if (bat_percent < 0) {
		xbm2fb(&batwarn_xbm, (uint16_t *)fb_write, col, row);
		return;
	}

	xbm2fb(&bat_xbm, (uint16_t *)fb_write, col, row);
	bat_percent /= 10;
	for (int i=1; i <= bat_percent; i++) {
		fb_write[col + i] = fb_write[col];
	}
}

static void fb_putchar(char c, int col, int row)
{
	for (int i=0; i < 6; i++) {
		if (col + i >= LED_COLS) break;
		fb_write[col + i] = (fb_write[col + i] & ~(0x7f << row))
				| (font5x7[c - ' '][i] << row);
	}
}

static void fb_puts(char *s, int len, int col, int row)
{
	while (*s && len--) {
		fb_putchar(*s, col, row);
		col += 6;
		s++;
	}
}

static void fb_putchar_small(char c, int col, int row)
{
    for (int i = 0; i < 4; i++) {
        if (col + i >= LED_COLS) break;
        fb_write[col + i] = (fb_write[col + i] & ~(0x1f << row)) | (font3x5[c - ' '][i] << row);
    }
}

static void fb_puts_small(char *s, int len, int col, int row)
{
    while (*s && len--) {
        fb_putchar_small(*s, col, row);
        col += 4;
        s++;
    }
}

// Full-screen redraws: draw into the back buffer, then swap at frame boundary
static void scr_begin(void)
{
	fb_begin_update();
	memset((void *)fb_write, 0, LED_COLS * sizeof(uint16_t));
}

static void scr_end(void)
{
	fb_end_update();
	fb_swap();
}

static void disp_auth_code(uint16_t code)
{
	scr_begin();
	char buf[5];
	buf[0] = '0' + (code / 1000) % 10;
	buf[1] = '0' + (code / 100)  % 10;
	buf[2] = '0' + (code / 10)   % 10;
	buf[3] = '0' + (code)        % 10;
	buf[4] = '\0';
	fb_puts(buf, 4, 8, 2);   // centered on 44-col display, row 2
	scr_end();
}

static void disp_clock()
{
    uint16_t year, month, day, hour, minute, second;
    RTC_GetTime(&year, &month, &day, &hour, &minute, &second);
    scr_begin();

    char buf[6];
    buf[0] = '0' + hour / 10;
    buf[1] = '0' + hour % 10;
    buf[2] = ':';
    buf[3] = '0' + minute / 10;
    buf[4] = '0' + minute % 10;
    buf[5] = '\0';

    fb_puts(buf, 5, 2, 2);
    scr_end();
}

static void disp_menu()
{
    scr_begin();

    int page = menu_cursor / 2;
    int item0 = page * 2;
    int item1 = page * 2 + 1;

    // top item
    if (menu_cursor == item0)
        fb_putchar_small('>', 0, 0);
    fb_puts_small((char *)menu_labels[item0],
                  strlen(menu_labels[item0]), 4, 0);

    // bottom item
    if (item1 < MENU_ITEMS_COUNT) {
        if (menu_cursor == item1)
            fb_putchar_small('>', 0, 6);
        fb_puts_small((char *)menu_labels[item1],
                      strlen(menu_labels[item1]), 4, 6);
    }
    scr_end();
}

static void menu_up(){
	menu_cursor--;
    if (menu_cursor < 0) menu_cursor = MENU_ITEMS_COUNT - 1;
    disp_menu();
}

static void menu_down(){
	menu_cursor++;
	if(menu_cursor > MENU_ITEMS_COUNT - 1) menu_cursor = 0;
	disp_menu();
}

static void menu_select(){
    switch (menu_cursor) {
        case MENU_IDX_ANIMATION:
            mode = NORMAL;
            btn_onOnePress(KEY1, NULL);
            btn_onOnePress(KEY2, bm_transition);
            mode_setup_normal();
            break;
        case MENU_IDX_BLE:
			mode = DOWNLOAD;
			btn_onOnePress(KEY2, NULL);
			btn_onLongPress(KEY1, NULL);
			btn_onLongPress(KEY2, return_to_menu);
			if (badge_cfg.ble_security) {
				uint16_t auth_code = tmos_rand() % 10000;
				legacy_set_auth_code(auth_code);
				ble_enable_advertise();
		#if HW_KEY_COUNT == 4
				auxbtn_onOnePress(KEY4, bt_pairing_bypass);
		#else
				btn_onLongPress(KEY1, bt_pairing_bypass);
		#endif
				disp_auth_code(auth_code);
			} else {
				ble_enable_advertise();
				start_ble_animation();
		#if HW_KEY_COUNT == 4
				auxbtn_onOnePress(KEY4, return_to_menu);
		#else
				btn_onLongPress(KEY1, return_to_menu);
		#endif
			}
			break;
        case MENU_IDX_CLOCK:
            enter_clock_submenu();
            break;
        case MENU_IDX_GAMES:
            enter_games_submenu();
            break;
        case MENU_IDX_BRIGHT:
            enter_bright_submenu();
            break;
        case MENU_IDX_SECURITY:
            enter_security_submenu();
            break;
        case MENU_IDX_OFF:
            mode = POWER_OFF;
            stop_all_animation(); // blank the display right away
            // On 2-key boards OFF is chosen by holding KEY1, which is also the
            // wake-up key: shutting down while it is held lets the bounce on
            // release wake the badge straight back up
            while (isPressed(KEY1))
                DelayMs(10);
            DelayMs(50);
            poweroff();
            break;
    }
}

static void disp_stopwatch()
{
    uint32_t cs = sw_centiseconds;
    uint32_t minutes = cs / 6000;
    uint32_t seconds = (cs % 6000) / 100;
    uint32_t centis  = cs % 100;

    char buf[8];
    buf[0] = '0' + minutes % 10;  // M:SS:cs - to be able to fit it on the badge at the usual font size 5x7
    buf[1] = ':';
    buf[2] = '0' + seconds / 10;
    buf[3] = '0' + seconds % 10;
    buf[4] = ':';
    buf[5] = '0' + centis / 10;
    buf[6] = '0' + centis % 10;
    buf[7] = '\0';

    scr_begin();
    fb_puts(buf, 7, 2, 2);
    scr_end();
}

static void sw_startstop()
{
    if (sw_state == SW_STOPPED) {
        sw_state = SW_RUNNING;
        tmos_start_reload_task(common_taskid, STOPWATCH_TICK, 10000 / 625);
    } else {
        sw_state = SW_STOPPED;
        tmos_stop_task(common_taskid, STOPWATCH_TICK);
    }
}

static void sw_reset()
{
    sw_centiseconds = 0;
    disp_stopwatch();
}

static void sw_back()
{
    sw_state = SW_STOPPED;
    tmos_stop_task(common_taskid, STOPWATCH_TICK);
    enter_clock_submenu();
}

// Clock submenu: 0 = Time, 1 = Stopwatch
static int clock_submenu_sel = 0;

static void disp_clock_submenu()
{
    scr_begin();
    if (clock_submenu_sel == 0)
        fb_putchar_small('>', 0, 0);
    else
        fb_putchar_small('>', 0, 6);

    fb_puts_small("TIME", 4, 4, 0);
    fb_puts_small("STOPWATCH", 9, 4, 6);
    scr_end();
}

static void clock_submenu_nav()
{
    clock_submenu_sel ^= 1;
    disp_clock_submenu();
}

static void clock_submenu_select()
{
    if (clock_submenu_sel == 0) {
        // Enter Time mode
        clock_active = 1;
        tmos_start_reload_task(common_taskid, CLOCK_TICK, 1000000 / 625);
        btn_onOnePress(KEY1, NULL);
        btn_onOnePress(KEY2, NULL);
		btn_onLongPress(KEY1, NULL);
        btn_onLongPress(KEY2, enter_clock_submenu);
    } else {
        // Enter Stopwatch mode
        sw_state = SW_STOPPED;
        sw_centiseconds = 0;
        disp_stopwatch();
        btn_onOnePress(KEY1, sw_startstop);
        btn_onOnePress(KEY2, sw_reset);
		btn_onLongPress(KEY1, NULL);
        btn_onLongPress(KEY2, sw_back);
    }
}

static int security_submenu_sel = 0;  // 0 = ENABLE, 1 = DISABLE 

static void disp_security_submenu()
{
    scr_begin();
    if (security_submenu_sel == 0)
        fb_putchar_small('>', 0, 0);
    else
        fb_putchar_small('>', 0, 6);

    fb_puts_small("ENABLE", 6, 4, 0);
    fb_puts_small("DISABLE", 7, 4, 6);
    scr_end();
}

static void security_submenu_nav()
{
    security_submenu_sel ^= 1;
    disp_security_submenu();
}

static void security_submenu_select()
{
    badge_cfg.ble_security = (security_submenu_sel == 0) ? 1 : 0;
    cfg_writeflash_def(&badge_cfg);
    return_to_menu();
}

static void bt_pairing_bypass()
{
    legacy_bypass_auth();       // skip auth for this session
#if HW_KEY_COUNT == 4
    auxbtn_onOnePress(KEY4, return_to_menu);  // restore KEY4 to normal
#else
    btn_onLongPress(KEY1, return_to_menu);    // restore KEY1 long-press to normal
#endif
    start_ble_animation();      // drop PIN display, show BT animation
}

// BRIGHT submenu: [small sun] +--+--|--+ [big sun]
// Changes apply immediately; save keeps them, cancel restores.
#define BRIGHT_SLIDER_X     12
#define BRIGHT_SLIDER_STEP  6  // multiple of 3 so ticks start a dash

static int bright_prev;

static void fb_setpx(int col, int row)
{
	if (col >= 0 && col < LED_COLS && row >= 0 && row < LED_ROWS)
		fb_write[col] |= 1 << row;
}

static void disp_bright_submenu()
{
	// Column bitmaps, bit 0 = top row
	static const uint16_t sun_small[] = {0x08, 0x00, 0x1c, 0x5d, 0x1c, 0x00, 0x08};
	static const uint16_t sun_big[] = {
		0x010, 0x082, 0x038, 0x07c, 0x17d, 0x07c, 0x038, 0x082, 0x010
	};
	const int end = BRIGHT_SLIDER_X + BRIGHT_SLIDER_STEP * (BRIGHTNESS_LEVELS - 1);

	scr_begin();
	for (int i = 0; i < 7; i++)
		fb_write[2 + i] |= sun_small[i] << 2;
	for (int i = 0; i < 9; i++)
		fb_write[34 + i] |= sun_big[i] << 1;

	// Track: dashed line, 2 on 1 off, with a tick at every level
	for (int x = BRIGHT_SLIDER_X; x <= end; x++)
		if ((x - BRIGHT_SLIDER_X) % 3 != 2)
			fb_setpx(x, 5);
	for (int l = 0; l < BRIGHTNESS_LEVELS; l++) {
		fb_setpx(BRIGHT_SLIDER_X + BRIGHT_SLIDER_STEP * l, 4);
		fb_setpx(BRIGHT_SLIDER_X + BRIGHT_SLIDER_STEP * l, 6);
	}

	// Knob at the current level: a line taller than the ticks
	int p = BRIGHT_SLIDER_X + BRIGHT_SLIDER_STEP * badge_cfg.led_brightness;
	for (int y = 2; y <= 8; y++)
		fb_setpx(p, y);
	scr_end();
}

static void bright_down()
{
	if (badge_cfg.led_brightness > 0) {
		badge_cfg.led_brightness--;
		disp_bright_submenu();
	}
}

static void bright_up()
{
	if (badge_cfg.led_brightness < BRIGHTNESS_LEVELS - 1) {
		badge_cfg.led_brightness++;
		disp_bright_submenu();
	}
}

static void bright_save()
{
	cfg_writeflash_def(&badge_cfg);
	return_to_menu();
}

static void bright_cancel()
{
	badge_cfg.led_brightness = bright_prev;
	return_to_menu();
}

static void enter_bright_submenu()
{
	stop_all_animation();
	bright_prev = badge_cfg.led_brightness;
	btn_onOnePress(KEY1, bright_down);
	btn_onOnePress(KEY2, bright_up);
#if HW_KEY_COUNT == 4
	btn_onLongPress(KEY1, NULL);
	btn_onLongPress(KEY2, NULL);
	auxbtn_onOnePress(KEY3, bright_save);
	auxbtn_onOnePress(KEY4, bright_cancel);
#else
	btn_onLongPress(KEY1, bright_save);
	btn_onLongPress(KEY2, bright_cancel);
#endif
	disp_bright_submenu();
}

static void enter_security_submenu()
{
    stop_all_animation();
    // cursor starts on current state
    security_submenu_sel = badge_cfg.ble_security ? 0 : 1;
    btn_onOnePress(KEY1, security_submenu_nav);   // navigate up/down
    btn_onOnePress(KEY2, security_submenu_nav);   // navigate up/down
#if HW_KEY_COUNT == 4
    auxbtn_onOnePress(KEY3, security_submenu_select);  // confirm
    auxbtn_onOnePress(KEY4, return_to_menu);           // cancel
#else
    btn_onLongPress(KEY1, security_submenu_select);    // confirm
    btn_onLongPress(KEY2, return_to_menu);              // cancel
#endif
    disp_security_submenu();
}

static void enter_clock_submenu()
{
    clock_active = 0;
    tmos_stop_task(common_taskid, CLOCK_TICK);
    stop_all_animation();
    clock_submenu_sel = 0;

    btn_onOnePress(KEY1, clock_submenu_nav);
    btn_onOnePress(KEY2, clock_submenu_nav);
#if HW_KEY_COUNT == 4
	btn_onLongPress(KEY1, NULL);
    btn_onLongPress(KEY2, NULL);
    auxbtn_onOnePress(KEY3, clock_submenu_select);
    auxbtn_onOnePress(KEY4, return_to_menu);
#else
    btn_onLongPress(KEY1, clock_submenu_select);
    btn_onLongPress(KEY2, return_to_menu);
#endif

    disp_clock_submenu();
}

// Games submenu: 0 = Snake, 1 = Flappy, 2 = Pong
#define GAMES_COUNT 3
static int games_submenu_sel = 0;

static void disp_games_submenu(void)
{
    scr_begin();
    static const char *labels[] = { "SNAKE", "FLAPPY", "PONG" };

    int page  = games_submenu_sel / 2;
    int item0 = page * 2;
    int item1 = page * 2 + 1;

    if (games_submenu_sel == item0)
        fb_putchar_small('>', 0, 0);
    fb_puts_small((char *)labels[item0], strlen(labels[item0]), 4, 0);

    if (item1 < GAMES_COUNT) {
        if (games_submenu_sel == item1)
            fb_putchar_small('>', 0, 6);
        fb_puts_small((char *)labels[item1], strlen(labels[item1]), 4, 6);
    }
    scr_end();
}

static void games_submenu_nav(void)
{
    games_submenu_sel = (games_submenu_sel + 1) % GAMES_COUNT;
    disp_games_submenu();
}

static void games_submenu_select(void)
{
    mode = GAME;
    stop_all_animation();
    switch (games_submenu_sel) {
        case 0:
            game_start((uint16_t *)fb_display);
            break;
        case 1:
            flappy_start((uint16_t *)fb_display);
            break;
        case 2:
            pong_start((uint16_t *)fb_display);
            break;
    }
}

static void enter_games_submenu(void)
{
    games_submenu_sel = 0;
    stop_all_animation();

    btn_onOnePress(KEY1, games_submenu_nav);
    btn_onOnePress(KEY2, games_submenu_nav);
#if HW_KEY_COUNT == 4
    auxbtn_onOnePress(KEY3, games_submenu_select);
    auxbtn_onOnePress(KEY4, return_to_menu);
#else
    btn_onLongPress(KEY1, games_submenu_select);
    btn_onLongPress(KEY2, return_to_menu);
#endif

    disp_games_submenu();
}

void return_to_menu()
{
    stop_all_animation();
    tmos_stop_task(common_taskid, CLOCK_TICK);
    tmos_stop_task(common_taskid, STOPWATCH_TICK);
    sw_state = SW_STOPPED;
    clock_active = 0;

    mode = MENU;
    btn_onOnePress(KEY1, menu_up);
    btn_onOnePress(KEY2, menu_down);
#if HW_KEY_COUNT == 4
    btn_onLongPress(KEY1, NULL);
    btn_onLongPress(KEY2, NULL);
    auxbtn_onOnePress(KEY3, menu_select);
    auxbtn_onOnePress(KEY4, return_to_menu);
#else
    btn_onLongPress(KEY1, menu_select);
    btn_onLongPress(KEY2, NULL);
#endif
    disp_menu();
}

static void disp_charging()
{
	int blink = 0;
	while (1) {
		btn_tick();
		int percent = batt_raw2percent(batt_raw());

		if (charging_status()) {
			fb_begin_update();
			disp_bat_stt(blink ? percent : 0, 2, 2);
			if (ani_xbm_next_frame(&fabm_xbm, (uint16_t *)fb_write, 16, 0) == 0) {
				fb_puts(VERSION_ABBR, sizeof(VERSION_ABBR), 16, 2);
				fb_putchar(' ', 40, 2);
			}
			fb_end_update();
			fb_swap();
			blink = !blink;
			DelayMs(500);
		} else {
			fb_begin_update();
			disp_bat_stt(percent, 7, 2);
			fb_end_update();
			fb_swap();
			DelayMs(500);
			return;
		}
	}
}

void clean_bmlist()
{
	bm_t *curr_bm = bmlist_current();
	while (curr_bm->next != curr_bm)
		bmlist_drop(curr_bm->next);
}

void reload_bmlist()
{
	clean_bmlist();
	load_bmlist();
}

static void mode_setup_normal()
{
	btn_onOnePress(KEY2, bm_transition);
	btn_onLongPress(KEY1, NULL);
	btn_onLongPress(KEY2, return_to_menu);
	reload_bmlist();
	start_normal_animation();
}

static void disp_ble_off()
{
    scr_begin();
    fb_puts_small("BLUETOOTH", 9, 4, 0);
    fb_puts_small("OFF", 3, 4, 6);
    scr_end();
}

void handle_after_rx()
{
    if (badge_cfg.reset_rx) {
        SYS_ResetExecute();
    } else {
        tmos_stop_task(common_taskid, CLOCK_TICK);
        tmos_stop_task(common_taskid, STOPWATCH_TICK);
        sw_state = SW_STOPPED;
        clock_active = 0;
        stop_all_animation();
        disp_ble_off();
        tmos_start_task(common_taskid, BLE_OFF_DONE, 1500000 / 625);  
    }
}

int main()
{
	SetSysClock(CLK_SOURCE_PLL_60MHz);
	
	debug_init();
	PRINT("\nDebug console is on UART%d\n", DEBUG);

	cdc_onWrite(usb_rx_dispatch);
	hiddev_onWrite(usb_rx_dispatch);
	usb_start();

	led_init();
	TMR0_TimerInit((FREQ_SYS / 3000) / 2);
	TMR0_ITCfg(ENABLE, TMR0_3_IT_CYC_END);
	PFIC_EnableIRQ(TMR0_IRQn);

	bmlist_init(LED_COLS * 4);

	btn_init();
	btn_onOnePress(KEY1, menu_up);
	btn_onOnePress(KEY2, menu_down);
	btn_onLongPress(KEY1, menu_select);

#if HW_KEY_COUNT == 4
	auxbtn_init();
	auxbtn_onOnePress(KEY3, menu_select);
	auxbtn_onOnePress(KEY4, return_to_menu);
#endif

	power_init();
	disp_charging();
	cfg_init();
	xbm_t spl = {
		.bits = &(badge_cfg.splash_bm_bits),
		.w = badge_cfg.splash_bm_w,
		.h = badge_cfg.splash_bm_h,
		.fh = badge_cfg.splash_bm_fh,
	};
	play_splash(&spl, 0, 0, badge_cfg.splash_speedT);

	load_bmlist();

	ble_setup();

	spawn_tasks();
	btn_init_task();
#if HW_KEY_COUNT == 4
	auxbtn_init_task();
#endif
	game_init();
	flappy_init();
	pong_init();
	stop_all_animation();

	mode = MENU;
	disp_menu();
	while (1) {
		TMOS_SystemProcess();
	}
}

__INTERRUPT
__HIGH_CODE
void TMR0_IRQHandler(void)
{
	static int i;
	int state;

	if (TMR0_GetITFlag(TMR0_3_IT_CYC_END)) {
		i++;
		state = i&3;

		if (state == 0) {
			if ((i >> 1) >= LED_COLS) {
				i = 0;
				// One complete frame has been scanned
				led_frame_complete = 1;
				// If a swap is pending, perform it now
				fb_swap_isr();
			}
			// Read from front buffer (fb_display) for LED output
			led_write2dcol(i >> 2, fb_display[i >> 1], fb_display[(i >> 1) + 1]);
		}
		else if (state > (badge_cfg.led_brightness&3))
			leds_releaseall();

		TMR0_ClearITFlag(TMR0_3_IT_CYC_END);
	}
}