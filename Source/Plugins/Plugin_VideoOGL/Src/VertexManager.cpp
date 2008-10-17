#include "Globals.h"

#include <fstream>

#include "MemoryUtil.h"
#include "Profiler.h"
#include "Render.h"
#include "ImageWrite.h"
#include "BPMemory.h"
#include "TextureMngr.h"
#include "PixelShaderManager.h"
#include "VertexShaderManager.h"
#include "VertexLoader.h"
#include "VertexManager.h"

#define MAX_BUFFER_SIZE 0x4000

static GLuint s_vboBuffers[0x40] = {0};
static int s_nCurVBOIndex = 0; // current free buffer
static u8 *s_pBaseBufferPointer = NULL;
static vector< pair<int, int> > s_vStoredPrimitives; // every element, mode and count to be passed to glDrawArrays
static u32 s_prevcomponents; // previous state set

u8* VertexManager::s_pCurBufferPointer = NULL;

const GLenum c_primitiveType[8] =
{
    GL_QUADS,
    0, //nothing
    GL_TRIANGLES,
    GL_TRIANGLE_STRIP,
    GL_TRIANGLE_FAN,
    GL_LINES,
    GL_LINE_STRIP,
    GL_POINTS
};

// internal state for loading vertices
void (*fnSetupVertexPointers)() = NULL;

