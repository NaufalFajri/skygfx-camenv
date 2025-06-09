#include "skygfx.h"
#include "neo.h"
#include "ini_parser.hpp"
#include "debugmenu_public.h"
#include "ModuleList.hpp"
//#include <fstream>

HMODULE dllModule;
DebugMenuAPI gDebugMenuAPI;
char asipath[MAX_PATH];

extern std::map<std::string, TexInfo*> texdb;
extern int32 texdbOffset;
//std::fstream lg;

// Only-once settings
//bool ps2grassFiles;
bool disableClouds;
bool disableGamma;
bool fixPcCarLight;
int explicitBuildingPipe;
int transparentLockon;
int fixShadows;
bool iCanHasNeoDrops = true;
bool iCanHasbuildingPipe = true;
bool iCanHasvehiclePipe = true;
bool iCanHasSunGlare = true;

bool privateHooks;
bool forceWindShader;

int fixingSAMP;
HMODULE UG_mod;
typedef bool(*UG_EventHook)(void* pData);
void (*UG_RegisterEventCallback)(const char *type, UG_EventHook hook);

int numConfigs;
int currentConfig = 0;
Config configs[10];
Config *config = &configs[0];
int original_bRadiosity = 0;

void *grassPixelShader;
void *simplePS;
void *simpleStochasticPS;
void *gpCurrentPixelShaderForDefaultCallbacks;
void *gpCurrentVertexShaderForDefaultCallbacks;
bool gRenderingSpheremap;
CVector reflectionCamPos;

static int defaultColourLeftUOffset;
static int defaultColourRightUOffset;
static int defaultColourTopVOffset;
static int defaultColourBottomVOffset;

DWORD& TempBufferVerticesStored = *reinterpret_cast<DWORD*>(0xC4B950);
DWORD& TempBufferIndicesStored = *reinterpret_cast<DWORD*>(0xC4B954);
RwImVertexIndex* TempBufferRenderIndexList = reinterpret_cast<RwImVertexIndex*>(0xC4B958);
RwIm3DVertex* TempVertexBuffer = reinterpret_cast<RwIm3DVertex*>(0xC4D958);

CVector2D windPos;

void refreshMenu(void);

extern "C" {
__declspec(dllexport) Config*
GetConfig(void)
{
	return config;
}
}

char*
getpath(char *path)
{
	static char tmppath[MAX_PATH];
	FILE *f;

	f = fopen(path, "r");
	if(f){
		fclose(f);
		return path;
	}
	extern char asipath[];
	strncpy(tmppath, asipath, MAX_PATH);
	strcat(tmppath, path);
	f = fopen(tmppath, "r");
	if(f){
		fclose(f);
		return tmppath;
	}
	return NULL;
}

WRAPPER void _rwD3D9RenderStateFlushCache(void) { EAXJMP(0x7FC200); }

// SAMP fucks with the render states directly instead of using RW. Synching the fog states
// before and after rendering and using SAMP graphics restore seems to make things work.
void
fixSAMP(void)
{
	static D3DCAPS9 *Caps=(D3DCAPS9*)0xC9BF00;
	static int *FogConvTable=(int*)0x8848FC;
	if(fixingSAMP){
		int fog, fogtype;
		RwRenderStateGet(rwRENDERSTATEFOGENABLE, &fog);
		RwRenderStateGet(rwRENDERSTATEFOGTYPE, &fogtype);
		_rwD3D9RenderStateFlushCache();
		RwD3D9SetRenderState(D3DRS_FOGENABLE, fog);
		d3d9device->SetRenderState(D3DRS_FOGENABLE, fog);
		int table, vertex;
		if((Caps->RasterCaps & D3DPRASTERCAPS_FOGTABLE) && (Caps->RasterCaps & D3DPRASTERCAPS_WFOG)){
			table = FogConvTable[fogtype];
			vertex = D3DFOG_NONE;
		}else{
			table = D3DFOG_NONE;
			vertex = FogConvTable[fogtype];
		}
		RwD3D9SetRenderState(D3DRS_FOGTABLEMODE, table);
		RwD3D9SetRenderState(D3DRS_FOGVERTEXMODE, vertex);
		d3d9device->SetRenderState(D3DRS_FOGTABLEMODE, table);
		d3d9device->SetRenderState(D3DRS_FOGVERTEXMODE, vertex);
	}
}

void
D3D9Render(RxD3D9ResEntryHeader *resEntryHeader, RxD3D9InstanceData *instanceData)
{
	fixSAMP();
	if(resEntryHeader->indexBuffer)
		RwD3D9DrawIndexedPrimitive(resEntryHeader->primType, instanceData->baseIndex, 0, instanceData->numVertices, instanceData->startIndex, instanceData->numPrimitives);
	else
		RwD3D9DrawPrimitive(resEntryHeader->primType, instanceData->baseIndex, instanceData->numPrimitives);
}

void
D3D9RenderDual(int dual, RxD3D9ResEntryHeader *resEntryHeader, RxD3D9InstanceData *instanceData, TexInfo *texInfo)
{
	RwBool hasAlpha;
	int alphafunc, alpharef;
	int zwrite;
	// this also takes texture alpha into account
	RwD3D9GetRenderState(D3DRS_ALPHABLENDENABLE, &hasAlpha);
	RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &zwrite);
	RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTION, &alphafunc);
	if (texInfo && !texInfo->dualPass) {
		dual = false;
	}
	if (dual && hasAlpha && zwrite) {
		int zwriteThreshold = config->zwriteThreshold;
		if (texInfo && texInfo->zwriteThreshold > 0) {
			zwriteThreshold = texInfo->zwriteThreshold;
		}
		RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTIONREF, &alpharef);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)zwriteThreshold);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONGREATEREQUAL);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
		D3D9Render(resEntryHeader, instanceData);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONLESS);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
		D3D9Render(resEntryHeader, instanceData);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)zwrite);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)alpharef);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)alphafunc);
	}else if(!zwrite){
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONALWAYS);
		D3D9Render(resEntryHeader, instanceData);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)alphafunc);
	}else
		D3D9Render(resEntryHeader, instanceData);
}

// Add a dual pass to the PC pipeline, the lazy way
void
D3D9RenderBlack_DUAL(RxD3D9ResEntryHeader *resEntryHeader, RxD3D9InstanceData *instanceData)
{
	RwD3D9SetPixelShader(NULL);
	RwD3D9SetVertexShader(instanceData->vertexShader);
	D3D9RenderDual(config->dualPassVehicle, resEntryHeader, instanceData);
}

void
D3D9RenderDefault_DUAL(RxD3D9ResEntryHeader *resEntryHeader, RxD3D9InstanceData *instanceData, RwUInt8 flags, RwTexture *texture)
{
	if(flags & (rxGEOMETRY_TEXTURED2 | rxGEOMETRY_TEXTURED)){
		RwD3D9SetTexture(texture, 0);
		RwD3D9SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		RwD3D9SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		RwD3D9SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		RwD3D9SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		RwD3D9SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		RwD3D9SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	}else{
		RwD3D9SetTexture(NULL, 0);
		RwD3D9SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		RwD3D9SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		RwD3D9SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
		RwD3D9SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	}
	RwD3D9SetPixelShader(NULL);
	RwD3D9SetVertexShader(instanceData->vertexShader);
	D3D9RenderDual(config->dualPassVehicle, resEntryHeader, instanceData);
}

WRAPPER double CTimeCycle_GetAmbientRed(void) { EAXJMP(0x560330); }
WRAPPER double CTimeCycle_GetAmbientGreen(void) { EAXJMP(0x560340); }
WRAPPER double CTimeCycle_GetAmbientBlue(void) { EAXJMP(0x560350); }

void
resetValues(void)
{
	CPostEffects::m_bRadiosity = config->doRadiosity;
	CPostEffects::m_RadiosityFilterPasses = config->radiosityFilterPasses;
	CPostEffects::m_RadiosityRenderPasses = config->radiosityRenderPasses;
	CPostEffects::m_RadiosityIntensity = config->radiosityIntensity;

	CPostEffects::m_colourLeftUOffset = config->offLeft;
	CPostEffects::m_colourRightUOffset = config->offRight;
	CPostEffects::m_colourTopVOffset = config->offTop;
	CPostEffects::m_colourBottomVOffset = config->offBottom;

	if(config->grainFilter == 0){
		CPostEffects::m_InfraredVisionGrainStrength = 0x18;
		CPostEffects::m_NightVisionGrainStrength = 0x10;
	}else{
		CPostEffects::m_InfraredVisionGrainStrength = 0x40;
		CPostEffects::m_NightVisionGrainStrength = 0x30;
	}

	CPostEffects::m_bYCbCrFilter = config->bYCbCrFilter;
	CPostEffects::m_lumaScale = config->lumaScale;
	CPostEffects::m_lumaOffset = config->lumaOffset;
	CPostEffects::m_crScale = config->crScale;
	CPostEffects::m_crOffset = config->crOffset;
	CPostEffects::m_cbScale = config->cbScale;
	CPostEffects::m_cbOffset = config->cbOffset;

	// night vision ambient green
	// not if this is the correct switch, maybe ps2ModulateWorld?
	//if(config->nightVision == 0)
	//	Patch<float>(0x735F8B, 0.4f);
	//else
	//	Patch<float>(0x735F8B, 1.0f);

	//*(int*)0x8D37D0 = config->detailedWaterDist;

	// how to handle forced z-test in PC version
	switch(config->coronaZtest){
	case 0:
		// Disable (PS2)
		Nop(0x6FB17C, 3);
		break;
	case 1:
		// Enable (PC)
		Patch(0x6FB17C + 0, (uint8)0xFF);
		Patch(0x6FB17C + 1, (uint8)0x51);
		Patch(0x6FB17C + 2, (uint8)0x20);
		break;
	case -1:
		// don't touch
		break;
	}

}

void
refreshIni(void)
{
	resetValues();
	refreshMenu();
}


RpAtomic *(*plantTab0)[4] = (RpAtomic *(*)[4])0xC039F0;
RpAtomic *(*plantTab1)[4] = (RpAtomic *(*)[4])0xC03A00;

RpAtomic*
grassRenderCallback(RpAtomic *atomic)
{
	RpAtomic *ret;
	int cullmode;
	RwRGBAReal color = { 0.0f, 0.0, 1.0f, 1.0f };

	if(config->ps2ModulateGrass){
		gpCurrentPixelShaderForDefaultCallbacks = grassPixelShader;
		RwRGBARealFromRwRGBA(&color, &atomic->geometry->matList.materials[0]->color);
		RwD3D9SetPixelShaderConstant(0, &color, 1);
	}

	RwRenderStateGet(rwRENDERSTATECULLMODE, &cullmode);
	if(!config->backfaceCull)
		RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)rwCULLMODECULLNONE);


	RxPipeline *pipe;
	int alphatest, alpharef;
	int dodual = 0;
	int detach = 0;
	pipe = atomic->pipeline;
	if(pipe == NULL)
		pipe = *(RxPipeline**)(*(DWORD*)0xC97B24+0x3C+dword_C9BC60);
	if(config->dualPassGrass && config->zwriteThresholdGrass > 0){
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
		RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTION, (void*)&alphatest);
		RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)&alpharef);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)config->zwriteThresholdGrass);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONGREATEREQUAL);
		RxPipelineExecute(pipe, atomic, 1);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONLESS);
		pipe = RxPipelineExecute(pipe, atomic, 1);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)alphatest);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)alpharef);
	}else
		pipe = RxPipelineExecute(pipe, atomic, 1);
	ret = pipe ? atomic : NULL;


	RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)cullmode);
	gpCurrentPixelShaderForDefaultCallbacks = NULL;

	return ret;
}

