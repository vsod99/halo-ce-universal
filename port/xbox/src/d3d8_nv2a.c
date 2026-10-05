/*
D3D8_NV2A.C

The Xbox port's Direct3D 8 device: the Xbox SDK's Direct3D the game uses,
written as the NV2A's push buffer methods, through pbkit (nxdk_nv2a.c).

The game is Xbox code, so most of it needs no translation:
- the vertex shaders are NV2A microcode, loaded as they are; the SDK's
  assembler put the viewport transform in them (c-38 and c-37, which the
  viewport sets);
- the pixel shaders are register combiner values, and the "simple" render
  states carry their own methods (D3DSIMPLERENDERSTATEENCODE);
- textures, vertex buffers and surfaces are in contiguous memory, their
  Data the physical address the GPU reads (d3d8_resources.c);
- the enumerations (comparisons, blend factors, primitive types, vertex
  types, clear flags) are the NV2A's values.

The PC menus' pictures (PNGs, menu_files.c) stand in for the small
placeholder textures the game draws for their bitmaps, as on the other
ports; the Xbox build embeds copies at the size they are drawn
(tools/xbox_menu_art.py), decoded when first drawn and kept while there is
room (art_texture).

The other render and texture stage states are kept in D3D__RenderState and
D3D__TextureState and written before the next draw, as the SDK's Direct3D
does with its deferred states. The back buffer is pbkit's: one of three it
flips between at the vertical blank. The CPU makes a frame while the GPU
draws the one before; fences tell it when what it overwrites is no longer
read (the synchronisation section).
*/

#include "xgpu.h"
#include "nxdk_platform.h"
#include "port_config.h"
#include "menu_files.h"
#include "png_decode.h"

#include <nv_regs.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480

#define NV097_SET_SHADOW_DEPTH_FUNC 0x00001E6C

/* ---------- the screen's width: the Xbox's 640 */

long halo_screen_width(void)
{
	return SCREEN_WIDTH;
}

long halo_screen_commit(void)
{
	return SCREEN_WIDTH;
}

void halo_screen_ui_offset(unsigned char centered)
{
	(void)centered;
}

/* ---------- state the XDK header's inline functions read and write */

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;
BYTE D3D__StateBlockDirty[1024];

/* ---------- the push buffer */

/* a method of the 3D object (subchannel 0) with that many words after it;
NONINCREASING sends them all to the one method */
#define METHOD(method, count) (((DWORD)(count) << 18) | (method))
#define METHOD_NONINCREASING(method, count) (0x40000000 | METHOD(method, count))
/* the most words one method takes */
#define METHOD_MAXIMUM_COUNT 2047

#define PUSH1(p, method, value) do { *(p)++ = METHOD(method, 1); *(p)++ = (DWORD)(value); } while (0)
#define PUSH2(p, method, a, b) do { *(p)++ = METHOD(method, 2); *(p)++ = (DWORD)(a); *(p)++ = (DWORD)(b); } while (0)

static DWORD float_bits(float value)
{
	DWORD bits;

	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static float bits_float(DWORD bits)
{
	float value;

	memcpy(&value, &bits, sizeof(value));
	return value;
}

static DWORD *push_begin(unsigned long dwords)
{
	return (DWORD *)xbox_gpu_begin(dwords);
}

static void push_end(DWORD *end)
{
	xbox_gpu_end((unsigned long *)end);
}

/* ---------- the device */

#define VERTEX_SHADER_SIGNATURE 0x4e563276 /* 'NV2v' */
#define VERTEX_ATTRIBUTES 16
#define PROGRAM_SLOTS 136

struct vertex_element
{
	BYTE stream;
	BYTE type;      /* D3DVSDT_*: the NV2A's type and size */
	WORD offset;
};

struct vertex_shader
{
	DWORD signature;
	unsigned long instruction_count;
	DWORD *instructions;
	/* per vertex register: none if its type is D3DVSDT_NONE */
	struct vertex_element elements[VERTEX_ATTRIBUTES];
};

static struct
{
	/* the device the game holds: only its address matters */
	DWORD device;
	BOOL created;
	D3DSurface back_buffer;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DVIEWPORT8 viewport;
	D3DMATRIX transforms[D3DTS_MAX];
	DWORD shader_constant_mode;

	/* the declaration in use, and the program at each address */
	struct vertex_shader *vertex_shader;
	struct vertex_shader *programs[PROGRAM_SLOTS];

	struct
	{
		D3DVertexBuffer *buffer;
		DWORD data;
		UINT stride;
	} streams[16];
	UINT base_vertex;
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];

	/* the surface format last written, and the target's size when it is
	swizzled (0 when it is linear) */
	DWORD surface_format;
	DWORD swizzled_width, swizzled_height;

	/* to write before the next draw */
	BOOL surface_dirty;
	BOOL states_dirty;
	BOOL textures_dirty;
	BOOL arrays_dirty;
	/* the base vertex the arrays were last written with, or ~0 */
	UINT arrays_base;
} device;

/* the frame whose draws go to the log (debug.gpu_trace_frame in
config.toml, as the other ports), and whether the first immediate-mode
vertex of the draw being traced is still to be logged */
static unsigned long frame_number;
static BOOL trace_immediate_pending;

/* whether the frame being drawn goes to the log (debug.screenshot_every:
screenshot_to_log), and the targets besides the screen it drew into,
which go with it */
#define SCREENSHOT_TARGETS 8
static D3DSurface screenshot_targets[SCREENSHOT_TARGETS];
static int screenshot_target_count;

static BOOL screenshot_frame(void)
{
	static long every = -1;

	if (every < 0)
		every = config_integer("debug.screenshot_every");
	return every > 0 && (frame_number + 1) % (unsigned long)every == 0;
}

static volatile unsigned int flip_count;
static D3DCALLBACK vertical_blank_callback;
static BOOL vertical_blank_thread_started;

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);

/* replaces main/d3d_intimacy.cpp, which reads the counter out of the Xbox
Direct3D runtime's private device structure */
volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

/* ---------- the vertical blank */

static DWORD WINAPI vertical_blank_thread(LPVOID unused)
{
	(void)unused;
	for (;;)
	{
		D3DCALLBACK callback;

		xbox_gpu_wait_vertical_blank();
		callback = vertical_blank_callback;
		if (callback)
			callback(0);
	}
	return 0;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	vertical_blank_callback = callback;
	if (!vertical_blank_thread_started && xbox_gpu_ready())
	{
		HANDLE thread = CreateThread(NULL, 0, vertical_blank_thread, NULL, 0, NULL);

		if (thread)
		{
			CloseHandle(thread);
			vertical_blank_thread_started = TRUE;
		}
		else
		{
			platform_log("Direct3D: cannot start the vertical blank thread");
		}
	}
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	if (xbox_gpu_ready())
		xbox_gpu_wait_vertical_blank();
}

/* ---------- surfaces */

