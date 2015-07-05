/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#endif
#include <string.h>
#include <stdlib.h>

#include "io.h"
#include "blastem.h"
#include "render.h"

const char * device_type_names[] = {
	"3-button gamepad",
	"6-button gamepad",
	"Mega Mouse",
	"Menacer",
	"Justifier",
	"Sega multi-tap",
	"EA 4-way Play cable A",
	"EA 4-way Play cable B",
	"Sega Parallel Transfer Board",
	"Generic Device",
	"None"
};

enum {
	BIND_NONE,
	BIND_UI,
	BIND_GAMEPAD1,
	BIND_GAMEPAD2,
	BIND_GAMEPAD3,
	BIND_GAMEPAD4,
	BIND_GAMEPAD5,
	BIND_GAMEPAD6,
	BIND_GAMEPAD7,
	BIND_GAMEPAD8
};

typedef enum {
	UI_DEBUG_MODE_INC,
	UI_DEBUG_PAL_INC,
	UI_ENTER_DEBUGGER,
	UI_SAVE_STATE,
	UI_SET_SPEED,
	UI_NEXT_SPEED,
	UI_PREV_SPEED,
	UI_EXIT
} ui_action;

typedef struct {
	io_port *port;
	uint8_t bind_type;
	uint8_t subtype_a;
	uint8_t subtype_b;
	uint8_t value;
} keybinding;

typedef struct {
	keybinding bindings[4];
	uint8_t    state;
} joydpad;

keybinding * bindings[256];
keybinding * joybindings[MAX_JOYSTICKS];
joydpad * joydpads[MAX_JOYSTICKS];
const uint8_t dpadbits[] = {RENDER_DPAD_UP, RENDER_DPAD_DOWN, RENDER_DPAD_LEFT, RENDER_DPAD_RIGHT};

void bind_key(int keycode, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b, uint8_t value)
{
	int bucket = keycode >> 8 & 0xFF;
	if (!bindings[bucket]) {
		bindings[bucket] = malloc(sizeof(keybinding) * 256);
		memset(bindings[bucket], 0, sizeof(keybinding) * 256);
	}
	int idx = keycode & 0xFF;
	bindings[bucket][idx].bind_type = bind_type;
	bindings[bucket][idx].subtype_a = subtype_a;
	bindings[bucket][idx].subtype_b = subtype_b;
	bindings[bucket][idx].value = value;
}

void bind_button(int joystick, int button, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b, uint8_t value)
{
	if (joystick >= MAX_JOYSTICKS) {
		return;
	}
	if (!joybindings[joystick]) {
		int num = render_joystick_num_buttons(joystick);
		if (!num) {
			return;
		}
		joybindings[joystick] = malloc(sizeof(keybinding)*num);
		memset(joybindings[joystick], 0, sizeof(keybinding)*num);
	}
	joybindings[joystick][button].bind_type = bind_type;
	joybindings[joystick][button].subtype_a = subtype_a;
	joybindings[joystick][button].subtype_b = subtype_b;
	joybindings[joystick][button].value = value;
}

void bind_dpad(int joystick, int dpad, int direction, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b, uint8_t value)
{
	if (joystick >= MAX_JOYSTICKS) {
		return;
	}
	if (!joydpads[joystick]) {
		int num = render_joystick_num_hats(joystick);
		if (!num) {
			return;
		}
		joydpads[joystick] = malloc(sizeof(joydpad)*num);
		memset(joydpads[joystick], 0, sizeof(joydpad)*num);
	}
	for (int i = 0; i < 4; i ++) {
		if (dpadbits[i] & direction) {
			joydpads[joystick][dpad].bindings[i].bind_type = bind_type;
			joydpads[joystick][dpad].bindings[i].subtype_a = subtype_a;
			joydpads[joystick][dpad].bindings[i].subtype_b = subtype_b;
			joydpads[joystick][dpad].bindings[i].value = value;
			break;
		}
	}
}

#define GAMEPAD_BUTTON(PRI_SLOT, SEC_SLOT, VALUE)  (PRI_SLOT << 12 | SEC_SLOT << 8 | VALUE)