RpAtomic*
myDefaultCallback(RpAtomic *atomic)
{
	RxPipeline *pipe;
	int zwrite, alphatest, alpharef;
	int dodual = 0;
	int detach = 0;

	pipe = atomic->pipeline;
	if(pipe == NULL){
		pipe = *(RxPipeline**)(*(DWORD*)0xC97B24+0x3C+dword_C9BC60);
		RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, (void*)&zwrite);
		if(zwrite && config->dualPassDefault)
			dodual = 1;
	}else if(pipe == skinPipe && config->dualPassPed && config->zwriteThresholdPed > 0)
		dodual = 1;
	if(dodual){
		if (pipe == skinPipe) RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE); //this line was not on original skygfx, I don't know why, forced only on skin pipes (to be conservative) as it fixes parachute strings
		RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTION, (void*)&alphatest);
		RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)&alpharef);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)config->zwriteThresholdPed);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONGREATEREQUAL);
		RxPipelineExecute(pipe, atomic, 1);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONLESS);
		pipe = RxPipelineExecute(pipe, atomic, 1);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)alphatest);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)alpharef);
	}else
		pipe = RxPipelineExecute(pipe, atomic, 1);
	return pipe ? atomic : NULL;
}


void (*CTagManager__RenderTagForPC)(RpAtomic *atomic);
void (*CTagManager__SetupAtomic_orig)(RpAtomic *atomic);
void
CTagManager__SetupAtomic(RpAtomic *atomic)
{
	CTagManager__SetupAtomic_orig(atomic);
	/* Set the building pipeline so we have control over drawing.
	 * Note that we need the non-DN version. This works because this function
	 * is called after the building pipeline has already been set up. */
	SetPipelineID(atomic, RSPIPE_PC_CustomBuilding_PipeID);
	RpAtomicSetPipeline(atomic, CCustomBuildingPipeline__ObjPipeline);
	atomic->pipeline = CCustomBuildingPipeline__ObjPipeline;
}
void
CTagManager__RenderTag(RpAtomic *atomic)
{
	if(iCanHasbuildingPipe){
		/* building pipe can handle accurate PS2 behaviour */
		assert(atomic->pipeline == CCustomBuildingPipeline__ObjPipeline);
		atomic->renderCallBack(atomic);
	}else{
		/* Otherwise fall back */
		int alpharef;
		RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)&alpharef);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)64);
		CTagManager__RenderTagForPC(atomic);
		RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)alpharef);
	}
}

float
clamp(float f, float max)
{
	return f > max ? max : f;
}

// For Mobile-style lights
//static RwRGBAReal savedAmb;
//static RwRGBAReal savedDir;
//void
//BrightenLights(void)
//{
//	savedAmb = pAmbient->color;
//	pAmbient->color.red = clamp(pAmbient->color.red*1.5f, 1.0f);
//	pAmbient->color.green = clamp(pAmbient->color.green*1.5f, 1.0f);
//	pAmbient->color.blue = clamp(pAmbient->color.blue*1.5f, 1.0f);
//	savedDir = pDirect->color;
//	pDirect->color.red = clamp(pDirect->color.red*1.5f, 1.0f);
//	pDirect->color.green = clamp(pDirect->color.green*1.5f, 1.0f);
//	pDirect->color.blue = clamp(pDirect->color.blue*1.5f, 1.0f);
//}
//
//void
//RestoreLights(void)
//{
//	pAmbient->color = savedAmb;
//	pDirect->color = savedDir;
//}

// no longer needed as we're using our own render functions now
//RxNodeDefinition *nodeD3D9SkinAtomicAllInOneCSL = (RxNodeDefinition*)0x8DED08;
//RxNodeBodyFn __rwSkinD3D9AtomicAllInOneNode_orig;
//RwBool __rwSkinD3D9AtomicAllInOneNode_hook(RxPipelineNode *self, const RxPipelineNodeParam *params)
//{
//	// Don't render peds when we're rendering reflections
//	if(gRenderingSpheremap)
//		return 1;
////	BrightenLights();
//	RwBool ret = __rwSkinD3D9AtomicAllInOneNode_orig(self, params);
////	RestoreLights();
//	return ret;
//}

RwRGBAReal &AmbientLightColourForFrame_PedsCarsAndObjects = *(RwRGBAReal*)0xC886C4;
RwRGBAReal &DirectionalLightColourForFrame = *(RwRGBAReal*)0xC886B4;

uint8 *ambRed = (uint8*)0xB7C3C8;
uint8 *ambGreen = (uint8*)0xB7C310;
uint8 *ambBlue = (uint8*)0xB7C258;

void (*SetLightsWithTimeOfDayColour_orig)(RpWorld*);
void
SetLightsWithTimeOfDayColour(RpWorld *world)
{
	if(GetAsyncKeyState(VK_F5) & 0x8000){
		memset(ambRed, 0xFF, 184);
		memset(ambGreen, 0xFF, 184);
		memset(ambBlue, 0xFF, 184);
	}else{
		memset(ambRed, 0, 184);
		memset(ambGreen, 0, 184);
		memset(ambBlue, 0, 184);
	}
	SetLightsWithTimeOfDayColour_orig(world);
return;

	// Multiplied by 1.5 on mobile

	float mult = 1.5f;

	AmbientLightColourForFrame_PedsCarsAndObjects.red = clamp(AmbientLightColourForFrame_PedsCarsAndObjects.red*mult, 1.0f);
	AmbientLightColourForFrame_PedsCarsAndObjects.green = clamp(AmbientLightColourForFrame_PedsCarsAndObjects.green*mult, 1.0f);
	AmbientLightColourForFrame_PedsCarsAndObjects.blue = clamp(AmbientLightColourForFrame_PedsCarsAndObjects.blue*mult, 1.0f);

	DirectionalLightColourForFrame.red = clamp(DirectionalLightColourForFrame.red*mult, 1.0f);
	DirectionalLightColourForFrame.green = clamp(DirectionalLightColourForFrame.green*mult, 1.0f);
	DirectionalLightColourForFrame.blue = clamp(DirectionalLightColourForFrame.blue*mult, 1.0f);
	RpLightSetColor(pDirect, &DirectionalLightColourForFrame);
}


int tmpintensity;
RwTexture **tmptexture = (RwTexture**)0xc02dc0;
RwRGBA *CPlantMgr_AmbientColor = (RwRGBA*)0xC03A44;

RpMaterial*
setTextureAndColor(RpMaterial *material, RwRGBA *color)
{
	RwTexture *texture;
	RwRGBA newcolor;
	uint col[3];

	texture = *tmptexture;
	col[0] = color->red;
	col[1] = color->green;
	col[2] = color->blue;
	if(config->grassAddAmbient){
		col[0] += CTimeCycle_GetAmbientRed()*255;
		col[1] += CTimeCycle_GetAmbientGreen()*255;
		col[2] += CTimeCycle_GetAmbientBlue()*255;
		if(col[0] > 255) col[0] = 255;
		if(col[1] > 255) col[1] = 255;
		if(col[2] > 255) col[2] = 255;
	}
	newcolor.red = (tmpintensity * col[0]) >> 8;
	newcolor.green = (tmpintensity * col[1]) >> 8;
	newcolor.blue = (tmpintensity * col[2]) >> 8;
	newcolor.alpha = color->alpha;
	material->color = newcolor;
	if(material->texture != texture)
		RpMaterialSetTexture(material, texture);
	return material;
}

void CMessages__AddMessageJumpQWithNumber(char* text, unsigned int time, unsigned short flag, int n1, int n2, int n3, int n4, int n5, int n6, bool bPreviousBrief)
{
	((void(__cdecl*)(char*, unsigned int, unsigned short, int, int, int, int, int, int, bool))0x69E4E0)(text, time, flag, n1, n2, n3, n4, n5, n6, bPreviousBrief);
}

void __declspec(naked)
fixSeed(void)
{
	_asm{
	// 0x5DADB7
		mov	ecx, [config]
		cmp	[ecx+4], 0	// fixGrassPlacement
		jle	dontfix
		mov	ecx, [esp+54h]
		mov	ebx, [ecx+eax*4]
		mov	ebp, [ebx+4]
		lea	edi, [ebp+10h]
		mov	eax, [esi+48h]

		push	5DADCCh
		retn

	dontfix:
		fld	dword ptr [esi+48h]
		mov	ecx, [esp+54h]
		push	5DADBEh
		retn

	}
}

// copy color as is and save intensity (eax) for use in setTextureAndColor() later
void __declspec(naked)
saveIntensity(void)
{
	_asm{
	// 0x5DAE61
		movzx	eax, byte ptr [esi+44h]
		mov	[tmpintensity], eax
		mov	al, byte ptr [esi+40h]	// color
		mov	cl, byte ptr [esi+41h]
		mov	dl, byte ptr [esi+42h]
		mov     byte ptr [esp+10h], al	// local variable color
		mov     byte ptr [esp+11h], cl
		mov     byte ptr [esp+12h], dl
		mov     eax, [esi+3Ch]	// code expects texture in eax
		push	5DAEB7h
		retn
	}
}

// from Silent
void __declspec(naked)
rxD3D9DefaultRenderCallback_Hook(void)
{
	_asm
	{
		mov	ecx, [gpCurrentPixelShaderForDefaultCallbacks]
		cmp	eax, ecx	// _rwD3D9LastPixelShaderUsed
		je	rxD3D9DefaultRenderCallback_Hook_Return
		mov	dword ptr ds:[8E244Ch], ecx
		push	ecx
		mov	eax, dword ptr ds:[0C97C28h]	// RwD3D9Device
		push	eax
		mov	ecx, [eax]
		call	dword ptr [ecx+1ACh]

	rxD3D9DefaultRenderCallback_Hook_Return:
		push	756E17h
		retn
	}
}