/* the screen's back buffer: pbkit's current one (xbox_gpu_back_buffer) */
static void back_buffer_initialize(D3DSurface *surface)
{
	unsigned long width, height, pitch;

	xbox_gpu_screen(&width, &height, &pitch);
	memset(surface, 0, sizeof(*surface));
	surface->Common = D3DCOMMON_TYPE_SURFACE | 1;
	surface->Data = xbox_gpu_back_buffer();
	surface->Format = ((DWORD)D3DFMT_LIN_A8R8G8B8 << D3DFORMAT_FORMAT_SHIFT) | (2 << D3DFORMAT_DIMENSION_SHIFT) |
		D3DFORMAT_DMACHANNEL_A;
	surface->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
		((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
}

/* the NV2A's surface color and zeta formats, 0 if it has none */
static DWORD surface_color_format(DWORD format)
{
	switch (format)
	{
	case D3DFMT_A8R8G8B8: case D3DFMT_LIN_A8R8G8B8: return NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
	case D3DFMT_X8R8G8B8: case D3DFMT_LIN_X8R8G8B8: return NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8;
	case D3DFMT_R5G6B5: case D3DFMT_LIN_R5G6B5: return NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5;
	case D3DFMT_X1R5G5B5: case D3DFMT_LIN_X1R5G5B5: return NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5;
	case D3DFMT_L8: case D3DFMT_LIN_L8: return NV097_SET_SURFACE_FORMAT_COLOR_LE_B8;
	case D3DFMT_G8B8: case D3DFMT_LIN_G8B8: return NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8;
	default: return 0;
	}
}

static DWORD surface_zeta_format(DWORD format, BOOL *floating)
{
	*floating = format == D3DFMT_F24S8 || format == D3DFMT_LIN_F24S8 || format == D3DFMT_F16 || format == D3DFMT_LIN_F16;
	switch (format)
	{
	case D3DFMT_D24S8: case D3DFMT_LIN_D24S8: case D3DFMT_F24S8: case D3DFMT_LIN_F24S8:
		return NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;
	case D3DFMT_D16: case D3DFMT_LIN_D16: case D3DFMT_F16: case D3DFMT_LIN_F16:
		return NV097_SET_SURFACE_FORMAT_ZETA_Z16;
	default:
		return 0;
	}
}

static unsigned long log2_unsigned(unsigned long value)
{
	unsigned long result = 0;

	while (value > 1)
	{
		value >>= 1;
		result++;
	}
	return result;
}

/* the depth buffer's largest value (the viewport's z scale) */
static float depth_scale(void)
{
	if (device.depth_stencil)
	{
		struct xgpu_texture_description description;

		xgpu_texture_describe(device.depth_stencil->Format, device.depth_stencil->Size, &description);
		if (description.format == D3DFMT_D16 || description.format == D3DFMT_LIN_D16 ||
			description.format == D3DFMT_F16 || description.format == D3DFMT_LIN_F16)
		{
			return 65535.0f;
		}
	}
	return 16777215.0f;
}

static void surface_apply(void)
{
	struct xgpu_texture_description color, zeta;
	DWORD color_format, zeta_format = 0, format, color_pitch, zeta_pitch = 0, control;
	BOOL floating = FALSE;
	DWORD *p;

	device.surface_dirty = FALSE;
	if (!device.render_target)
		return;
	xgpu_texture_describe(device.render_target->Format, device.render_target->Size, &color);
	color_format = surface_color_format(color.format);
	if (!color_format)
	{
		static DWORD logged;

		if (logged != color.format)
			platform_log("Direct3D: no NV2A surface for render target format %lu", (unsigned long)color.format);
		logged = color.format;
		color_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
	}
	color_pitch = color.linear ? color.pitch : color.width * (color_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 ||
		color_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8 ? 4 : color_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_B8 ? 1 : 2);
	if (device.depth_stencil)
	{
		xgpu_texture_describe(device.depth_stencil->Format, device.depth_stencil->Size, &zeta);
		zeta_format = surface_zeta_format(zeta.format, &floating);
		zeta_pitch = zeta.linear ? zeta.pitch : zeta.width * (zeta_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? 2 : 4);
	}
	format = color_format | (zeta_format ? zeta_format : NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) << 4;
	device.surface_format = format;
	device.swizzled_width = color.linear ? 0 : color.width;
	device.swizzled_height = color.linear ? 0 : color.height;
	if (color.linear)
		format |= NV097_SET_SURFACE_FORMAT_TYPE_PITCH << 8;
	else
		format |= (NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE << 8) | (log2_unsigned(color.width) << 16) |
			(log2_unsigned(color.height) << 24);
	/* stencil writes, texture perspective; a floating point depth buffer */
	control = NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE | NV097_SET_CONTROL0_TEXTURE_PERSPECTIVE_ENABLE;
	if (floating)
		control |= NV097_SET_CONTROL0_Z_FORMAT_FLOAT;
	if (D3D__RenderState[D3DRS_ZENABLE] == D3DZB_USEW)
		control |= NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE;
	if (D3D__RenderState[D3DRS_YUVENABLE])
		control |= NV097_SET_CONTROL0_COLOR_SPACE_CONVERT_CRYCB_TO_RGB;

	p = push_begin(32);
	PUSH2(p, NV097_SET_CONTEXT_DMA_COLOR, XBOX_GPU_DMA_COLOR, XBOX_GPU_DMA_ZETA);
	PUSH1(p, NV097_SET_SURFACE_FORMAT, format);
	PUSH1(p, NV097_SET_SURFACE_PITCH, color_pitch | ((zeta_pitch ? zeta_pitch : color_pitch) << 16));
	PUSH1(p, NV097_SET_SURFACE_COLOR_OFFSET, device.render_target->Data & 0x03ffffff);
	PUSH1(p, NV097_SET_SURFACE_ZETA_OFFSET, zeta_format ? device.depth_stencil->Data & 0x03ffffff : 0);
	PUSH2(p, NV097_SET_SURFACE_CLIP_HORIZONTAL, color.width << 16, color.height << 16);
	PUSH1(p, NV097_SET_CONTROL0, control);
	push_end(p);
	/* depth testing depends on there being a depth buffer */
	device.states_dirty = TRUE;
}

/* ---------- vertex shader constants */

static void constants_write(unsigned long first, const void *data, unsigned long count)
{
	const DWORD *words = data;

	while (count)
	{
		unsigned long batch = count > 8 ? 8 : count;
		DWORD *p = push_begin(4 + batch * 4);

		PUSH1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, first);
		*p++ = METHOD(NV097_SET_TRANSFORM_CONSTANT, batch * 4);
		memcpy(p, words, batch * 16);
		p += batch * 4;
		push_end(p);
		first += batch;
		words += batch * 4;
		count -= batch;
	}
}

/* Direct3D's reserved constants c-38 and c-37 map clip space to the screen
(the SDK's assembler ends every program with that transform); the half
pixel and a little more puts Direct3D's pixel centers where the NV2A
samples, as the SDK does */
static void viewport_apply(void)
{
	float zscale = depth_scale();
	float constants[2][4];
	DWORD *p;

	constants[0][0] = device.viewport.Width * 0.5f;
	constants[0][1] = -(float)device.viewport.Height * 0.5f;
	constants[0][2] = zscale * (device.viewport.MaxZ - device.viewport.MinZ);
	constants[0][3] = 0.0f;
	constants[1][0] = device.viewport.X + device.viewport.Width * 0.5f + 0.53125f;
	constants[1][1] = device.viewport.Y + device.viewport.Height * 0.5f + 0.53125f;
	constants[1][2] = zscale * device.viewport.MinZ;
	constants[1][3] = 0.0f;
	if (!(device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
		constants_write(XGPU_VERTEX_CONSTANT_BIAS - 38, constants, 2);
	p = push_begin(8);
	PUSH2(p, NV097_SET_CLIP_MIN, float_bits(zscale * device.viewport.MinZ), float_bits(zscale * device.viewport.MaxZ));
	push_end(p);
}

/* ---------- creation */

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	(void)sdk_version;
	return (Direct3D *)1;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

static void render_state_defaults(void)
{
	int index;

	D3D__RenderState[D3DRS_ZENABLE] = D3DZB_TRUE;
	D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
	D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
	D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
	D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
	D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
	D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
	D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
	D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
	D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
	D3D__RenderState[D3DRS_BACKFILLMODE] = D3DFILL_SOLID;
	D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
	D3D__RenderState[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
	D3D__RenderState[D3DRS_STENCILMASK] = 0xff;
	D3D__RenderState[D3DRS_STENCILWRITEMASK] = 0xff;
	D3D__RenderState[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
	D3D__RenderState[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
	D3D__RenderState[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
	D3D__RenderState[D3DRS_SHADEMODE] = 0x1d01; /* D3DSHADE_GOURAUD */
	D3D__RenderState[D3DRS_LINEWIDTH] = float_bits(1.0f);
	D3D__RenderState[D3DRS_MULTISAMPLEMASK] = 0xffffffff;
	D3D__RenderState[D3DRS_SWATHWIDTH] = 3; /* D3DSWATH_128 */
	for (index = 0; index < D3DTSS_MAXSTAGES; index++)
	{
		D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
		D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
		D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
		D3D__TextureState[index][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
		D3D__TextureState[index][D3DTSS_MINFILTER] = D3DTEXF_POINT;
		D3D__TextureState[index][D3DTSS_MAXANISOTROPY] = 1;
		D3D__TextureState[index][D3DTSS_TEXCOORDINDEX] = index;
	}
}

static void device_start(void)
{
	DWORD *p;
	int index;

	back_buffer_initialize(&device.back_buffer);
	d3d8_surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, SCREEN_WIDTH, SCREEN_HEIGHT);
	device.render_target = &device.back_buffer;
	device.depth_stencil = &device.depth_buffer;
	for (index = 0; index < D3DTS_MAX; index++)
	{
		device.transforms[index]._11 = 1.0f;
		device.transforms[index]._22 = 1.0f;
		device.transforms[index]._33 = 1.0f;
		device.transforms[index]._44 = 1.0f;
	}
	device.viewport.Width = SCREEN_WIDTH;
	device.viewport.Height = SCREEN_HEIGHT;
	device.viewport.MaxZ = 1.0f;
	device.arrays_base = ~0U;
	render_state_defaults();

	/* vertex programs, with every constant */
	p = push_begin(16);
	PUSH1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
		(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM << __builtin_ctz(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE)) |
		(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV << __builtin_ctz(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE)));
	PUSH1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
	push_end(p);
	/* every simple state, as the defaults left them */
	for (index = 0; index < D3DRS_SIMPLE_MAX; index++)
	{
		p = push_begin(2);
		PUSH1(p, D3DSIMPLERENDERSTATEENCODE[index] & 0xffff, D3D__RenderState[index]);
		push_end(p);
	}
	device.surface_dirty = TRUE;
	device.states_dirty = TRUE;
	device.textures_dirty = TRUE;
	device.arrays_dirty = TRUE;
	viewport_apply();
}

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	(void)presentation_parameters;
	if (!xbox_gpu_ready())
		return E_FAIL;
	if (!device.created)
	{
		device_start();
		device.created = TRUE;
		platform_log("Direct3D: the NV2A through pbkit");
	}
	*returned_device = (D3DDevice *)&device.device;
	return S_OK;
}

ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)back_buffer;
	(void)type;
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (render_target)
		device.render_target = render_target;
	if (render_target && render_target != &device.back_buffer && screenshot_frame())
	{
		int i;

		for (i = 0; i < screenshot_target_count && screenshot_targets[i].Data != render_target->Data; i++)
			;
		if (i == screenshot_target_count && i < SCREENSHOT_TARGETS)
			screenshot_targets[screenshot_target_count++] = *render_target;
	}
	device.depth_stencil = depth_stencil;
	device.surface_dirty = TRUE;
	/* as Direct3D, the viewport becomes the whole new target */
	if (device.render_target)
	{
		struct xgpu_texture_description description;

		xgpu_texture_describe(device.render_target->Format, device.render_target->Size, &description);
		device.viewport.X = 0;
		device.viewport.Y = 0;
		device.viewport.Width = description.width;
		device.viewport.Height = description.height;
		device.viewport.MinZ = 0.0f;
		device.viewport.MaxZ = 1.0f;
	}
	viewport_apply();
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
	viewport_apply();
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = device.transforms[state];
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	device.shader_constant_mode = mode;
}

/* ---------- synchronisation

The GPU writes a count into the reports' memory once it has drawn all
that comes before it in the push buffer (a semaphore release: the fence,
nxdk_nv2a.c). Each frame ends with one, and the CPU makes the next frame
while the GPU draws it.

As the Xbox's Direct3D keeps its push buffer time there, a resource's Lock
field holds the fence after the last draw that used it (resource_used), or
0. The texture cache waits for it before it gives a texture's memory to
another (D3DResource_IsBusy), and so does a lock that may overwrite what
the GPU reads (d3d8_resources.c): the dynamic vertices' first lock of each
frame. */

static DWORD fence_inserted;

static DWORD fence_insert(void)
{
	DWORD *p;

	if (!xbox_gpu_reports())
		return fence_inserted;
	p = push_begin(6);
	PUSH1(p, NV097_SET_CONTEXT_DMA_SEMAPHORE, XBOX_GPU_DMA_REPORT);
	PUSH1(p, NV097_SET_SEMAPHORE_OFFSET, XBOX_GPU_FENCE_OFFSET);
	PUSH1(p, NV097_BACK_END_WRITE_SEMAPHORE_RELEASE, ++fence_inserted);
	push_end(p);
	return fence_inserted;
}

/* a fence of this frame's draws so far is sent first */
static BOOL fence_passed(DWORD fence)
{
	if (!xbox_gpu_reports())
		return !xbox_gpu_busy();
	if ((LONG)(fence - fence_inserted) > 0)
		fence_insert();
	if ((LONG)(xbox_gpu_fence() - fence) >= 0)
		return TRUE;
	xbox_gpu_kick();
	return FALSE;
}

static void fence_wait(DWORD fence)
{
	if (!xbox_gpu_reports())
	{
		xbox_gpu_wait_idle();
		return;
	}
	if ((LONG)(fence - fence_inserted) > 0)
		fence_insert();
	xbox_gpu_wait_fence(fence);
}

/* a draw or clear uses it: busy until the next fence */
static void resource_used(void *resource)
{
	if (resource)
		((DWORD *)resource)[2] = fence_inserted + 1;
}

/* the fence a resource waits for, or 0: resources from the maps come with
other values there, which no draw wrote */
static DWORD resource_fence(D3DResource *resource)
{
	DWORD fence = ((const DWORD *)resource)[2];

	if ((LONG)(fence - (fence_inserted + 1)) > 0)
	{
		static BOOL logged;

		if (!logged)
			platform_log("Direct3D: a resource's lock field holds %08lx, no fence (%08lx sent)",
				(unsigned long)fence, (unsigned long)fence_inserted);
		logged = TRUE;
		return 0;
	}
	return fence;
}

BOOL WINAPI D3DResource_IsBusy(D3DResource *resource)
{
	DWORD fence = resource_fence(resource);

	return fence && !fence_passed(fence);
}

void WINAPI D3DResource_BlockUntilNotBusy(D3DResource *resource)
{
	DWORD fence = resource_fence(resource);

	if (fence)
		fence_wait(fence);
}

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return xbox_gpu_busy();
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	if (callback)
	{
		xbox_gpu_wait_idle();
		callback(context);
	}
}

/* ---------- visibility (occlusion) tests

The GPU counts the pixels that pass the depth and stencil tests between
Begin and End, and writes the count into the reports' memory (16 bytes an
index: nxdk_nv2a.c). Until it has, the count holds a value no count has. */

#define VISIBILITY_TEST_PENDING 0xffffffffUL

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
	DWORD *p;

	if (!xbox_gpu_reports())
		return;
	p = push_begin(6);
	PUSH1(p, NV097_SET_CONTEXT_DMA_REPORT, XBOX_GPU_DMA_REPORT);
	PUSH1(p, NV097_CLEAR_REPORT_VALUE, NV097_CLEAR_REPORT_VALUE_TYPE_ZPASS_PIXEL_CNT);
	PUSH1(p, NV097_SET_ZPASS_PIXEL_COUNT_ENABLE, 1);
	push_end(p);
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	volatile unsigned long *reports = xbox_gpu_reports();
	DWORD *p;

	if (index >= XBOX_GPU_REPORT_BYTES / 16)
		return E_INVALIDARG;
	if (!reports)
		return S_OK;
	reports[index * 4 + 2] = VISIBILITY_TEST_PENDING;
	p = push_begin(6);
	PUSH1(p, NV097_SET_ZPASS_PIXEL_COUNT_ENABLE, 0);
	PUSH1(p, NV097_GET_REPORT, (NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT << 24) | (index * 16));
	/* xemu writes its pending reports when the report DMA is set (its
	pgraph's SET_CONTEXT_DMA_REPORT); the same one again changes nothing
	on the console */
	PUSH1(p, NV097_SET_CONTEXT_DMA_REPORT, XBOX_GPU_DMA_REPORT);
	push_end(p);
	return S_OK;
}

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	volatile unsigned long *reports = xbox_gpu_reports();
	unsigned long count;

	if (index >= XBOX_GPU_REPORT_BYTES / 16)
		return E_INVALIDARG;
	/* with no reports, everything is visible */
	count = reports ? reports[index * 4 + 2] : 0x7fffffff;
	if (count == VISIBILITY_TEST_PENDING)
	{
		xbox_gpu_kick();
		return D3DERR_TESTINCOMPLETE;
	}
	if (time_stamp)
		*time_stamp = reports ? reports[index * 4] | ((ULONGLONG)reports[index * 4 + 1] << 32) : 0;
	if (result)
		*result = count;
	return S_OK;
}

