/*
D3D8_SOFT.C

The Xbox Direct3D 8 device of the Nspire port, drawn in software on the
calculator's 320x240 screen (port/nspire/README.md).

The game drives the device through the XDK's inline functions, which keep
the simple render states in D3D__RenderState and call here for the rest, as
on the other ports (port/linux/src/d3d8_gl.c, which this follows). The game
lays out a 640x480 screen; the device draws it at half that size straight
into the screen's pixels (posix_video.c), with a 16-bit depth buffer of the
same size. Draws go to the rasterizer (soft_rasterizer.c).

The back buffer and depth buffer the game is given describe 640x480
surfaces, as it expects, over a single page: the picture is in the screen,
and the calculator has no memory for 2.4 MB the game never reads.
*/

#include "xgpu.h"
#include "nspire.h"
#include "soft_rasterizer.h"
#include "soft_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the start of the program (Ndless's linker script puts it at 0) */
extern char _start[];

#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);

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

/* ---------- the device */

struct soft_device soft_device;

static volatile unsigned int flip_count;
static D3DCALLBACK vertical_blank_callback;

static struct
{
	unsigned long draws, immediate_draws, clears, primitives;
	unsigned long long frame_start;
	unsigned long frame_ticks;
} statistics;

static D3DDevice *device_pointer(void)
{
	return (D3DDevice *)&soft_device;
}

static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

/* replaces main/d3d_intimacy.cpp, which reads the counter out of the Xbox
Direct3D runtime's private device structure */
volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	vertical_blank_callback = callback;
}

/* the calculator draws far below 60 frames a second: there is never a
vertical blank to wait for */
void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
}

/* ---------- surfaces */

static BOOL surface_is_depth(DWORD format)
{
	return format == D3DFMT_D24S8 || format == D3DFMT_F24S8 || format == D3DFMT_D16 || format == D3DFMT_F16 ||
		format == D3DFMT_LIN_D24S8 || format == D3DFMT_LIN_F24S8 || format == D3DFMT_LIN_D16 || format == D3DFMT_LIN_F16;
}

void soft_surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height, BOOL *depth)
{
	struct xgpu_texture_description description;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	*width = description.width;
	*height = description.height;
	*depth = surface_is_depth(description.format);
}

/* a 640x480 surface over one page (see the top of the file) */
static void screen_surface_initialize(D3DSurface *surface, D3DFORMAT format, DWORD data)
{
	unsigned long pitch = SCREEN_WIDTH * 4;

	memset(surface, 0, sizeof(*surface));
	surface->Common = D3DCOMMON_TYPE_SURFACE | 1;
	surface->Data = data;
	surface->Format = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A;
	surface->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
		((SCREEN_HEIGHT - 1) << D3DSIZE_HEIGHT_SHIFT) | (SCREEN_WIDTH - 1);
}

/* ---------- constants and the viewport */

static void constants_store(unsigned long first, const void *data, unsigned long count)
{
	unsigned long constant;

	memcpy(soft_device.constants[first], data, count * sizeof(soft_device.constants[0]));
	soft_device.constants_serial++;
	for (constant = first; constant < first + count && constant < XGPU_VERTEX_CONSTANT_COUNT; constant++)
		soft_device.constants_dirty[constant / 32] |= 1UL << (constant % 32);
}