bool VertexManager::Init()
{
	Destroy();

	s_GlobalVtxDesc.Hex = 0;
	s_prevcomponents = 0;
	s_pBaseBufferPointer = (u8*)AllocateMemoryPages(MAX_BUFFER_SIZE);
	s_pCurBufferPointer = s_pBaseBufferPointer;

	s_nCurVBOIndex = 0;
	glGenBuffers(ARRAYSIZE(s_vboBuffers), s_vboBuffers);
	for (u32 i = 0; i < ARRAYSIZE(s_vboBuffers); ++i) {
		glBindBuffer(GL_ARRAY_BUFFER, s_vboBuffers[i]);
		glBufferData(GL_ARRAY_BUFFER, MAX_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
	}

	glEnableClientState(GL_VERTEX_ARRAY);
	fnSetupVertexPointers = NULL;
	GL_REPORT_ERRORD();

	return true;
}

void VertexManager::Destroy()
{
	FreeMemoryPages(s_pBaseBufferPointer, MAX_BUFFER_SIZE); s_pBaseBufferPointer = s_pCurBufferPointer = NULL;
	glDeleteBuffers(ARRAYSIZE(s_vboBuffers), s_vboBuffers);
	memset(s_vboBuffers, 0, sizeof(s_vboBuffers));

	s_vStoredPrimitives.resize(0);
	s_nCurVBOIndex = 0;
	ResetBuffer();
}

void VertexManager::ResetBuffer()
{
	s_nCurVBOIndex = (s_nCurVBOIndex + 1) % ARRAYSIZE(s_vboBuffers);
	s_pCurBufferPointer = s_pBaseBufferPointer;
	s_vStoredPrimitives.resize(0);
}

void VertexManager::ResetComponents()
{
	s_prevcomponents = 0;
	glDisableVertexAttribArray(SHADER_POSMTX_ATTRIB);
	glDisableClientState(GL_NORMAL_ARRAY);
	glDisableVertexAttribArray(SHADER_NORM1_ATTRIB);
	glDisableVertexAttribArray(SHADER_NORM2_ATTRIB);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_SECONDARY_COLOR_ARRAY);
	for (int i = 0; i < 8; i++)
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

int VertexManager::GetRemainingSize()
{
	return MAX_BUFFER_SIZE - (int)(s_pCurBufferPointer - s_pBaseBufferPointer);
}

void VertexManager::AddVertices(int primitive, int numvertices)
{
	_assert_( numvertices > 0 );

	ADDSTAT(stats.thisFrame.numPrims, numvertices);
	s_vStoredPrimitives.push_back(pair<int, int>(c_primitiveType[primitive], numvertices));

#ifdef _DEBUG
	static const char *sprims[8] = {"quads", "nothing", "tris", "tstrip", "tfan", "lines", "lstrip", "points"};
	PRIM_LOG("prim: %s, c=%d\n", sprims[primitive], numvertices);
#endif
}

void VertexManager::Flush()
{
	if (s_vStoredPrimitives.size() == 0)
		return;

	_assert_(fnSetupVertexPointers != NULL);
	_assert_(s_pCurBufferPointer != s_pBaseBufferPointer);

#ifdef _DEBUG
	PRIM_LOG("frame%d:\ncomps=0x%x, texgen=%d, numchan=%d, dualtex=%d, ztex=%d, proj=%d, cole=%d, alpe=%d, ze=%d\n", g_Config.iSaveTargetId, s_prevcomponents, xfregs.numTexGens,
		xfregs.nNumChans, (int)xfregs.bEnableDualTexTransform, bpmem.ztex2.op, VertexShaderMngr::rawProjection[6]==0,
		bpmem.blendmode.colorupdate, bpmem.blendmode.alphaupdate, bpmem.zmode.updateenable);
	for (int i = 0; i < xfregs.nNumChans; ++i) {
		LitChannel* ch = &xfregs.colChans[i].color;
		PRIM_LOG("colchan%d: matsrc=%d, light=0x%x, ambsrc=%d, diffunc=%d, attfunc=%d\n", i, ch->matsource, ch->GetFullLightMask(), ch->ambsource, ch->diffusefunc, ch->attnfunc);
		ch = &xfregs.colChans[i].alpha;
		PRIM_LOG("alpchan%d: matsrc=%d, light=0x%x, ambsrc=%d, diffunc=%d, attfunc=%d\n", i, ch->matsource, ch->GetFullLightMask(), ch->ambsource, ch->diffusefunc, ch->attnfunc);
	}

	for (int i = 0; i < xfregs.numTexGens; ++i) {
		TexMtxInfo tinfo = xfregs.texcoords[i].texmtxinfo;
		if (tinfo.texgentype != XF_TEXGEN_EMBOSS_MAP ) tinfo.hex &= 0x7ff;
		if (tinfo.texgentype != XF_TEXGEN_REGULAR ) tinfo.projection = 0;

		PRIM_LOG("txgen%d: proj=%d, input=%d, gentype=%d, srcrow=%d, embsrc=%d, emblght=%d, postmtx=%d, postnorm=%d\n",
			i, tinfo.projection, tinfo.inputform, tinfo.texgentype, tinfo.sourcerow, tinfo.embosssourceshift, tinfo.embosslightshift,
			xfregs.texcoords[i].postmtxinfo.index, xfregs.texcoords[i].postmtxinfo.normalize);
	}

	PRIM_LOG("pixel: tev=%d, ind=%d, texgen=%d, dstalpha=%d, alphafunc=0x%x\n", bpmem.genMode.numtevstages+1, bpmem.genMode.numindstages,
		bpmem.genMode.numtexgens, (u32)bpmem.dstalpha.enable, (bpmem.alphaFunc.hex>>16)&0xff);
#endif

	DVSTARTPROFILE();

	GL_REPORT_ERRORD();

	glBindBuffer(GL_ARRAY_BUFFER, s_vboBuffers[s_nCurVBOIndex]);
	glBufferData(GL_ARRAY_BUFFER, s_pCurBufferPointer-s_pBaseBufferPointer, s_pBaseBufferPointer, GL_STREAM_DRAW);
	GL_REPORT_ERRORD();

	// setup the pointers
	fnSetupVertexPointers();
	GL_REPORT_ERRORD();

	// set the textures
	{
		DVProfileFunc _pf("VertexManager::Flush:textures");
	
		u32 usedtextures = 0;
		for (u32 i = 0; i < (u32)bpmem.genMode.numtevstages + 1; ++i) {
			if (bpmem.tevorders[i/2].getEnable(i & 1))
				usedtextures |= 1<<bpmem.tevorders[i/2].getTexMap(i & 1);
		}

		if (bpmem.genMode.numindstages > 0) {
			for (u32 i = 0; i < (u32)bpmem.genMode.numtevstages + 1; ++i) {
				if (bpmem.tevind[i].IsActive() && bpmem.tevind[i].bt < bpmem.genMode.numindstages) {
					usedtextures |= 1<<bpmem.tevindref.getTexMap(bpmem.tevind[i].bt);
				}
			}
		}

		u32 nonpow2tex = 0;
		for (int i = 0; i < 8; i++) {
			if (usedtextures & (1 << i)) {
				glActiveTexture(GL_TEXTURE0+i);
			
				FourTexUnits &tex = bpmem.tex[i>>2];
				TextureMngr::TCacheEntry* tentry = TextureMngr::Load(i, (tex.texImage3[i&3].image_base/* & 0x1FFFFF*/) << 5,
					tex.texImage0[i&3].width+1, tex.texImage0[i&3].height+1,
					tex.texImage0[i&3].format, tex.texTlut[i&3].tmem_offset<<9, tex.texTlut[i&3].tlut_format);

				if (tentry != NULL) {
					// texture loaded fine, set dims for pixel shader
					if (tentry->isNonPow2) {
						PixelShaderMngr::SetTexDims(i, tentry->w, tentry->h, tentry->mode.wrap_s, tentry->mode.wrap_t);
						nonpow2tex |= 1 << i;
						if (tentry->mode.wrap_s > 0) nonpow2tex |= 1 << (8 + i);
						if (tentry->mode.wrap_t > 0) nonpow2tex |= 1 << (16 + i);
						TextureMngr::EnableTexRECT(i);
					}
					// if texture is power of two, set to ones (since don't need scaling)
					else 
					{
						PixelShaderMngr::SetTexDims(i, tentry->w, tentry->h, 0, 0);
						TextureMngr::EnableTex2D(i);
					}
					if (g_Config.iLog & CONF_PRIMLOG) {
						// save the textures
						char strfile[255];
						sprintf(strfile, "frames/tex%.3d_%d.tga", g_Config.iSaveTargetId, i);
						SaveTexture(strfile, tentry->isNonPow2?GL_TEXTURE_RECTANGLE_ARB:GL_TEXTURE_2D, tentry->texture, tentry->w, tentry->h);
					}
				}
				else {
					ERROR_LOG("error loading tex\n");
					TextureMngr::DisableStage(i); // disable since won't be used
				}
			}
			else {
				TextureMngr::DisableStage(i); // disable since won't be used
			}
		}

		PixelShaderMngr::SetTexturesUsed(nonpow2tex);
	}

	FRAGMENTSHADER* ps = PixelShaderMngr::GetShader();
	VERTEXSHADER* vs = VertexShaderMngr::GetShader(s_prevcomponents);
	_assert_(ps != NULL && vs != NULL);

	bool bRestoreBuffers = false;
	if (Renderer::GetZBufferTarget()) {
		if (bpmem.zmode.updateenable) {
			if (!bpmem.blendmode.colorupdate) {
				Renderer::SetRenderMode(bpmem.blendmode.alphaupdate?Renderer::RM_ZBufferAlpha:Renderer::RM_ZBufferOnly);    
			}
		}
		else {
			Renderer::SetRenderMode(Renderer::RM_Normal);
			// remove temporarily
			glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
			bRestoreBuffers = true;
		}
	} else {
		Renderer::SetRenderMode(Renderer::RM_Normal);
	}

	// set global constants
	VertexShaderMngr::SetConstants(*vs);
	PixelShaderMngr::SetConstants(*ps);

	// finally bind
	glBindProgramARB(GL_VERTEX_PROGRAM_ARB, vs->glprogid);
	glBindProgramARB(GL_FRAGMENT_PROGRAM_ARB, ps->glprogid);

#ifdef _DEBUG
	PRIM_LOG("\n");
#endif

	int offset = 0;
	for (vector< pair<int, int> >::const_iterator it = s_vStoredPrimitives.begin(); it != s_vStoredPrimitives.end(); ++it) {
		glDrawArrays(it->first, offset, it->second);
		offset += it->second;
	}

#ifdef _DEBUG
	if (g_Config.iLog & CONF_PRIMLOG) {
		// save the shaders
		char strfile[255];
		sprintf(strfile, "frames/ps%.3d.txt", g_Config.iSaveTargetId);
		std::ofstream fps(strfile);
		fps << ps->strprog.c_str();
		sprintf(strfile, "frames/vs%.3d.txt", g_Config.iSaveTargetId);
		ofstream fvs(strfile);
		fvs << vs->strprog.c_str();
	}

	if (g_Config.iLog & CONF_SAVETARGETS) {
		char str[128];
		sprintf(str, "frames/targ%.3d.tga", g_Config.iSaveTargetId);
		Renderer::SaveRenderTarget(str, 0);
	}
#endif
	g_Config.iSaveTargetId++;

	GL_REPORT_ERRORD();

	if (bRestoreBuffers) {
		GLenum s_drawbuffers[2] = {GL_COLOR_ATTACHMENT0_EXT, GL_COLOR_ATTACHMENT1_EXT};
		glDrawBuffers(2, s_drawbuffers);
		SetColorMask();
	}

	ResetBuffer();
}

void VertexManager::LoadCPReg(u32 SubCmd, u32 Value)
{
	switch (SubCmd & 0xF0)
	{
	case 0x30:
		VertexShaderMngr::SetTexMatrixChangedA(Value);
		break;
	case 0x40:
		VertexShaderMngr::SetTexMatrixChangedB(Value);
		break;

	case 0x50:
		s_GlobalVtxDesc.Hex &= ~0x1FFFF; // keep the Upper bits
		s_GlobalVtxDesc.Hex |= Value;
		break;
	case 0x60:
		s_GlobalVtxDesc.Hex &= 0x1FFFF; // keep the lower 17Bits
		s_GlobalVtxDesc.Hex |= (u64)Value << 17;
		break;

	case 0x70: g_VertexLoaders[SubCmd & 7].SetVAT_group0(Value); _assert_((SubCmd & 0x0F) < 8); break;
	case 0x80: g_VertexLoaders[SubCmd & 7].SetVAT_group1(Value); _assert_((SubCmd & 0x0F) < 8); break;
	case 0x90: g_VertexLoaders[SubCmd & 7].SetVAT_group2(Value); _assert_((SubCmd & 0x0F) < 8); break;

	case 0xA0: arraybases[SubCmd & 0xF]   = Value & 0xFFFFFFFF; break;
	case 0xB0: arraystrides[SubCmd & 0xF] = Value & 0xFF;      break;
	}
}

void VertexManager::EnableComponents(u32 components)
{
    if (s_prevcomponents != components) {
        VertexManager::Flush();

        // matrices
        if ((components & VB_HAS_POSMTXIDX) != (s_prevcomponents & VB_HAS_POSMTXIDX)) {
            if (components & VB_HAS_POSMTXIDX)
                glEnableVertexAttribArray(SHADER_POSMTX_ATTRIB);
            else
                glDisableVertexAttribArray(SHADER_POSMTX_ATTRIB);
        }

        // normals
        if ((components & VB_HAS_NRM0) != (s_prevcomponents & VB_HAS_NRM0)) {
            if (components & VB_HAS_NRM0)
                glEnableClientState(GL_NORMAL_ARRAY);
            else
                glDisableClientState(GL_NORMAL_ARRAY);
        }
        if ((components & VB_HAS_NRM1) != (s_prevcomponents & VB_HAS_NRM1)) {
            if (components & VB_HAS_NRM1) {
                glEnableVertexAttribArray(SHADER_NORM1_ATTRIB);
                glEnableVertexAttribArray(SHADER_NORM2_ATTRIB);
            }
            else {
                glDisableVertexAttribArray(SHADER_NORM1_ATTRIB);
                glDisableVertexAttribArray(SHADER_NORM2_ATTRIB);
            }
        }

        // color
        for (int i = 0; i < 2; ++i) {
            if ((components & (VB_HAS_COL0 << i)) != (s_prevcomponents & (VB_HAS_COL0 << i))) {
                if (components & (VB_HAS_COL0 << 0))
                    glEnableClientState(i ? GL_SECONDARY_COLOR_ARRAY : GL_COLOR_ARRAY);
                else
                    glDisableClientState(i ? GL_SECONDARY_COLOR_ARRAY : GL_COLOR_ARRAY);
            }
        }

        // tex
        for (int i = 0; i < 8; ++i) {
            if ((components & (VB_HAS_UV0 << i)) != (s_prevcomponents & (VB_HAS_UV0 << i))) {
                glClientActiveTexture(GL_TEXTURE0 + i);
                if (components & (VB_HAS_UV0 << i))
                    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                else
                    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            }
        }

        s_prevcomponents = components;
    }
}
