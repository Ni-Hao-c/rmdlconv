#pragma once

#include <string>
#include <vector>

struct StaticCollisionPatchResult
{
	int oldCollisionCount = 0;
	int newCollisionCount = 0;
	int collisionBytes = 0;
	int droppedHullCount = 0;
};

StaticCollisionPatchResult PatchStaticCollisionFromObj(
	const std::string& modelPath,
	const std::string& hullObjPath,
	const std::string& outPath);

struct StaticCollisionBatchJob
{
	std::string modelPath;
	std::string hullObjPath;
	std::string outPath;
};

struct StaticCollisionPipelineJob
{
	std::string modelPath;
	std::string visibleObjPath;
	std::string outPath;
};

struct StaticCollisionBatchJobResult
{
	bool ok = false;
	StaticCollisionBatchJob job;
	StaticCollisionPatchResult patch;
	std::string error;
};

std::vector<StaticCollisionBatchJob> ReadStaticCollisionBatchJobs(const std::string& path);

std::vector<StaticCollisionBatchJobResult> PatchStaticCollisionBatch(
	const std::vector<StaticCollisionBatchJob>& jobs,
	int threadCount);

std::vector<StaticCollisionPipelineJob> ReadStaticCollisionPipelineJobs(const std::string& path);

void MakeKdopHullsFromObj(
	const std::string& inputObjPath,
	const std::string& outputObjPath,
	int dop,
	double margin,
	int maxHulls,
	double eps);