static void viewport_update_constants(void)
{
	/* Direct3D's reserved constants c[-38] and c[-37] map clip space to
	the screen; zscale is the depth buffer's range */
	float zscale = 16777215.0f;

	if (soft_device.depth_stencil)
	{
		struct xgpu_texture_description description;

		xgpu_texture_describe(soft_device.depth_stencil->Format, soft_device.depth_stencil->Size, &description);
		if (description.format == D3DFMT_D16 || description.format == D3DFMT_LIN_D16 ||
			description.format == D3DFMT_F16 || description.format == D3DFMT_LIN_F16)
		{
			zscale = 65535.0f;
		}
	}
	soft_device.viewport_scale[0] = soft_device.viewport.Width * 0.5f;
	soft_device.viewport_scale[1] = -(float)soft_device.viewport.Height * 0.5f;
	soft_device.viewport_scale[2] = zscale * (soft_device.viewport.MaxZ - soft_device.viewport.MinZ);
	soft_device.viewport_scale[3] = 0.0f;
	soft_device.viewport_offset[0] = soft_device.viewport.X + soft_device.viewport.Width * 0.5f;
	soft_device.viewport_offset[1] = soft_device.viewport.Y + soft_device.viewport.Height * 0.5f;
	soft_device.viewport_offset[2] = zscale * soft_device.viewport.MinZ;
	soft_device.viewport_offset[3] = 0.0f;
	soft_device.depth_scale = zscale;
	if (!(soft_device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
	{
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 38, soft_device.viewport_scale, 1);
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 37, soft_device.viewport_offset, 1);
	}
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

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!soft_device.created)
	{
		void *page;

		memset(&soft_device, 0, sizeof(soft_device));
		if (presentation_parameters)
			soft_device.presentation = *presentation_parameters;
		page = platform_contiguous_alloc(0x1000, 0x1000, PLATFORM_ANY_PHYSICAL_ADDRESS, PAGE_READWRITE);
		screen_surface_initialize(&soft_device.back_buffer, D3DFMT_LIN_A8R8G8B8,
			page ? PLATFORM_VIRTUAL_TO_PHYSICAL(page) : 0);
		screen_surface_initialize(&soft_device.depth_buffer, D3DFMT_LIN_D24S8,
			page ? PLATFORM_VIRTUAL_TO_PHYSICAL(page) : 0);
		soft_device.render_target = &soft_device.back_buffer;
		soft_device.depth_stencil = &soft_device.depth_buffer;
		for (index = 0; index < D3DTS_MAX; index++)
		{
			soft_device.transforms[index]._11 = 1.0f;
			soft_device.transforms[index]._22 = 1.0f;
			soft_device.transforms[index]._33 = 1.0f;
			soft_device.transforms[index]._44 = 1.0f;
		}
		soft_device.viewport.Width = SCREEN_WIDTH;
		soft_device.viewport.Height = SCREEN_HEIGHT;
		soft_device.viewport.MaxZ = 1.0f;
		soft_device.next_vertex_shader_id = 1;
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
		D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
		D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
		D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
		D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILWRITEMASK] = 0xff;
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
		{
			D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
		}
		viewport_update_constants();

		if (!nspire_video_initialize())
			nspire_fatal("cannot set the screen's video mode");
		soft_rasterizer_initialize();
		soft_device.created = TRUE;
		nspire_log("Direct3D: software device, 320x240");
	}
	*returned_device = device_pointer();
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
	caps->MaxAnisotropy = 1;
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
	soft_device.back_buffer.Common++;
	*result = &soft_device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = soft_device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	extern void soft_rasterizer_texture_target_flush(void);

	/* (what was drawn into a small target, into its texture) */
	soft_rasterizer_texture_target_flush();
	if (render_target)
		soft_device.render_target = render_target;
	soft_device.depth_stencil = depth_stencil;
	/* like Direct3D, reset the viewport to the whole new target */
	if (soft_device.render_target)
	{
		unsigned long width, height;
		BOOL depth;

		soft_surface_dimensions(soft_device.render_target, &width, &height, &depth);
		soft_device.viewport.X = 0;
		soft_device.viewport.Y = 0;
		soft_device.viewport.Width = width;
		soft_device.viewport.Height = height;
		soft_device.viewport.MinZ = 0.0f;
		soft_device.viewport.MaxZ = 1.0f;
	}
	viewport_update_constants();
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	soft_device.viewport = *viewport;
	viewport_update_constants();
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		soft_device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = soft_device.transforms[state];
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	soft_device.shader_constant_mode = mode;
	viewport_update_constants();
}

/* ---------- synchronisation: everything is drawn when it is asked for */

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	if (callback)
		callback(context);
}