#define DPAD_UP      GAMEPAD_BUTTON(GAMEPAD_TH0, GAMEPAD_TH1, 0x01)
#define BUTTON_Z     GAMEPAD_BUTTON(GAMEPAD_EXTRA, GAMEPAD_NONE, 0x01)
#define DPAD_DOWN    GAMEPAD_BUTTON(GAMEPAD_TH0, GAMEPAD_TH1, 0x02)
#define BUTTON_Y     GAMEPAD_BUTTON(GAMEPAD_EXTRA, GAMEPAD_NONE, 0x02)
#define DPAD_LEFT    GAMEPAD_BUTTON(GAMEPAD_TH1, GAMEPAD_NONE, 0x04)
#define BUTTON_X     GAMEPAD_BUTTON(GAMEPAD_EXTRA, GAMEPAD_NONE, 0x04)
#define DPAD_RIGHT   GAMEPAD_BUTTON(GAMEPAD_TH1, GAMEPAD_NONE, 0x08)
#define BUTTON_MODE  GAMEPAD_BUTTON(GAMEPAD_EXTRA, GAMEPAD_NONE, 0x08)
#define BUTTON_A     GAMEPAD_BUTTON(GAMEPAD_TH0, GAMEPAD_NONE, 0x10)
#define BUTTON_B     GAMEPAD_BUTTON(GAMEPAD_TH1, GAMEPAD_NONE, 0x10)
#define BUTTON_START GAMEPAD_BUTTON(GAMEPAD_TH0, GAMEPAD_NONE, 0x20)
#define BUTTON_C     GAMEPAD_BUTTON(GAMEPAD_TH1, GAMEPAD_NONE, 0x20)

void bind_gamepad(int keycode, int gamepadnum, int button)
{

	if (gamepadnum < 1 || gamepadnum > 8) {
		return;
	}
	uint8_t bind_type = gamepadnum - 1 + BIND_GAMEPAD1;
	bind_key(keycode, bind_type, button >> 12, button >> 8 & 0xF, button & 0xFF);
}

void bind_button_gamepad(int joystick, int joybutton, int gamepadnum, int padbutton)
{
	if (gamepadnum < 1 || gamepadnum > 8) {
		return;
	}
	uint8_t bind_type = gamepadnum - 1 + BIND_GAMEPAD1;
	bind_button(joystick, joybutton, bind_type, padbutton >> 12, padbutton >> 8 & 0xF, padbutton & 0xFF);
}

void bind_dpad_gamepad(int joystick, int dpad, uint8_t direction, int gamepadnum, int button)
{
	if (gamepadnum < 1 || gamepadnum > 8) {
		return;
	}
	uint8_t bind_type = gamepadnum - 1 + BIND_GAMEPAD1;
	bind_dpad(joystick, dpad, direction, bind_type, button >> 12, button >> 8 & 0xF, button & 0xFF);
}

void bind_ui(int keycode, ui_action action, uint8_t param)
{
	bind_key(keycode, BIND_UI, action, 0, param);
}

void bind_button_ui(int joystick, int joybutton, ui_action action, uint8_t param)
{
	bind_button(joystick, joybutton, BIND_UI, action, 0, param);
}

void bind_dpad_ui(int joystick, int dpad, uint8_t direction, ui_action action, uint8_t param)
{
	bind_dpad(joystick, dpad, direction, BIND_UI, action, 0, param);
}

void handle_binding_down(keybinding * binding)
{
	if (binding->bind_type >= BIND_GAMEPAD1)
	{
		if (binding->subtype_a <= GAMEPAD_EXTRA && binding->port) {
			binding->port->input[binding->subtype_a] |= binding->value;
		}
		if (binding->subtype_b <= GAMEPAD_EXTRA && binding->port) {
			binding->port->input[binding->subtype_b] |= binding->value;
		}
	}
}

void handle_keydown(int keycode)
{
	int bucket = keycode >> 8 & 0xFF;
	if (!bindings[bucket]) {
		return;
	}
	int idx = keycode & 0xFF;
	keybinding * binding = bindings[bucket] + idx;
	handle_binding_down(binding);
}

void handle_joydown(int joystick, int button)
{
	if (!joybindings[joystick]) {
		return;
	}
	keybinding * binding = joybindings[joystick] + button;
	handle_binding_down(binding);
}

uint8_t ui_debug_mode = 0;
uint8_t ui_debug_pal = 0;

int current_speed = 0;
int num_speeds = 1;
uint32_t * speeds = NULL;