void __declspec(naked)
rxD3D9DefaultRenderCallback_VertexShaderHook(void)
{
	_asm
	{
		mov	eax, ecx
		mov	ecx, [gpCurrentVertexShaderForDefaultCallbacks]
		cmp	eax, ecx	// _rwD3D9LastVertexShaderUsed
		je	rxD3D9DefaultRenderCallback_VertexShaderHook_Return
		mov	dword ptr ds : [8E2448h] , ecx
		push	ecx
		mov	eax, dword ptr ds : [0C97C28h]	// RwD3D9Device
		push	eax
		mov	ecx, [eax]
		call	dword ptr[ecx + 170h]

		rxD3D9DefaultRenderCallback_VertexShaderHook_Return :
		push	7572E7h
		retn
	}
}

char
CPlantMgr_Initialise(void)
{
	char (*oldfunc)(void) = (char (*)(void))0x5DD910;
	char ret;
	ret = oldfunc();

	HRSRC resource = FindResource(dllModule, MAKEINTRESOURCE(IDR_GRASSPS), RT_RCDATA);
	RwUInt32 *shader = (RwUInt32*)LoadResource(dllModule, resource);
	RwD3D9CreatePixelShader(shader, &grassPixelShader);
	FreeResource(shader);

	RpAtomic *atomic;
	for(int i = 0; i < 4; i++){
		atomic = (*plantTab0)[i];
		atomic->renderCallBack = grassRenderCallback;
		atomic = (*plantTab1)[i];
		atomic->renderCallBack = grassRenderCallback;
	}
	return ret;
}

struct FX
{
	char data[0x54];
	int fxQuality;
	int GetFxQuality_ped(void);
	int GetFxQuality_stencil(void);
};

int
FX::GetFxQuality_ped(void)
{
	if(config->pedShadows >= 0)
		return config->pedShadows ? 3 : 0;
	return this->fxQuality;
}

int
FX::GetFxQuality_stencil(void)
{
	if(config->stencilShadows >= 0)
		return config->stencilShadows ? 3 : 0;
	return this->fxQuality;
}

unsigned __int64 rand_seed = 1;
float ps2randnormalize = 1.0f/0x80000000;

int ps2rand()
{
	rand_seed = 0x5851F42D4C957F2D * rand_seed + 1;
	return ((rand_seed >> 32) & 0x7FFFFFFF);
}

void ps2srand(unsigned int seed)
{
	rand_seed = seed;
}

void __declspec(naked) floatbitpattern(void)
{
	_asm {
		fstp [esp-4]
		mov eax, [esp-4]
		sar eax,1
		ret
	}
}

WRAPPER void gtasrand(unsigned int seed) { EAXJMP(0x821B11); }

void
mysrand(unsigned int seed)
{
	gtasrand(ps2rand());
//	gtasrand(seed);
}

WRAPPER void CVehicle__DoSunGlare(void *this_) { EAXJMP(0x6DD6F0); }

void __declspec(naked) doglare(void)
{
	_asm {
		mov	ecx, [config]
		cmp	[ecx+8], 0	// doglare
		jle	noglare
		mov	ecx,esi
		call	CVehicle__DoSunGlare
	noglare:
		mov     [esp+0D4h], edi
		push	6ABD04h
		retn
	}
}

struct PointLight
{
	RwV3d pos;
	RwV3d dir;
	float radius;
	float color[3];
	void *attachedTo;
	char type;
	char fogType;
	char generateExtraShadows;
	char pad;
};

PointLight *pointLights = (PointLight*)0xC3F0E0;

WRAPPER void
CSprite__RenderBufferedOneXLUSprite_Rotate_Aspect_orig(float x, float y, float z, float a4, float a5, RwUInt8 r, RwUInt8 g, RwUInt8 b, RwInt16 f, int a10, float a11, RwUInt8 alpha) { EAXJMP(0x70E780); }

int currentLight;
char *stkp;

void
CSprite__RenderBufferedOneXLUSprite_Rotate_Aspect(float x, float y, float z, float a4, float a5, RwUInt8 r, RwUInt8 g, RwUInt8 b, RwInt16 f, int a10, float a11, RwUInt8 alpha)
{
	_asm mov [currentLight], esi
	_asm mov [stkp], ebp
	float mult = *(float*)(stkp + 0x48);
	currentLight /= sizeof(PointLight);

	float add = pointLights[currentLight].fogType == 1 ? 0.0f : 16.0f;
	r = mult*pointLights[currentLight].color[0]+add;
	g = mult*pointLights[currentLight].color[1]+add;
	b = mult*pointLights[currentLight].color[2]+add;
	f = 0xFF;
	CSprite__RenderBufferedOneXLUSprite_Rotate_Aspect_orig(x, y, z, a4, a5, r, g, b, f, a10, a11, alpha);
}

WRAPPER void
CreateRoadsignTexture_RwTextureSetName_orig(RwTexture* texture, char* name) { EAXJMP(0x7F38A0); }

void CreateRoadsignTexture_RwTextureSetName(RwTexture* texture, char* name)
{
	strcpy_s(texture->name, 32, "roadsign");
	*RWPLUGINOFFSET(TexInfo*, texture, texdbOffset) = FindTexInfo("roadsign");
	CreateRoadsignTexture_RwTextureSetName_orig(texture, name);
}

WRAPPER RpAtomic*
CreateRoadsignAtomicA_RpAtomicSetGeometry_orig(RpAtomic* atomic, RpGeometry* geometry, int sameBoundingSphere) { EAXJMP(0x749D40); }

void
CreateRoadsignAtomicA_RpAtomicSetGeometry(RpAtomic* atomic, RpGeometry* geometry, int sameBoundingSphere)
{
	// still not good, maybe create a new one for specially for roadsign
	// try looking at roadsign text without roadsign plate, the text should be smooth like that
	SetPipelineID(atomic, RSPIPE_PC_CustomBuilding_PipeID);
	RpAtomicSetPipeline(atomic, CCustomBuildingPipeline__ObjPipeline);
	atomic->pipeline = CCustomBuildingPipeline__ObjPipeline;

	CreateRoadsignAtomicA_RpAtomicSetGeometry_orig(atomic, geometry, sameBoundingSphere);
}


/*
struct CVector { float x, y, z; };
WRAPPER void CWaterLevel__CalculateWavesForCoordinate(int x, int y, float a3, float a4, float *z, float *colorMult, float *a7, CVector *vecnormal){ EAXJMP(0x6E6EF0); }
void
CWaterLevel__CalculateWavesForCoordinate_hook(int x, int y, float a3, float a4, float *z, float *colorMult, float *a7, CVector *vecnormal)
{
	CWaterLevel__CalculateWavesForCoordinate(x, y, a3, a4, z, colorMult, a7, vecnormal);
	*colorMult = 0.577f;
}
*/

static int alphafunc;
static void setMoonAlphaBlendStates(void){
	RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTION, &alphafunc);
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONALWAYS);
	RwD3D9SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, 1);
	RwD3D9SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
	RwD3D9SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_SRCALPHA);
	RwD3D9SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
}
static void restoreMoonAlphaBlendStates(void){
	RwD3D9SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, 0);
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)alphafunc);
}
void __declspec(naked) renderMoonMask(void)
{
	_asm {
		call setMoonAlphaBlendStates
		mov  eax,0x70D000
		call eax
		call restoreMoonAlphaBlendStates
		push 0x713C51
		retn
	}
}

void (*CSkidmarks__Render_orig)(void);
void CSkidmarks__Render(void)
{
	int alphafunc;
	RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTION, &alphafunc);
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONALWAYS);
	CSkidmarks__Render_orig();
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)alphafunc);
}

static uint32_t RenderScene_A;
WRAPPER void RenderScene(void) { VARJMP(RenderScene_A); }

void RenderReflectionMap_leeds(void);
void RenderReflectionScene(void);
void DrawDebugEnvMap(void);

bool
RenderScene_before(void*)
{
	// Do this because far and fog plane are set AFTER calling BeingUpdate in Idle()
	RwCameraEndUpdate(Scene.camera);
	RwCameraBeginUpdate(Scene.camera);

	// update wind
	float freq = 0.01f;
	float modifierX = freq + (freq * CWeather__WindDir.x);
	float modifierY = freq + (freq * CWeather__WindDir.y);
	windPos.x += modifierX * CTimer__ms_fTimeStep;
	windPos.y += modifierY * CTimer__ms_fTimeStep;

	return true;
}

bool
RenderScene_after(void*)
{
	if(config->vehiclePipe == CAR_NEO)
		CarPipe::RenderEnvTex();
	else if(config->vehiclePipe == CAR_LCS || config->vehiclePipe == CAR_VCS)
		RenderReflectionMap_leeds();
	DrawDebugEnvMap();
	return true;
}
void
RenderScene_hook(void)
{
	RenderScene_before(nil);
	RenderScene();
	RenderScene_after(nil);
}

int (*PipelinePluginAttach)(void);
int
myPluginAttach(void)
{
	return (bool)(PipelinePluginAttach() && PDSPipePluginAttach() && TexDBPluginAttach() && EDEDPluginAttach());
}

void (*InitialiseGame)(void);
void
InitialiseGame_hook(void)
{
	ONCE;
	if(!UG_RegisterEventCallback)
		InterceptCall(&RenderScene_A, RenderScene_hook, 0x53EABF);
	else{
		UG_RegisterEventCallback("EVENT_BEFORE_RENDERSCENE", RenderScene_before);
		UG_RegisterEventCallback("EVENT_AFTER_RENDERSCENE", RenderScene_after);
	}

	/*lg.open("skygfx.log", std::fstream::out | std::fstream::trunc);
	lg << "test" << "\n";
	lg.flush();*/

void envmaphooks(void);
envmaphooks();
	neoInit();
	initTexDB();
	InitialiseGame();
}

void* RwIm3DTransform(RwIm3DVertex* pVerts, RwUInt32 numVerts, RwMatrix* ltm, RwUInt32 flags) {
	return ((void* (__cdecl*)(RwIm3DVertex*, RwUInt32, RwMatrix*, RwUInt32))0x7EF450)(pVerts, numVerts, ltm, flags);
}

RwBool RwIm3DEnd(void) {
	return ((RwBool(__cdecl*)(void))0x7EF520)();
}

RwBool RwIm3DRenderIndexedPrimitive(RwPrimitiveType primType, RwImVertexIndex* indices, RwInt32 numIndices) {
	return ((RwBool(__cdecl*)(RwPrimitiveType, RwImVertexIndex*, RwInt32))0x7EF550)(primType, indices, numIndices);
}