/* ---------- visibility (occlusion) tests: the rasterizer counts pixels */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
	soft_device.visibility_test_active = TRUE;
	soft_device.visibility_count = 0;
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	if (!soft_device.visibility_test_active)
		return S_OK;
	soft_device.visibility_test_active = FALSE;
	index %= SOFT_VISIBILITY_SLOTS;
	/* a pixel here is four of the game's */
	soft_device.visibility_results[index] = soft_device.visibility_count * 4;
	return S_OK;
}

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	if (time_stamp)
		*time_stamp = 0;
	if (result)
		*result = soft_device.visibility_results[index % SOFT_VISIBILITY_SLOTS];
	return S_OK;
}

/* ---------- render and texture stage state */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	(void)method;
	(void)value;
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float offset = -(float)value;
	float slope = offset * 0.25f;
	DWORD enable = value != 0;

	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZSLOPESCALE], &slope, sizeof(slope));
	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET], &offset, sizeof(offset));
	D3D__RenderState[D3DRS_POINTOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_WIREFRAMEOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_SOLIDOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_ZBIAS] = value;
}

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if (state == D3DRS_ZBIAS)
		D3DDevice_SetRenderState_ZBias(value);
	else if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3D__RenderState[state] = value; }

COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(ZEnable, D3DRS_ZENABLE)
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
COMPLEX_RENDER_STATE(YuvEnable, D3DRS_YUVENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
		soft_device.textures[stage] = texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
		soft_device.palettes[stage] = palette;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	/* the definition's members are the pixel shader render states */
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
}

/* ---------- vertex shaders */

static unsigned long vertex_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: return 2;
	case D3DVSDT_SHORT2: return 4;
	case D3DVSDT_SHORT3: return 6;
	case D3DVSDT_SHORT4: return 8;
	case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	case D3DVSDT_FLOAT2H: return 12;
	default: return 0;
	}
}

