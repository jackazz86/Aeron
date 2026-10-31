/* Nintendo Switch input for Aeron: libnx pad as a standard gamepad, gyro
 * aiming as relative mouse motion (toggled with L3+R3), and a stick-driven
 * pointer for the menus. Compiled to nothing on other platforms. */
#ifdef __SWITCH__
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define CFG_PATH "sdmc:/switch/openxw/switch.cfg"

static struct {
	int ready, gyro_on, cursor_ready, chord_latched, pointer_active;
	PadState pad;
	HidSixAxisSensorHandle sixaxis[4];
	HidVibrationDeviceHandle vib[2];
	uint64_t last_sample[4];
	float cursor_x, cursor_y;
	/* Tunables, overridable from CFG_PATH (key=value per line). */
	float gyro_yaw, gyro_sweep, gyro_pitch, cursor_speed, stick_dead;
	int gyro_default_on, invert_x, invert_y;
} nx = { .gyro_yaw = 9.0f, .gyro_sweep = 6.0f, .gyro_pitch = 9.0f, .cursor_speed = 900.0f, .stick_dead = 0.15f };

static void NX_LoadConfig(void) {
	FILE* f = fopen(CFG_PATH, "r");
	char line[128];
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char key[48];
		float v;
		if (sscanf(line, "%47[^=]=%f", key, &v) != 2)
			continue;
		if (!strcmp(key, "gyro_yaw"))
			nx.gyro_yaw = v;
		else if (!strcmp(key, "gyro_sweep"))
			nx.gyro_sweep = v;
		else if (!strcmp(key, "gyro_pitch"))
			nx.gyro_pitch = v;
		else if (!strcmp(key, "cursor_speed"))
			nx.cursor_speed = v;
		else if (!strcmp(key, "stick_dead"))
			nx.stick_dead = v;
		else if (!strcmp(key, "gyro_default_on"))
			nx.gyro_default_on = (int)v;
		else if (!strcmp(key, "gyro_invert_x"))
			nx.invert_x = (int)v;
		else if (!strcmp(key, "gyro_invert_y"))
			nx.invert_y = (int)v;
	}
	fclose(f);
}