void handle_binding_up(keybinding * binding)
{
	switch(binding->bind_type)
	{
	case BIND_GAMEPAD1:
	case BIND_GAMEPAD2:
		if (binding->subtype_a <= GAMEPAD_EXTRA && binding->port) {
			binding->port->input[binding->subtype_a] &= ~binding->value;
		}
		if (binding->subtype_b <= GAMEPAD_EXTRA && binding->port) {
			binding->port->input[binding->subtype_b] &= ~binding->value;
		}
		break;
	case BIND_UI:
		switch (binding->subtype_a)
		{
		case UI_DEBUG_MODE_INC:
			ui_debug_mode++;
			if (ui_debug_mode == 4) {
				ui_debug_mode = 0;
			}
			genesis->vdp->debug = ui_debug_mode;
			break;
		case UI_DEBUG_PAL_INC:
			ui_debug_pal++;
			if (ui_debug_pal == 4) {
				ui_debug_pal = 0;
			}
			genesis->vdp->debug_pal = ui_debug_pal;
			break;
		case UI_ENTER_DEBUGGER:
			break_on_sync = 1;
			break;
		case UI_SAVE_STATE:
			save_state = 1;
			break;
		case UI_NEXT_SPEED:
			current_speed++;
			if (current_speed >= num_speeds) {
				current_speed = 0;
			}
			printf("Setting speed to %d: %d\n", current_speed, speeds[current_speed]);
			set_speed_percent(genesis, speeds[current_speed]);
			break;
		case UI_PREV_SPEED:
			current_speed--;
			if (current_speed < 0) {
				current_speed = num_speeds - 1;
			}
			printf("Setting speed to %d: %d\n", current_speed, speeds[current_speed]);
			set_speed_percent(genesis, speeds[current_speed]);
			break;
		case UI_SET_SPEED:
			if (binding->value < num_speeds) {
				current_speed = binding->value;
				printf("Setting speed to %d: %d\n", current_speed, speeds[current_speed]);
				set_speed_percent(genesis, speeds[current_speed]);
			} else {
				printf("Setting speed to %d\n", speeds[current_speed]);
				set_speed_percent(genesis, binding->value);
			}
			break;
		case UI_EXIT:
			exit(0);
		}
		break;
	}
}

void handle_keyup(int keycode)
{
	int bucket = keycode >> 8 & 0xFF;
	if (!bindings[bucket]) {
		return;
	}
	int idx = keycode & 0xFF;
	keybinding * binding = bindings[bucket] + idx;
	handle_binding_up(binding);
}

void handle_joyup(int joystick, int button)
{
	if (!joybindings[joystick]) {
		return;
	}
	keybinding * binding = joybindings[joystick] + button;
	handle_binding_up(binding);
}

void handle_joy_dpad(int joystick, int dpadnum, uint8_t value)
{
	if (!joydpads[joystick]) {
		return;
	}
	joydpad * dpad = joydpads[joystick] + dpadnum;
	uint8_t newdown = (value ^ dpad->state) & value;
	uint8_t newup = ((~value) ^ (~dpad->state)) & (~value);
	dpad->state = value;
	for (int i = 0; i < 4; i++) {
		if (newdown & dpadbits[i]) {
			handle_binding_down(dpad->bindings + i);
		} else if(newup & dpadbits[i]) {
			handle_binding_up(dpad->bindings + i);
		}
	}
}

int parse_binding_target(char * target, tern_node * padbuttons, int * ui_out, int * padnum_out, int * padbutton_out)
{
	int gpadslen = strlen("gamepads.");
	if (!memcmp(target, "gamepads.", gpadslen)) {
		if (target[gpadslen] >= '1' && target[gpadslen] <= '8') {
			int padnum = target[gpadslen] - '0';
			int button = tern_find_int(padbuttons, target + gpadslen + 1, 0);
			if (button) {
				*padnum_out = padnum;
				*padbutton_out = button;
				return 1;
			} else {
				if (target[gpadslen+1]) {
					fprintf(stderr, "Gamepad mapping string '%s' refers to an invalid button '%s'\n", target, target + gpadslen + 1);
				} else {
					fprintf(stderr, "Gamepad mapping string '%s' has no button component\n", target);
				}
			}
		} else {
			fprintf(stderr, "Gamepad mapping string '%s' refers to an invalid gamepad number %c\n", target, target[gpadslen]);
		}
	} else if(!memcmp(target, "ui.", strlen("ui."))) {
		*padbutton_out = 0;
		if (!strcmp(target + 3, "vdp_debug_mode")) {
			*ui_out = UI_DEBUG_MODE_INC;
		} else if(!strcmp(target + 3, "vdp_debug_pal")) {
			*ui_out = UI_DEBUG_PAL_INC;
		} else if(!strcmp(target + 3, "enter_debugger")) {
			*ui_out = UI_ENTER_DEBUGGER;
		} else if(!strcmp(target + 3, "save_state")) {
			*ui_out = UI_SAVE_STATE;
		} else if(!memcmp(target + 3, "set_speed.", strlen("set_speed."))) {
			*ui_out = UI_SET_SPEED;
			*padbutton_out = atoi(target + 3 + strlen("set_speed."));
		} else if(!strcmp(target + 3, "next_speed")) {
			*ui_out = UI_NEXT_SPEED;
		} else if(!strcmp(target + 3, "prev_speed")) {
			*ui_out = UI_PREV_SPEED;
		} else if(!strcmp(target + 3, "exit")) {
			*ui_out = UI_EXIT;
		} else {
			fprintf(stderr, "Unreconized UI binding type %s\n", target);
			return 0;
		}
		return 2;
	} else {
		fprintf(stderr, "Unrecognized binding type %s\n", target);
	}
	return 0;
}

