/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include <string.h>
#include <string>
#include <cstdlib>
#include <vector>
#include "SDL.h"
#include "dosbox.h"
#include "inout.h"
#include "logging.h"
#include "setup.h"
#include "joystick.h"
#include "pic.h"
#include "support.h"
#include "control.h"

#define RANGE 64
#define TIMEOUT 10

#define OHMS 120000/2
#define JOY_S_CONSTANT 0.0000242
#define S_PER_OHM 0.000000011

struct JoyStick {
	bool enabled;
	float xpos,ypos;
	double xtick,ytick;
	Bitu xcount,ycount;
	bool button[2];
};

JoystickType joytype;
static JoyStick stick[2];

static uint32_t last_write = 0;
static bool write_active = false;
static bool swap34 = false;
bool button_wrapping_enabled = true;
bool modjoy_rawvalue_log = false;
bool modjoy_axis_log = false;
ModJoyAxisBinding modjoy_axis_bindings[max_modjoy_axes] = {};

extern bool autofire; //sdl_mapper.cpp
extern int joy1axes[]; //sdl_mapper.cpp
extern int joy2axes[]; //sdl_mapper.cpp

static std::string TrimModJoyString(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

static void ResetModJoyBinding(ModJoyAxisBinding& binding)
{
    binding.configured = false;
    binding.sdl_joystick_index = -1;
    binding.sdl_axis_index = -1;
    binding.sdl_joystick = nullptr;
    binding.joystick_name[0] = 0;
}

static bool ParseModJoyBinding(const std::string& value, ModJoyAxisBinding& binding)
{
    ResetModJoyBinding(binding);

    std::string input = TrimModJoyString(value);
    if (input.empty())
        return false;

    std::string joystick_name = {};
    std::string axis_spec = {};

    if (input[0] == '"') {
        const auto closing_quote = input.find('"', 1);
        if (closing_quote == std::string::npos)
            return false;

        joystick_name = input.substr(1, closing_quote - 1);
        axis_spec = TrimModJoyString(input.substr(closing_quote + 1));
        if (!axis_spec.empty() && axis_spec[0] == ',')
            axis_spec = TrimModJoyString(axis_spec.substr(1));
    } else {
        const auto comma = input.find(',');
        if (comma == std::string::npos)
            return false;

        joystick_name = TrimModJoyString(input.substr(0, comma));
        axis_spec = TrimModJoyString(input.substr(comma + 1));
    }

    if (joystick_name.empty() || axis_spec.size() < 5 || strncasecmp(axis_spec.c_str(), "axis", 4))
        return false;

    char* end_ptr = nullptr;
    const long axis_index = strtol(axis_spec.c_str() + 4, &end_ptr, 10);
    const std::string remainder = TrimModJoyString(end_ptr ? end_ptr : "");
    if (end_ptr == axis_spec.c_str() + 4 || !remainder.empty() || axis_index < 0)
        return false;

    safe_strncpy(binding.joystick_name, joystick_name.c_str(), sizeof(binding.joystick_name));
    binding.configured = true;
    binding.sdl_axis_index = static_cast<int>(axis_index);
    return true;
}

namespace {

struct OpenModJoystickDevice {
    SDL_Joystick* sdl_joystick = nullptr;
    std::string joystick_name = {};
};

class ModJoystickManager {
public:
    void Initialize()
    {
        Shutdown();

        const auto joystick_count = SDL_NumJoysticks();
        opened_devices.reserve(static_cast<size_t>(joystick_count));

        for (int joystick_index = 0; joystick_index < joystick_count; joystick_index++) {
            SDL_Joystick* sdl_joystick = SDL_JoystickOpen(joystick_index);
            if (sdl_joystick == nullptr) {
                LOG_MSG("modjoy: unable to open SDL joystick %d: %s", joystick_index, SDL_GetError());
                continue;
            }

#if defined(C_SDL2)
            const char* joystick_name = SDL_JoystickNameForIndex(joystick_index);
#else
            const char* joystick_name = SDL_JoystickName(joystick_index);
#endif

            OpenModJoystickDevice device = {};
            device.sdl_joystick = sdl_joystick;
            device.joystick_name = joystick_name ? joystick_name : "[unknown joystick]";
            opened_devices.push_back(device);
        }

        ResolveMappings();
        initialized = true;
    }

    void Shutdown()
    {
        for (auto& binding : modjoy_axis_bindings) {
            binding.sdl_joystick_index = -1;
            binding.sdl_joystick = nullptr;
        }

        for (auto& device : opened_devices) {
            if (device.sdl_joystick != nullptr) {
                SDL_JoystickClose(device.sdl_joystick);
                device.sdl_joystick = nullptr;
            }
        }

        opened_devices.clear();
        initialized = false;
    }

    void ResolveMappings()
    {
        for (auto& binding : modjoy_axis_bindings) {
            binding.sdl_joystick_index = -1;
            binding.sdl_joystick = nullptr;

            if (!binding.configured || binding.sdl_axis_index < 0)
                continue;

            for (size_t device_index = 0; device_index < opened_devices.size(); device_index++) {
                auto& device = opened_devices[device_index];
                if (device.joystick_name != binding.joystick_name)
                    continue;
                if (device.sdl_joystick == nullptr)
                    continue;
                if (binding.sdl_axis_index >= SDL_JoystickNumAxes(device.sdl_joystick))
                    continue;

                binding.sdl_joystick = device.sdl_joystick;
                binding.sdl_joystick_index = static_cast<int>(device_index);
                break;
            }
        }
    }

    int16_t GetAxis(const int axis_index) const
    {
        if (axis_index < 0 || axis_index >= max_modjoy_axes)
            return 0;

        const auto& binding = modjoy_axis_bindings[axis_index];
        if (binding.sdl_joystick == nullptr)
            return 0;

        return SDL_JoystickGetAxis(static_cast<SDL_Joystick*>(binding.sdl_joystick),
                                   binding.sdl_axis_index);
    }

    int GetDeviceCount() const
    {
        return static_cast<int>(opened_devices.size());
    }

    const char* GetDeviceName(const int device_index) const
    {
        if (device_index < 0 || device_index >= static_cast<int>(opened_devices.size()))
            return "[unknown joystick]";

        return opened_devices[device_index].joystick_name.c_str();
    }

    int16_t GetDeviceAxisValue(const int device_index, const int axis_index) const
    {
        if (device_index < 0 || device_index >= static_cast<int>(opened_devices.size()))
            return 0;
        if (axis_index < 0 || axis_index >= max_modjoy_axes)
            return 0;

        auto* sdl_joystick = opened_devices[device_index].sdl_joystick;
        if (sdl_joystick == nullptr)
            return 0;
        if (axis_index >= SDL_JoystickNumAxes(sdl_joystick))
            return 0;

        return SDL_JoystickGetAxis(sdl_joystick, axis_index);
    }

private:
    bool initialized = false;
    std::vector<OpenModJoystickDevice> opened_devices = {};
};

ModJoystickManager mod_joystick_manager = {};

}

static Bitu read_p201(Bitu port,Bitu iolen) {
    (void)iolen;//UNUSED
    (void)port;//UNUSED
	/* Reset Joystick to 0 after TIMEOUT ms */
	if(write_active && ((PIC_Ticks - last_write) > TIMEOUT)) {
		write_active = false;
		stick[0].xcount = 0;
		stick[1].xcount = 0;
		stick[0].ycount = 0;
		stick[1].ycount = 0;
//		LOG_MSG("reset by time %d %d",PIC_Ticks,last_write);
	}

	/**  Format of the byte to be returned:       
	**                        | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
	**                        +-------------------------------+
	**                          |   |   |   |   |   |   |   |
	**  Joystick B, Button 2 ---+   |   |   |   |   |   |   +--- Joystick A, X Axis
	**  Joystick B, Button 1 -------+   |   |   |   |   +------- Joystick A, Y Axis
	**  Joystick A, Button 2 -----------+   |   |   +----------- Joystick B, X Axis
	**  Joystick A, Button 1 ---------------+   +--------------- Joystick B, Y Axis
	**/
	uint8_t ret=0xff;
	if (stick[0].enabled) {
		if (stick[0].xcount) stick[0].xcount--; else ret&=~1;
		if (stick[0].ycount) stick[0].ycount--; else ret&=~2;
		if (stick[0].button[0]) ret&=~16;
		if (stick[0].button[1]) ret&=~32;
	}
	if (stick[1].enabled) {
		if (stick[1].xcount) stick[1].xcount--; else ret&=~4;
		if (stick[1].ycount) stick[1].ycount--; else ret&=~8;
		if (stick[1].button[0]) ret&=~64;
		if (stick[1].button[1]) ret&=~128;
	}
	return ret;
}

static Bitu read_p201_timed(Bitu port,Bitu iolen) {
    (void)port;//UNUSED
    (void)iolen;//UNUSED
	uint8_t ret=0xff;
	double currentTick = PIC_FullIndex();
	if( stick[0].enabled ){
		if( stick[0].xtick < currentTick ) ret &=~1;
		if( stick[0].ytick < currentTick ) ret &=~2;
	}
	if( stick[1].enabled ){
		if( stick[1].xtick < currentTick ) ret &=~4;
		if( stick[1].ytick < currentTick ) ret &=~8;
	}

	if (stick[0].enabled) {
		if (stick[0].button[0]) ret&=~16;
		if (stick[0].button[1]) ret&=~32;
	}
	if (stick[1].enabled) {
		if (stick[1].button[0]) ret&=~64;
		if (stick[1].button[1]) ret&=~128;
	}
	return ret;
}

static void write_p201(Bitu port,Bitu val,Bitu iolen) {
    (void)val;//UNUSED
    (void)port;//UNUSED
    (void)iolen;//UNUSED
	/* Store writetime index */
	write_active = true;
	last_write = (uint32_t)PIC_Ticks;
	if (stick[0].enabled) {
		stick[0].xcount=(Bitu)((stick[0].xpos*RANGE)+RANGE);
		stick[0].ycount=(Bitu)((stick[0].ypos*RANGE)+RANGE);
	}
	if (stick[1].enabled) {
		stick[1].xcount=(Bitu)(((swap34? stick[1].ypos : stick[1].xpos)*RANGE)+RANGE);
		stick[1].ycount=(Bitu)(((swap34? stick[1].xpos : stick[1].ypos)*RANGE)+RANGE);
	}

}
static void write_p201_timed(Bitu port,Bitu val,Bitu iolen) {
    (void)val;//UNUSED
    (void)port;//UNUSED
    (void)iolen;//UNUSED
	// Store writetime index
	// Axes take time = 24.2 microseconds + ( 0.011 microseconds/ohm * resistance )
	// to reset to 0
	// Precalculate the time at which each axis hits 0 here
	double currentTick = PIC_FullIndex();
	if (stick[0].enabled) {
		stick[0].xtick = currentTick + 1000.0*( JOY_S_CONSTANT + S_PER_OHM *
	                         (double)((stick[0].xpos+1.0)* OHMS) );
		stick[0].ytick = currentTick + 1000.0*( JOY_S_CONSTANT + S_PER_OHM *
		                 (double)((stick[0].ypos+1.0)* OHMS) );
	}
	if (stick[1].enabled) {
		stick[1].xtick = currentTick + 1000.0*( JOY_S_CONSTANT + S_PER_OHM *
		                 (double)((swap34? stick[1].ypos : stick[1].xpos)+1.0) * OHMS);
		stick[1].ytick = currentTick + 1000.0*( JOY_S_CONSTANT + S_PER_OHM *
		                 (double)((swap34? stick[1].xpos : stick[1].ypos)+1.0) * OHMS);
	}
}

void JOYSTICK_Enable(Bitu which,bool enabled) {
	LOG(LOG_MISC,LOG_DEBUG)("JOYSTICK: Stick %u enable=%u",(int)which,enabled?1:0);
	if (which<2) stick[which].enabled=enabled;
}

void JOYSTICK_Button(Bitu which,Bitu num,bool pressed) {
	if ((which<2) && (num<2)) stick[which].button[num]=pressed;
}

void JOYSTICK_Move_X(Bitu which,float x) {
	if (which<2) {
		stick[which].xpos=x;
	}
}

void JOYSTICK_Move_Y(Bitu which,float y) {
	if (which<2) {
		stick[which].ypos=y;
	}
}

bool JOYSTICK_IsEnabled(Bitu which) {
	if (which<2) return stick[which].enabled;
	return false;
}

bool JOYSTICK_GetButton(Bitu which, Bitu num) {
	if ((which<2) && (num<2)) return stick[which].button[num];
	return false;
}

float JOYSTICK_GetMove_X(Bitu which) {
	if (which<2) return stick[which].xpos;
	return 0.0f;
}

float JOYSTICK_GetMove_Y(Bitu which) {
	if (which<2) return stick[which].ypos;
	return 0.0f;
}

class JOYSTICK:public Module_base{
private:
	IO_ReadHandleObject ReadHandler;
	IO_WriteHandleObject WriteHandler;
public:
	JOYSTICK(Section* configuration):Module_base(configuration){
		Section_prop * section=static_cast<Section_prop *>(configuration);

		bool timed = section->Get_bool("timed");
		if(timed) {
			ReadHandler.Install(0x201,read_p201_timed,IO_MB);
			WriteHandler.Install(0x201,write_p201_timed,IO_MB);
		} else {
			ReadHandler.Install(0x201,read_p201,IO_MB);
			WriteHandler.Install(0x201,write_p201,IO_MB);
		}
	}
};

static JOYSTICK* test = NULL;

void JOYSTICK_Destroy(Section* sec) {
    (void)sec;//UNUSED
    ModJoystick_Shutdown();
    if (test != NULL) {
        delete test;
        test = NULL;
    }
}

void JOYSTICK_OnPowerOn(Section* sec) {
    (void)sec;//UNUSED
    if (test == NULL) {
        LOG(LOG_MISC,LOG_DEBUG)("Allocating joystick emulation");
        test = new JOYSTICK(control->GetSection("joystick"));
    }
}

void JOYSTICK_Init() {
	LOG(LOG_MISC,LOG_DEBUG)("Initializing joystick emulation");

	/* NTS: Joystick emulation does not work if we init joystick type AFTER mapper init.
	 *      We cannot wait for poweron/reset signal for determination of joystick type.
	 *      But, I/O port setup can happen later. */
	{
		Section_prop * section=static_cast<Section_prop *>(control->GetSection("joystick"));

		const char * type=section->Get_string("joysticktype");
		if (!strcasecmp(type,"none"))       joytype = JOY_NONE;
		else if (!strcasecmp(type,"false")) joytype = JOY_NONE;
		else if (!strcasecmp(type,"auto"))  joytype = JOY_AUTO;
		else if (!strcasecmp(type,"2axis")) joytype = JOY_2AXIS;
		else if (!strcasecmp(type,"4axis")) joytype = JOY_4AXIS;
		else if (!strcasecmp(type,"4axis_2")) joytype = JOY_4AXIS_2;
		else if (!strcasecmp(type,"fcs"))   joytype = JOY_FCS;
		else if (!strcasecmp(type,"ch"))    joytype = JOY_CH;
		else if (!strcasecmp(type,"modjoy")) joytype = JOY_MODJOY;
		else joytype = JOY_AUTO;

		autofire = section->Get_bool("autofire");
		swap34 = section->Get_bool("swap34");
		button_wrapping_enabled = section->Get_bool("buttonwrap");
		modjoy_rawvalue_log = section->Get_bool("modjoy_rawvalue_log");
		modjoy_axis_log = section->Get_bool("modjoy_axis_log");
		stick[0].enabled = false;
		stick[1].enabled = false;
		stick[0].xtick = stick[0].ytick = stick[1].xtick =
		                 stick[1].ytick = PIC_FullIndex();
		
		// retrieves axes mapping
		auto joysticks = 2;
		auto axes = 8;
		for (auto i = 0; i < joysticks; i++)
		{
			for (auto j = 0; j < axes; j++)
			{
				auto propname = "joy" + std::to_string(i + 1) + "axis" + std::to_string(j);
				auto axis = section->Get_int(propname);
				if (i == 0)
				{
					joy1axes[j] = axis;
				}
				else
				{
					joy2axes[j] = axis;
				}
			}
		}

        for (auto i = 0; i < 4; i++) {
            auto propname = "modjoyaxis" + std::to_string(i);
            ParseModJoyBinding(section->Get_string(propname), modjoy_axis_bindings[i]);
        }
	}

	AddExitFunction(AddExitFunctionFuncPair(JOYSTICK_Destroy),true);

    if (!IS_PC98_ARCH)
        AddVMEventFunction(VM_EVENT_POWERON,AddVMEventFunctionFuncPair(JOYSTICK_OnPowerOn));
}

//save state support
namespace
{
class SerializeStick : public SerializeGlobalPOD
{
public:
    SerializeStick() : SerializeGlobalPOD("Joystick")
    {
        registerPOD(joytype);
        registerPOD(stick);
        registerPOD(last_write);
        registerPOD(write_active);
        registerPOD(swap34);
        registerPOD(button_wrapping_enabled);
        registerPOD(modjoy_rawvalue_log);
        registerPOD(modjoy_axis_log);
        registerPOD(modjoy_axis_bindings);
        registerPOD(autofire);
    }
} dummy;
}

void ModJoystick_Initialize()
{
    mod_joystick_manager.Initialize();
}

void ModJoystick_Shutdown()
{
    mod_joystick_manager.Shutdown();
}

int16_t ModJoystick_GetAxis(const int axis_index)
{
    return mod_joystick_manager.GetAxis(axis_index);
}

void ModJoystick_ReadAxes(int16_t* axis_values, const int axis_count)
{
    if (axis_values == nullptr || axis_count <= 0)
        return;

    for (int axis_index = 0; axis_index < axis_count; axis_index++)
        axis_values[axis_index] = 0;

    SDL_JoystickUpdate();

    const int read_count = axis_count < max_modjoy_axes ? axis_count : max_modjoy_axes;
    for (int axis_index = 0; axis_index < read_count; axis_index++)
        axis_values[axis_index] = mod_joystick_manager.GetAxis(axis_index);
}

int ModJoystick_GetDeviceCount()
{
    return mod_joystick_manager.GetDeviceCount();
}

const char* ModJoystick_GetDeviceName(const int device_index)
{
    return mod_joystick_manager.GetDeviceName(device_index);
}

int16_t ModJoystick_GetDeviceAxisValue(const int device_index, const int axis_index)
{
    return mod_joystick_manager.GetDeviceAxisValue(device_index, axis_index);
}