static void NX_Init(void) {
	if (nx.ready)
		return;
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&nx.pad);
	hidGetSixAxisSensorHandles(&nx.sixaxis[0], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
	hidGetSixAxisSensorHandles(&nx.sixaxis[2], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
	hidGetSixAxisSensorHandles(&nx.sixaxis[3], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
	for (int i = 0; i < 4; ++i)
		hidStartSixAxisSensor(nx.sixaxis[i]);
	hidInitializeVibrationDevices(&nx.vib[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
	hidInitializeVibrationDevices(&nx.vib[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
	NX_LoadConfig();
	nx.gyro_on = nx.gyro_default_on;
	nx.ready = 1;
}

/* One short pulse when the gyro goes off, two when it goes on. */
static void NX_Pulse(int times) {
	HidVibrationValue on = { .amp_low = 0.4f, .freq_low = 160.0f, .amp_high = 0.4f, .freq_high = 320.0f };
	HidVibrationValue off = { .amp_low = 0, .freq_low = 160.0f, .amp_high = 0, .freq_high = 320.0f };
	for (int i = 0; i < times; ++i) {
		hidSendVibrationValue(nx.vib[0], &on);
		hidSendVibrationValue(nx.vib[1], &on);
		svcSleepThread(90000000LL);
		hidSendVibrationValue(nx.vib[0], &off);
		hidSendVibrationValue(nx.vib[1], &off);
		svcSleepThread(90000000LL);
	}
}

static void NX_Key(AeronInputSnapshot* in, int key, int down) {
	if (down) {
		in->key_pressed[key] = 1;
		in->key_typed[key] = 1;
		in->key_down[key] = 1;
	} else {
		in->key_released[key] = 1;
		in->key_down[key] = 0;
	}
	if (in->key_event_count < AERON_KEY_EVENT_CAPACITY)
		in->key_events[in->key_event_count++] =
			(AeronKeyEvent) { .chord = { (uint16_t)key, 0 }, .down = (uint8_t)down, .repeat = 0 };
}

static int16_t NX_Stick(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* Fills controller slot 0 with a standard gamepad. */
void AeronSwitch_UpdateControllers(AeronInputSnapshot* input) {
	NX_Init();
	padUpdate(&nx.pad);
	uint64_t held = padGetButtons(&nx.pad);

	AeronControllerSnapshot* c = &input->controllers[0];
	for (int i = 1; i < AERON_CONTROLLER_MAX; ++i)
		memset(&input->controllers[i], 0, sizeof input->controllers[i]);
	uint32_t prev = c->gamepad_buttons;
	if (!c->connected) {
		memset(c, 0, sizeof *c);
		c->connected = 1;
		c->kind = AERON_CONTROLLER_KIND_GAMEPAD;
		c->instance_id = 1;
		snprintf(c->name, sizeof c->name, "Nintendo Switch");
		snprintf(c->guid, sizeof c->guid, "0300000057696e7465726e6f2d4e5800");
		c->gamepad_available_axes = (1u << AERON_GAMEPAD_AXIS_COUNT) - 1;
		c->gamepad_available_buttons = (1u << AERON_GAMEPAD_BUTTON_COUNT) - 1;
	}
	uint32_t b = 0;
#define MAP(nxbtn, ab)    \
	if (held & (nxbtn)) \
	b |= 1u << (ab)
	MAP(HidNpadButton_A, AERON_GAMEPAD_BUTTON_SOUTH);
	MAP(HidNpadButton_B, AERON_GAMEPAD_BUTTON_EAST);
	MAP(HidNpadButton_Y, AERON_GAMEPAD_BUTTON_WEST);
	MAP(HidNpadButton_X, AERON_GAMEPAD_BUTTON_NORTH);
	MAP(HidNpadButton_Minus, AERON_GAMEPAD_BUTTON_BACK);
	MAP(HidNpadButton_Plus, AERON_GAMEPAD_BUTTON_START);
	MAP(HidNpadButton_L, AERON_GAMEPAD_BUTTON_LEFT_SHOULDER);
	MAP(HidNpadButton_R, AERON_GAMEPAD_BUTTON_RIGHT_SHOULDER);
	MAP(HidNpadButton_Up, AERON_GAMEPAD_BUTTON_DPAD_UP);
	MAP(HidNpadButton_Down, AERON_GAMEPAD_BUTTON_DPAD_DOWN);
	MAP(HidNpadButton_Left, AERON_GAMEPAD_BUTTON_DPAD_LEFT);
	MAP(HidNpadButton_Right, AERON_GAMEPAD_BUTTON_DPAD_RIGHT);
#undef MAP
	HidAnalogStickState l = padGetStickPos(&nx.pad, 0), r = padGetStickPos(&nx.pad, 1);
	c->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX] = NX_Stick(l.x);
	c->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTY] = NX_Stick(-l.y);
	c->gamepad_axes[AERON_GAMEPAD_AXIS_RIGHTX] = NX_Stick(r.x);
	c->gamepad_axes[AERON_GAMEPAD_AXIS_RIGHTY] = NX_Stick(-r.y);
	c->gamepad_axes[AERON_GAMEPAD_AXIS_LEFT_TRIGGER] = (held & HidNpadButton_ZL) ? 32767 : 0;
	c->gamepad_axes[AERON_GAMEPAD_AXIS_RIGHT_TRIGGER] = (held & HidNpadButton_ZR) ? 32767 : 0;
	/* L3 / R3 pressed alone act as buttons (a short pulse on release); pressed
	 * together they only toggle the gyro and are never reported. */
	{
		static struct {
			uint64_t down_us;
			int down, chorded, pulse;
		} st[2];
		uint64_t now_us = Aeron_NowUs();
		int both = (held & HidNpadButton_StickL) && (held & HidNpadButton_StickR);
		for (int i = 0; i < 2; ++i) {
			int d = (held & (i ? HidNpadButton_StickR : HidNpadButton_StickL)) != 0;
			if (d && !st[i].down) {
				st[i].down_us = now_us;
				st[i].chorded = 0;
			}
			if (d && both)
				st[i].chorded = 1;
			if (!d && st[i].down && !st[i].chorded && now_us - st[i].down_us < 600000)
				st[i].pulse = 3;
			st[i].down = d;
			if (st[i].pulse > 0) {
				b |= 1u << (i ? AERON_GAMEPAD_BUTTON_RIGHT_STICK : AERON_GAMEPAD_BUTTON_LEFT_STICK);
				--st[i].pulse;
			}
		}
	}
	c->gamepad_buttons = b;
	c->gamepad_pressed_buttons = b & ~prev;
	c->gamepad_released_buttons = prev & ~b;

	/* L3+R3 toggles the gyro once per press of the chord. */
	int chord = (held & HidNpadButton_StickL) && (held & HidNpadButton_StickR);
	if (chord && !nx.chord_latched) {
		nx.gyro_on ^= 1;
		NX_Pulse(nx.gyro_on ? 2 : 1);
	}
	nx.chord_latched = chord;

	/* Minus: Escape (settings menu, skips the intro) in the frontend, but in flight
	 * it toggles classic/modern graphics (the Tab shortcut). Plus keeps the pause
	 * binding; holding it in flight opens the settings menu (see settings.c). */
	static int minus_prev, minus_key;
	int minus = (held & HidNpadButton_Minus) != 0;
	if (minus != minus_prev) {
		if (minus)
			minus_key = g_aeron.relative_mouse_enabled ? AERON_KEY_TAB : AERON_KEY_ESCAPE;
		NX_Key(input, minus_key, minus);
	}
	minus_prev = minus;
}

/* Mean angular velocity (1.0 = one revolution per second) over the sensor
 * samples that arrived since the last call. Returns 0 when none did. */
static int NX_Gyro(float* gx, float* gy, float* gz) {
	int style = (int)padGetStyleSet(&nx.pad);
	int h = (style & HidNpadStyleTag_NpadHandheld) ? 3 : (style & HidNpadStyleTag_NpadFullKey) ? 2 : 0;
	HidSixAxisSensorState st[17];
	int n = (int)hidGetSixAxisSensorStates(nx.sixaxis[h], st, 17);
	float sx = 0, sy = 0, sz = 0;
	int fresh = 0;
	uint64_t newest = nx.last_sample[h];
	for (int i = 0; i < n; ++i) {
		if (st[i].sampling_number <= nx.last_sample[h])
			continue;
		sx += st[i].angular_velocity.x;
		sy += st[i].angular_velocity.y;
		sz += st[i].angular_velocity.z;
		if (st[i].sampling_number > newest)
			newest = st[i].sampling_number;
		++fresh;
	}
	nx.last_sample[h] = newest;
	if (!fresh)
		return 0;
	*gx = sx / fresh;
	*gy = sy / fresh;
	*gz = sz / fresh;
	return 1;
}

/* In relative mode (flight) the gyro supplies motion deltas; outside flight the
 * pointer only follows touch input. */
void AeronSwitch_UpdateMouse(AeronInputSnapshot* input, float dt_sec) {
	const float w = (float)(input->window_width > 0 ? input->window_width : 1280);
	const float h = (float)(input->window_height > 0 ? input->window_height : 720);
	if (!nx.cursor_ready) {
		nx.cursor_x = w * 0.5f;
		nx.cursor_y = h * 0.5f;
		nx.cursor_ready = 1;
	}
	if (g_aeron.relative_mouse_enabled) {
		input->mouse.buttons = 0;
		input->mouse.pressed_buttons = input->mouse.released_buttons = 0;
		if (nx.gyro_on) {
			float gx = 0, gy = 0, gz = 0;
			if (NX_Gyro(&gx, &gy, &gz)) {
				/* Horizontal turn is tilt (Y) plus sweep (Z); vertical is the nod (X). */
				float dx = (gy * nx.gyro_yaw + gz * nx.gyro_sweep) * 360.0f * dt_sec;
				float dy = gx * nx.gyro_pitch * 360.0f * dt_sec;
				input->mouse.relative_x += nx.invert_x ? -dx : dx;
				input->mouse.relative_y += nx.invert_y ? -dy : dy;
			}
		}
		return;
	}
	/* Menus: the game moves its own cursor from the stick axes and A / B, so only a
	 * touch moves the Aeron pointer here. Driving a second cursor from the same stick
	 * made the two fight each other. */
	if (input->mouse.relative_x != 0.0f || input->mouse.relative_y != 0.0f) {
		nx.cursor_x = (float)input->mouse.raw_x;
		nx.cursor_y = (float)input->mouse.raw_y;
	}
	nx.cursor_x = nx.cursor_x < 0 ? 0 : nx.cursor_x > w - 1 ? w - 1 : nx.cursor_x;
	nx.cursor_y = nx.cursor_y < 0 ? 0 : nx.cursor_y > h - 1 ? h - 1 : nx.cursor_y;
	input->mouse.raw_x = (int)nx.cursor_x;
	input->mouse.raw_y = (int)nx.cursor_y;
}

/* Opens or closes the Switch keyboard (used while a name field is on screen). */
void AeronSwitch_TextInput(int enable) {
	if (!g_aeron.window)
		return;
	if (enable)
		SDL_StartTextInput(g_aeron.window);
	else
		SDL_StopTextInput(g_aeron.window);
}
#endif