/* ---------- render states */

/* the simple ones are their methods' values */
void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	DWORD *p = push_begin(2);

	PUSH1(p, method & 0xffff, value);
	push_end(p);
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
	{
		D3D__RenderState[state] = value;
		device.states_dirty = TRUE;
	}
}

static void simple_state_store(D3DRENDERSTATETYPE state, DWORD value)
{
	D3D__RenderState[state] = value;
	D3DDevice_SetRenderState_Simple(D3DSIMPLERENDERSTATEENCODE[state], value);
}

/* As the Xbox's D3D8 does it: a z bias is a polygon offset of -bias depth
units plus -bias/4 times the polygon's depth slope, enabled for every fill
mode (port/linux/src/d3d8_gl.c) */
void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float offset = -(float)value;
	DWORD enable = value != 0;

	D3D__RenderState[D3DRS_ZBIAS] = value;
	simple_state_store(D3DRS_POLYGONOFFSETZSLOPESCALE, float_bits(offset * 0.25f));
	simple_state_store(D3DRS_POLYGONOFFSETZOFFSET, float_bits(offset));
	simple_state_store(D3DRS_POINTOFFSETENABLE, enable);
	simple_state_store(D3DRS_WIREFRAMEOFFSETENABLE, enable);
	simple_state_store(D3DRS_SOLIDOFFSETENABLE, enable);
}

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if (state == D3DRS_ZBIAS)
		D3DDevice_SetRenderState_ZBias(value);
	else if ((unsigned long)state < D3DRS_SIMPLE_MAX)
		simple_state_store(state, value);
	else if ((unsigned long)state < D3DRS_MAX)
		D3DDevice_SetRenderState_Deferred(state, value);
}

