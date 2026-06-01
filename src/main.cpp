// Copyright (c) 2022, rexx
// See LICENSE.txt for licensing information (GPL v3)

#include <pch.h>
#include <core/CommandLine.h>
#include <studio/versions.h>
#include <studio/collision/static_collision.h>
#include <core/utils.h>

#include <atomic>
#include <sstream>
#include <thread>
#include <vector>

const char* pszVersionHelpString = {
	"Please input the version of your model:\n"
	"-- OLD --\n"
	"8:    s0,1\n"
	"9:    s2\n"
	"10:   s3,4\n"
	"11:   s5\n"
	"12:   s6\n"
	"-- NEW --\n"
	"12.1: s7,8\n"
	"12.2: s9,10,11\n"
	"13:   s12\n"
	"14:   s13.1\n"
	"14.1: s14\n"
	"> "
};

const char* pszRSeqVersionHelpString = {
	"Please input the version of your sequence : \n"
	"7:    s0,1,3,4,5,6\n"
	"7.1:  s7,8\n"
	"10:   s9,10,11,12,13,14\n"
	"11:   s15\n"
	"> "
};

static void PrintUsage()
{
	printf(
		"usage:\n"
		"  rmdlconv -nopause -convertmodel <model.mdl|dir> -targetversion <53|54> [-outputdir dir]\n"
		"  rmdlconv -nopause -patchstaticcollision <base.mdl> -collisionobj <hulls.obj> -output <out.mdl>\n"
		"  rmdlconv -nopause -batchpatchstaticcollision <jobs.tsv> [-threads N]\n"
		"  rmdlconv -nopause -batchbuildstaticcollision <jobs.tsv> -workdir <dir> [-python python.exe] [-coacdworker coacd_worker.py] [-threads N]\n"
		"\n"
		"static collision job formats:\n"
		"  batchpatchstaticcollision: base.mdl<TAB>hulls.obj<TAB>out.mdl\n"
		"  batchbuildstaticcollision: base.mdl<TAB>visible.obj<TAB>out.mdl\n"
		"\n"
		"static collision options:\n"
		"  -dop <18|26|50|74>              default 18\n"
		"  -margin <value>                 default 2\n"
		"  -max-hulls <count>              default 32\n"
		"  -coacd-threshold <value>        default 0.03\n"
		"  -coacd-max-hulls <count>        default 64\n"
		"  -coacd-resolution <count>       default 3000\n"
		"  -coacd-mcts-iterations <count>  default 200\n"
		"  -coacd-max-ch-vertex <count>    default 96\n"
		"  -force                          rebuild CoACD cache\n"
		"\n"
		"preferred wrapper:\n"
		"  python batch_static_collision_pipeline_cpp.py <input.mdl|models_dir> <outdir>\n"
	);
}

static std::string QuoteArg(const std::string& value)
{
	return "\"" + value + "\"";
}

static std::string SafeJobName(const std::string& path, size_t index)
{
	std::string name = std::filesystem::path(path).stem().u8string();
	if (name.empty())
		name = "job";
	for (char& c : name)
	{
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.')
			c = '_';
	}
	return std::to_string(index) + "_" + name;
}

static void WriteCoacdWorkerJobs(
	const std::vector<StaticCollisionPipelineJob>& jobs,
	const std::vector<std::string>& coacdObjPaths,
	const std::string& path)
{
	const std::filesystem::path parentPath = std::filesystem::path(path).parent_path();
	if (!parentPath.empty())
		std::filesystem::create_directories(parentPath);

	std::ofstream output(path);
	if (!output)
		Error("couldn't open CoACD worker jobs file: %s\n", path.c_str());

	for (size_t i = 0; i < jobs.size(); ++i)
		output << jobs[i].visibleObjPath << "\t" << coacdObjPaths[i] << "\n";
}