static void parse_declaration(struct soft_vertex_shader *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offsets[16] = { 0 };

	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;
		unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		switch (token_type)
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
			else if (object->element_count < XGPU_VERTEX_ATTRIBUTE_COUNT)
			{
				struct soft_vertex_element *element = &object->elements[object->element_count++];

				element->reg = (unsigned char)(token & D3DVSD_VERTEXREGMASK);
				element->stream = (unsigned char)stream;
				element->type = (unsigned char)((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				element->bytes = (unsigned char)vertex_type_bytes(element->type);
				element->offset = (unsigned short)offsets[stream];
				offsets[stream] += element->bytes;
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
	struct soft_vertex_shader *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = SOFT_VERTEX_SHADER_SIGNATURE;
	object->id = soft_device.next_vertex_shader_id++;
	if (function)
	{
		/* header: program type in the low word, instruction count in the high */
		object->instruction_count = function[0] >> 16;
		object->instructions = malloc(object->instruction_count * 4 * sizeof(DWORD));
		if (object->instructions)
			memcpy(object->instructions, function + 1, object->instruction_count * 4 * sizeof(DWORD));
		else
			object->instruction_count = 0;
	}
	parse_declaration(object, declaration);
	*handle = (DWORD)object;
	return S_OK;
}

static struct soft_vertex_shader *vertex_shader_from_handle(DWORD handle)
{
	struct soft_vertex_shader *object = (struct soft_vertex_shader *)handle;

	if (!handle || (handle & 1) || object->signature != SOFT_VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct soft_vertex_shader *object = vertex_shader_from_handle(handle);

	if (object)
	{
		soft_device.vertex_shader = object;
		soft_device.program_address = 0;
		soft_device.program_slots[0] = object;
	}
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	if (address < SOFT_VERTEX_PROGRAM_SLOTS)
		soft_device.program_slots[address] = vertex_shader_from_handle(handle);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct soft_vertex_shader *object = vertex_shader_from_handle(handle);

	if (object)
		soft_device.vertex_shader = object;
	if (address < SOFT_VERTEX_PROGRAM_SLOTS)
		soft_device.program_address = address;
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct soft_vertex_shader *object = vertex_shader_from_handle(handle);

	*size = object ? object->instruction_count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + XGPU_VERTEX_CONSTANT_BIAS;

	if (first < 0 || first >= XGPU_VERTEX_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > XGPU_VERTEX_CONSTANT_COUNT)
		constant_count = XGPU_VERTEX_CONSTANT_COUNT - first;
	constants_store((unsigned long)first, constant_data, constant_count);
}

/* the program that runs: the one loaded at the selected address, else the
current shader's own */
struct soft_vertex_shader *soft_current_program(void)
{
	struct soft_vertex_shader *program = soft_device.program_slots[soft_device.program_address];

	return program ? program : soft_device.vertex_shader;
}

/* ---------- drawing */

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number >= 16)
		return;
	soft_device.streams[stream_number].data = stream_data ? stream_data->Data : 0;
	soft_device.streams[stream_number].stride = stride;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	soft_device.base_vertex_index = base_vertex_index;
	D3D__IndexData = index_data ? (WORD *)index_data->Data : NULL;
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT vertex_count)
{
	if (!vertex_count)
		return;
	statistics.draws++;
	soft_draw(primitive_type, vertex_count, NULL, start_vertex, NULL);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	if (!vertex_count || !index_data)
		return;
	statistics.draws++;
	soft_draw(primitive_type, vertex_count, index_data, 0, NULL);
}

/* ---------- immediate mode */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	soft_device.immediate_active = TRUE;
	soft_device.immediate_type = primitive_type;
	soft_device.immediate_count = 0;
}

static void immediate_emit(void)
{
	unsigned long floats = XGPU_VERTEX_ATTRIBUTE_COUNT * 4;

	if (soft_device.immediate_count == soft_device.immediate_capacity)
	{
		unsigned long capacity = soft_device.immediate_capacity ? soft_device.immediate_capacity * 2 : 64;
		float *vertices = realloc(soft_device.immediate_vertices, capacity * floats * sizeof(float));

		if (!vertices)
			return;
		soft_device.immediate_vertices = vertices;
		soft_device.immediate_capacity = capacity;
	}
	memcpy(soft_device.immediate_vertices + soft_device.immediate_count * floats, soft_device.attributes,
		floats * sizeof(float));
	soft_device.immediate_count++;
}

void WINAPI D3DDevice_End(void)
{
	soft_device.immediate_active = FALSE;
	if (!soft_device.immediate_count)
		return;
	statistics.immediate_draws++;
	soft_draw(soft_device.immediate_type, soft_device.immediate_count, NULL, 0, soft_device.immediate_vertices);
}

static void set_attribute(INT reg, float a, float b, float c, float d)
{
	BOOL emit = FALSE;

	if (reg == D3DVSDE_VERTEX)
	{
		reg = 0;
		emit = TRUE;
	}
	if (reg < 0 || reg >= XGPU_VERTEX_ATTRIBUTE_COUNT)
		return;
	soft_device.attributes[reg][0] = a;
	soft_device.attributes[reg][1] = b;
	soft_device.attributes[reg][2] = c;
	soft_device.attributes[reg][3] = d;
	/* (the rasterizer reuses a draw's vertices only while inputs stay the same) */
	soft_device.constants_serial++;
	/* like the hardware, writing register 0 completes a vertex */
	if (soft_device.immediate_active && (emit || reg == 0))
		immediate_emit();
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	set_attribute(reg, a, b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	set_attribute(reg, a, b, c, d);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	set_attribute(reg, (float)a, (float)b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	set_attribute(reg, a / 255.0f, b / 255.0f, c / 255.0f, d / 255.0f);
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	float value[4];

	color_to_vec4(color, value);
	set_attribute(reg, value[0], value[1], value[2], value[3]);
}

/* ---------- clearing */

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	DWORD index;

	(void)stencil;
	statistics.clears++;
	if (!count || !rectangles)
	{
		/* the NV2A clips a clear without rectangles to the viewport */
		soft_clear(soft_device.viewport.X, soft_device.viewport.Y,
			soft_device.viewport.X + soft_device.viewport.Width, soft_device.viewport.Y + soft_device.viewport.Height,
			flags, color, z);
		return;
	}
	for (index = 0; index < count; index++)
	{
		long left = rectangles[index].x1 > (long)soft_device.viewport.X ? rectangles[index].x1 : (long)soft_device.viewport.X;
		long top = rectangles[index].y1 > (long)soft_device.viewport.Y ? rectangles[index].y1 : (long)soft_device.viewport.Y;
		long right = rectangles[index].x2 < (long)(soft_device.viewport.X + soft_device.viewport.Width) ?
			rectangles[index].x2 : (long)(soft_device.viewport.X + soft_device.viewport.Width);
		long bottom = rectangles[index].y2 < (long)(soft_device.viewport.Y + soft_device.viewport.Height) ?
			rectangles[index].y2 : (long)(soft_device.viewport.Y + soft_device.viewport.Height);

		if (left < right && top < bottom)
			soft_clear(left, top, right, bottom, flags, color, z);
	}
}

/* ---------- presentation */

static void draw_status(void)
{
	struct nspire_paging_statistics paging;
	char line[96];
	unsigned long long now = nspire_ticks();

	if (statistics.frame_start)
		statistics.frame_ticks = (unsigned long)(now - statistics.frame_start);
	statistics.frame_start = now;
	nspire_paging_get_statistics(&paging);
	snprintf(line, sizeof(line), "%lu ms %lu draws %lu tris  rd %lu ev %lu pin %lu/%lu",
		statistics.frame_ticks * 1000UL / 32768UL, statistics.draws + statistics.immediate_draws,
		statistics.primitives, paging.reads, paging.evictions, paging.pinned_blocks, paging.slots);
	nspire_video_text(2, 2, line);
	nspire_input_describe(line, sizeof(line));
	nspire_video_text(2, 12, line);
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle,
	void *unused, void *unused2)
{
	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	statistics.primitives = soft_rasterizer_take_primitive_count();
	soft_rasterizer_resolve();
	draw_status();
	soft_capture_screen(nspire_video_pixels(), NSPIRE_SCREEN_WIDTH, NSPIRE_SCREEN_HEIGHT);
	nspire_video_present();
	flip_count++;
	soft_device.frame++;
	/* the first frames one by one: how far a run got, if it stops */
	if (soft_device.frame <= 10)
	{
		nspire_log("frame %lu: %lu ms, %lu draws, %lu immediate, %lu primitives drawn", soft_device.frame,
			statistics.frame_ticks * 1000UL / 32768UL, statistics.draws, statistics.immediate_draws, statistics.primitives);
	}
	if (soft_device.frame % 8 == 0)
	{
		struct nspire_alignment_statistics alignment;
		static unsigned long sites_logged;

		nspire_alignment_get_statistics(&alignment);
		nspire_log("unaligned accesses done byte by byte: %lu", alignment.fixes);
		for (; sites_logged < alignment.site_count; sites_logged++)
			nspire_log("  unaligned access at halo.elf %08lx", alignment.sites[sites_logged] - (unsigned long)&_start);
		nspire_log("frame %lu: %lu ms, %lu draws, %lu immediate, %lu clears, %lu primitives",
			soft_device.frame, statistics.frame_ticks * 1000UL / 32768UL, statistics.draws,
			statistics.immediate_draws, statistics.clears, statistics.primitives);
		nspire_profile_report(8);
		soft_rasterizer_report(8);
	}
	/* one frame's draws one by one, once the level has settled */
	soft_rasterizer_frame_end();
	/* (and a captured frame's, to set beside the replay's) */
	if (soft_device.frame == 11 || soft_device.frame % 96 == 95 || soft_capture_active())
		soft_rasterizer_log_next_frame();
	statistics.draws = statistics.immediate_draws = statistics.clears = 0;
	/* the game counts vertical blanks to pace itself; one per frame */
	if (vertical_blank_callback)
		vertical_blank_callback(0);
	/* the game's other threads (input polling) run while the main one
	waits, which it otherwise never does */
	nspire_yield();
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}