/* the rest are written with the deferred ones, before the next draw */
#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3DDevice_SetRenderState_Deferred(state, value); }

COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_RENDER_STATE(StencilFail, D3DRS_STENCILFAIL)
COMPLEX_RENDER_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_RENDER_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_RENDER_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_RENDER_STATE(LogicOp, D3DRS_LOGICOP)
COMPLEX_RENDER_STATE(EdgeAntiAlias, D3DRS_EDGEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleAntiAlias, D3DRS_MULTISAMPLEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleMask, D3DRS_MULTISAMPLEMASK)
COMPLEX_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
COMPLEX_RENDER_STATE(ShadowFunc, D3DRS_SHADOWFUNC)
COMPLEX_RENDER_STATE(LineWidth, D3DRS_LINEWIDTH)
COMPLEX_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

/* (these two change the surface's control word too) */
void WINAPI D3DDevice_SetRenderState_ZEnable(DWORD value)
{
	D3DDevice_SetRenderState_Deferred(D3DRS_ZENABLE, value);
	device.surface_dirty = TRUE;
}

void WINAPI D3DDevice_SetRenderState_YuvEnable(DWORD value)
{
	D3DDevice_SetRenderState_Deferred(D3DRS_YUVENABLE, value);
	device.surface_dirty = TRUE;
}

/* a D3DCOLOR (ARGB) as the NV2A's fog color (ABGR) */
static DWORD color_swap_red_blue(DWORD color)
{
	return (color & 0xff00ff00) | ((color >> 16) & 0xff) | ((color & 0xff) << 16);
}

static void states_apply(void)
{
	const DWORD *rs = D3D__RenderState;
	BOOL depth = device.depth_stencil && rs[D3DRS_ZENABLE] != D3DZB_FALSE;
	DWORD fog_mode = NV097_SET_FOG_MODE_V_LINEAR;
	float fog_bias = 1.0f, fog_scale = 1.0f;
	DWORD cull = rs[D3DRS_CULLMODE];
	float line_width = bits_float(rs[D3DRS_LINEWIDTH]);
	DWORD *p;

	device.states_dirty = FALSE;
	/* fog: the vertex program's oFog as it is, or the table's curve of it
	(xemu's pgraph glsl/vsh.c gives the NV2A's formulas) */
	switch (rs[D3DRS_FOGTABLEMODE])
	{
	case D3DFOG_LINEAR:
	{
		float start = bits_float(rs[D3DRS_FOGSTART]), end = bits_float(rs[D3DRS_FOGEND]);

		fog_scale = end != start ? -1.0f / (end - start) : 0.0f;
		fog_bias = 1.0f - end * fog_scale;
		break;
	}
	case D3DFOG_EXP:
		fog_mode = NV097_SET_FOG_MODE_V_EXP;
		fog_bias = 1.5f;
		fog_scale = -bits_float(rs[D3DRS_FOGDENSITY]) / (2.0f * 5.5451774f);
		break;
	case D3DFOG_EXP2:
		fog_mode = NV097_SET_FOG_MODE_V_EXP2;
		fog_bias = 1.5f;
		fog_scale = -bits_float(rs[D3DRS_FOGDENSITY]) / (2.0f * 2.3548200f);
		break;
	default:
		break;
	}

	p = push_begin(64);
	PUSH1(p, NV097_SET_DEPTH_TEST_ENABLE, depth);
	PUSH1(p, NV097_SET_STENCIL_TEST_ENABLE, device.depth_stencil && rs[D3DRS_STENCILENABLE]);
	PUSH1(p, NV097_SET_STENCIL_OP_FAIL, rs[D3DRS_STENCILFAIL]);
	PUSH1(p, NV097_SET_FOG_ENABLE, rs[D3DRS_FOGENABLE] != 0);
	PUSH1(p, NV097_SET_FOG_MODE, fog_mode);
	*p++ = METHOD(NV097_SET_FOG_PARAMS, 3);
	*p++ = float_bits(fog_bias);
	*p++ = float_bits(fog_scale);
	*p++ = 0;
	PUSH1(p, NV097_SET_FOG_COLOR, color_swap_red_blue(rs[D3DRS_FOGCOLOR]));
	PUSH1(p, NV097_SET_LIGHTING_ENABLE, 0);
	PUSH1(p, NV097_SET_SPECULAR_ENABLE, rs[D3DRS_SPECULARENABLE] != 0);
	/* Direct3D names the faces it culls by their winding; the NV2A by
	front and back, of the front face's winding */
	PUSH1(p, NV097_SET_CULL_FACE_ENABLE, cull != D3DCULL_NONE);
	PUSH1(p, NV097_SET_FRONT_FACE, rs[D3DRS_FRONTFACE]);
	PUSH1(p, NV097_SET_CULL_FACE, cull == rs[D3DRS_FRONTFACE] ? NV097_SET_CULL_FACE_V_FRONT : NV097_SET_CULL_FACE_V_BACK);
	PUSH2(p, NV097_SET_FRONT_POLYGON_MODE, rs[D3DRS_FILLMODE], rs[D3DRS_FILLMODE]);
	PUSH1(p, NV097_SET_LOGIC_OP_ENABLE, rs[D3DRS_LOGICOP] != 0 /* D3DLOGICOP_NONE */);
	PUSH1(p, NV097_SET_LOGIC_OP, rs[D3DRS_LOGICOP]);
	PUSH1(p, NV097_SET_LINE_WIDTH, (DWORD)(line_width * 8.0f) & 0x1ff);
	PUSH1(p, NV097_SET_SHADER_STAGE_PROGRAM, rs[D3DRS_PSTEXTUREMODES]);
	if (rs[D3DRS_SHADOWFUNC] >= D3DCMP_NEVER)
		PUSH1(p, NV097_SET_SHADOW_DEPTH_FUNC, rs[D3DRS_SHADOWFUNC] - D3DCMP_NEVER);
	push_end(p);
}

/* ---------- texture stages */

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
	{
		D3D__TextureState[stage][type] = value;
		device.textures_dirty = TRUE;
	}
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	/* (vertex programs write the coordinates themselves) */
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	D3DDevice_SetTextureState_Deferred(stage, D3DTSS_BORDERCOLOR, value);
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	D3DDevice_SetTextureState_Deferred(stage, D3DTSS_COLORKEYCOLOR, value);
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	D3DDevice_SetTextureState_Deferred(stage, type, value);
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
	{
		device.textures[stage] = texture;
		device.textures_dirty = TRUE;
	}
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
	{
		device.palettes[stage] = palette;
		device.textures_dirty = TRUE;
	}
}

/* the NV2A's minifying filter for Direct3D's minifying and mip filters */
static DWORD texture_min_filter(DWORD min, DWORD mip)
{
	BOOL linear = min != D3DTEXF_POINT;

	if (min >= D3DTEXF_QUINCUNX)
		return 7; /* convolution */
	switch (mip)
	{
	case D3DTEXF_NONE: return linear ? 2 : 1;
	case D3DTEXF_POINT: return linear ? 4 : 3;
	default: return linear ? 6 : 5;
	}
}