void process_keys(tern_node * cur, tern_node * special, tern_node * padbuttons, char * prefix)
{
	char * curstr = NULL;
	int len;
	if (!cur) {
		return;
	}
	char onec[2];
	if (prefix) {
		len = strlen(prefix);
		curstr = malloc(len + 2);
		memcpy(curstr, prefix, len);
	} else {
		curstr = onec;
		len = 0;
	}
	curstr[len] = cur->el;
	curstr[len+1] = 0;
	if (cur->el) {
		process_keys(cur->straight.next, special, padbuttons, curstr);
	} else {
		int keycode = tern_find_int(special, curstr, 0);
		if (!keycode) {
			keycode = curstr[0];
			if (curstr[1] != 0) {
				fprintf(stderr, "%s is not recognized as a key identifier, truncating to %c\n", curstr, curstr[0]);
			}
		}
		char * target = cur->straight.value.ptrval;
		int ui_func, padnum, button;
		int bindtype = parse_binding_target(target, padbuttons, &ui_func, &padnum, &button);
		if (bindtype == 1) {
			bind_gamepad(keycode, padnum, button);
		} else if(bindtype == 2) {
			bind_ui(keycode, ui_func, button);
		}
	}
	process_keys(cur->left, special, padbuttons, prefix);
	process_keys(cur->right, special, padbuttons, prefix);
	if (curstr && len) {
		free(curstr);
	}
}

void process_speeds(tern_node * cur, char * prefix)
{
	char * curstr = NULL;
	int len;
	if (!cur) {
		return;
	}
	char onec[2];
	if (prefix) {
		len = strlen(prefix);
		curstr = malloc(len + 2);
		memcpy(curstr, prefix, len);
	} else {
		curstr = onec;
		len = 0;
	}
	curstr[len] = cur->el;
	curstr[len+1] = 0;
	if (cur->el) {
		process_speeds(cur->straight.next, curstr);
	} else {
		int speed_index = atoi(curstr);
		if (speed_index < 1) {
			if (!strcmp(curstr, "0")) {
				fputs("Speed index 0 cannot be set to a custom value\n", stderr);
			} else {
				fprintf(stderr, "%s is not a valid speed index", curstr);
			}
		} else {
			if (speed_index >= num_speeds) {
				speeds = realloc(speeds, sizeof(uint32_t) * (speed_index+1));
				for(; num_speeds < speed_index + 1; num_speeds++) {
					speeds[num_speeds] = 0;
				}
			}
			speeds[speed_index] = atoi(cur->straight.value.ptrval);
		}
	}
	process_speeds(cur->left, prefix);
	process_speeds(cur->right, prefix);
	if (curstr && len) {
		free(curstr);
	}
}

void process_device(char * device_type, io_port * port)
{
	port->device_type = IO_NONE;
	if (!device_type)
	{
		return;
	}

	const int gamepad_len = strlen("gamepad");
	if (!memcmp(device_type, "gamepad", gamepad_len))
	{
		if (
			(device_type[gamepad_len] != '3' && device_type[gamepad_len] != '6')
			|| device_type[gamepad_len+1] != '.' || device_type[gamepad_len+2] < '1'
			|| device_type[gamepad_len+2] > '8' || device_type[gamepad_len+3] != 0
		)
		{
			fprintf(stderr, "%s is not a valid gamepad type\n", device_type);
		} else if (device_type[gamepad_len] == '3')
		{
			port->device_type = IO_GAMEPAD3;
		} else {
			port->device_type = IO_GAMEPAD6;
		}
		port->device.pad.gamepad_num = device_type[gamepad_len+2] - '1';
	} else if(!strcmp(device_type, "sega_parallel")) {
		port->device_type = IO_SEGA_PARALLEL;
		port->device.stream.data_fd = -1;
		port->device.stream.listen_fd = -1;
	} else if(!strcmp(device_type, "generic")) {
		port->device_type = IO_GENERIC;
		port->device.stream.data_fd = -1;
		port->device.stream.listen_fd = -1;
	}
}

