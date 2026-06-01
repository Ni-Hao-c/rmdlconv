# rmdlconv
copyright (c) 2022, rexx

## instructions
1. drag and drop .mdl file on rmdlconv.exe

OR

1. make a batch file with one or more of the supported commands.
2. run the batch file.

---
### supported versions
main versions:
- Portal 2 (v49) -> Apex Legends Season 3 (v54 - rmdl v10)
- Titanfall 2 (v53) -> Apex Legends Season 3 (v54 - rmdl v10)

partially supported:
- Titanfall (v52) -> Titanfall 2 (v53)

unsupported but planned:
- Portal 2 (v49) -> Titanfall 2 (v53)
- Titanfall (v52) -> Apex Legends Season 3 (v54 - rmdl v10)


### supported commands
- "-nopause": automatically close console after running
- "-convertmodel": path to model(s) you wish to convert
  examples: "-convertmodel C:\Among\us.mdl" "-convertmodel C:\Among"
- "-targetversion": version you would like models to be upgraded to
  examples: "-targetversion 53" "-targetversion 54"
- "-outputdir": custom directory for files to be output into
  examples: "-outputdir E:\SuS"
- "-patchstaticcollision": patch a Titanfall 2 v53 `.mdl` with static collision hulls from an OBJ file
  example: `rmdlconv.exe -nopause -patchstaticcollision base.mdl -collisionobj hulls.obj -output out.mdl`
- "-batchpatchstaticcollision": patch many v53 `.mdl` files from a tab-separated job file
  job file columns: `base.mdl<TAB>hulls.obj<TAB>out.mdl`
  example: `rmdlconv.exe -nopause -batchpatchstaticcollision jobs.tsv -threads 8`
- "-batchbuildstaticcollision": build static collision from visible OBJ files, using a Python CoACD worker and C++ k-DOP/static-collision writing
  job file columns: `base.mdl<TAB>visible.obj<TAB>out.mdl`
  example: `rmdlconv.exe -nopause -batchbuildstaticcollision jobs.tsv -workdir work -python python.exe -coacdworker coacd_worker.py -threads 8`
- "-convertsequence": unfinished

## Titanfall static collision pipeline

This repository includes an experimental pipeline for generating Titanfall 2 v53 static collision from visible model geometry. It was built for Titanfall 1 -> Titanfall 2/Northstar conversion tests.

The recommended wrapper is Python. The short form only needs an input model or model directory and an output directory:

```powershell
python batch_static_collision_pipeline_cpp.py C:\models\containers C:\collision_out
```

The wrapper tries to find a Python environment that can import `coacd`, `numpy`, and `trimesh`. You can override this with `--python` or the `RMDLCONV_COACD_PYTHON` environment variable.
`--threads` controls both the C++ patching stage and the number of CoACD worker processes used by the Python worker.

Optional full form:

```powershell
python batch_static_collision_pipeline_cpp.py C:\models\vehicle\straton\straton_imc_gunship_static.mdl C:\collision_out `
  --workdir C:\collision_work `
  --model-root C:\models `
  --rmdlconv .\bin\Release\rmdlconv.exe `
  --python C:\Path\To\Blender\python.exe `
  --threads 2
```

If `--workdir` is not specified, the wrapper creates one next to this Python file, named `_collision_work_<input-name>`.

The wrapper does this:

1. ensures input models are v53 when possible,
2. converts v53 models to v54 temporarily to export visible VG geometry,
3. exports visible OBJ with `export_vg_obj.py`,
4. runs `coacd_worker.py` once for the batch,
5. builds k-DOP hulls in C++,
6. writes Respawn v53 static collision into the output `.mdl`.

The Python CoACD worker expects Python packages:

```text
coacd
numpy
trimesh
```

Using Blender's bundled Python is a convenient way to provide those packages, but the worker does not use `bpy` or require the Blender UI.

Fast smoke-test parameters:

```powershell
--max-hulls 16 --coacd-max-hulls 24 --coacd-resolution 800 --coacd-mcts-iterations 40 --coacd-max-ch-vertex 64
```

Higher-quality parameters:

```powershell
--max-hulls 32 --coacd-max-hulls 64 --coacd-resolution 3000 --coacd-mcts-iterations 200 --coacd-max-ch-vertex 96
```

Outputs are written under the output directory and a `report.json` / `report.csv` summary is generated. Existing CoACD OBJ outputs in `--workdir` are reused unless `--force` is passed.

### known issues
animation conversion is not currently supported and there may be various issues when using models in game

converted models will almost definitely **not work** in R5Reloaded if they only contain 1 bone, as the game deals with them differently

The static collision pipeline is experimental. Validate generated collision in-game before overwriting installed mod files.