void (*CWaterLevel__RenderAndEmptyRenderBuffer)(void);
void
CWaterLevel__RenderAndEmptyRenderBuffer_hook(void)
{
	/*if (TempBufferVerticesStored)
	{
		//GenerateNormals();
		if (g_pCustomWaterPipe)
			g_pCustomWaterPipe->RenderWater(TempVertexBuffer, TempBufferVerticesStored, TempBufferRenderIndexList, TempBufferIndicesStored);
	}
	TempBufferIndicesStored = 0;
	TempBufferVerticesStored = 0;*/

	_rwD3D9RenderStateFlushCache();

	if (TempBufferVerticesStored) {
		//RwD3D9SetStreamSource(0, TempVertexBuffer, 0, sizeof(RwIm3DVertex));
		//RwD3D9SetIndices(TempBufferRenderIndexList);
		//RwD3D9SetVertexDeclaration(TempVertexBuffer);
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)true);
		RwD3D9SetPixelShader(simplePS);
		//RwD3D9DrawIndexedPrimitive(3, (RwInt32)TempVertexBuffer, 0, TempBufferVerticesStored, 0, TempBufferIndicesStored);
		RwD3D9DrawIndexedPrimitiveUP(3, 0, TempBufferVerticesStored, TempBufferIndicesStored, TempBufferRenderIndexList, TempVertexBuffer, sizeof(RwIm3DVertex));
	}

	/*if (TempBufferVerticesStored)
	{
		if (RwIm3DTransform(TempVertexBuffer, TempBufferVerticesStored, 0, 1u))
		{
			RwIm3DRenderIndexedPrimitive(RwPrimitiveType::rwPRIMTYPETRILIST, TempBufferRenderIndexList, TempBufferIndicesStored);
			RwIm3DEnd();
		}
	}*/
	TempBufferVerticesStored = 0;
	TempBufferIndicesStored = 0;
}

/*void (*StartWaterRender)(void);
void
StartWaterRender_hook(void)
{
	RwD3D9SetPixelShader(simpleStochasticPS);
	gpCurrentPixelShaderForDefaultCallbacks = simpleStochasticPS;
	StartWaterRender();
}

void (*EndWaterRender)(void);
void
EndWaterRender_hook(void)
{
	EndWaterRender();
}*/

// not working yet
//void __declspec(naked) selectVM(void)
//{
//	_asm {
//		test	[esp+0x20],1
//		jz	window
//
//		push 0x7463B8
//		retn
//
//	window:
//		push 0x7462C5
//		retn
//	}
//}






int
readhex(char *str)
{
	int n = 0;
	if(strlen(str) > 2)
		sscanf(str+2, "%X", &n);
	return n;
}

BOOL FileExists(LPCTSTR szPath)
{
	DWORD dwAttrib = GetFileAttributes(szPath);
	return dwAttrib != INVALID_FILE_ATTRIBUTES && 
		   !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY);
}

struct StrAssoc
{
	const char *key;
	int val;

	static int get(StrAssoc *desc, const char *key);
};
int
StrAssoc::get(StrAssoc *desc, const char *key)
{
	for(; desc->key[0] != '\0'; desc++)
		if(strcmpi(desc->key, key) == 0)
			return desc->val;
	return desc->val;
}

void
findInis(void)
{
	char modulePath[MAX_PATH];
	GetModuleFileName(dllModule, modulePath, MAX_PATH);
	size_t nLen = strlen(modulePath);
	modulePath[nLen+1] = L'\0';
	modulePath[nLen] = L'i';
	modulePath[nLen-1] = L'n';
	modulePath[nLen-2] = L'i';
	modulePath[nLen-3] = L'.';
	modulePath[nLen-4] = '1';

	numConfigs = 0;
	while(numConfigs < 9 && FileExists(modulePath)){
		modulePath[nLen-4]++;
		numConfigs++;
	}
}

int
readhex(const char *str)
{
	int n = 0;
	if(strlen(str) > 2)
		sscanf(str+2, "%X", &n);
	return n;
}

int
readint(const std::string &s, int default = 0)
{
	try{
		return std::stoi(s);
	}catch(...){
		return default;
	}
}

float
readfloat(const std::string &s, float default = 0)
{
	try{
		return std::stof(s);
	}catch(...){
		return default;
	}
}

static int explicitBuildingPipe_tmp;