static int RunStaticCollisionPipeline(
	const std::vector<StaticCollisionPipelineJob>& jobs,
	const std::string& workdir,
	const std::string& pythonPath,
	const std::string& coacdWorkerPath,
	int threads,
	bool force,
	double coacdThreshold,
	int coacdMaxHulls,
	int coacdResolution,
	int coacdMctsIterations,
	int coacdMaxChVertex,
	int dop,
	double margin,
	int maxHulls,
	double eps)
{
	if (jobs.empty())
	{
		printf("no jobs found\n");
		return 0;
	}

	std::filesystem::create_directories(workdir);
	std::vector<std::string> coacdObjPaths(jobs.size());
	std::vector<std::string> kdopObjPaths(jobs.size());

	for (size_t i = 0; i < jobs.size(); ++i)
	{
		const std::string name = SafeJobName(jobs[i].outPath, i);
		coacdObjPaths[i] = (std::filesystem::path(workdir) / (name + ".coacd.obj")).u8string();
		kdopObjPaths[i] = (std::filesystem::path(workdir) / (name + ".kdop.obj")).u8string();
	}

	const std::string coacdJobsPath = (std::filesystem::path(workdir) / "coacd_jobs.tsv").u8string();
	const std::string coacdReportPath = (std::filesystem::path(workdir) / "coacd_report.jsonl").u8string();
	WriteCoacdWorkerJobs(jobs, coacdObjPaths, coacdJobsPath);

	std::ostringstream command;
	command << QuoteArg(pythonPath)
		<< " " << QuoteArg(coacdWorkerPath)
		<< " --jobs " << QuoteArg(coacdJobsPath)
		<< " --report " << QuoteArg(coacdReportPath)
		<< " --threshold " << coacdThreshold
		<< " --max-hulls " << coacdMaxHulls
		<< " --resolution " << coacdResolution
		<< " --mcts-iterations " << coacdMctsIterations
		<< " --max-ch-vertex " << coacdMaxChVertex;
	if (force)
		command << " --force";

	const std::string commandLine = "\"" + command.str() + "\"";
	printf("running CoACD worker for %zu job(s)\n", jobs.size());
	const int coacdStatus = std::system(commandLine.c_str());
	if (coacdStatus != 0)
		Error("CoACD worker failed with exit code %i\n", coacdStatus);

	if (threads <= 0)
		threads = static_cast<int>(std::thread::hardware_concurrency());
	if (threads <= 0)
		threads = 1;
	threads = min(threads, static_cast<int>(jobs.size()));

	std::atomic<size_t> nextJob = 0;
	std::atomic<int> ok = 0;
	std::vector<std::string> errors(jobs.size());
	std::vector<std::thread> workers;
	workers.reserve(threads);

	for (int workerIndex = 0; workerIndex < threads; ++workerIndex)
	{
		workers.emplace_back([&]() {
			while (true)
			{
				const size_t index = nextJob.fetch_add(1);
				if (index >= jobs.size())
					return;

				try
				{
					MakeKdopHullsFromObj(coacdObjPaths[index], kdopObjPaths[index], dop, margin, maxHulls, eps);
					PatchStaticCollisionFromObj(jobs[index].modelPath, kdopObjPaths[index], jobs[index].outPath);
					++ok;
				}
				catch (const std::exception& ex)
				{
					errors[index] = ex.what();
				}
				catch (...)
				{
					errors[index] = "unknown error";
				}
			}
		});
	}

	for (std::thread& worker : workers)
		worker.join();

	for (size_t i = 0; i < jobs.size(); ++i)
	{
		if (errors[i].empty())
			printf("[%zu/%zu] ok %s\n", i + 1, jobs.size(), jobs[i].outPath.c_str());
		else
			printf("[%zu/%zu] error %s: %s\n", i + 1, jobs.size(), jobs[i].modelPath.c_str(), errors[i].c_str());
	}

	printf("static collision pipeline complete: total %zu, ok %i, errors %zu\n",
		jobs.size(), ok.load(), jobs.size() - ok.load());
	return ok.load() == static_cast<int>(jobs.size()) ? 0 : 1;
}