static DWORD texture_mag_filter(DWORD mag)
{
	return mag >= D3DTEXF_QUINCUNX ? 4 : mag == D3DTEXF_POINT ? 1 : 2;
}

/* ---------- the PC menus' pictures */

/* the most memory the decoded pictures keep, and the largest side one has
(a picture from a menus folder may be a desktop's) */
#define ART_BUDGET (8UL * 1024 * 1024)
#define ART_LARGEST 1024
#define ART_TEXTURES 256

struct art_texture
{
	/* the placeholder texture's Data, and the PNG decoded for it */
	DWORD placeholder;
	const unsigned char *png;
	void *texels;
	DWORD data, format;
	unsigned long bytes;
	unsigned long last_frame;
	BOOL failed;
};

static struct art_texture art_textures[ART_TEXTURES];
static unsigned long art_bytes;

static unsigned long largest_power_of_two(unsigned long value, unsigned long most)
{
	unsigned long result = 1;

	while (result * 2 <= value && result * 2 <= most)
		result *= 2;
	return result;
}

/* the NV2A's swizzled order: the bits of x and y interleaved, x first,
the larger side's last bits alone */
static unsigned long swizzle_offset(unsigned long x, unsigned long y, unsigned long width, unsigned long height)
{
	unsigned long offset = 0, bit = 1, mask;

	for (mask = 1; mask < width || mask < height; mask <<= 1)
	{
		if (mask < width)
		{
			if (x & mask)
				offset |= bit;
			bit <<= 1;
		}
		if (mask < height)
		{
			if (y & mask)
				offset |= bit;
			bit <<= 1;
		}
	}
	return offset;
}

static void art_release(struct art_texture *art)
{
	if (art->texels)
	{
		platform_contiguous_free(art->texels);
		art_bytes -= art->bytes;
	}
	memset(art, 0, sizeof(*art));
}

/* makes room for that many bytes from the pictures no draw of this frame
or the last uses (the GPU has finished the frames before those: Present) */
static void art_make_room(unsigned long bytes)
{
	while (art_bytes + bytes > ART_BUDGET)
	{
		struct art_texture *oldest = NULL;
		int index;

		for (index = 0; index < ART_TEXTURES; index++)
		{
			struct art_texture *art = &art_textures[index];

			if (art->texels && art->last_frame + 2 <= frame_number && (!oldest || art->last_frame < oldest->last_frame))
				oldest = art;
		}
		if (!oldest)
			return;
		art_release(oldest);
	}
}

static void art_decode(struct art_texture *art, const unsigned char *png, unsigned long png_size)
{
	unsigned long png_width = 0, png_height = 0, width, height, x, y;
	unsigned char *pixels = png_decode(png, png_size, &png_width, &png_height);
	DWORD *texels;

	if (!pixels)
	{
		platform_log("menus: a picture is not an 8-bit RGBA PNG; not drawn");
		art->failed = TRUE;
		return;
	}
	/* a power of two each side (the copies the build embeds are already
	that size), sampled nearest */
	width = largest_power_of_two(png_width, ART_LARGEST);
	height = largest_power_of_two(png_height, ART_LARGEST);
	art_make_room(width * height * 4);
	texels = platform_contiguous_alloc(width * height * 4, D3DTEXTURE_ALIGNMENT, PLATFORM_ANY_PHYSICAL_ADDRESS,
		PAGE_READWRITE);
	if (!texels)
	{
		platform_log("menus: no memory for a %lux%lu picture; not drawn", width, height);
		free(pixels);
		art->failed = TRUE;
		return;
	}
	for (y = 0; y < height; y++)
	{
		const unsigned char *row = pixels + (y * png_height / height) * png_width * 4;

		for (x = 0; x < width; x++)
		{
			const unsigned char *rgba = row + (x * png_width / width) * 4;

			texels[swizzle_offset(x, y, width, height)] =
				(DWORD)rgba[3] << 24 | (DWORD)rgba[0] << 16 | (DWORD)rgba[1] << 8 | rgba[2];
		}
	}
	free(pixels);
	art->texels = texels;
	art->bytes = width * height * 4;
	art_bytes += art->bytes;
	art->data = PLATFORM_VIRTUAL_TO_PHYSICAL(texels);
	art->format = ((DWORD)D3DFMT_A8R8G8B8 << D3DFORMAT_FORMAT_SHIFT) | (1 << D3DFORMAT_MIPMAP_SHIFT) |
		(log2_unsigned(width) << D3DFORMAT_USIZE_SHIFT) | (log2_unsigned(height) << D3DFORMAT_VSIZE_SHIFT) |
		(2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_BORDERSOURCE_COLOR | D3DFORMAT_DMACHANNEL_A;
}

/* the picture standing for a texture, or NULL: only the menus' 4x4
placeholders have one */
static const struct art_texture *art_texture(const DWORD *header, const struct xgpu_texture_description *description)
{
	struct art_texture *art = NULL, *free_slot = NULL;
	unsigned long png_size;
	const unsigned char *png;
	int index;

	if (description->width != 4 || description->height != 4 || description->format != D3DFMT_A8R8G8B8)
		return NULL;
	png = menu_art_png(header[1], &png_size);
	if (!png)
		return NULL;
	for (index = 0; index < ART_TEXTURES && !art; index++)
	{
		if (art_textures[index].placeholder == header[1])
			art = &art_textures[index];
		else if (!free_slot && !art_textures[index].placeholder)
			free_slot = &art_textures[index];
	}
	/* (the menus may give the placeholder another picture) */
	if (art && art->png != png)
		art_release(art);
	else if (!art && !(art = free_slot))
		return NULL;
	if (!art->placeholder)
	{
		art->placeholder = header[1];
		art->png = png;
		art_decode(art, png, png_size);
	}
	art->last_frame = frame_number;
	return art->failed ? NULL : art;
}

static void texture_stage_apply(DWORD *(*p), int stage)
{
	const DWORD *ts = D3D__TextureState[stage];
	const DWORD *header = (const DWORD *)device.textures[stage];
	struct xgpu_texture_description description;
	DWORD base = NV097_SET_TEXTURE_OFFSET + stage * 64;
	DWORD offset = header ? header[1] : 0, format = header ? header[3] : 0;
	const struct art_texture *art;
	DWORD control, filter, max_lod, anisotropy = 0;
	float bias;
	long bias_fixed;

	if (!header)
	{
		PUSH1(*p, NV097_SET_TEXTURE_CONTROL0 + stage * 64, 0);
		return;
	}
	xgpu_texture_describe(header[3], header[4], &description);
	art = art_texture(header, &description);
	if (art)
	{
		offset = art->data;
		format = art->format;
		description.levels = 1;
	}
	max_lod = description.levels > 1 ? (description.levels - 1) << 8 : 0;
	if (max_lod > 0xfff)
		max_lod = 0xfff;
	if (ts[D3DTSS_MAXANISOTROPY] >= 4)
		anisotropy = 2;
	else if (ts[D3DTSS_MAXANISOTROPY] >= 2)
		anisotropy = 1;
	control = NV097_SET_TEXTURE_CONTROL0_ENABLE | ((ts[D3DTSS_MAXMIPLEVEL] << 8 & 0xfff) << 18) | (max_lod << 6) |
		(anisotropy << 4) | (ts[D3DTSS_ALPHAKILL] ? NV097_SET_TEXTURE_CONTROL0_ALPHA_KILL_ENABLE : 0) |
		(ts[D3DTSS_COLORKEYOP] & NV097_SET_TEXTURE_CONTROL0_COLOR_KEY_MODE);
	bias = bits_float(ts[D3DTSS_MIPMAPLODBIAS]);
	bias_fixed = (long)(bias * 256.0f);
	if (bias_fixed > 0xfff)
		bias_fixed = 0xfff;
	if (bias_fixed < -0x1000)
		bias_fixed = -0x1000;
	filter = ((DWORD)bias_fixed & 0x1fff) | (texture_min_filter(ts[D3DTSS_MINFILTER], ts[D3DTSS_MIPFILTER]) << 16) |
		(texture_mag_filter(ts[D3DTSS_MAGFILTER]) << 24) | (ts[D3DTSS_COLORSIGN] & 0xf0000000);

	/* offset, format, address, control 0 and 1, filter, image rectangle,
	palette and border color are consecutive methods */
	*(*p)++ = METHOD(base, 2);
	*(*p)++ = offset & 0x03ffffff;
	*(*p)++ = format;
	PUSH1(*p, base + 0x08, (ts[D3DTSS_ADDRESSU] & 0xf) | (ts[D3DTSS_ADDRESSV] & 0xf) << 8 |
		(ts[D3DTSS_ADDRESSW] & 0xf) << 16 | D3D__RenderState[D3DRS_WRAP0 + stage]);
	PUSH2(*p, base + 0x0c, control, description.linear ? description.pitch << 16 : 0);
	PUSH1(*p, base + 0x14, filter);
	if (description.linear)
		PUSH1(*p, base + 0x1c, description.width << 16 | description.height);
	if (description.format == D3DFMT_P8 && device.palettes[stage])
	{
		const DWORD *palette = (const DWORD *)device.palettes[stage];

		PUSH1(*p, base + 0x20, (palette[1] & 0x03ffffc0) | ((palette[0] >> D3DPALETTE_COMMON_PALETTESIZE_SHIFT) & 3) << 2);
	}
	PUSH1(*p, base + 0x24, ts[D3DTSS_BORDERCOLOR]);
	if (stage > 0)
	{
		*(*p)++ = METHOD(base + 0x28, 6);
		*(*p)++ = ts[D3DTSS_BUMPENVMAT00];
		*(*p)++ = ts[D3DTSS_BUMPENVMAT01];
		*(*p)++ = ts[D3DTSS_BUMPENVMAT10];
		*(*p)++ = ts[D3DTSS_BUMPENVMAT11];
		*(*p)++ = ts[D3DTSS_BUMPENVLSCALE];
		*(*p)++ = ts[D3DTSS_BUMPENVLOFFSET];
	}
	PUSH1(*p, NV097_SET_COLOR_KEY_COLOR + stage * 4, ts[D3DTSS_COLORKEYCOLOR]);
}

static void textures_apply(void)
{
	DWORD *p = push_begin(D3DTSS_MAXSTAGES * 32);
	int stage;

	device.textures_dirty = FALSE;
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
		texture_stage_apply(&p, stage);
	push_end(p);
}

/* the definition's members are the pixel shader render states, which are
all simple: their methods take them as they are */
void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	DWORD state;

	if (!definition)
		return;
	memcpy(&D3D__RenderState[D3DRS_PSALPHAINPUTS0], definition->PSAlphaInputs, sizeof(definition->PSAlphaInputs));
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD] = definition->PSFinalCombinerInputsABCD;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG] = definition->PSFinalCombinerInputsEFG;
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT0_0], definition->PSConstant0, sizeof(definition->PSConstant0));
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT1_0], definition->PSConstant1, sizeof(definition->PSConstant1));
	memcpy(&D3D__RenderState[D3DRS_PSALPHAOUTPUTS0], definition->PSAlphaOutputs, sizeof(definition->PSAlphaOutputs));
	memcpy(&D3D__RenderState[D3DRS_PSRGBINPUTS0], definition->PSRGBInputs, sizeof(definition->PSRGBInputs));
	D3D__RenderState[D3DRS_PSCOMPAREMODE] = definition->PSCompareMode;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0] = definition->PSFinalCombinerConstant0;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1] = definition->PSFinalCombinerConstant1;
	memcpy(&D3D__RenderState[D3DRS_PSRGBOUTPUTS0], definition->PSRGBOutputs, sizeof(definition->PSRGBOutputs));
	D3D__RenderState[D3DRS_PSCOMBINERCOUNT] = definition->PSCombinerCount;
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = definition->PSTextureModes;
	D3D__RenderState[D3DRS_PSDOTMAPPING] = definition->PSDotMapping;
	D3D__RenderState[D3DRS_PSINPUTTEXTURE] = definition->PSInputTexture;
	for (state = D3DRS_PS_MIN; state < D3DRS_PS_MAX; state++)
	{
		/* (54 is unused) */
		if (state != D3DRS_PSCOMBINERCOUNT + 1)
			D3DDevice_SetRenderState_Simple(D3DSIMPLERENDERSTATEENCODE[state], D3D__RenderState[state]);
	}
	device.states_dirty = TRUE;
}