char * io_name(int i)
{
	switch (i)
	{
	case 0:
		return "1";
	case 1:
		return "2";
	case 2:
		return "EXT";
	default:
		return "invalid";
	}
}

static char * sockfile_name;
static void cleanup_sockfile()
{
	unlink(sockfile_name);
}

void setup_io_devices(tern_node * config, io_port * ports)
{
	tern_node *io_nodes = tern_get_node(tern_find_path(config, "io\0devices\0"));
	char * io_1 = tern_find_ptr(io_nodes, "1");
	char * io_2 = tern_find_ptr(io_nodes, "2");
	char * io_ext = tern_find_ptr(io_nodes, "ext");

	process_device(io_1, ports);
	process_device(io_2, ports+1);
	process_device(io_ext, ports+2);

	for (int i = 0; i < 3; i++)
	{
#ifndef _WIN32
		if (ports[i].device_type == IO_SEGA_PARALLEL)
		{
			char *pipe_name = tern_find_path(config, "io\0parallel_pipe\0").ptrval;
			if (!pipe_name)
			{
				fprintf(stderr, "IO port %s is configured to use the sega parallel board, but no paralell_pipe is set!\n", io_name(i));
				ports[i].device_type = IO_NONE;
			} else {
				printf("IO port: %s connected to device '%s' with pipe name: %s\n", io_name(i), device_type_names[ports[i].device_type], pipe_name);
				if (!strcmp("stdin", pipe_name))
				{
					ports[i].device.stream.data_fd = STDIN_FILENO;
				} else {
					if (mkfifo(pipe_name, 0666) && errno != EEXIST)
					{
						fprintf(stderr, "Failed to create fifo %s for Sega parallel board emulation: %d %s\n", pipe_name, errno, strerror(errno));
						ports[i].device_type = IO_NONE;
					} else {
						ports[i].device.stream.data_fd = open(pipe_name, O_NONBLOCK | O_RDONLY);
						if (ports[i].device.stream.data_fd == -1)
						{
							fprintf(stderr, "Failed to open fifo %s for Sega parallel board emulation: %d %s\n", pipe_name, errno, strerror(errno));
							ports[i].device_type = IO_NONE;
						}
					}
				}
			}
		} else if (ports[i].device_type == IO_GENERIC) {
			char *sock_name = tern_find_path(config, "io\0socket\0").ptrval;
			if (!sock_name)
			{
				fprintf(stderr, "IO port %s is configured to use generic IO, but no socket is set!\n", io_name(i));
				ports[i].device_type = IO_NONE;
			} else {
				printf("IO port: %s connected to device '%s' with socket name: %s\n", io_name(i), device_type_names[ports[i].device_type], sock_name);
				ports[i].device.stream.data_fd = -1;
				ports[i].device.stream.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
				size_t pathlen = strlen(sock_name);
				size_t addrlen = offsetof(struct sockaddr_un, sun_path) + pathlen + 1;
				struct sockaddr_un *saddr = malloc(addrlen);
				saddr->sun_family = AF_UNIX;
				memcpy(saddr->sun_path, sock_name, pathlen+1);
				if (bind(ports[i].device.stream.listen_fd, (struct sockaddr *)saddr, addrlen))
				{
					fprintf(stderr, "Failed to bind socket for IO Port %s to path %s: %d %s\n", io_name(i), sock_name, errno, strerror(errno));
					goto cleanup_sock;
				}
				if (listen(ports[i].device.stream.listen_fd, 1))
				{
					fprintf(stderr, "Failed to listen on socket for IO Port %s: %d %s\n", io_name(i), errno, strerror(errno));
					goto cleanup_sockfile;
				}
				sockfile_name = sock_name;
				atexit(cleanup_sockfile);
				continue;
cleanup_sockfile:
				unlink(sock_name);
cleanup_sock:
				close(ports[i].device.stream.listen_fd);
				ports[i].device_type = IO_NONE;
			}
		} else 
#endif
		if (ports[i].device_type == IO_GAMEPAD3 || ports[i].device_type == IO_GAMEPAD6) {
			printf("IO port %s connected to gamepad #%d with type '%s'\n", io_name(i), ports[i].device.pad.gamepad_num + 1, device_type_names[ports[i].device_type]);
		} else {
			printf("IO port %s connected to device '%s'\n", io_name(i), device_type_names[ports[i].device_type]);
		}
	}
}

