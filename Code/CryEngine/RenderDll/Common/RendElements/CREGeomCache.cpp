// Copyright 2001-2016 Crytek GmbH / Crytek Group. All rights reserved.

// ------------------------------------------------------------------------
//  File name:   CREGeomCache.cpp
//  Created:     17/10/2012 by Axel Gneiting
//  Description: Backend part of geometry cache rendering
// -------------------------------------------------------------------------
//
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"

#if defined(USE_GEOM_CACHES)

	#include <CryRenderer/RenderElements/RendElement.h>
	#include <Cry3DEngine/CREGeomCache.h>
	#include <Cry3DEngine/I3DEngine.h>
	#include "../Renderer.h"
	#include "../Common/PostProcess/PostEffects.h"

//#include "XRenderD3D9/DriverD3D.h"

std::vector<CREGeomCache*> CREGeomCache::ms_updateList[2];
CryCriticalSection CREGeomCache::ms_updateListCS[2];

CREGeomCache::CREGeomCache()
{
	m_bUpdateFrame[0] = false;
	m_bUpdateFrame[1] = false;
	m_transformUpdateState[0] = 0;
	m_transformUpdateState[1] = 0;

	mfSetType(eDATA_GeomCache);
	mfUpdateFlags(FCEF_TRANSFORM);
}

CREGeomCache::~CREGeomCache()
{
	CryAutoLock<CryCriticalSection> lock1(ms_updateListCS[0]);
	CryAutoLock<CryCriticalSection> lock2(ms_updateListCS[1]);

	stl::find_and_erase(ms_updateList[0], this);
	stl::find_and_erase(ms_updateList[1], this);
}

void CREGeomCache::InitializeRenderElement(const uint numMeshes, _smart_ptr<IRenderMesh>* pMeshes, uint16 materialId)
{
	m_bUpdateFrame[0] = false;
	m_bUpdateFrame[1] = false;

	m_meshFillData[0].clear();
	m_meshFillData[1].clear();
	m_meshRenderData.clear();

	m_meshFillData[0].reserve(numMeshes);
	m_meshFillData[1].reserve(numMeshes);
	m_meshRenderData.reserve(numMeshes);

	for (uint i = 0; i < numMeshes; ++i)
	{
		SMeshRenderData meshRenderData;
		meshRenderData.m_pRenderMesh = pMeshes[i];
		m_meshRenderData.push_back(meshRenderData);
		m_meshFillData[0].push_back(meshRenderData);
		m_meshFillData[1].push_back(meshRenderData);
	}

	m_materialId = materialId;
}

void CREGeomCache::mfPrepare(bool bCheckOverflow)
{
	FUNCTION_PROFILER_RENDER_FLAT

	CRenderer* const pRenderer = gRenDev;

	if (bCheckOverflow)
	{
		pRenderer->FX_CheckOverflow(0, 0, this);
	}

	pRenderer->m_RP.m_CurVFormat = GetVertexFormat();
	pRenderer->m_RP.m_pRE = this;
	pRenderer->m_RP.m_FirstVertex = 0;
	pRenderer->m_RP.m_FirstIndex = 0;
	pRenderer->m_RP.m_RendNumIndices = 0;
	pRenderer->m_RP.m_RendNumVerts = 0;
}

void CREGeomCache::SetupMotionBlur(CRenderObject* pRenderObject, const SRenderingPassInfo& passInfo)
{
	CMotionBlur::SetupObject(pRenderObject, passInfo);

	if (pRenderObject->m_fDistance < CRenderer::CV_r_MotionBlurMaxViewDist)
	{
		pRenderObject->m_ObjFlags |= FOB_HAS_PREVMATRIX | FOB_MOTION_BLUR;
	}
}