// move on from this
void LegacyConversionHandling(CommandLine& cmdline)
{
	// using command args
	if (cmdline.argc > 2)
		return;

	if (!FILE_EXISTS(cmdline.argv[1]))
		Error("couldn't find input file\n");

	std::string mdlPath(cmdline.argv[1]);

	BinaryIO mdlIn;
	mdlIn.open(mdlPath, BinaryIOMode::Read);

	if (mdlIn.read<int>() == 'TSDI')
	{
		int mdlVersion = mdlIn.read<int>();

		switch (mdlVersion)
		{
		case MdlVersion::GARRYSMOD:
		{
			uintmax_t mdlFileSize = GetFileSize(mdlPath);

			mdlIn.seek(0, std::ios::beg);

			char* mdlBuf = new char[mdlFileSize];

			mdlIn.getReader()->read(mdlBuf, mdlFileSize);

			ConvertMDL48To54(mdlBuf, mdlPath, mdlPath);

			delete[] mdlBuf;

			break;
		}
		case MdlVersion::PORTAL2:
		{
			uintmax_t mdlFileSize = GetFileSize(mdlPath);

			mdlIn.seek(0, std::ios::beg);

			char* mdlBuf = new char[mdlFileSize];

			mdlIn.getReader()->read(mdlBuf, mdlFileSize);

			ConvertMDL49To54(mdlBuf, mdlPath, mdlPath);

			delete[] mdlBuf;

			break;
		}
		case MdlVersion::TITANFALL:
		{
			uintmax_t mdlFileSize = GetFileSize(mdlPath);

			mdlIn.seek(0, std::ios::beg);

			char* mdlBuf = new char[mdlFileSize];

			mdlIn.getReader()->read(mdlBuf, mdlFileSize);

			ConvertMDL52To53(mdlBuf, mdlPath, mdlPath);

			delete[] mdlBuf;

			break;
		}
		case MdlVersion::TITANFALL2:
		{
			uintmax_t mdlFileSize = GetFileSize(mdlPath);

			mdlIn.seek(0, std::ios::beg);

			char* mdlBuf = new char[mdlFileSize];

			mdlIn.getReader()->read(mdlBuf, mdlFileSize);

			ConvertMDL53To54(mdlBuf, mdlPath, mdlPath);

			delete[] mdlBuf;

			break;
		}
		case MdlVersion::APEXLEGENDS:
		{
			// rmdl subversion
			std::string version = "12.1";

			if (cmdline.HasParam("-version"))
			{
				version = cmdline.GetParamValue("-version", "12.1");
			}
			else
			{
				std::cout << pszVersionHelpString;
				std::cin >> version;
			}

			printf("Input file is RMDL v%s. attempting conversion...\n", version.c_str());

			if (version == "12.1") // handle 12.1 model conversions
			{
				// convert v12.1 vg to v9 vg
				std::string vgFilePath = ChangeExtension(mdlPath, "vg");

				if (FILE_EXISTS(vgFilePath))
				{
					uintmax_t vgInputSize = GetFileSize(vgFilePath);

					char* vgInputBuf = new char[vgInputSize];

					std::ifstream ifs(vgFilePath, std::ios::in | std::ios::binary);

					ifs.read(vgInputBuf, vgInputSize);

					// if 0tVG magic
					if (*(int*)vgInputBuf == 'GVt0')
						ConvertVGData_12_1(vgInputBuf, vgFilePath, ChangeExtension(mdlPath, "vg_conv"));
					else
						delete[] vgInputBuf;
				}
			}
			else if (version == "8")
			{
				intmax_t mdlFileSize = GetFileSize(mdlPath);

				mdlIn.seek(0, std::ios::beg);

				char* mdlBuf = new char[mdlFileSize];

				mdlIn.getReader()->read(mdlBuf, mdlFileSize);

				ConvertRMDL8To10(mdlBuf, mdlPath, mdlPath);

				delete[] mdlBuf;

				break;
			}
			else
			{
				Error("version is not currently supported\n");
			}

			break;
		}
		default:
		{
			Error("MDL version %i is currently unsupported\n", mdlVersion);
			break;
		}
		}
	}
	else if (mdlPath.find(".rseq"))
	{
		printf("seq gaming\n");

		std::string version = "7.1";

		if (cmdline.HasParam("-version"))
		{
			version = cmdline.GetParamValue("-version", "7.1");
		}
		else
		{
			std::cout << pszRSeqVersionHelpString;
			std::cin >> version;
		}

		uintmax_t seqFileSize = GetFileSize(mdlPath);

		mdlIn.seek(0, std::ios::beg);

		char* seqBuf = new char[seqFileSize];

		mdlIn.getReader()->read(seqBuf, seqFileSize);


		std::string rseqExtPath = ChangeExtension(mdlPath, "rseq_ext");
		char* seqExternalBuf = nullptr;
		if (FILE_EXISTS(rseqExtPath))
		{
			int seqExtFileSize = GetFileSize(rseqExtPath);

			seqExternalBuf = new char[seqExtFileSize];

			std::ifstream ifs(rseqExtPath, std::ios::in | std::ios::binary);

			ifs.read(seqExternalBuf, seqExtFileSize);
		}

		if (version == "7.1")
		{
			//printf("converting rseq version 7.1 to version 7\n");

			ConvertRSEQFrom71To7(seqBuf, seqExternalBuf, mdlPath);
		}
		else if (version == "10")
		{
			ConvertRSEQFrom10To7(seqBuf, seqExternalBuf, mdlPath);
		}

		delete[] seqBuf;
	}
	else
	{
		Error("invalid input file. must be a valid .(r)mdl file with magic 'IDST'\n");
	}
}