void map_bindings(io_port *ports, keybinding *bindings, int numbindings)
{
	for (int i = 0; i < numbindings; i++)
	{
		if (bindings[i].bind_type >= BIND_GAMEPAD1)
		{
			int num = bindings[i].bind_type - BIND_GAMEPAD1;
			for (int j = 0; j < 3; j++)
			{
				if ((ports[j].device_type == IO_GAMEPAD3
					 || ports[j].device_type ==IO_GAMEPAD6)
					 && ports[j].device.pad.gamepad_num == num
				)
				{
					bindings[i].port = ports + j;
					break;
				}
			}
		}
	}
}

void set_keybindings(io_port *ports)
{
	tern_node * special = tern_insert_int(NULL, "up", RENDERKEY_UP);
	special = tern_insert_int(special, "down", RENDERKEY_DOWN);
	special = tern_insert_int(special, "left", RENDERKEY_LEFT);
	special = tern_insert_int(special, "right", RENDERKEY_RIGHT);
	special = tern_insert_int(special, "enter", '\r');
	special = tern_insert_int(special, "esc", RENDERKEY_ESC);
	special = tern_insert_int(special, "lshift", RENDERKEY_LSHIFT);
	special = tern_insert_int(special, "rshift", RENDERKEY_RSHIFT);

	tern_node * padbuttons = tern_insert_int(NULL, ".up", DPAD_UP);
	padbuttons = tern_insert_int(padbuttons, ".down", DPAD_DOWN);
	padbuttons = tern_insert_int(padbuttons, ".left", DPAD_LEFT);
	padbuttons = tern_insert_int(padbuttons, ".right", DPAD_RIGHT);
	padbuttons = tern_insert_int(padbuttons, ".a", BUTTON_A);
	padbuttons = tern_insert_int(padbuttons, ".b", BUTTON_B);
	padbuttons = tern_insert_int(padbuttons, ".c", BUTTON_C);
	padbuttons = tern_insert_int(padbuttons, ".x", BUTTON_X);
	padbuttons = tern_insert_int(padbuttons, ".y", BUTTON_Y);
	padbuttons = tern_insert_int(padbuttons, ".z", BUTTON_Z);
	padbuttons = tern_insert_int(padbuttons, ".start", BUTTON_START);
	padbuttons = tern_insert_int(padbuttons, ".mode", BUTTON_MODE);

	tern_node * keys = tern_get_node(tern_find_path(config, "bindings\0keys\0"));
	process_keys(keys, special, padbuttons, NULL);
	char numstr[] = "00";
	tern_node * pads = tern_get_node(tern_find_path(config, "bindings\0pads\0"));
	if (pads) {
		for (int i = 0; i < 100 && i < render_num_joysticks(); i++)
		{
		
			if (i < 10) {
				numstr[0] = i + '0';
				numstr[1] = 0;
			} else {
				numstr[0] = i/10 + '0';
				numstr[1] = i%10 + '0';
			}
			tern_node * pad = tern_find_ptr(pads, numstr);
			if (pad) {
				tern_node * dpad_node = tern_find_ptr(pad, "dpads");
				if (dpad_node) {
					for (int dpad = 0; dpad < 10 && dpad < render_joystick_num_hats(i); dpad++)
					{
						numstr[0] = dpad + '0';
						numstr[1] = 0;
						tern_node * pad_dpad = tern_find_ptr(dpad_node, numstr);
						char * dirs[] = {"up", "down", "left", "right"};
						int dirnums[] = {RENDER_DPAD_UP, RENDER_DPAD_DOWN, RENDER_DPAD_LEFT, RENDER_DPAD_RIGHT};
						for (int dir = 0; dir < sizeof(dirs)/sizeof(dirs[0]); dir++) {
							char * target = tern_find_ptr(pad_dpad, dirs[dir]);
							if (target) {
								int ui_func, padnum, button;
								int bindtype = parse_binding_target(target, padbuttons, &ui_func, &padnum, &button);
								if (bindtype == 1) {
									bind_dpad_gamepad(i, dpad, dirnums[dir], padnum, button);
								} else if (bindtype == 2) {
									bind_dpad_ui(i, dpad, dirnums[dir], ui_func, button);
								}
							}
						}
					}
				}
				tern_node *button_node = tern_find_ptr(pad, "buttons");
				if (button_node) {
					for (int but = 0; but < 100 && but < render_joystick_num_buttons(i); but++)
					{
						if (but < 10) {
							numstr[0] = but + '0';
							numstr[1] = 0;
						} else {
							numstr[0] = but/10 + '0';
							numstr[1] = but%10 + '0';
						}
						char * target = tern_find_ptr(button_node, numstr);
						if (target) {
							int ui_func, padnum, button;
							int bindtype = parse_binding_target(target, padbuttons, &ui_func, &padnum, &button);
							if (bindtype == 1) {
								bind_button_gamepad(i, but, padnum, button);
							} else if (bindtype == 2) {
								bind_button_ui(i, but, ui_func, button);
							}
						}
					}
				}
			}
		}
	}
	tern_node * speed_nodes = tern_get_node(tern_find_path(config, "clocks\0speeds\0"));
	speeds = malloc(sizeof(uint32_t));
	speeds[0] = 100;
	process_speeds(speed_nodes, NULL);
	for (int i = 0; i < num_speeds; i++)
	{
		if (!speeds[i]) {
			fprintf(stderr, "Speed index %d was not set to a valid percentage!", i);
			speeds[i] = 100;
		}
	}
	for (int bucket = 0; bucket < 256; bucket++)
	{
		if (bindings[bucket])
		{
			map_bindings(ports, bindings[bucket], 256);
		}
	}
	for (int stick = 0; stick < MAX_JOYSTICKS; stick++)
	{
		if (joybindings[stick])
		{
			int numbuttons = render_joystick_num_buttons(stick);
			map_bindings(ports, joybindings[stick], render_joystick_num_buttons(stick));
		}
		if (joydpads[stick])
		{
			map_bindings(ports, joydpads[stick]->bindings, 4);
		}
	}
}