void
readIni(int n)
{
	int tmpint;
	char modulePath[MAX_PATH];
	GetModuleFileName(dllModule, modulePath, MAX_PATH);
	strncpy(asipath, modulePath, MAX_PATH);
	char *p = strrchr(asipath, '\\');
	if (p) p[1] = '\0';

	GetModuleFileName(dllModule, modulePath, MAX_PATH);
	size_t nLen = strlen(modulePath);
	Config *c;
	if(n > 0){
		modulePath[nLen+1] = L'\0';
		modulePath[nLen] = L'i';
		modulePath[nLen-1] = L'n';
		modulePath[nLen-2] = L'i';
		modulePath[nLen-3] = L'.';
		modulePath[nLen-4] = n+'0';
		c = &configs[n-1];
	}else{
		modulePath[nLen-1] = L'i';
		modulePath[nLen-2] = L'n';
		modulePath[nLen-3] = L'i';
		c = &configs[n];
	}
	linb::ini cfg;
	cfg.load_file(modulePath);

	c->keys[0] = readhex(cfg.get("SkyGfx", "keySwitch", "0x0").c_str());
	c->keys[1] = readhex(cfg.get("SkyGfx", "keyReload", "0x0").c_str());

	config->ps2ModulateGlobal = readint(cfg.get("SkyGfx", "ps2Modulate", ""), 0);
	config->dualPassGlobal = readint(cfg.get("SkyGfx", "dualPass", ""), 0);

	static StrAssoc buildPipeMap[] = {
		{"PS2",     BUILDING_PS2},
		{"PC",      BUILDING_XBOX},
		{"Xbox",    BUILDING_XBOX},
		{"",       -1},
	};
	c->buildingPipe = StrAssoc::get(buildPipeMap, cfg.get("SkyGfx", "buildingPipe", "").c_str());
	if(c->buildingPipe < 0){
		iCanHasbuildingPipe = false;
		c->buildingPipe = 0;
	}
	c->detailMaps = readint(cfg.get("SkyGfx", "detailMaps", ""), 0);
	c->stochastic = readint(cfg.get("SkyGfx", "stochasticTexturing", ""), 0);

	c->ps2ModulateBuilding = readint(cfg.get("SkyGfx", "ps2ModulateBuilding", ""), config->ps2ModulateGlobal);
	c->dualPassBuilding = readint(cfg.get("SkyGfx", "dualPassBuilding", ""), config->dualPassGlobal);

	static StrAssoc vehPipeMap[] = {
		{"PS2",     CAR_PS2},
		{"PC",      CAR_PC},
		{"Xbox",    CAR_XBOX},
		{"Spec",    CAR_SPEC},
		{"Neo",     CAR_NEO},
		{"Leeds",   CAR_LCS},
		{"LCS",     CAR_LCS},
		{"VCS",     CAR_VCS},
		{"Mobile",  CAR_MOBILE},
		{"Env",  CAR_ENV},
		{"",       -1},
	};
	c->vehiclePipe = StrAssoc::get(vehPipeMap, cfg.get("SkyGfx", "vehiclePipe", "").c_str());
	if(c->vehiclePipe < 0){
		iCanHasvehiclePipe = false;
		c->vehiclePipe = 0;
	}

	c->dualPassVehicle = readint(cfg.get("SkyGfx", "dualPassVehicle", ""), config->dualPassGlobal);
	c->leedsShininessMult = readfloat(cfg.get("SkyGfx", "leedsShininessMult", ""), 1.0);
	c->neoShininessMult = readfloat(cfg.get("SkyGfx", "neoShininessMult", ""), 1.0);
	c->neoSpecularityMult = readfloat(cfg.get("SkyGfx", "neoSpecularityMult", ""), 1.0);
	c->envShininessMult = readfloat(cfg.get("SkyGfx", "envShininessMult", ""), 1.0);
	c->envSpecularityMult = readfloat(cfg.get("SkyGfx", "envSpecularityMult", ""), 1.0);
	c->envPower = readfloat(cfg.get("SkyGfx", "envPower", ""), 20.0);
	c->envFresnel = readfloat(cfg.get("SkyGfx", "envFresnel", ""), 0.7);
	c->envMapSize = readint(cfg.get("SkyGfx", "envMapSize", ""), 256);
	c->envCarPaint1Fresnel = readfloat(cfg.get("SkyGfx", "envCarPaint1Fresnel", ""), 0.7);
	c->envCarPaint1Power = readfloat(cfg.get("SkyGfx", "envCarPaint1Power", ""), 20.0);
	c->envCarPaint1ShininessMult = readfloat(cfg.get("SkyGfx", "envCarPaint1ShininessMult", ""), 1.0);
	c->envCarPaint1SpecularityMult = readfloat(cfg.get("SkyGfx", "envCarPaint1SpecularityMult", ""), 1.0);
	c->envCarPaint2Fresnel = readfloat(cfg.get("SkyGfx", "envCarPaint2Fresnel", ""), 0.7);
	c->envCarPaint2Power = readfloat(cfg.get("SkyGfx", "envCarPaint2Power", ""), 20.0);
	c->envCarPaint2ShininessMult = readfloat(cfg.get("SkyGfx", "envCarPaint2ShininessMult", ""), 1.0);
	c->envCarPaint2SpecularityMult = readfloat(cfg.get("SkyGfx", "envCarPaint2SpecularityMult", ""), 1.0);
	c->envCarPaint3Fresnel = readfloat(cfg.get("SkyGfx", "envCarPaint3Fresnel", ""), 0.7);
	c->envCarPaint3Power = readfloat(cfg.get("SkyGfx", "envCarPaint3Power", ""), 20.0);
	c->envCarPaint3ShininessMult = readfloat(cfg.get("SkyGfx", "envCarPaint3ShininessMult", ""), 1.0);
	c->envCarPaint3SpecularityMult = readfloat(cfg.get("SkyGfx", "envCarPaint3SpecularityMult", ""), 1.0);
	c->envCarPaint4Fresnel = readfloat(cfg.get("SkyGfx", "envCarPaint4Fresnel", ""), 0.7);
	c->envCarPaint4Power = readfloat(cfg.get("SkyGfx", "envCarPaint4Power", ""), 20.0);
	c->envCarPaint4ShininessMult = readfloat(cfg.get("SkyGfx", "envCarPaint4ShininessMult", ""), 1.0);
	c->envCarPaint4SpecularityMult = readfloat(cfg.get("SkyGfx", "envCarPaint4SpecularityMult", ""), 1.0);

	int i = 1;
	while(i < c->envMapSize) i *= 2;
	c->envMapSize = i;
	c->envMapFarClipMult = readfloat(cfg.get("SkyGfx", "envMapFarClipMult", ""), 1.0);
	c->envMapUseLODs = readint(cfg.get("SkyGfx", "envMapUseLODs", ""), 0);
	c->doglare = readint(cfg.get("SkyGfx", "sunGlare", ""), -1);
	if(c->doglare < 0){
		iCanHasSunGlare = false;
		c->doglare = 0;
	}

	c->ps2ModulateGrass = readint(cfg.get("SkyGfx", "ps2ModulateGrass", ""), config->ps2ModulateGlobal);
	c->dualPassGrass = readint(cfg.get("SkyGfx", "dualPassGrass", ""), config->dualPassGlobal);
	c->grassAddAmbient = readint(cfg.get("SkyGfx", "grassAddAmbient", ""), 0);
//	ps2grassFiles = readint(cfg.get("SkyGfx", "ps2grassFiles", ""), 0);
	c->fixGrassPlacement = readint(cfg.get("SkyGfx", "grassFixPlacement", ""), 0);
	c->backfaceCull = readint(cfg.get("SkyGfx", "grassBackfaceCull", ""), 1);

	static StrAssoc boolMap[] = {
		{"0",       0},
		{"false",   0},
		{"1",       1},
		{"true",    1},
		{"",       -1},
	};
	c->dualPassDefault = readint(cfg.get("SkyGfx", "dualPassDefault", ""), config->dualPassGlobal);
	c->dualPassPed = readint(cfg.get("SkyGfx", "dualPassPed", ""), config->dualPassGlobal);
	c->pedShadows = StrAssoc::get(boolMap, cfg.get("SkyGfx", "pedShadows", "").c_str());
	c->stencilShadows = StrAssoc::get(boolMap, cfg.get("SkyGfx", "stencilShadows", "").c_str());
	disableClouds = readint(cfg.get("SkyGfx", "disableClouds", ""), 0);
	disableGamma = readint(cfg.get("SkyGfx", "disableGamma", ""), 0);
	transparentLockon = readint(cfg.get("SkyGfx", "transparentLockon", ""), 0);
	fixShadows = readint(cfg.get("SkyGfx", "fixShadows", ""), 0);
	c->lightningIlluminatesWorld = readint(cfg.get("SkyGfx", "lightningIlluminatesWorld", ""), 0);

	static StrAssoc colorFilterMap[] = {
		{"None",    COLORFILTER_NONE},
		{"PS2",     COLORFILTER_PS2},
		{"PC",      COLORFILTER_PC},
		{"Mobile",  COLORFILTER_MOBILE},
		{"III",     COLORFILTER_III},
		{"VC",      COLORFILTER_VC},
		{"VCS",     COLORFILTER_VCS},
		{"",        COLORFILTER_PC},
	};
	static StrAssoc ps2pcMap[] = {
		{"PS2",     0},
		{"PC",      1},
		{"",        1},
	};
	c->colorFilter = StrAssoc::get(colorFilterMap, cfg.get("SkyGfx", "colorFilter", "").c_str());
	ps2pcMap[2].val = c->colorFilter == COLORFILTER_PS2 ? 0 : 1;
	c->rgb1Mult = readfloat(cfg.get("SkyGfx", "rgb1Mult", ""), 1.0f);
	c->rgb2Mult = readfloat(cfg.get("SkyGfx", "rgb2Mult", ""), 1.0f);
	c->infraredVision = StrAssoc::get(ps2pcMap, cfg.get("SkyGfx", "infraredVision", "").c_str());
	c->nightVision = StrAssoc::get(ps2pcMap, cfg.get("SkyGfx", "nightVision", "").c_str());
	c->grainFilter = StrAssoc::get(ps2pcMap, cfg.get("SkyGfx", "grainFilter", "").c_str());
	c->usePCTimecyc = readint(cfg.get("SkyGfx", "usePCTimecyc", ""), 0);

	tmpint = readint(cfg.get("SkyGfx", "blurLeft", ""), 4000);
	c->offLeft = tmpint == 4000 ? defaultColourLeftUOffset : tmpint;
	tmpint = readint(cfg.get("SkyGfx", "blurTop", ""), 4000);
	c->offTop = tmpint == 4000 ? defaultColourTopVOffset : tmpint;
	tmpint = readint(cfg.get("SkyGfx", "blurRight", ""), 4000);
	c->offRight = tmpint == 4000 ? defaultColourRightUOffset : tmpint;
	tmpint = readint(cfg.get("SkyGfx", "blurBottom", ""), 4000);
	c->offBottom = tmpint == 4000 ? defaultColourBottomVOffset : tmpint;

	tmpint = readint(cfg.get("SkyGfx", "doRadiosity", ""), 4000);
	c->doRadiosity = tmpint == 4000 ? original_bRadiosity : tmpint;	// saved value from stream.ini

	static StrAssoc ps2shdrMap[] = {
		{"PS2",     0},
		{"Shader",  1},
		{"",        1},
	};
	c->radiosity = StrAssoc::get(ps2shdrMap, cfg.get("SkyGfx", "radiosity", "").c_str());

	c->vcsTrails = readint(cfg.get("SkyGfx", "vcsTrails", ""), 0);
	c->trailsLimit = readint(cfg.get("SkyGfx", "trailsLimit", ""), 80);
	c->trailsIntensity = readint(cfg.get("SkyGfx", "trailsIntensity", ""), 38);
	c->trailsResolution = readint(cfg.get("SkyGfx", "trailsResolution", ""), 1);

	c->radiosityFilterPasses = readint(cfg.get("SkyGfx", "radiosityFilterPasses", ""), 2);
	c->radiosityRenderPasses = readint(cfg.get("SkyGfx", "radiosityRenderPasses", ""), 1);
	c->radiosityIntensity = readint(cfg.get("SkyGfx", "radiosityIntensity", ""), 0x23);

	c->neoWaterDrops = readint(cfg.get("SkyGfx", "neoWaterDrops", ""), -1);
	if(c->neoWaterDrops < 0){
		iCanHasNeoDrops = false;
		c->neoWaterDrops = 0;
	}
	c->neoBloodDrops = readint(cfg.get("SkyGfx", "neoBloodDrops", ""), 0);
	fixPcCarLight = readint(cfg.get("SkyGfx", "fixPcCarLight", ""), 0);
	explicitBuildingPipe_tmp = readint(cfg.get("SkyGfx", "explicitBuildingPipe", ""), -1);
	c->tagsBuildingPipe = StrAssoc::get(buildPipeMap, cfg.get("SkyGfx", "tagsBuildingPipe", "").c_str());

	c->zwriteThreshold = readint(cfg.get("SkyGfx", "zwriteThreshold", ""), 128);
	if(c->zwriteThreshold < 0) c->zwriteThreshold = 0;
	if(c->zwriteThreshold > 255) c->zwriteThreshold = 255;

	c->zwriteThresholdGrass = readint(cfg.get("SkyGfx", "zwriteThresholdGrass", ""), 128);
	if(c->zwriteThresholdGrass < 0) c->zwriteThresholdGrass = 0;
	if(c->zwriteThresholdGrass > 255) c->zwriteThresholdGrass = 255;

	c->zwriteThresholdPed = readint(cfg.get("SkyGfx", "zwriteThresholdPed", ""), 128);
	if(c->zwriteThresholdPed < 0) c->zwriteThresholdPed = 0;
	if(c->zwriteThresholdPed > 255) c->zwriteThresholdPed = 255;

	c->coronaZtest = readint(cfg.get("SkyGfx", "coronaZtest", ""), -1);

	c->bYCbCrFilter = readint(cfg.get("SkyGfx", "YCbCrCorrection", ""), 0);
	c->lumaScale = readfloat(cfg.get("SkyGfx", "lumaScale", ""), 219.0f/255.0f);
	c->lumaOffset = readfloat(cfg.get("SkyGfx", "lumaOffset", ""), 16.0f/255.0f);
	c->cbScale = readfloat(cfg.get("SkyGfx", "CbScale", ""), 1.23f);
	c->cbOffset = readfloat(cfg.get("SkyGfx", "CbOffset", ""), 0.0f);
	c->crScale = readfloat(cfg.get("SkyGfx", "CrScale", ""), 1.23f);
	c->crOffset = readfloat(cfg.get("SkyGfx", "CrOffset", ""), 0.0f);


	privateHooks = readint(cfg.get("SkyGfx", "privateHooks", ""), 0);
	if (readint(cfg.get("SkyGfx", "forceWindShader", ""), 0) == 1) forceWindShader = true;
}

void
readInis(void)
{
	original_bRadiosity = CPostEffects::m_bRadiosity;
	if(numConfigs == 0)
		readIni(0);
	else
		for(int i = 1; i <= numConfigs; i++)
			readIni(i);
	refreshIni();
}

void
setConfig(void)
{
	if(currentConfig >= 0 && currentConfig < numConfigs){
		config = &configs[currentConfig];

		refreshIni();
	}
}

void
reloadAllInis(void)
{
	if(numConfigs == 0)
		readIni(0);
	else
		for(int i = 1; i <= numConfigs; i++)
			readIni(i);
	refreshIni();
}

// load asi ini again after having read stream ini as we need to know radiosity settings
void __declspec(naked)
afterStreamIni(void)
{
	_asm{
		call readInis
		retn
	}
}

#define MENUSETTINGS \
	X(ps2ModulateGlobal)		\
	X(ps2ModulateBuilding)		\
	X(ps2ModulateGrass)			\
	X(dualPassGlobal)				\
	X(dualPassDefault)				\
	X(dualPassBuilding)			\
	X(dualPassVehicle)			\
	X(dualPassPed)			\
	X(dualPassGrass)				\
	X(buildingPipe)				\
	X(tagsBuildingPipe)				\
	X(detailMaps)				\
	X(stochastic)				\
	X(vehiclePipe)				\
	X(leedsShininessMult)				\
	X(neoShininessMult)				\
	X(neoSpecularityMult)			\
	X(envShininessMult)				\
	X(envSpecularityMult)			\
	X(envPower)			\
	X(envFresnel)			\
	X(doglare)						\
	X(fixGrassPlacement)			\
	X(grassAddAmbient)			\
	X(backfaceCull)			\
	X(pedShadows)					\
	X(stencilShadows)				\
	X(colorFilter)					\
	X(doRadiosity)					\
	X(radiosity)					\
	X(lightningIlluminatesWorld)		\
	X(neoWaterDrops)			\
	X(neoBloodDrops)			\
	X(infraredVision)				\
	X(nightVision)					\
	X(grainFilter)					\
	X(offLeft)					\
	X(offRight)				\
	X(offTop)					\
	X(offBottom)				\
	X(radiosityFilterPasses)		\
	X(radiosityRenderPasses)		\
	X(radiosityIntensity)			\
	X(zwriteThreshold)			\
	X(zwriteThresholdGrass)			\
	X(zwriteThresholdPed)			\
	X(coronaZtest)				\
	X(bYCbCrFilter)				\
	X(lumaScale)				\
	X(lumaOffset)				\
	X(cbScale)				\
	X(cbOffset)				\
	X(crScale)				\
	X(crOffset)				\
	X(rgb1Mult)				\
	X(rgb2Mult)				\
	X(envMapSize)			\
	X(envMapUseLODs)			\
	X(envMapFarClipMult)

struct SkyGfxMenu
{
#define X(NAME) DebugMenuEntry *NAME;
MENUSETTINGS
#undef X
};
SkyGfxMenu menu;
bool hasMenu = false;

void
refreshMenu(void)
{
	if(hasMenu){
#define X(NAME) DebugMenuEntrySetAddress(menu.NAME, &config->NAME);
MENUSETTINGS
#undef X
	}
}

