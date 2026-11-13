from pathlib import Path
import hashlib, importlib.util, json, os, shutil, statistics, subprocess, threading, time
r=Path('/Users/nickkennedy/Code/macos-wine-xr')
b=r/'build-in-process/exd-warp'
out=r/'docs/results/dxmt-shader-ablation-20261111'
out.mkdir(exist_ok=True)
spec=importlib.util.spec_from_file_location('audit',r/'tools/analyze_dxmt_tbdr_audit.py')
audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)
telemetry='/Users/nickkennedy/Code/monado-2/build/arm64/tests/tests_macos_service_gpu_sync'
settings=[b/'prefix/drive_c/users/crossover/AppData/Local/EXD/Saved/SaveGames/GameSettings.sav',b/'prefix/drive_c/users/crossover/AppData/Local/EXD/Saved/Config/WindowsNoEditor/GameUserSettings.ini',b/'prefix/drive_c/users/crossover/AppData/Local/EXD/Saved/Config/WindowsNoEditor/Engine.ini']
def hashes(paths):return {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
before=hashes(settings)
manifest=dict(bridge_head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),seconds=60,selection='300 <= frame < 4000 and render_after >= 55',order=['A1','H1','P1','R1','F1','F2','R2','P2','H2','A2'],shader_cache=str(b/'shader-ablation-cache-20261111'),settings_before=before,binaries=hashes([b/'wine/lib/wine/x86_64-windows/d3d11.dll',b/'wine/lib/wine/x86_64-windows/dxgi.dll',b/'wine/lib/wine/x86_64-unix/winemetal.so',b/'prefix/drive_c/windows/system32/d3d11.dll',b/'prefix/drive_c/windows/system32/dxgi.dll']),source=hashes([r/'build-in-process/dxmt-warp-source/src/d3d11/d3d11_shader.cpp',r/'build-in-process/dxmt-warp-source/src/dxmt/dxmt_context.cpp',r/'patches/dxmt/0018-no-map-shader-component-diagnostic.patch']))
if (out/'manifest.json').exists():
 old=json.loads((out/'manifest.json').read_text())
 assert old['binaries']==manifest['binaries'] and old['settings_before']==manifest['settings_before']
 manifest=old
else:
 (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
shutil.copy2(__file__,out/'runner.py')
(b/'shader-ablation-cache-20261111').mkdir(exist_ok=True)
caffeinate=subprocess.Popen(['/usr/bin/caffeinate','-d','-i','-w',str(os.getpid())])
results=json.loads((out/'summary.json').read_text()) if (out/'summary.json').exists() else []
try:
 for name,mode in [('A1','baseline'),('H1','host'),('P1','position'),('R1','reads'),('F1','both'),('F2','both'),('R2','reads'),('P2','position'),('H2','host'),('A2','baseline')]:
  if any(row['run']==name for row in results):continue
  deadline=time.monotonic()+20
  while subprocess.run(['pgrep','-x','monado-service'],capture_output=True).returncode==0:
   if time.monotonic()>deadline:raise RuntimeError('Previous service has not exited')
   time.sleep(.2)
  env={k:v for k,v in os.environ.items() if not k.startswith(('MWXR_','DXMT_','XRT_','SIMULATED_','PSVR2_','OXR_','MTL_'))}
  env.update(MWXR_EXD_EXPERIMENT_ROOT=str(b),MWXR_EXD_SERVICE_ENV='SIMULATED_DESKTOP=1 XRT_MACOS_REFRESH_RATE_HZ=0',MWXR_VIEWPORT_SCALE_PERCENTAGE='143',MWXR_EXD_TAG='shaderablation-'+name,DXMT_SHADER_CACHE_PATH=str(b/'shader-ablation-cache-20261111'),XRT_MACOS_GPU_QUEUE_PRIORITY='0')
  if mode!='baseline':
   env.update(MWXR_EXD_STAY_WARPED='0x0',MWXR_EXD_STAY_WARPED_RATE='1.0')
   env['DXMT_RATE_MAP_DIAGNOSTIC_SHADER_MASK']=str({'host':0,'position':1,'reads':2,'both':3}[mode])
  stop=threading.Event()
  def sample():
   with (out/(name+'-thermal.jsonl')).open('w') as f:
    while not stop.is_set():
     p=subprocess.run([telemetry,'telemetry'],capture_output=True,text=True,timeout=5)
     f.write(json.dumps(dict(epoch=time.time(),exit=p.returncode,output=p.stdout.strip()))+'\n');f.flush()
     stop.wait(5)
  sampler=threading.Thread(target=sample);sampler.start()
  print('Starting '+name+' '+mode,flush=True)
  try:
   with (out/(name+'-launcher.log')).open('w') as f:
    p=subprocess.run([str(r/'scripts/run-exd-headset-compare.zsh'),'msaa4','60'],env=env,cwd=r,stdout=f,stderr=subprocess.STDOUT,timeout=130)
   if p.returncode:raise RuntimeError(name+' launcher failed; see log')
  finally:
   stop.set();sampler.join(timeout=7)
  run=max(b.glob('headset-*-shaderablation-'+name),key=lambda p:p.stat().st_mtime)
  log=run/'game/game.log'
  with log.open(errors='replace') as f:raw=audit.analyze(f,300,4000)
  frames=[v for v in raw['frames'] if v['render_after']>=55]
  warnings=sum('second layout in one pass' in line for line in log.open(errors='replace'))
  keys=['cb_interval_union_ms','command_buffers','render_before','render_after','mapped_render','fence_waits','waits','init_wait','load_proxy','store_proxy','prepare_ms','encode_ms']
  stats={k:audit.percentiles([v[k] for v in frames]) for k in keys}
  if len(frames)<1000 or warnings:raise RuntimeError(name+': missing frames or warning flood')
  if stats['mapped_render']['max']!=0:raise RuntimeError(name+': unexpected map')
  text=log.read_text(errors='replace')
  if mode!='baseline' and 'RATE_MAP_SHADER_DIAGNOSTIC mask='+env['DXMT_RATE_MAP_DIAGNOSTIC_SHADER_MASK'] not in text:raise RuntimeError(name+': shader selector not observed')
  if 'Device error at frame' in text:raise RuntimeError(name+': GPU error')
  after=hashes(settings)
  if before!=after:raise RuntimeError(name+': private settings were not restored')
  thermal=[json.loads(json.loads(line)['output']) for line in (out/(name+'-thermal.jsonl')).read_text().splitlines()]
  result=dict(run=name,mode=mode,path=str(run.relative_to(r)),loaded_frames=len(frames),warnings=warnings,statistics=stats,frame_period_ms=audit.percentiles([(y['start_ns']-x['start_ns'])/1e6 for x,y in zip(frames,frames[1:]) if y['frame']==x['frame']+1]),thermal_states=sorted(set(v['thermal'] for v in thermal)),low_power_states=sorted(set(v['low_power'] for v in thermal)),settings_restored=True)
  results.append(result)
  (out/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
  print(json.dumps({k:result[k] for k in ['run','mode','loaded_frames','warnings','thermal_states']}|{'gpu_ms':stats['cb_interval_union_ms']['median'],'prepare_ms':stats['prepare_ms']['median']}),flush=True)
 manifest['settings_after']=hashes(settings)
 (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
finally:
 caffeinate.terminate();caffeinate.wait()