#define TH 0x40
#define TH_TIMEOUT 56000

void io_adjust_cycles(io_port * port, uint32_t current_cycle, uint32_t deduction)
{
	/*uint8_t control = pad->control | 0x80;
	uint8_t th = control & pad->output;
	if (pad->input[GAMEPAD_TH0] || pad->input[GAMEPAD_TH1]) {
		printf("adjust_cycles | control: %X, TH: %X, GAMEPAD_TH0: %X, GAMEPAD_TH1: %X, TH Counter: %d, Timeout: %d, Cycle: %d\n", control, th, pad->input[GAMEPAD_TH0], pad->input[GAMEPAD_TH1], pad->th_counter,pad->timeout_cycle, current_cycle);
	}*/
	if (port->device_type == IO_GAMEPAD6)
	{
		if (current_cycle >= port->device.pad.timeout_cycle)
		{
			port->device.pad.th_counter = 0;
		} else {
			port->device.pad.timeout_cycle -= deduction;
		}
	}
}

#ifndef _WIN32
static void wait_for_connection(io_port * port)
{
	if (port->device.stream.data_fd == -1)
	{
		puts("Waiting for socket connection...");
		port->device.stream.data_fd = accept(port->device.stream.listen_fd, NULL, NULL);
		fcntl(port->device.stream.data_fd, F_SETFL, O_NONBLOCK | O_RDWR);
	}
}

static void service_pipe(io_port * port)
{
	uint8_t value;
	int numRead = read(port->device.stream.data_fd, &value, sizeof(value));
	if (numRead > 0)
	{
		port->input[IO_TH0] = (value & 0xF) | 0x10;
		port->input[IO_TH1] = (value >> 4) | 0x10;
	} else if(numRead == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
		fprintf(stderr, "Error reading pipe for IO port: %d %s\n", errno, strerror(errno));
	}
}

