// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    voodoo_banshee.h

    3dfx Voodoo Graphics SST-1/2 emulator.

***************************************************************************/

#ifndef MAME_VIDEO_VOODOO_BANSHEE_H
#define MAME_VIDEO_VOODOO_BANSHEE_H

#pragma once

#include "voodoo.h"


//**************************************************************************
//  CONSTANTS
//**************************************************************************

// nominal clock values
static constexpr u32 STD_VOODOO_BANSHEE_CLOCK = 90000000;
static constexpr u32 STD_VOODOO_3_CLOCK = 132000000;

#include "voodoo_regs.h"
#include "voodoo_render.h"



//**************************************************************************
//  VOODOO DEVICES
//**************************************************************************

class voodoo_banshee_device : public voodoo::voodoo_device_base
{
public:
	voodoo_banshee_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);

	u32 banshee_r(offs_t offset, u32 mem_mask = ~0);
	void banshee_w(offs_t offset, u32 data, u32 mem_mask = ~0);
	u32 banshee_fb_r(offs_t offset);
	void banshee_fb_w(offs_t offset, u32 data, u32 mem_mask = ~0);
	u32 banshee_io_r(offs_t offset, u32 mem_mask = ~0);
	void banshee_io_w(offs_t offset, u32 data, u32 mem_mask = ~0);
	u32 banshee_rom_r(offs_t offset);
	u8 banshee_vga_r(offs_t offset);
	void banshee_vga_w(offs_t offset, u8 data);

	virtual s32 banshee_2d_w(offs_t offset, u32 data) override;
	virtual int update(bitmap_rgb32 &bitmap, const rectangle &cliprect) override;

protected:
	// construction
	voodoo_banshee_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock, voodoo_model model);

	// device-level overrides
	virtual void device_start() override;

	// device-level overrides
	u32 banshee_agp_r(offs_t offset);
	void banshee_agp_w(offs_t offset, u32 data, u32 mem_mask = ~0);

	void banshee_blit_2d(u32 data);
	s32 lfb_direct_w(offs_t offset, u32 data, u32 mem_mask);

	// internal state
	struct banshee_info
	{
		u32 io[0x40];               // I/O registers
		u32 agp[0x80];              // AGP registers
		u8  vga[0x20];              // VGA registers
		u8  crtc[0x27];             // VGA CRTC registers
		u8  seq[0x05];              // VGA sequencer registers
		u8  gc[0x05];               // VGA graphics controller registers
		u8  att[0x15];              // VGA attribute registers
		u8  attff;                  // VGA attribute flip-flop

		u32 blt_regs[0x20];         // 2D Blitter registers
		u32 blt_dst_base = 0;
		u32 blt_dst_x = 0;
		u32 blt_dst_y = 0;
		u32 blt_dst_width = 0;
		u32 blt_dst_height = 0;
		u32 blt_dst_stride = 0;
		u32 blt_dst_bpp = 0;
		u32 blt_cmd = 0;
		u32 blt_src_base = 0;
		u32 blt_src_x = 0;
		u32 blt_src_y = 0;
		u32 blt_src_width = 0;
		u32 blt_src_height = 0;
		u32 blt_src_stride = 0;
		u32 blt_src_bpp = 0;
	};
	banshee_info m_banshee;                  // Banshee state
};


class voodoo_3_device : public voodoo_banshee_device
{
public:
	voodoo_3_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);
};


DECLARE_DEVICE_TYPE(VOODOO_BANSHEE, voodoo_banshee_device)
DECLARE_DEVICE_TYPE(VOODOO_3,       voodoo_3_device)

#endif // MAME_VIDEO_VOODOO_H