int main(int argc, char** argv)
{

	printf("rmdlconv - Copyright (c) %s, rexx\n", &__DATE__[7]);

	CommandLine cmdline(argc, argv);

	if (cmdline.HasParam("-help") || cmdline.HasParam("--help") || cmdline.HasParam("/?"))
	{
		PrintUsage();
		return 0;
	}

    if (argc < 2)
        Error("invalid usage\n");

	if (cmdline.HasParam("-convertmodel"))
	{
		if (!cmdline.HasParam("-targetversion"))
			Error("no '-targetversion' param found while trying to convert model(s)!!!\n required for proper conversion, exiting...\n");

		std::string modelPath = cmdline.GetParamValue("-convertmodel");
		int modelVersionTarget = atoi(cmdline.GetParamValue("-targetversion"));

		const char* customDir = nullptr; // custom base folder for models

		if (cmdline.HasParam("-outputdir"))
			customDir = cmdline.GetParamValue("-outputdir");

		UpgradeStudioModel(modelPath, modelVersionTarget, customDir);
	}

	if (cmdline.HasParam("-patchstaticcollision"))
	{
		std::string modelPath = cmdline.GetParamValue("-patchstaticcollision");
		std::string hullObjPath = cmdline.GetParamValue("-collisionobj");
		std::string outPath = cmdline.GetParamValue("-output");

		if (modelPath.empty() || hullObjPath.empty() || outPath.empty())
			Error("usage: rmdlconv -patchstaticcollision <model.mdl> -collisionobj <hulls.obj> -output <out.mdl>\n");

		StaticCollisionPatchResult result = PatchStaticCollisionFromObj(modelPath, hullObjPath, outPath);
		printf("patched static collision: %s\n", outPath.c_str());
		printf("old collision count %i, new %i, collision bytes %i\n",
			result.oldCollisionCount, result.newCollisionCount, result.collisionBytes);
		if (result.droppedHullCount > 0)
			printf("dropped hulls %i\n", result.droppedHullCount);
	}

	if (cmdline.HasParam("-batchpatchstaticcollision"))
	{
		std::string jobsPath = cmdline.GetParamValue("-batchpatchstaticcollision");
		const int threadCount = atoi(cmdline.GetParamValue("-threads", "0"));
		if (jobsPath.empty())
			Error("usage: rmdlconv -batchpatchstaticcollision <jobs.tsv> [-threads N]\n");

		std::vector<StaticCollisionBatchJob> jobs = ReadStaticCollisionBatchJobs(jobsPath);
		std::vector<StaticCollisionBatchJobResult> results = PatchStaticCollisionBatch(jobs, threadCount);

		int ok = 0;
		for (size_t i = 0; i < results.size(); ++i)
		{
			const StaticCollisionBatchJobResult& result = results[i];
			if (result.ok)
			{
				++ok;
				printf("[%zu/%zu] ok %s sections=%i bytes=%i\n",
					i + 1, results.size(), result.job.outPath.c_str(),
					result.patch.newCollisionCount, result.patch.collisionBytes);
			}
			else
			{
				printf("[%zu/%zu] error %s: %s\n",
					i + 1, results.size(), result.job.modelPath.c_str(), result.error.c_str());
			}
		}

		printf("batch static collision patch complete: total %zu, ok %i, errors %zu\n",
			results.size(), ok, results.size() - ok);
		if (ok != static_cast<int>(results.size()))
			return 1;
	}

	if (cmdline.HasParam("-batchbuildstaticcollision"))
	{
		std::string jobsPath = cmdline.GetParamValue("-batchbuildstaticcollision");
		std::string workdir = cmdline.GetParamValue("-workdir", "static_collision_work");
		std::string pythonPath = cmdline.GetParamValue("-python", "python");
		std::string coacdWorkerPath = cmdline.GetParamValue("-coacdworker", "coacd_worker.py");
		const int threads = atoi(cmdline.GetParamValue("-threads", "0"));
		const bool force = cmdline.HasParam("-force");
		const double coacdThreshold = atof(cmdline.GetParamValue("-coacd-threshold", "0.03"));
		const int coacdMaxHulls = atoi(cmdline.GetParamValue("-coacd-max-hulls", "64"));
		const int coacdResolution = atoi(cmdline.GetParamValue("-coacd-resolution", "3000"));
		const int coacdMctsIterations = atoi(cmdline.GetParamValue("-coacd-mcts-iterations", "200"));
		const int coacdMaxChVertex = atoi(cmdline.GetParamValue("-coacd-max-ch-vertex", "96"));
		const int dop = atoi(cmdline.GetParamValue("-dop", "18"));
		const double margin = atof(cmdline.GetParamValue("-margin", "2"));
		const int maxHulls = atoi(cmdline.GetParamValue("-max-hulls", "32"));
		const double eps = atof(cmdline.GetParamValue("-eps", "0.05"));

		if (jobsPath.empty())
			Error("usage: rmdlconv -batchbuildstaticcollision <jobs.tsv> -workdir <dir> [-threads N]\n");

		std::vector<StaticCollisionPipelineJob> jobs = ReadStaticCollisionPipelineJobs(jobsPath);
		return RunStaticCollisionPipeline(
			jobs,
			workdir,
			pythonPath,
			coacdWorkerPath,
			threads,
			force,
			coacdThreshold,
			coacdMaxHulls,
			coacdResolution,
			coacdMctsIterations,
			coacdMaxChVertex,
			dop,
			margin,
			maxHulls,
			eps);
	}

	if (cmdline.HasParam("-convertsequence"))
	{
		// todo
	}

	LegacyConversionHandling(cmdline); // this should be cut eventually

	if(!cmdline.HasParam("-nopause"))
		std::system("pause");

	return 0;
}