static void service_socket(io_port *port)
{
	uint8_t buf[32];
	uint8_t blocking = 0;
	int numRead = 0;
	while (numRead <= 0)
	{
		numRead = recv(port->device.stream.data_fd, buf, sizeof(buf), 0);
		if (numRead > 0)
		{
			port->input[IO_TH0] = buf[numRead-1];
			if (port->input[IO_STATE] == IO_READ_PENDING)
			{
				port->input[IO_STATE] = IO_READ;
				if (blocking)
				{
					//pending read satisfied, back to non-blocking mode
					fcntl(port->device.stream.data_fd, F_SETFL, O_RDWR | O_NONBLOCK);
				}
			} else if (port->input[IO_STATE] == IO_WRITTEN) {
				port->input[IO_STATE] = IO_READ;
			}
		} else if (numRead == 0) {
			port->device.stream.data_fd = -1;
			wait_for_connection(port);
		} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
			fprintf(stderr, "Error reading from socket for IO port: %d %s\n", errno, strerror(errno));
			close(port->device.stream.data_fd);
			wait_for_connection(port);
		} else if (port->input[IO_STATE] == IO_READ_PENDING) {
			//clear the nonblocking flag so the next read will block
			if (!blocking)
			{
				fcntl(port->device.stream.data_fd, F_SETFL, O_RDWR);
				blocking = 1;
			}
		} else {
			//no new data, but that's ok
			break;
		}
	}

	if (port->input[IO_STATE] == IO_WRITE_PENDING)
	{
		uint8_t value = port->output & port->control;
		int written = 0;
		blocking = 0;
		while (written <= 0)
		{
			send(port->device.stream.data_fd, &value, sizeof(value), 0);
			if (written > 0)
			{
				port->input[IO_STATE] = IO_WRITTEN;
				if (blocking)
				{
					//pending write satisfied, back to non-blocking mode
					fcntl(port->device.stream.data_fd, F_SETFL, O_RDWR | O_NONBLOCK);
				}
			} else if (written == 0) {
				port->device.stream.data_fd = -1;
				wait_for_connection(port);
			} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
				fprintf(stderr, "Error writing to socket for IO port: %d %s\n", errno, strerror(errno));
				close(port->device.stream.data_fd);
				wait_for_connection(port);
			} else {
				//clear the nonblocking flag so the next write will block
				if (!blocking)
				{
					fcntl(port->device.stream.data_fd, F_SETFL, O_RDWR);
					blocking = 1;
				}
			}
		}
	}
}
#endif

void io_data_write(io_port * port, uint8_t value, uint32_t current_cycle)
{
	switch (port->device_type)
	{
	case IO_GAMEPAD6:
		if (port->control & TH) {
			//check if TH has changed
			if ((port->output & TH) ^ (value & TH)) {
				if (current_cycle >= port->device.pad.timeout_cycle) {
					port->device.pad.th_counter = 0;
				}
				if (!(value & TH)) {
					port->device.pad.th_counter++;
				}
				port->device.pad.timeout_cycle = current_cycle + TH_TIMEOUT;
			}
		}
		port->output = value;
		break;
#ifndef _WIN32
	case IO_GENERIC:
		wait_for_connection(port);
		port->input[IO_STATE] = IO_WRITE_PENDING;
		port->output = value;
		service_socket(port);
		break;
#endif
	default:
		port->output = value;
	}

}

uint8_t io_data_read(io_port * port, uint32_t current_cycle)
{
	uint8_t control = port->control | 0x80;
	uint8_t th = control & port->output & 0x40;
	uint8_t input;
	switch (port->device_type)
	{
	case IO_GAMEPAD3:
	{
		input = port->input[th ? GAMEPAD_TH1 : GAMEPAD_TH0];
		break;
	}
	case IO_GAMEPAD6:
	{
		if (current_cycle >= port->device.pad.timeout_cycle) {
			port->device.pad.th_counter = 0;
		}
		/*if (port->input[GAMEPAD_TH0] || port->input[GAMEPAD_TH1]) {
			printf("io_data_read | control: %X, TH: %X, GAMEPAD_TH0: %X, GAMEPAD_TH1: %X, TH Counter: %d, Timeout: %d, Cycle: %d\n", control, th, port->input[GAMEPAD_TH0], port->input[GAMEPAD_TH1], port->th_counter,port->timeout_cycle, context->current_cycle);
		}*/
		if (th) {
			if (port->device.pad.th_counter == 3) {
				input = port->input[GAMEPAD_EXTRA];
			} else {
				input = port->input[GAMEPAD_TH1];
			}
		} else {
			if (port->device.pad.th_counter == 3) {
				input = port->input[GAMEPAD_TH0] | 0xF;
			} else if(port->device.pad.th_counter == 4) {
				input = port->input[GAMEPAD_TH0]  & 0x30;
			} else {
				input = port->input[GAMEPAD_TH0] | 0xC;
			}
		}
		break;
	}
#ifndef _WIN32
	case IO_SEGA_PARALLEL:
		if (!th)
		{
			service_pipe(port);
		}
		input = ~port->input[th ? IO_TH1 : IO_TH0];
		break;
	case IO_GENERIC:
		if (port->input[IO_TH0] & 0x80 && port->input[IO_STATE] == IO_WRITTEN)
		{
			//device requested a blocking read after writes
			port->input[IO_STATE] = IO_READ_PENDING;
		}
		service_socket(port);
		input = ~port->input[IO_TH0];
		break;
#endif
	default:
		input = 0;
		break;
	}
	uint8_t value = ((~input) & (~control)) | (port->output & control);
	/*if (port->input[GAMEPAD_TH0] || port->input[GAMEPAD_TH1]) {
		printf ("value: %X\n", value);
	}*/
	return value;
}