/* ---------- vertex shaders */

static void parse_declaration(struct vertex_shader *shader, const DWORD *declaration)
{
	unsigned long stream = 0, reg;
	unsigned long offsets[16] = { 0 };

	for (reg = 0; reg < VERTEX_ATTRIBUTES; reg++)
		shader->elements[reg].type = D3DVSDT_NONE;
	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;

		switch ((token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT)
		{
		case D3DVSD_TOKEN_STREAM:
			stream = token & D3DVSD_STREAMNUMBERMASK;
			break;
		case D3DVSD_TOKEN_STREAMDATA:
			if (token & D3DVSD_DATALOADTYPEMASK)
			{
				/* skip: the count is in dwords, or in bytes with bit 27 */
				unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

				offsets[stream] += (token & 0x08000000) ? count : count * 4;
			}
			else
			{
				struct vertex_element *element = &shader->elements[token & D3DVSD_VERTEXREGMASK];
				DWORD type = (token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT;
				/* the NV2A's sizes: components times bytes, CMP 4, FLOAT2H 12 */
				static const BYTE type_bytes[16] = { 1, 2, 4, 0, 1, 2, 4 };
				unsigned long components = (type >> 4) & 0xf;

				element->stream = (BYTE)stream;
				element->type = (BYTE)type;
				element->offset = (WORD)offsets[stream];
				if (type == D3DVSDT_NORMPACKED3)
					offsets[stream] += 4;
				else if (type == D3DVSDT_FLOAT2H)
					offsets[stream] += 12;
				else
					offsets[stream] += components * type_bytes[type & 0xf];
			}
			break;
		case D3DVSD_TOKEN_CONSTMEM:
			declaration += ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT) * 4;
			break;
		case D3DVSD_TOKEN_EXT:
			declaration += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
			break;
		default:
			break;
		}
	}
}

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader *shader = calloc(1, sizeof(*shader));

	(void)usage;
	if (!shader)
		return E_OUTOFMEMORY;
	shader->signature = VERTEX_SHADER_SIGNATURE;
	if (function)
	{
		/* header: program type in the low word, instruction count in the high */
		shader->instruction_count = function[0] >> 16;
		shader->instructions = malloc(shader->instruction_count * 16);
		if (!shader->instructions)
		{
			free(shader);
			return E_OUTOFMEMORY;
		}
		memcpy(shader->instructions, function + 1, shader->instruction_count * 16);
	}
	parse_declaration(shader, declaration);
	/* (even: odd handles are fixed-function vertex formats) */
	*handle = (DWORD)shader;
	return S_OK;
}

static struct vertex_shader *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader *shader = (struct vertex_shader *)handle;

	if (!handle || (handle & 1) || shader->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return shader;
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	/* the program memory and the device may still name it; it is small */
	(void)handle;
}

static void program_load(struct vertex_shader *shader, DWORD address)
{
	unsigned long done = 0;
	DWORD *p;

	if (!shader->instruction_count || address >= PROGRAM_SLOTS || device.programs[address] == shader)
		return;
	p = push_begin(2);
	PUSH1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, address);
	push_end(p);
	while (done < shader->instruction_count)
	{
		unsigned long batch = shader->instruction_count - done > 8 ? 8 : shader->instruction_count - done;

		p = push_begin(1 + batch * 4);
		*p++ = METHOD(NV097_SET_TRANSFORM_PROGRAM, batch * 4);
		memcpy(p, shader->instructions + done * 4, batch * 16);
		p += batch * 4;
		push_end(p);
		done += batch;
	}
	/* what it overwrote is gone */
	for (done = 0; done < shader->instruction_count && address + done < PROGRAM_SLOTS; done++)
		device.programs[address + done] = NULL;
	device.programs[address] = shader;
}

static void program_start(DWORD address)
{
	DWORD *p = push_begin(2);

	PUSH1(p, NV097_SET_TRANSFORM_PROGRAM_START, address);
	push_end(p);
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct vertex_shader *shader = vertex_shader_from_handle(handle);

	if (!shader)
		return;
	device.vertex_shader = shader;
	device.arrays_dirty = TRUE;
	program_load(shader, 0);
	program_start(0);
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader *shader = vertex_shader_from_handle(handle);

	if (shader)
		program_load(shader, address);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader *shader = vertex_shader_from_handle(handle);

	if (shader)
	{
		device.vertex_shader = shader;
		device.arrays_dirty = TRUE;
	}
	program_start(address);
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader *shader = vertex_shader_from_handle(handle);

	*size = shader ? shader->instruction_count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + XGPU_VERTEX_CONSTANT_BIAS;

	if (first < 0 || first >= XGPU_VERTEX_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > XGPU_VERTEX_CONSTANT_COUNT)
		constant_count = XGPU_VERTEX_CONSTANT_COUNT - first;
	constants_write((unsigned long)first, constant_data, constant_count);
}