bool CREGeomCache::Update(const int flags, const bool bTessellation)
{
	FUNCTION_PROFILER_RENDER_FLAT

	// Wait until render node update has finished
	const int threadId = gRenDev->m_RP.m_nProcessThreadID;
	while (m_transformUpdateState[threadId])
	{
		CrySleep(0);
	}

	// Check if update was successful and if so copy data to render buffer
	if (m_bUpdateFrame[threadId])
	{
		m_meshRenderData = m_meshFillData[threadId];
	}

	const uint numMeshes = m_meshFillData[threadId].size();
	bool bRet = true;

	for (uint nMesh = 0; nMesh < numMeshes; ++nMesh)
	{
		SMeshRenderData& meshData = m_meshFillData[threadId][nMesh];
		CRenderMesh* const pRenderMesh = static_cast<CRenderMesh*>(meshData.m_pRenderMesh.get());

		if (pRenderMesh && pRenderMesh->m_Modified[threadId].linked())
		{
			// Sync the async render mesh update. This waits for the fill thread started from main thread if it's still running.
			// We need to do this manually, because geom caches don't use CREMesh.
			pRenderMesh->SyncAsyncUpdate(threadId);

			CRenderMesh* pVertexContainer = pRenderMesh->_GetVertexContainer();
			bool bSucceed = pRenderMesh->RT_CheckUpdate(pVertexContainer, pRenderMesh->GetVertexFormat(), flags | VSM_MASK, bTessellation);
			if (bSucceed)
			{
				pRenderMesh->m_Modified[threadId].erase();
			}

			if (!bSucceed || !pVertexContainer->_HasVBStream(VSF_GENERAL))
			{
				bRet = false;
			}
		}
	}

	return bRet;
}

void CREGeomCache::UpdateModified()
{
	FUNCTION_PROFILER_RENDER_FLAT

	const int threadId = gRenDev->m_RP.m_nProcessThreadID;
	CryAutoLock<CryCriticalSection> lock(ms_updateListCS[threadId]);

	for (std::vector<CREGeomCache*>::iterator iter = ms_updateList[threadId].begin();
	     iter != ms_updateList[threadId].end(); iter = ms_updateList[threadId].erase(iter))
	{
		CREGeomCache* pRenderElement = *iter;
		pRenderElement->Update(0, false);
	}
}

bool CREGeomCache::mfUpdate(EVertexFormat eVertFormat, int Flags, bool bTessellation)
{
	const bool bRet = Update(Flags, bTessellation);

	const int threadId = gRenDev->m_RP.m_nProcessThreadID;
	CryAutoLock<CryCriticalSection> lock(ms_updateListCS[threadId]);
	stl::find_and_erase(ms_updateList[threadId], this);

	m_Flags &= ~FCEF_DIRTY;
	return bRet;
}

volatile int* CREGeomCache::SetAsyncUpdateState(int& threadId)
{
	FUNCTION_PROFILER_RENDER_FLAT

	  ASSERT_IS_MAIN_THREAD(gRenDev->m_pRT);
	threadId = gRenDev->m_RP.m_nFillThreadID;

	m_bUpdateFrame[threadId] = false;

	CryAutoLock<CryCriticalSection> lock(ms_updateListCS[threadId]);
	stl::push_back_unique(ms_updateList[threadId], this);

	CryInterlockedIncrement(&m_transformUpdateState[threadId]);
	return &m_transformUpdateState[threadId];
}

DynArray<CREGeomCache::SMeshRenderData>* CREGeomCache::GetMeshFillDataPtr()
{
	FUNCTION_PROFILER_RENDER_FLAT

	  assert(gRenDev->m_pRT->IsMainThread(true));
	const int threadId = gRenDev->m_RP.m_nFillThreadID;
	return &m_meshFillData[threadId];
}

DynArray<CREGeomCache::SMeshRenderData>* CREGeomCache::GetRenderDataPtr()
{
	FUNCTION_PROFILER_RENDER_FLAT

	  assert(gRenDev->m_pRT->IsMainThread(true));
	return &m_meshRenderData;
}

void CREGeomCache::DisplayFilledBuffer(const int threadId)
{
	if (m_bUpdateFrame[threadId])
	{
		// You need to call SetAsyncUpdateState before DisplayFilledBuffer
		__debugbreak();
	}
	m_bUpdateFrame[threadId] = true;
}

EVertexFormat CREGeomCache::GetVertexFormat() const
{
	return eVF_P3F_C4B_T2F;
}

bool CREGeomCache::GetGeometryInfo(SGeometryInfo& streams, bool bSupportTessellation)
{
	ZeroStruct(streams);
	streams.eVertFormat = GetVertexFormat();
	streams.nFirstIndex = 0;
	streams.nFirstVertex = 0;
	streams.nNumIndices = 0;
	streams.primitiveType = eptTriangleList;
	return true;
}

void CREGeomCache::Draw(CRenderObject* pObj, const SGraphicsPipelinePassContext& ctx)
{
	//mfUpdate(0, FCEF_TRANSFORM, false); //TODO: check if correct

}

#endif