void
toggledDual(void)
{
	// override, we can't do better
	config->dualPassBuilding = config->dualPassGlobal;
	config->dualPassVehicle = config->dualPassGlobal;
	config->dualPassPed = config->dualPassGlobal;
	config->dualPassGrass = config->dualPassGlobal;
	config->dualPassDefault = config->dualPassGlobal;
}

void
toggledModulation(void)
{
	// override, we can't do better
	config->ps2ModulateBuilding = config->ps2ModulateGlobal;
	config->ps2ModulateGrass = config->ps2ModulateGlobal;
}

void
changeEnvMapSize(void)
{
	int i = 1;
	// increase or decrease by power of two. doesn't work for 4 or below
	if(config->envMapSize+1 & config->envMapSize)
		while(i < config->envMapSize) i *= 2;
	else
		while(i < config->envMapSize/2) i *= 2;
	config->envMapSize = i;
}

// should we need to check this out again
//float mirrorVal = 216.0f;
//void
//fixMirrors(void)
//{
//	Patch(0x726516 + 6, mirrorVal);
//	Patch(0x726534 + 6, mirrorVal);
//	Patch(0x726552 + 6, mirrorVal);
//	Patch(0x726570 + 6, mirrorVal);
//}

/*
WRAPPER void CTimeCycle__Initialise(bool) { EAXJMP(0x5BBAC0); }

void
LoadTimecycle(const char *filename)
{
	static char timecyc[256];
	strncpy(timecyc, filename, 256);
	Patch(0x5BBAD9 + 1, timecyc);
	CTimeCycle__Initialise(false);
}
*/

void
installMenu(void)
{
	DebugMenuEntry *e;
	if(DebugMenuLoad()){
		static const char *ps2pcStr[] = { "PS2", "PC" };
		static const char *buildPipeStr[] = { "PS2", "Xbox", "Mobile" };
		static const char *vehPipeStr[] = { "PS2", "PC", "Xbox", "Spec", "Mobile", "Neo", "LCS", "VCS", "Env" };
		static const char *colFilterStr[] = { "None", "PS2", "PC", "Mobile", "III", "VC", "VCS" };
		static const char *lightningStr[] = { "Sky only", "Sky and objects" };
		static const char *shadStr[] = { "Default", "PS2", "PC" };
		static const char *radStr[] = { "PS2", "Shader" };
		static const char *coronaStr[] = { "-", "default (PS2)", "Force (PC)" };
		e = DebugMenuAddVar("SkyGFX", "Config", &currentConfig, setConfig, 1, 0, numConfigs-1, nil);
		DebugMenuEntrySetWrap(e, true);
		DebugMenuAddCmd("SkyGFX", "Reload Inis", reloadAllInis);

		menu.dualPassGlobal = DebugMenuAddVarBool32("SkyGFX", "Dual-pass Global", &config->dualPassGlobal, toggledDual);
		menu.ps2ModulateGlobal = DebugMenuAddVarBool32("SkyGFX", "PS2-modulate Global", &config->ps2ModulateGlobal, toggledModulation);
		if(iCanHasbuildingPipe){
			menu.buildingPipe = DebugMenuAddVar("SkyGFX", "Building Pipeline", &config->buildingPipe, nil, 1, BUILDING_PS2, NUMBUILDINGPIPES-1, buildPipeStr);
			DebugMenuEntrySetWrap(menu.buildingPipe, true);
			menu.tagsBuildingPipe = DebugMenuAddVar("SkyGFX", "Tags Building Pipeline", &config->tagsBuildingPipe, nil, 1, BUILDING_PS2, NUMBUILDINGPIPES - 1, buildPipeStr);
			DebugMenuEntrySetWrap(menu.tagsBuildingPipe, true);
			menu.detailMaps = DebugMenuAddVarBool32("SkyGFX", "Detail Maps", &config->detailMaps, nil);
			menu.stochastic = DebugMenuAddVarBool32("SkyGFX", "Stochastic Texturing", &config->stochastic, nil);
		}
		if(iCanHasvehiclePipe){
			menu.vehiclePipe = DebugMenuAddVar("SkyGFX", "Vehicle Pipeline", &config->vehiclePipe, nil, 1, CAR_PS2, NUMCARPIPES-1, vehPipeStr);
			DebugMenuEntrySetWrap(menu.vehiclePipe, true);
		}
		menu.envMapSize = DebugMenuAddVar("SkyGFX", "Vehicle Env Map Size", &config->envMapSize, changeEnvMapSize, 1, 4, 2048, nil);
		menu.envMapFarClipMult = DebugMenuAddVar("SkyGFX|Misc", "Vehicle Env Map Far Clip Mult", &config->envMapFarClipMult, nil, 0.1f, 0.0f, 10.0f);
		//menu.envMapUseLODs = DebugMenuAddVarBool32("SkyGFX", "Vehicle Env Map Use LODs", &config->envMapUseLODs, nil);
		menu.grassAddAmbient = DebugMenuAddVarBool32("SkyGFX", "Add Ambient to Grass", &config->grassAddAmbient, nil);
		menu.backfaceCull = DebugMenuAddVarBool32("SkyGFX", "Grass Backface Culling", &config->backfaceCull, nil);
		menu.pedShadows = DebugMenuAddVar("SkyGFX", "Ped Shadows", &config->pedShadows, nil, 1, -1, 1, shadStr);
		DebugMenuEntrySetWrap(menu.pedShadows, true);
		menu.stencilShadows = DebugMenuAddVar("SkyGFX", "Stencil Shadows", &config->stencilShadows, nil, 1, -1, 1, shadStr);
		DebugMenuEntrySetWrap(menu.stencilShadows, true);
		// TODO: allow III/VC somehow?
		menu.colorFilter = DebugMenuAddVar("SkyGFX", "Colour filter", &config->colorFilter, resetValues, 1, COLORFILTER_NONE, COLORFILTER_MOBILE, colFilterStr);
		DebugMenuEntrySetWrap(menu.colorFilter, true);
		menu.doRadiosity = DebugMenuAddVarBool32("SkyGFX", "Radiosity", &config->doRadiosity, resetValues);
		menu.radiosity = DebugMenuAddVar("SkyGFX", "Radiosity type", &config->radiosity, nil, 1, 0, 1, radStr);
		DebugMenuEntrySetWrap(menu.radiosity, true);
		if(iCanHasNeoDrops){
			menu.neoWaterDrops = DebugMenuAddVarBool32("SkyGFX", "Neo Water drops", &config->neoWaterDrops, nil);
			menu.neoBloodDrops = DebugMenuAddVarBool32("SkyGFX", "Neo-style Blood drops", &config->neoBloodDrops, nil);
#ifdef DEBUG
			DebugMenuAddVarBool8("SkyGFX", "Spray Water drops", (int8*)&WaterDrops::sprayWater, nil);
			DebugMenuAddVarBool8("SkyGFX", "Spray Blood drops", (int8*)&WaterDrops::sprayBlood, nil);
#endif
		}

		DebugMenuAddVarBool8("SkyGFX|Misc", "Blur PS2 Colour Filter", (int8_t*)&CPostEffects::m_bBlurColourFilter, nil);
		if(iCanHasSunGlare)
			menu.doglare = DebugMenuAddVarBool32("SkyGFX|Misc", "Sun Glare", &config->doglare, nil);
		menu.leedsShininessMult = DebugMenuAddVar("SkyGFX|Misc", "Leeds Car Shininess", &config->leedsShininessMult, nil, 0.1f, 0.0f, 10.0f);
		menu.neoShininessMult = DebugMenuAddVar("SkyGFX|Misc", "Neo Car Shininess", &config->neoShininessMult, nil, 0.1f, 0.0f, 10.0f);
		menu.neoSpecularityMult = DebugMenuAddVar("SkyGFX|Misc", "Neo Car Specularity", &config->neoSpecularityMult, nil, 0.1f, 0.0f, 10.0f);
		menu.envShininessMult = DebugMenuAddVar("SkyGFX|Misc", "Env Car Shininess", &config->envShininessMult, nil, 0.1f, 0.0f, 10.0f);
		menu.envSpecularityMult = DebugMenuAddVar("SkyGFX|Misc", "Env Car Specularity", &config->envSpecularityMult, nil, 0.1f, 0.0f, 10.0f);
		menu.envPower = DebugMenuAddVar("SkyGFX|Misc", "Env Car Power", &config->envPower, nil, 1.0f, 0.0f, 2000.0f);
		menu.envFresnel = DebugMenuAddVar("SkyGFX|Misc", "Env Car Fresnel", &config->envFresnel, nil, 0.1f, 0.0f, 10.0f);
		menu.fixGrassPlacement = DebugMenuAddVarBool32("SkyGFX|Misc", "Fix Grass Placement", &config->fixGrassPlacement, nil);
		menu.lightningIlluminatesWorld = DebugMenuAddVar("SkyGFX|Misc", "Lightning illuminates", &config->lightningIlluminatesWorld, nil, 1, 0, 1, lightningStr);
		DebugMenuEntrySetWrap(menu.lightningIlluminatesWorld, true);
		menu.coronaZtest = DebugMenuAddVar("SkyGFX|Misc", "Corona Z test", &config->coronaZtest, resetValues, 1, -1, 1, coronaStr);
		DebugMenuEntrySetWrap(menu.coronaZtest, true);

		menu.dualPassDefault = DebugMenuAddVarBool32("SkyGFX|Advanced", "Dual-pass Default", &config->dualPassDefault, nil);
		menu.dualPassBuilding = DebugMenuAddVarBool32("SkyGFX|Advanced", "Dual-pass Buildings", &config->dualPassBuilding, nil);
		menu.dualPassVehicle = DebugMenuAddVarBool32("SkyGFX|Advanced", "Dual-pass Vehicles", &config->dualPassVehicle, nil);
		menu.dualPassPed = DebugMenuAddVarBool32("SkyGFX|Advanced", "Dual-pass Peds", &config->dualPassPed, nil);
		menu.dualPassGrass = DebugMenuAddVarBool32("SkyGFX|Advanced", "Dual-pass Grass", &config->dualPassGrass, nil);
		menu.zwriteThreshold = DebugMenuAddVar("SkyGFX|Advanced", "Dual-pass Alpha Threshold", &config->zwriteThreshold, nil, 1, 0, 255, nil);
		menu.zwriteThresholdGrass = DebugMenuAddVar("SkyGFX|Advanced", "Dual-pass Alpha Grass Threshold", &config->zwriteThresholdGrass, nil, 1, 0, 255, nil);
		menu.zwriteThresholdPed = DebugMenuAddVar("SkyGFX|Advanced", "Dual-pass Alpha Ped Threshold", &config->zwriteThresholdPed, nil, 1, 0, 255, nil);
		menu.ps2ModulateBuilding = DebugMenuAddVarBool32("SkyGFX|Advanced", "PS2-modulate Buildings", &config->ps2ModulateBuilding, nil);
		menu.ps2ModulateGrass = DebugMenuAddVarBool32("SkyGFX|Advanced", "PS2-modulate Grass", &config->ps2ModulateGrass, nil);
		menu.infraredVision = DebugMenuAddVar("SkyGFX|Advanced", "Infrared vision", &config->infraredVision, nil, 1, 0, 1, ps2pcStr);
		DebugMenuEntrySetWrap(menu.infraredVision, true);
		menu.nightVision = DebugMenuAddVar("SkyGFX|Advanced", "Night vision", &config->nightVision, nil, 1, 0, 1, ps2pcStr);
		DebugMenuEntrySetWrap(menu.nightVision, true);
		menu.grainFilter = DebugMenuAddVar("SkyGFX|Advanced", "Grain filter", &config->grainFilter, resetValues, 1, 0, 1, ps2pcStr);
		DebugMenuEntrySetWrap(menu.grainFilter, true);

		menu.rgb1Mult = DebugMenuAddVar("SkyGFX|Advanced", "RGB1 Mult", &config->rgb1Mult, resetValues, 1.0f, 0.0f, 10.0f);
		menu.rgb2Mult = DebugMenuAddVar("SkyGFX|Advanced", "RGB2 Mult", &config->rgb2Mult, resetValues, 1.0f, 0.0f, 10.0f);

		menu.bYCbCrFilter = DebugMenuAddVarBool8("SkyGFX|ScreenFX", "Enable YCbCr tweak", (int8_t*)&config->bYCbCrFilter, resetValues);
		menu.lumaScale    = DebugMenuAddVar("SkyGFX|ScreenFX", "Y scale", &config->lumaScale, resetValues, 0.004f, 0.0f, 10.0f);
		menu.lumaOffset   = DebugMenuAddVar("SkyGFX|ScreenFX", "Y offset", &config->lumaOffset, resetValues, 0.004f, -1.0f, 1.0f);
		menu.cbScale      = DebugMenuAddVar("SkyGFX|ScreenFX", "Cb scale", &config->cbScale, resetValues, 0.004f, 0.0f, 10.0f);
		menu.cbOffset     = DebugMenuAddVar("SkyGFX|ScreenFX", "Cb offset", &config->cbOffset, resetValues, 0.004f, -1.0f, 1.0f);
		menu.crScale      = DebugMenuAddVar("SkyGFX|ScreenFX", "Cr scale", &config->crScale, resetValues, 0.004f, 0.0f, 10.0f);
		menu.crOffset     = DebugMenuAddVar("SkyGFX|ScreenFX", "Cr offset", &config->crOffset, resetValues, 0.004f, -1.0f, 1.0f);

		menu.crOffset     = DebugMenuAddVar("SkyGFX|ScreenFX", "Cr offset", &config->crOffset, resetValues, 0.004f, -1.0f, 1.0f);

/*
		DebugMenuAddVarBool32("SkyGFX", "Timecycle usePC", &config->usePCTimecyc, nil);
		DebugMenuAddCmd("SkyGFX", "Timecycle PostFX Alpha *1", [](){
				Nop(0x5BBF6F, 2);
				Nop(0x5BBF83, 2);
			});
		DebugMenuAddCmd("SkyGFX", "Timecycle PostFX Alpha *2", [](){
				Patch<uint16>(0x5BBF6F, 0xC0DC);
				Patch<uint16>(0x5BBF83, 0xC0DC);
			});
		DebugMenuAddCmd("SkyGFX", "Load PS2 timecyc.dat", [](){ LoadTimecycle("timecyc.dat"); });
		DebugMenuAddCmd("SkyGFX", "Load PS2 timecyc_pc.dat", [](){ LoadTimecycle("timecyc_pc.dat"); });
*/

//#ifdef DEBUG
//		DebugMenuAddVar("Debug", "Mirror Z", &mirrorVal, fixMirrors, 0.05f, 200.0f, 250.0f);
//#endif

		hasMenu = true;
		//void privatepatches(void);
		//privatepatches();
	}
}