/* ---------- drawing */

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number >= 16)
		return;
	device.streams[stream_number].buffer = stream_data;
	device.streams[stream_number].data = stream_data ? stream_data->Data : 0;
	device.streams[stream_number].stride = stride;
	device.arrays_dirty = TRUE;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	D3D__IndexData = index_data ? (WORD *)index_data->Data : NULL;
	device.base_vertex = base_vertex_index;
}

/* the vertex arrays of the declaration in use, from the first vertex the
draw counts from */
static void arrays_apply(UINT base)
{
	const struct vertex_shader *shader = device.vertex_shader;
	DWORD formats[VERTEX_ATTRIBUTES], offsets[VERTEX_ATTRIBUTES];
	DWORD *p;
	int reg;

	device.arrays_dirty = FALSE;
	device.arrays_base = base;
	for (reg = 0; reg < VERTEX_ATTRIBUTES; reg++)
	{
		const struct vertex_element *element = shader ? &shader->elements[reg] : NULL;
		DWORD data = element ? device.streams[element->stream].data : 0;

		if (!element || element->type == D3DVSDT_NONE || !data)
		{
			/* the attribute keeps its last value */
			formats[reg] = D3DVSDT_NONE;
			offsets[reg] = 0;
			continue;
		}
		formats[reg] = element->type | device.streams[element->stream].stride << 8;
		offsets[reg] = (data + element->offset + base * device.streams[element->stream].stride) & 0x03ffffff;
	}
	p = push_begin(2 * (1 + VERTEX_ATTRIBUTES));
	*p++ = METHOD(NV097_SET_VERTEX_DATA_ARRAY_OFFSET, VERTEX_ATTRIBUTES);
	memcpy(p, offsets, sizeof(offsets));
	p += VERTEX_ATTRIBUTES;
	*p++ = METHOD(NV097_SET_VERTEX_DATA_ARRAY_FORMAT, VERTEX_ATTRIBUTES);
	memcpy(p, formats, sizeof(formats));
	p += VERTEX_ATTRIBUTES;
	push_end(p);
}

static void prepare_draw(UINT base)
{
	int index;

	for (index = 0; index < D3DTSS_MAXSTAGES; index++)
	{
		resource_used(device.textures[index]);
		resource_used(device.palettes[index]);
	}
	for (index = 0; index < 16; index++)
		resource_used(device.streams[index].buffer);
	resource_used(device.render_target);
	resource_used(device.depth_stencil);
	if (device.surface_dirty)
		surface_apply();
	if (device.states_dirty)
		states_apply();
	if (device.textures_dirty)
		textures_apply();
	if (device.arrays_dirty || device.arrays_base != base)
		arrays_apply(base);
}

static BOOL trace_frame(void)
{
	static long frame = -2;

	if (frame == -2)
		frame = config_integer("debug.gpu_trace_frame");
	return frame >= 0 && frame_number == (unsigned long)frame;
}

static void trace_draw(const char *kind, DWORD primitive, unsigned long count)
{
	char textures[4 * 20] = "";
	size_t used = 0;
	int stage;

	/* each stage's texture: its data and format words */
	for (stage = 0; stage < 4; stage++)
	{
		const DWORD *texture = (const DWORD *)device.textures[stage];

		used += snprintf(textures + used, sizeof(textures) - used, " %08lx/%08lx",
			texture ? (unsigned long)texture[1] : 0, texture ? (unsigned long)texture[3] : 0);
	}
	platform_log("trace %s prim %lu count %lu target %08lx viewport %lu,%lu %lux%lu tex%s modes %08lx "
		"blend %d %lx %lx zenable %lu colorwrite %08lx", kind, (unsigned long)primitive, count,
		device.render_target ? (unsigned long)device.render_target->Data : 0, (unsigned long)device.viewport.X,
		(unsigned long)device.viewport.Y, (unsigned long)device.viewport.Width, (unsigned long)device.viewport.Height,
		textures, (unsigned long)D3D__RenderState[D3DRS_PSTEXTUREMODES], (int)D3D__RenderState[D3DRS_ALPHABLENDENABLE],
		(unsigned long)D3D__RenderState[D3DRS_SRCBLEND], (unsigned long)D3D__RenderState[D3DRS_DESTBLEND],
		(unsigned long)D3D__RenderState[D3DRS_ZENABLE], (unsigned long)D3D__RenderState[D3DRS_COLORWRITEENABLE]);
}

static void begin_end(DWORD primitive)
{
	DWORD *p = push_begin(2);

	PUSH1(p, NV097_SET_BEGIN_END, primitive);
	push_end(p);
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT vertex_count)
{
	if (!vertex_count)
		return;
	prepare_draw(0);
	if (trace_frame())
		trace_draw("arrays", primitive_type, vertex_count);
	begin_end(primitive_type);
	while (vertex_count)
	{
		/* each word draws up to 256 vertices */
		unsigned long words = (vertex_count + 255) / 256, index;
		DWORD *p;

		if (words > 512)
			words = 512;
		p = push_begin(1 + words);
		*p++ = METHOD_NONINCREASING(NV097_DRAW_ARRAYS, words);
		for (index = 0; index < words && vertex_count; index++)
		{
			UINT count = vertex_count > 256 ? 256 : vertex_count;

			*p++ = ((count - 1) << 24) | start_vertex;
			start_vertex += count;
			vertex_count -= count;
		}
		push_end(p);
	}
	begin_end(NV097_SET_BEGIN_END_OP_END);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	if (!vertex_count || !index_data)
		return;
	prepare_draw(device.base_vertex);
	if (trace_frame())
		trace_draw("indexed", primitive_type, vertex_count);
	begin_end(primitive_type);
	/* two indices a word, the odd last one alone */
	while (vertex_count >= 2)
	{
		unsigned long words = vertex_count / 2;
		DWORD *p;

		if (words > 1024)
			words = 1024;
		p = push_begin(1 + words);
		*p++ = METHOD_NONINCREASING(NV097_ARRAY_ELEMENT16, words);
		memcpy(p, index_data, words * 4);
		p += words;
		push_end(p);
		index_data += words * 2;
		vertex_count -= words * 2;
	}
	if (vertex_count)
	{
		DWORD *p = push_begin(2);

		PUSH1(p, NV097_ARRAY_ELEMENT32, *index_data);
		push_end(p);
	}
	begin_end(NV097_SET_BEGIN_END_OP_END);
}

/* ---------- immediate mode: vertices written into the push buffer */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	prepare_draw(0);
	if (trace_frame())
	{
		trace_draw("immediate", primitive_type, 0);
		trace_immediate_pending = TRUE;
	}
	begin_end(primitive_type);
}

void WINAPI D3DDevice_End(void)
{
	begin_end(NV097_SET_BEGIN_END_OP_END);
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	DWORD *p = push_begin(3);

	if (trace_immediate_pending && reg == 0)
	{
		platform_log("trace   first vertex %g %g", a, b);
		trace_immediate_pending = FALSE;
	}

	PUSH2(p, NV097_SET_VERTEX_DATA2F_M + reg * 8, float_bits(a), float_bits(b));
	push_end(p);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	DWORD *p = push_begin(5);

	*p++ = METHOD(NV097_SET_VERTEX_DATA4F_M + reg * 16, 4);
	*p++ = float_bits(a);
	*p++ = float_bits(b);
	*p++ = float_bits(c);
	*p++ = float_bits(d);
	push_end(p);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	DWORD *p = push_begin(2);

	PUSH1(p, NV097_SET_VERTEX_DATA2S + reg * 4, (WORD)a | (DWORD)(WORD)b << 16);
	push_end(p);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	DWORD *p = push_begin(2);

	PUSH1(p, NV097_SET_VERTEX_DATA4UB + reg * 4, a | (DWORD)b << 8 | (DWORD)c << 16 | (DWORD)d << 24);
	push_end(p);
}

