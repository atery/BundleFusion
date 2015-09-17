#pragma once
#ifndef SUBMAP_MANAGER_H
#define SUBMAP_MAnAGER_H

#include "SiftGPU/CUDATimer.h"
#include "GlobalBundlingState.h"
#include "mLibCuda.h"

extern "C" void updateTrajectoryCU(
	float4x4* d_globalTrajectory, unsigned int numGlobalTransforms,
	float4x4* d_completeTrajectory, unsigned int numCompleteTransforms,
	float4x4* d_localTrajectories, unsigned int numLocalTransformsPerTrajectory, unsigned int numLocalTrajectories
	);

extern "C" void initNextGlobalTransformCU(
	float4x4* d_globalTrajectory, unsigned int numGlobalTransforms,
	float4x4* d_localTrajectories, unsigned int numLocalTransformsPerTrajectory);

class SubmapManager {
public:
	CUDACache* currentLocalCache;
	CUDACache* nextLocalCache;
	CUDACache* globalCache; 

	SIFTImageManager* currentLocal;
	SIFTImageManager* nextLocal;
	SIFTImageManager* global;

	float4x4* d_globalTrajectory;
	float4x4* d_completeTrajectory;
	float4x4* d_localTrajectories;

	SubmapManager() {
		currentLocal = NULL;
		nextLocal = NULL;
		global = NULL;
		m_numTotalFrames = 0;
		m_submapSize = 0;

		m_localTimer = NULL;
		m_globalTimer = NULL;

		d_globalTrajectory = NULL;
		d_completeTrajectory = NULL;
		d_localTrajectories = NULL;
	}
	void init(unsigned int maxNumGlobalImages, unsigned int maxNumLocalImages, unsigned int maxNumKeysPerImage,
		unsigned int submapSize, const CUDAImageManager* imageManager, unsigned int numTotalFrames = (unsigned int)-1)
	{
		// cache
		const unsigned int downSampWidth = GlobalBundlingState::get().s_downsampledWidth;
		const unsigned int downSampHeight = GlobalBundlingState::get().s_downsampledHeight;

		const float scaleWidth = (float)downSampWidth / (float)imageManager->getIntegrationWidth();
		const float scaleHeight = (float)downSampHeight / (float)imageManager->getIntegrationHeight();
		mat4f intrinsicsDownsampled = imageManager->getIntrinsics();
		intrinsicsDownsampled._m00 *= scaleWidth;  intrinsicsDownsampled._m02 *= scaleWidth;
		intrinsicsDownsampled._m11 *= scaleHeight; intrinsicsDownsampled._m12 *= scaleHeight;

		currentLocalCache = new CUDACache(downSampWidth, downSampHeight, maxNumLocalImages, intrinsicsDownsampled);
		nextLocalCache = new CUDACache(downSampWidth, downSampHeight, maxNumLocalImages, intrinsicsDownsampled);
		globalCache = new CUDACache(downSampWidth, downSampHeight, maxNumGlobalImages, intrinsicsDownsampled);

		// sift manager
		currentLocal = new SIFTImageManager(maxNumLocalImages, maxNumKeysPerImage);
		nextLocal = new SIFTImageManager(maxNumLocalImages, maxNumKeysPerImage);
		global = new SIFTImageManager(maxNumGlobalImages, maxNumKeysPerImage);

		m_numTotalFrames = numTotalFrames;
		m_submapSize = submapSize;		

		if (GlobalBundlingState::get().s_enableDetailedTimings) {
			m_localTimer = new CUDATimer();
			m_globalTimer = new CUDATimer();

			currentLocal->setTimer(m_localTimer);
			global->setTimer(m_globalTimer);
		}

		
		MLIB_CUDA_SAFE_CALL(cudaMalloc(&d_globalTrajectory, sizeof(float4x4)*maxNumGlobalImages));
		MLIB_CUDA_SAFE_CALL(cudaMalloc(&d_completeTrajectory, sizeof(float4x4)*maxNumGlobalImages*m_submapSize));
		MLIB_CUDA_SAFE_CALL(cudaMalloc(&d_localTrajectories, sizeof(float4x4)*maxNumLocalImages*maxNumGlobalImages));

		float4x4 id;	id.setIdentity();
		MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_globalTrajectory, &id, sizeof(float4x4), cudaMemcpyHostToDevice));
		std::vector<mat4f> initialLocalTrajectories(maxNumLocalImages * maxNumGlobalImages, mat4f::identity());
		MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_localTrajectories, initialLocalTrajectories.data(), sizeof(float4x4) * initialLocalTrajectories.size(), cudaMemcpyHostToDevice));
	}
	void setTotalNumFrames(unsigned int n) {
		m_numTotalFrames = n;
	}
	~SubmapManager() {
		SAFE_DELETE(currentLocal);
		SAFE_DELETE(nextLocal);
		SAFE_DELETE(global);

		SAFE_DELETE(currentLocalCache);
		SAFE_DELETE(nextLocalCache);
		SAFE_DELETE(globalCache);

		MLIB_CUDA_SAFE_FREE(d_globalTrajectory);
		MLIB_CUDA_SAFE_FREE(d_completeTrajectory);
		MLIB_CUDA_SAFE_FREE(d_localTrajectories);
	}
	void evaluateTimings() {
		if (GlobalBundlingState::get().s_enableDetailedTimings) {
			std::cout << "********* LOCAL TIMINGS *********" << std::endl;
			m_localTimer->evaluate(true, true);
			std::cout << std::endl << std::endl;
			std::cout << "********* GLOBAL TIMINGS *********" << std::endl;
			m_globalTimer->evaluate(true, true);
			std::cout << std::endl << std::endl;
		}
	}

	float4x4* getLocalTrajectoryGPU(unsigned int localIdx) const {
		return d_localTrajectories + localIdx * (m_submapSize + 1);
	}

	// update complete trajectory with new global trajectory info
	void updateTrajectory(unsigned int curFrame) {
		updateTrajectoryCU(d_globalTrajectory, global->getNumImages(),
			d_completeTrajectory, curFrame,
			d_localTrajectories, m_submapSize + 1, global->getNumImages());
	}

	void initializeNextGlobalTransform(bool useIdentity = false) {
		const unsigned int numGlobalFrames = global->getNumImages();
		MLIB_ASSERT(numGlobalFrames >= 1);
		if (useIdentity) {
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_globalTrajectory + numGlobalFrames, d_globalTrajectory + numGlobalFrames - 1, sizeof(float4x4), cudaMemcpyDeviceToDevice));
		}
		else {
			initNextGlobalTransformCU(d_globalTrajectory, numGlobalFrames, d_localTrajectories, m_submapSize + 1);
		}
	}

	void switchLocal() {
		currentLocal->reset();
		SIFTImageManager* tmp = currentLocal;
		currentLocal = nextLocal;
		nextLocal = tmp;
		currentLocal->setTimer(m_localTimer);

		currentLocalCache->reset();
		CUDACache* tmpCache = currentLocalCache;
		currentLocalCache = nextLocalCache;
		nextLocalCache = tmpCache;
	}

	bool isLastFrame(unsigned int curFrame) const { return (curFrame + 1) == m_numTotalFrames; }
	bool isLastLocalFrame(unsigned int curFrame) const { return (curFrame >= m_submapSize && (curFrame % m_submapSize) == 0); }
	unsigned int getCurrLocal(unsigned int curFrame) const {
		const unsigned int curLocalIdx = (curFrame + 1 == m_numTotalFrames) ? (curFrame / m_submapSize) : (curFrame / m_submapSize) - 1; // adjust for endframe
		return curLocalIdx;
	}

private:

	unsigned int m_numTotalFrames;
	unsigned int m_submapSize;

	CUDATimer* m_localTimer;
	CUDATimer* m_globalTimer;
};

#endif