static int (*IsAlreadyRunning)();
int
InjectDelayedPatches()
{
	if(IsAlreadyRunning())
		return TRUE;

	// post init stuff
	findInis();
	if(numConfigs == 0)
		readIni(0);
	else
		readIni(1);
	// only load one ini for now, others are loaded later by readInis()

	fixingSAMP = ModuleList().Get(L"samp") || ModuleList().Get(L"SAMPGraphicRestore");
	UG_mod = ModuleList().Get(L"Underground_Core");
	if(UG_mod)
		UG_RegisterEventCallback = (void (*)(const char*, UG_EventHook))GetProcAddress(UG_mod, "RegisterEventCallback");

	if(UG_RegisterEventCallback)
		UG_RegisterEventCallback("EVENT_INITPOSTEFFECTS", CPostEffects::Initialise_skygfx);
	else
		InterceptCall(&CPostEffects::Initialise_orig, CPostEffects::Initialise, 0x5BD779);
	InterceptCall(&InitialiseGame, InitialiseGame_hook, 0x748CFB);

	// Stop timecycle from converting colour filter alphas
	Nop(0x5BBF6F, 2);
	Nop(0x5BBF83, 2);

	// don't assign building pipe just because a model has two sets of prelight
	// or when it already has a pipeline
	explicitBuildingPipe = explicitBuildingPipe_tmp;

	// custom building pipeline
	if(iCanHasbuildingPipe)
		hookBuildingPipe();

	// custom vehicle pipeline
	if(iCanHasvehiclePipe)
		hookVehiclePipe();

// fuck this, can't be fixed so easily anyway
//	if(ps2grassFiles){
//		Patch<const char*>(0x5DDA87, "grass2_1.dff");
//		Patch<const char*>(0x5DDA8F, "grass2_2.dff");
//		Patch<const char*>(0x5DDA97, "grass2_3.dff");
//		Patch<const char*>(0x5DDA9F, "grass2_4.dff");
//
//		Patch<const char*>(0x5DDAC3, "grass3_1.dff");
//		Patch<const char*>(0x5DDACB, "grass3_2.dff");
//		Patch<const char*>(0x5DDAD3, "grass3_3.dff");
//		Patch<const char*>(0x5DDADB, "grass3_4.dff");
//		Patch<uint>(0x5DDB14 + 1, 0xC03A30+4);
//		Patch<uint>(0x5DDB21 + 2, 0xC03A30+8);
//		Patch<uint>(0x5DDB2F + 2, 0xC03A30+12);
//	}

	// use static ped shadows
	InjectHook(0x5E675E, &FX::GetFxQuality_ped);
	InjectHook(0x5E676D, &FX::GetFxQuality_ped);
	InjectHook(0x706BC4, &FX::GetFxQuality_ped);
	InjectHook(0x706BD3, &FX::GetFxQuality_ped);
	// stencil???
	InjectHook(0x7113B8, &FX::GetFxQuality_stencil);
	InjectHook(0x711D95, &FX::GetFxQuality_stencil);
	// vehicle, pole
	InjectHook(0x70F9B8, &FX::GetFxQuality_stencil);

	if(fixPcCarLight){
		// carenv light diffuse
		Patch<uint>(0x5D88D1 +6, 0);
		Patch<uint>(0x5D88DB +6, 0);
		Patch<uint>(0x5D88E5 +6, 0);
		// carenv light ambient
		Patch<uint>(0x5D88F9 +6, 0);
		Patch<uint>(0x5D8903 +6, 0);
		Patch<uint>(0x5D890D +6, 0);
		// use local viewer for spec light
		// ...or not... (PS2 doesn't)
		// Patch<uchar>(0x5D9AD0 +1, 1);
	}

	if(disableClouds)
		// jump over cloud loop
		InjectHook(0x714145, 0x71422A, PATCH_JUMP);

	if(disableGamma)
		InjectHook(0x74721C, 0x7472F3, PATCH_JUMP);

	if(iCanHasNeoDrops)
		hookWaterDrops();

	// sun glare on cars
	if(iCanHasSunGlare)
		InjectHook(0x6ABCFD, doglare, PATCH_JUMP);

	// remove black background of lockon siphon
	if(transparentLockon > 0){
		InjectHook(0x742E33, 0x742EC1, PATCH_JUMP);
		InjectHook(0x742FE0, 0x743085, PATCH_JUMP);
	}


	if(fixShadows){
		// Remove 0.06 z-offset from shadows, PS2 doesn't do it
		static float shadowoffset = 0.0f;
		Patch(0x709B2D + 2, &shadowoffset);
		Patch(0x709B8C + 2, &shadowoffset);
		Patch(0x709BC5 + 2, &shadowoffset);
		Patch(0x709BF4 + 2, &shadowoffset);
		Patch(0x709C91 + 2, &shadowoffset);

		Patch(0x709E9C + 2, &shadowoffset);
		Patch(0x709EBA + 2, &shadowoffset);
		Patch(0x709ED5 + 2, &shadowoffset);

		Patch(0x70B21F + 2, &shadowoffset);
		Patch(0x70B371 + 2, &shadowoffset);
		Patch(0x70B4CF + 2, &shadowoffset);
		Patch(0x70B633 + 2, &shadowoffset);

		Patch(0x7085A7 + 2, &shadowoffset);

		// Change z-hack multiplier from 2.0 to 256.0 as on PS2
		*(float*)0x8CD4F0 = 256.0f;
	}

	if(privateHooks){
		// PS2 splash
		static const char *loadsc0 = "loadsc0";
		Patch(0x5901BD + 1, loadsc0);

		// nvidia is not the way it's meant to be played
		Nop(0x748AA8, 0x748AE7-0x748AA8);

	//	// lens flare for police cars
	//	Patch<uint8>(0x6AB9E6 + 1, 2);
	//	Patch<uint8>(0x6ABA33 + 1, 2);
	//	// and change distance to 50 (150*LODdist normally)
	//	static const float one = 1.0f;
	//	Patch(0x6AB9BE + 2, &one);
	//	Patch(0x6ABA0B + 2, &one);
	//	static const float flaredist = 50.0f;
	//	Patch(0x6AB9C6 + 2, &flaredist);
	//	Patch(0x6ABA13 + 2, &flaredist);
	}

	// test water shader
	//InterceptCall(&StartWaterRender, StartWaterRender_hook, 0x6EF664);
	//InterceptCall(&EndWaterRender, EndWaterRender_hook, 0x6F00BE);

	/*InterceptCall(&CWaterLevel__RenderAndEmptyRenderBuffer, CWaterLevel__RenderAndEmptyRenderBuffer_hook, 0x6E8790);
	InterceptCall(&CWaterLevel__RenderAndEmptyRenderBuffer, CWaterLevel__RenderAndEmptyRenderBuffer_hook, 0x6E8EF1);
	InterceptCall(&CWaterLevel__RenderAndEmptyRenderBuffer, CWaterLevel__RenderAndEmptyRenderBuffer_hook, 0x6E91E4);
	InterceptCall(&CWaterLevel__RenderAndEmptyRenderBuffer, CWaterLevel__RenderAndEmptyRenderBuffer_hook, 0x6E9963);*/

	installMenu();

	return FALSE;
}