/* a D3DCOLOR's red, green, blue and alpha */
void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	if (trace_immediate_pending)
		platform_log("trace   color %d %08lx c0: %08lx %08lx %08lx %08lx %08lx", reg, (unsigned long)color,
			(unsigned long)D3D__RenderState[D3DRS_PSCONSTANT0_0], (unsigned long)D3D__RenderState[D3DRS_PSCONSTANT0_1],
			(unsigned long)D3D__RenderState[D3DRS_PSCONSTANT1_0], (unsigned long)D3D__RenderState[D3DRS_PSCONSTANT0_4],
			(unsigned long)D3D__RenderState[D3DRS_PSCOMBINERCOUNT]);
	D3DDevice_SetVertexData4ub(reg, (BYTE)(color >> 16), (BYTE)(color >> 8), (BYTE)color, (BYTE)(color >> 24));
}

/* ---------- clearing */

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	DWORD depth_value, *p;
	D3DRECT whole;
	DWORD index;

	if (device.surface_dirty)
		surface_apply();
	resource_used(device.render_target);
	resource_used(device.depth_stencil);
	if (!device.depth_stencil)
		flags &= ~(DWORD)(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
	flags &= D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL;
	if (!flags)
		return;
	if (depth_scale() > 65535.0f)
		depth_value = ((DWORD)(z * 16777215.0f) << 8) | (stencil & 0xff);
	else
		depth_value = (DWORD)(z * 65535.0f);
	/* without rectangles, the viewport (which keeps a split-screen
	window's clear in its window) */
	if (!count || !rectangles)
	{
		whole.x1 = device.viewport.X;
		whole.y1 = device.viewport.Y;
		whole.x2 = device.viewport.X + device.viewport.Width;
		whole.y2 = device.viewport.Y + device.viewport.Height;
		rectangles = &whole;
		count = 1;
	}
	for (index = 0; index < count; index++)
	{
		LONG left = rectangles[index].x1 > (LONG)device.viewport.X ? rectangles[index].x1 : (LONG)device.viewport.X;
		LONG top = rectangles[index].y1 > (LONG)device.viewport.Y ? rectangles[index].y1 : (LONG)device.viewport.Y;
		LONG right = rectangles[index].x2 < (LONG)(device.viewport.X + device.viewport.Width) ?
			rectangles[index].x2 : (LONG)(device.viewport.X + device.viewport.Width);
		LONG bottom = rectangles[index].y2 < (LONG)(device.viewport.Y + device.viewport.Height) ?
			rectangles[index].y2 : (LONG)(device.viewport.Y + device.viewport.Height);

		if (left >= right || top >= bottom)
			continue;
		if (device.swizzled_width)
		{
			/* the NV2A clears no swizzled surface; the whole of one in one
			value is the same in any order, so it is cleared as a pitch
			surface, and the next draw makes it swizzled again */
			if (left || top || (DWORD)right != device.swizzled_width || (DWORD)bottom != device.swizzled_height)
			{
				static BOOL logged;

				if (!logged)
					platform_log("Direct3D: part of a %lux%lu swizzled target not cleared",
						(unsigned long)device.swizzled_width, (unsigned long)device.swizzled_height);
				logged = TRUE;
				continue;
			}
			p = push_begin(2);
			PUSH1(p, NV097_SET_SURFACE_FORMAT, device.surface_format | NV097_SET_SURFACE_FORMAT_TYPE_PITCH << 8);
			push_end(p);
			device.surface_dirty = TRUE;
		}
		p = push_begin(10);
		PUSH2(p, NV097_SET_CLEAR_RECT_HORIZONTAL, (DWORD)left | (DWORD)(right - 1) << 16,
			(DWORD)top | (DWORD)(bottom - 1) << 16);
		PUSH2(p, NV097_SET_ZSTENCIL_CLEAR_VALUE, depth_value, color);
		PUSH1(p, NV097_CLEAR_SURFACE, flags);
		push_end(p);
	}
}

/* ---------- presentation */

void xbox_log_write(const char *text);

/* debug.screenshot_every on the Xbox: the frame goes to the log, which the
development loop saves as a PNG in its run's folder (tools/xbox_dev.py),
as "screenshot FRAME WIDTH HEIGHT", a line of base64 RGB a row, each
starting "~", and "screenshot end" (xemu downloads a surface the CPU
reads, so the back buffer in memory is what the GPU drew). The frame's
other targets follow as "screenshot FRAME WIDTH HEIGHT ADDRESS" */
static void screenshot_to_log(unsigned long frame, const D3DSurface *surface)
{
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	static char line[1 + SCREEN_WIDTH * 4 + 2];
	struct xgpu_texture_description description;
	unsigned long width, height, pitch, x, y, bytes;
	const BYTE *pixels;
	char header[64];

	if (surface == &device.back_buffer)
	{
		xbox_gpu_screen(&width, &height, &pitch);
		description.format = D3DFMT_LIN_A8R8G8B8;
		description.linear = TRUE;
		snprintf(header, sizeof(header), "screenshot %lu %lu %lu\n", frame, width, height);
	}
	else
	{
		xgpu_texture_describe(surface->Format, surface->Size, &description);
		width = description.width;
		height = description.height;
		pitch = description.pitch;
		snprintf(header, sizeof(header), "screenshot %lu %lu %lu %08lx\n", frame, width, height,
			(unsigned long)surface->Data);
	}
	switch (description.format)
	{
	case D3DFMT_A8R8G8B8: case D3DFMT_LIN_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_LIN_X8R8G8B8:
		bytes = 4;
		break;
	case D3DFMT_R5G6B5: case D3DFMT_LIN_R5G6B5:
		bytes = 2;
		break;
	default:
		return;
	}
	if (width > SCREEN_WIDTH)
		return;
	pixels = (const BYTE *)((surface->Data & 0x03ffffff) | 0x80000000UL);
	xbox_log_write(header);
	for (y = 0; y < height; y++)
	{
		char *out = line;

		*out++ = '~';
		/* a pixel's red, green and blue, three bytes, as four digits */
		for (x = 0; x < width; x++)
		{
			const BYTE *texel = pixels + (description.linear ? y * pitch + x * bytes :
				swizzle_offset(x, y, width, height) * bytes);
			DWORD value;

			if (bytes == 4)
				value = (DWORD)texel[2] << 16 | (DWORD)texel[1] << 8 | texel[0];
			else
			{
				DWORD texel16 = texel[0] | (DWORD)texel[1] << 8;

				value = (texel16 >> 11 & 31) * 255 / 31 << 16 | (texel16 >> 5 & 63) * 255 / 63 << 8 |
					(texel16 & 31) * 255 / 31;
			}
			*out++ = digits[value >> 18 & 63];
			*out++ = digits[value >> 12 & 63];
			*out++ = digits[value >> 6 & 63];
			*out++ = digits[value & 63];
		}
		*out++ = '\n';
		*out = 0;
		xbox_log_write(line);
	}
	xbox_log_write("screenshot end\n");
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle,
	void *unused, void *unused2)
{
	static unsigned long frame;
	static DWORD last_frame_fence;
	DWORD fence;

	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	fence = fence_insert();
	if (screenshot_frame())
	{
		int i;

		fence_wait(fence);
		screenshot_to_log(frame + 1, &device.back_buffer);
		for (i = 0; i < screenshot_target_count; i++)
			screenshot_to_log(frame + 1, &screenshot_targets[i]);
		screenshot_target_count = 0;
	}
	xbox_gpu_present();
	/* the GPU draws this frame while the CPU makes the next; the one
	before must be finished (what the frames' draws read: synchronisation) */
	fence_wait(last_frame_fence);
	last_frame_fence = fence;
	/* pbkit drew its own surface methods for the next back buffer */
	device.back_buffer.Data = xbox_gpu_back_buffer();
	device.surface_dirty = TRUE;
	flip_count++;
	frame++;
	frame_number = frame;
	/* how far a run got, now and then, and how fast since the last time
	(by the vertical blanks: 60 a second) */
	if (frame <= 3 || frame % 300 == 0)
	{
		static unsigned long last_frame, last_blank;
		unsigned long blank = xbox_gpu_vertical_blank_count();

		if (last_blank && blank != last_blank)
			platform_log("frame %lu, %lu.%lu fps", frame, (frame - last_frame) * 60 / (blank - last_blank),
				(frame - last_frame) * 600 / (blank - last_blank) % 10);
		else
			platform_log("frame %lu", frame);
		last_frame = frame;
		last_blank = blank;
	}
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}