BOOL WINAPI
DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
	if(reason == DLL_PROCESS_ATTACH){
		// TODO: is this correct?
		if(*(DWORD*)DynBaseAddress(0x82457C) != 0x94BF &&
		   *(DWORD*)DynBaseAddress(0x8245BC) == 0x94BF)
			return FALSE;
		dllModule = hInst;

		if(GetAsyncKeyState(VK_F8) & 0x8000){
			AllocConsole();
			freopen("CONIN$", "r", stdin);
			freopen("CONOUT$", "w", stdout);
			freopen("CONOUT$", "w", stderr);
		}

		for(int i = 0; i < 10; i++)
			configs[i].version = VERSION;

		/* Fix order of multiplication */
		InjectHook(0x7646E0, _rwD3D9VSGetComposedTransformMatrix, PATCH_JUMP);

		defaultColourLeftUOffset = CPostEffects::m_colourLeftUOffset;
		defaultColourRightUOffset = CPostEffects::m_colourRightUOffset;
		defaultColourTopVOffset = CPostEffects::m_colourTopVOffset;
		defaultColourBottomVOffset = CPostEffects::m_colourBottomVOffset;

		// windowed - not working yet
//		Nop(0x7462FF, 2);
//		Nop(0x745B55, 2);
///		InjectHook(0x74639B, selectVM, PATCH_JUMP);

		// moon mask
		InjectHook(0x713C4C, renderMoonMask, PATCH_JUMP);

		IsAlreadyRunning = (int(*)())(*(int*)(0x74872D+1) + 0x74872D + 5);
		InjectHook(0x74872D, InjectDelayedPatches);

		InjectHook(0x5BCF14, afterStreamIni, PATCH_JUMP);

		InjectHook(0x7491C0, myDefaultCallback, PATCH_JUMP);

		InjectHook(0x5BF8EA, CPlantMgr_Initialise);
		InjectHook(0x756DFE, rxD3D9DefaultRenderCallback_Hook, PATCH_JUMP);
		//InjectHook(0x7572CC, rxD3D9DefaultRenderCallback_VertexShaderHook, PATCH_JUMP);
		InjectHook(0x5DADB7, fixSeed, PATCH_JUMP);
		InjectHook(0x5DAE61, saveIntensity, PATCH_JUMP);
		Patch(0x5DAEC8, setTextureAndColor);

		// add dual pass for PC pipeline
		InjectHook(0x5D9EEB, D3D9RenderDefault_DUAL);
		InjectHook(0x5D9EFB, D3D9RenderBlack_DUAL);

		// give vehicle pipe to upgrade parts
		InjectHook(0x4C88F0, 0x5DA610, PATCH_JUMP);

		// fix roadsign alpha
		InjectHook(0x6FED44, CreateRoadsignTexture_RwTextureSetName);
		InjectHook(0x6FF236, CreateRoadsignAtomicA_RpAtomicSetGeometry);

		// jump over code that sets alpha ref to 140 (not on PS2).
		// This caused skidmarks to disappear when rendering the neo reflection scene
		InjectHook(0x553AD1, 0x553AE5, PATCH_JUMP);
		// ... was not enough. disable alpha test for skidmarks
		InterceptCall(&CSkidmarks__Render_orig, CSkidmarks__Render, 0x53E175);

		/* Don't change tag material. Instead handle it by special code in the render CB */
		InterceptCall(&CTagManager__RenderTagForPC, CTagManager__RenderTag, 0x534335);
		InterceptCall(&CTagManager__SetupAtomic_orig, CTagManager__SetupAtomic, 0x4C4412);
		*(void**)0xA9AD78 = (void*)TagRenderCB;	/* This is the (unused) material pipeline of player tags */

		// postfx
		InjectHook(0x704D1E, CPostEffects::ColourFilter_switch);
		InjectHook(0x704D5D, CPostEffects::Radiosity);
		InjectHook(0x704FB3, CPostEffects::Radiosity);
		InjectHook(0x704D48, CPostEffects::DarknessFilter_fix);

		// infrared vision
		InjectHook(0x704F4B, CPostEffects::InfraredVision_PS2);
		InjectHook(0x704F59, CPostEffects::Grain_PS2);
		// night vision
		InjectHook(0x704EDA, CPostEffects::NightVision_PS2);
		InjectHook(0x704EE8, CPostEffects::Grain_PS2);
		// rain
		InjectHook(0x705078, CPostEffects::Grain_PS2);
		// unused
		InjectHook(0x705091, CPostEffects::Grain_PS2);

		InjectHook(0x53EBE9, CPostEffects::DrawFinalEffects);

		// fix pointlight fog
		InjectHook(0x700B6B, CSprite__RenderBufferedOneXLUSprite_Rotate_Aspect);

		InjectHook(0x44E82E, ps2rand);
		InjectHook(0x44ECEE, ps2rand);
		InjectHook(0x42453B, ps2rand);
		InjectHook(0x42454D, ps2rand);

		///
		InterceptCall(&PipelinePluginAttach, myPluginAttach, 0x53D903);
	//	InjectHook(0x53D903, myPluginAttach);

		// procobj placement. Not really broken but whatever
		InjectHook(0x5A3C7D, ps2srand);
		InjectHook(0x5A3DFB, ps2srand);
		InjectHook(0x5A3C75, ps2rand);
		InjectHook(0x5A3CB9, ps2rand);
		InjectHook(0x5A3CDB, ps2rand);
		InjectHook(0x5A3CF2, ps2rand);
		Patch(0x5A3CC8, &ps2randnormalize);
		Patch(0x5A3CEA, &ps2randnormalize);
		Patch(0x5A3D05, &ps2randnormalize);
		// a few more procobjs
		InjectHook(0x5A3476, ps2rand);
		InjectHook(0x5A34AB, ps2rand);
		InjectHook(0x5A34E0, ps2rand);
		InjectHook(0x5A3515, ps2rand);
		Patch(0x5A348D + 2, &ps2randnormalize);
		Patch(0x5A34C2 + 2, &ps2randnormalize);
		Patch(0x5A34FB + 2, &ps2randnormalize);
		Patch(0x5A352F + 2, &ps2randnormalize);

//		InjectHook(0x5A3C6E, floatbitpattern);

		// increase multipass distance
		static float multipassMultiplier = 1000.0f;	// default 45.0
		Patch<float*>(0x73290A+2, &multipassMultiplier);

		// increase env mat pool
		// better do it in a limit adjuster
		//Patch(0x5DA08D + 1, 0x4000);

		// Get rid of the annoying dotproduct check in visibility renderCBs
		Nop(0x733313, 2);	// VehicleHiDetailCB
		Nop(0x73405A, 2);	// VehicleHiDetailAlphaCB
		Nop(0x733403, 2);	// TrainHiDetailCB
		Nop(0x73431A, 2);	// TrainHiDetailAlphaCB
		Nop(0x73444A, 2);	// VehicleHiDetailAlphaCB_BigVehicle

		// change grass close far to ps2 values...but they appear to be handled differently?
		Patch<float>(0x5DDB3D+1, 78.0f);
//		Patch<float>(0x5DDB42+1, 5.0f);	// this is too high, grass disappears o_O

		// High detail water color multiplier is multiplied by 0.65 and added to 0.27, why?
		// Removing this silly calculation seems to work better.
		Nop(0x6E716B, 6);
		Nop(0x6E7176, 6);

		// Hook Skpin Pipe so we can easily cull peds
		// TODO: do this with other pipelines too
		// No longer needed
//		__rwSkinD3D9AtomicAllInOneNode_orig = nodeD3D9SkinAtomicAllInOneCSL->nodeMethods.nodeBody;
//		nodeD3D9SkinAtomicAllInOneCSL->nodeMethods.nodeBody = __rwSkinD3D9AtomicAllInOneNode_hook;

//		InterceptCall(&SetLightsWithTimeOfDayColour_orig, SetLightsWithTimeOfDayColour, 0x53E997);


		// Camera planes in CRenderer::RenderEverythingBarRoads
		static float zoffset = 0.0f;
		Patch(0x553C7D + 2, &zoffset);
		Nop(0x553C78, 5);
		Nop(0x553C9A, 5);	// begin update
		Nop(0x553CD1, 5);
		Nop(0x553CEC, 5);	// begin update
		//Nop(0x553CB8, 5);	// render LODs

		// Fix mirrors
		Patch(0x726516 + 6, 216.1f);
		Patch(0x726534 + 6, 216.1f);
		Patch(0x726552 + 6, 216.1f);
		Patch(0x726570 + 6, 216.1f);

void hooktexdb();
		hooktexdb();


		//void dumpMenu(void);
		//dumpMenu();
		
		//InjectHook(0x5A3C7D, mysrand);
		//InjectHook(0x5A3DFB, mysrand);
		//InjectHook(0x5A3C75, ps2rand);

//		Nop(0x748054, 10);
///		Nop(0x748063, 5);
//		Nop(0x747FB0, 10);
//		Nop(0x748A87, 12);
//		InjectHook(0x747F98, 0x748446, PATCH_JUMP);



		// Water tests
//		Nop(0x6EFC9B, 5);	// disable CWaterLevel::RenderWaterTriangle
		//Nop(0x6EFE4C, 5);	// disable CWaterLevel::RenderWaterRectangle
//		Nop(0x6F004E, 5);	// disable CWaterLevel::RenderWaterRectangle outside
	//	Nop(0x6ECED8, 5);	// disable CWaterLevel::RenderFlatWaterRectangle
	//	Nop(0x6ECE15, 5);	// disable CWaterLevel::RenderFlatWaterRectangle
	//	Nop(0x6ECD69, 5);	// disable CWaterLevel::RenderHighDetailWaterRectangle

	//	Nop(0x6EC509, 5);	// disable CWaterLevel::RenderFlatWaterRectangle_OneLayer
//		Nop(0x6EC5B8, 5);	// disable CWaterLevel::RenderFlatWaterRectangle_OneLayer
	//	Nop(0x6EBA1D, 5);	// disable CWaterLevel::RenderHighDetailWaterRectangle_OneLayer
//		Nop(0x6EBAF8, 5);	// disable CWaterLevel::RenderHighDetailWaterRectangle_OneLayer

	//	InjectHook(0x6E9760, CWaterLevel__CalculateWavesForCoordinate_hook);
	//	InjectHook(0x6E8CB8, CWaterLevel__CalculateWavesForCoordinate_hook);

		// Cloud tests
		//Nop(0x53E1AF, 5);	// CClouds::MovingFogRender
		//Nop(0x53E1B4, 5);	// CClouds::VolumetricCloudsRender
		//Nop(0x53E121, 5);	// CClouds::RenderBottomFromHeight

		// remove some shadows for mobile test
		//Nop(0x53E0C3, 5);
		//Nop(0x53E0C8, 5);
	}

	return TRUE;
}
