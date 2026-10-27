#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Matched GPU comparison of Half-Life: Alyx graphics paths on one Monado service.

Variants (each a full game run):

  wine-dxmt     Windows Alyx, D3D11 -> DXMT -> Metal, under the CrossOver rig,
                through xrizer and the in-process wineopenxr runtime.
  wine-vulkan   The same with -vulkan: Vulkan -> winevulkan -> MoltenVK. A check
                only, to separate Source 2's D3D11 renderer from DXMT. Not runnable
                yet: wineopenxr offers only D3D11 (Metal-backed) sessions.
  native-mvk    Valve's Linux ARM64 Alyx through Klepton, native xrizer and MoltenVK.

Every run uses a new temporary offscreen Monado service from the same build
(kk-tests/with-matching-monado-headset.py), with the remote driver so the same
recorded head and controller motion can be replayed into each run. The same
save, fidelity level and display size are used, so the runtime recommends the
same render size to both games. Windows Alyx reaches the service through an
x86_64 client built from the same tree.

Runs alternate (A B B A ...) and wait for the thermal state to be nominal.
Each run records a Metal System Trace of all processes and Monado's app pacing
trace for the same window, then analysis.py reports GPU ms per frame.

Prerequisites, checked before any run: native macOS Steam signed in (native
variant), the rig's Windows Steam running in its prefix and signed in (Wine
variants), no Alyx already running.
"""
import argparse
import datetime
import hashlib
import json
import os
import plistlib
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analysis  # noqa: E402

HOME = Path.home()
BRIDGE = Path(__file__).resolve().parents[2]
NATIVE_ROOT = Path(os.environ.get('ALYX_NATIVE_ROOT', HOME / 'Code/alyx-native'))
RIG = HOME / 'Windows'
WINDOWS_ALYX = RIG / 'prefix/drive_c/Program Files (x86)/Steam/steamapps/common/Half-Life Alyx'
NATIVE_GAME = NATIVE_ROOT / 'workspace/alyx-native-game-root'

DEFAULTS = dict(
    service_build=NATIVE_ROOT / 'build/monado-upstream-clean-desktop',
    x86_client_json=NATIVE_ROOT / 'build/monado-upstream-clean-desktop-x86_64/openxr_monado-dev.json',
    wrapper=NATIVE_ROOT / 'kk-tests/with-matching-monado-headset.py',
    replayer=NATIVE_ROOT / 'kk-tests/session-replay.py',
    thermal=NATIVE_ROOT / 'workspace/alyx-thermal',
    save_backup=NATIVE_ROOT / 'runs/crash-repro/save-backup-20261026',
    wine_tree=HOME / 'Code/wine-crossover/runtime',
    prefix=RIG / 'prefix',
    xrizer_root=RIG / 'openvr-runtimes/xrizer',
    moltenvk_icd=Path('/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json'),
)

VARIANTS = {
    'wine-dxmt': dict(kind='wine', process='hlvr', extra_args=[]),
    'wine-vulkan': dict(kind='wine', process='hlvr', extra_args=['-vulkan']),
    'native-mvk': dict(kind='native', process='alyx_host', extra_args=[]),
    # Measurement only: colour typeless textures without PixelFormatView (DXMT
    # claude/xr-frame-boundary), so Metal can compress them. Not correct in general.
    'wine-dxmt-strip': dict(kind='wine', process='hlvr', extra_args=[],
                            env=dict(DXMT_PIXEL_FORMAT_VIEW_EXPERIMENT='strip')),
}

SIZE_PATTERNS = ('Recommended render target size', 'wineopenxr: swapchain=', 'recreating swapchain',
                 'Render target size', 'vkCreateSwapchain')


def uptime_ns():
    return time.clock_gettime_ns(time.CLOCK_UPTIME_RAW)


def run_order(variants, repetitions):
    """A B ... then reversed, repeated: each variant sees early and late slots equally."""
    order = []
    for i in range(repetitions):
        order += list(variants) if i % 2 == 0 else list(reversed(variants))
    return order


def thermal_state(tool):
    try:
        return int(subprocess.run([str(tool)], capture_output=True, text=True, timeout=10).stdout.strip())
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def wait_for_cool(tool, minimum_s, maximum_s, log):
    """Wait at least minimum_s, then until the thermal state is nominal (0) or maximum_s passes."""
    start = time.monotonic()
    while True:
        elapsed = time.monotonic() - start
        state = thermal_state(tool)
        if elapsed >= minimum_s and state in (0, None):
            return state
        if elapsed >= maximum_s:
            log(f'thermal state still {state} after {maximum_s:.0f} s; continuing')
            return state
        time.sleep(5)


def pids_named(name):
    """Pids whose executable is exactly name. Wine processes show as "wine" to pgrep; ps's
    command column has the program's Windows path (C:\\...\\hlvr.exe), so match its last part."""
    out = subprocess.run(['ps', '-axo', 'pid=,comm='], capture_output=True, text=True).stdout
    pids = []
    for line in out.splitlines():
        pid, _, command = line.strip().partition(' ')
        if command.replace('\\', '/').rsplit('/', 1)[-1] == name and pid.isdigit():
            pids.append(int(pid))
    return pids


def running(name):
    return bool(pids_named(name))


def tree_sha256(path):
    path = Path(path)
    return {str(f.relative_to(path)): hashlib.sha256(f.read_bytes()).hexdigest()
            for f in sorted(path.rglob('*')) if f.is_file()}


def file_sha256(path):
    try:
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    except OSError:
        return None


class GameState:
    """Puts the backed-up save in the game's save folder and restores what was there afterwards.

    The originals (save folder, convars and any other kept files) are copied to disk
    before the first change, so they survive even if the runner is killed.
    """

    def __init__(self, hlvr, save_backup, keep_dir, extra_files=()):
        self.save = hlvr / 'save'
        self.backup = save_backup
        self.keep = keep_dir
        self.files = [hlvr / 'cfg/machine_convars_vr.vcfg', *extra_files]
        self.manifest = keep_dir / 'original-files.json'

    def prepare(self):
        kept = self.keep / 'original-save'
        if self.save.exists() and not kept.exists():
            shutil.copytree(self.save, kept, copy_function=shutil.copy2)
        manifest = json.loads(self.manifest.read_text()) if self.manifest.exists() else {}
        for i, path in enumerate(self.files):
            if str(path) in manifest:
                continue  # an earlier run in this comparison already kept the original
            copy = self.keep / f'original-file-{len(manifest)}-{path.name}'
            if path.exists():
                shutil.copy2(path, copy)
                manifest[str(path)] = copy.name
            else:
                manifest[str(path)] = None
            self.manifest.write_text(json.dumps(manifest, indent=2) + '\n')
        if self.save.exists():
            shutil.rmtree(self.save)
        shutil.copytree(self.backup, self.save, copy_function=shutil.copy2)
        if tree_sha256(self.save) != tree_sha256(self.backup):
            raise RuntimeError(f'save copy check failed: {self.save}')

    def restore(self):
        kept = self.keep / 'original-save'
        if kept.exists():
            if self.save.exists():
                shutil.rmtree(self.save)
            shutil.copytree(kept, self.save, copy_function=shutil.copy2)
        ok = not kept.exists() or tree_sha256(self.save) == tree_sha256(kept)
        manifest = json.loads(self.manifest.read_text()) if self.manifest.exists() else {}
        for path, copy in manifest.items():
            path = Path(path)
            if copy is None:
                path.unlink(missing_ok=True)
                ok = ok and not path.exists()
            else:
                shutil.copy2(self.keep / copy, path)
                ok = ok and path.read_bytes() == (self.keep / copy).read_bytes()
        return ok


OVERRIDE_CSV = 'vr_fidelity_levels_override.csv'


def override_csv(scale, msaa):
    """A one-level fidelity table: level 0 at this render target scale and MSAA."""
    rows = int(os.environ.get('ALYX_COMPARE_OVERRIDE_ROWS', '1'))  # experiment: rows of the same level
    body = ''.join(f'{i},{1 if i == 0 else ""},{msaa},{scale:g},,,,\n' for i in range(rows))
    return 'hlvr,default,vr_msaa,vr_render_target_scale,min_gpu_memory,min_width,min_height,\n' + body + 'end,,,,,,,\n'


def render_scale(args, variant):
    kind = VARIANTS[variant]['kind']
    specific = args.render_scale_native if kind == 'native' else args.render_scale_wine
    scale = specific if specific is not None else args.render_scale
    return scale if scale is not None and scale > 0 else None


def game_args(args, name):
    variant = VARIANTS[name]
    level = 0 if render_scale(args, name) is not None else args.fidelity
    common = ['-novid', '+vr_fidelity_level_auto', '0', '+vr_fidelity_level', str(level),
              '+vr_symmetric_fov', '0', '+volume', '0', '+load', args.save_name]
    if level == 0 and render_scale(args, name) is not None and not getattr(args, 'no_msaa_args', False):
        # Without +vr_msaa on the command line the Windows build ignores the table's
        # vr_msaa and scale and renders its scene at the full recommended size with 8x
        # MSAA (2026-10-26, DXMT texture log). -msaa makes no difference.
        common += ['+vr_msaa', str(args.msaa)]
    if variant['kind'] == 'native':
        common += ['+vr_fdm', '0']  # D3D11 has no fragment density map; keep the work comparable
    else:
        # The native launcher already starts with +map startup. Without it first, the Windows
        # build queues its start-up map after the save request and discards the save.
        common = ['+map', 'startup'] + common
    return common + variant['extra_args'] + args.game_arg


def service_env(args, timing_dir):
    return dict(
        P_OVERRIDE_ACTIVE_CONFIG='remote', QWERTY_ENABLE='0',
        REMOTE_W_PIXELS=str(args.display[0]), REMOTE_H_PIXELS=str(args.display[1]),
        XRT_COMPOSITOR_DEFAULT_FRAMERATE=str(args.refresh),
        PSVR2_TIMING_TRACE='1', PSVR2_TIMING_TRACE_DIR=str(timing_dir),
    )


def build_command(args, variant_name, run_dir):
    """(argv, env) for one run, wrapped by the matching-service launcher."""
    variant = VARIANTS[variant_name]
    game_out = run_dir / 'game'
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('KK_', 'MESA_KK_', 'XRIZER_', 'QWERTY_', 'MONADO_SERVICE_ENV', 'XRT_', 'P_OVERRIDE',
                                'IPC_', 'XR_RUNTIME_JSON', 'VK_', 'WINE', 'MWXR_', 'ALYX_', 'VR_OVERRIDE'))}
    env.update(service_env(args, run_dir / 'timing'))
    env['ALYX_MONADO_MODE'] = 'offscreen'
    env.update(variant.get('env', {}))
    # Render size: the runtime's recommendation, per client. With +vr_msaa, a fidelity
    # scale below 1 makes Alyx submit bounds that xrizer rejects (InvalidBounds), so the
    # table scale stays at 1 and the recommendation sets the size.
    percent = args.viewport_percent_native if variant['kind'] == 'native' and args.viewport_percent_native \
        else args.viewport_percent
    if percent:
        env['OXR_VIEWPORT_SCALE_PERCENTAGE'] = str(percent)
    for item in args.game_env:  # for the game (and, through the wrapper, the service)
        key, _, value = item.partition('=')
        env[key] = value
    wrapper = [sys.executable, str(args.wrapper), '--mode', 'offscreen', '--build', str(args.service_build),
               '--output', str(game_out), '--']
    if variant['kind'] == 'native':
        # The game's own temp folder (and single-instance lock), set for the game only: the
        # service's socket lives under the wrapper's TMPDIR, which must stay short (104 bytes).
        game_tmp = Path('/private/tmp') / f'alyx-gpu-compare-{run_dir.name}'
        game_tmp.mkdir(exist_ok=True)
        client = ['env', f'TMPDIR={game_tmp}/',
                  str(NATIVE_ROOT / 'run-alyx-gpu-timing'), '--driver', 'moltenvk', '--icd', str(args.moltenvk_icd),
                  '--runtime-json', str(args.service_build / 'openxr_monado-dev.json'),
                  '--output', str(game_out), '--timeout', str(args.timeout), '--']
        client += game_args(args, variant_name)
    else:
        env.update(
            ALYX_GAME_ROOT=str(WINDOWS_ALYX), MWXR_XRIZER_ROOT=str(args.xrizer_root),
            MWXR_IN_PROCESS_WINE=str(args.wine_tree), MWXR_IN_PROCESS_PREFIX=str(args.prefix),
            MWXR_NATIVE_RUNTIME_JSON=str(args.x86_client_json), ALYX_START_STEAM='0',
            ALYX_LOG_DIR=str(game_out), WINEMSYNC='1', MWXR_IN_PROCESS_BUILD=str(args.in_process_build),
            ALYX_BASE_ARGS=' '.join(['-console', '-vconsole', '-nowindow', '-condebug'] + game_args(args, variant_name)),
        )
        client = [str(BRIDGE / 'scripts/run-in-process-alyx.zsh')]
    return wrapper + client, env


def prerequisites(args, variants):
    problems = []
    if running('alyx_host') or running('hlvr.exe'):
        problems.append('Alyx is already running')
    if any(VARIANTS[v]['kind'] == 'native' for v in variants) and not running('steam_osx'):
        problems.append('native Steam is not running (needed by native-mvk)')
    if 'wine-vulkan' in variants:
        problems.append('wine-vulkan needs XR_KHR_vulkan_enable in wineopenxr, which exposes only D3D11 (Metal-backed) '
                        'sessions so far')
    if any(VARIANTS[v]['kind'] == 'wine' for v in variants):
        if not running('steam.exe'):
            problems.append("the rig's Windows Steam is not running (start it with ~/Windows/run-steam.zsh)")
        if not args.x86_client_json.exists():
            problems.append(f'missing x86_64 client {args.x86_client_json}')
        if not (WINDOWS_ALYX / 'game/bin/win64/hlvr.exe').exists():
            problems.append(f'missing Windows Alyx at {WINDOWS_ALYX}')
    for path in (args.service_build / 'src/xrt/targets/service/monado-service', args.wrapper, args.save_backup):
        if not Path(path).exists():
            problems.append(f'missing {path}')
    if args.replay and not Path(args.replay).exists():
        problems.append(f'missing recording {args.replay}')
    return problems


def game_process_name(variant):
    # Exact process names: a pattern over whole command lines also matches unrelated shells.
    return 'alyx_host' if VARIANTS[variant]['kind'] == 'native' else 'hlvr.exe'


def find_pid(variant):
    pids = pids_named(game_process_name(variant))
    return pids[0] if pids else None


def _signal_pids(name, sig):
    for pid in pids_named(name):
        try:
            os.kill(pid, sig)
        except ProcessLookupError:
            pass


def stop_game(args, variant, wrapper_proc, log):
    """Stop the game so the wrapper can clean up its own service.

    The wrapper must never be killed first: its game runs in a session of its own,
    so killing the wrapper orphans both the game and the temporary Monado service.
    """
    wine = VARIANTS[variant]['kind'] == 'wine'
    name = game_process_name(variant)
    if wine:
        env = dict(os.environ, WINEPREFIX=str(args.prefix), WINEMSYNC='1',
                   DYLD_FALLBACK_LIBRARY_PATH=f"{args.wine_tree}/lib/wine/x86_64-unix:{args.wine_tree}/lib")
        subprocess.run([str(args.wine_tree / 'bin/wine'), 'taskkill', '/IM', 'hlvr.exe', '/F'], env=env,
                       capture_output=True, timeout=60)
    else:
        try:
            os.killpg(wrapper_proc.pid, signal.SIGINT)  # the wrapper forwards Ctrl+C to the launcher
        except ProcessLookupError:
            pass
    for sig, wait_s in ((None, 90), (signal.SIGTERM, 30), (signal.SIGKILL, 30)):
        if sig is not None:
            log(f'the game has not stopped; sending it {sig.name}')
            _signal_pids(name, sig)
        try:
            wrapper_proc.wait(timeout=wait_s)
            return
        except subprocess.TimeoutExpired:
            continue
    # Last resort: the wrapper is stuck. Remove its service by its label, then the wrapper.
    label = f'org.freedesktop.monado.alyx-kosmickrisp.{wrapper_proc.pid}'
    log(f'the wrapper did not exit; booting out {label}')
    subprocess.run(['launchctl', 'bootout', f'gui/{os.getuid()}/{label}'], capture_output=True, timeout=10)
    os.killpg(wrapper_proc.pid, signal.SIGTERM)
    wrapper_proc.wait(timeout=30)


def collect_sizes(run_dir):
    sizes = []
    for f in sorted((run_dir / 'game').rglob('*')):
        if not f.is_file() or f.suffix not in ('.log', '.txt', ''):
            continue
        try:
            for line in f.read_text(errors='replace').splitlines():
                if any(p in line for p in SIZE_PATTERNS):
                    sizes.append(line.strip()[-160:])
        except OSError:
            continue
    seen = []
    for s in sizes:
        if s not in seen:
            seen.append(s)
    return seen[:12]


def ktrace_files():
    """xctrace's raw kernel recordings, which it leaves in the user temp folder (0.1 to 4 GB each)."""
    temp = Path(subprocess.run(['getconf', 'DARWIN_USER_TEMP_DIR'], capture_output=True, text=True).stdout.strip())
    return set(temp.glob('instruments*.ktrace')) if temp.is_dir() else set()


def remove_new_ktraces(before, record):
    """Delete the raw recordings this run's xctrace left behind, unless something still has them open."""
    removed = 0
    for path in sorted(ktrace_files() - before):
        if subprocess.run(['lsof', '-t', str(path)], capture_output=True).stdout.strip():
            record['problems'].append(f'left {path.name}: still open')
            continue
        size = path.stat().st_size
        path.unlink(missing_ok=True)
        removed += size
    record['ktrace_bytes_removed'] = removed


def record_trace(args, run_dir, record):
    """xctrace for --capture seconds; returns its exit code. Stops it cleanly if it overruns."""
    if args.trace_template == 'gpu':
        what = ['--instrument', 'GPU']  # only the GPU intervals the analysis uses: far smaller
    else:
        what = ['--template', args.trace_template]
    record['_ktraces_before'] = sorted(str(p) for p in ktrace_files())
    begin = uptime_ns()
    # The trace's own start time is wall clock; this pair converts it to Monado's clock.
    record['clock_pair'] = [time.time(), uptime_ns()]
    with (run_dir / 'xctrace.log').open('w') as xlog:
        xc = subprocess.Popen(['xcrun', 'xctrace', 'record', *what, '--all-processes',
                               '--time-limit', f'{args.capture:.0f}s', '--output', str(run_dir / 'metal.trace')],
                              stdout=xlog, stderr=subprocess.STDOUT)
        try:
            code = xc.wait(timeout=args.capture + 600)
        except subprocess.TimeoutExpired:
            record['problems'].append('xctrace overran; stopped it')
            xc.send_signal(signal.SIGINT)
            try:
                code = xc.wait(timeout=180)
            except subprocess.TimeoutExpired:
                xc.kill()
                code = xc.wait()
    # Only a fallback: xctrace can take a minute to start and to finish, so
    # analysis uses the trace's recorded start and duration when it can.
    record['capture_uptime_ns'] = [begin, uptime_ns()]
    if code != 0:
        record['problems'].append(f'xctrace exited {code}')
    return code


def run_game(args, run_dir, name, variant, argv, env, record, log):
    """Launch, warm up, trace. Returns (game wrapper process, replayer process or None)."""
    started = time.time()
    launcher_log = (run_dir / 'launcher.log').open('w')
    proc = subprocess.Popen(argv, env=env, stdout=launcher_log, stderr=subprocess.STDOUT,
                            start_new_session=True, cwd=str(NATIVE_ROOT))
    replayer = None
    if args.replay:
        replayer = subprocess.Popen(
            [sys.executable, str(args.replayer), str(args.replay), '--from', str(args.replay_from),
             '--start-after', str(args.replay_start_after)] + (['--duration', str(args.replay_duration)]
                                                    if args.replay_duration else []),
            stdout=(run_dir / 'replay.log').open('w'), stderr=subprocess.STDOUT)
    pid = None
    deadline = time.monotonic() + 180
    while pid is None and time.monotonic() < deadline and proc.poll() is None:
        time.sleep(1)
        pid = find_pid(variant)
    if pid is None:
        record['problems'].append('the game process did not appear')
        return proc, replayer
    log(f'{name}: game pid {pid}; warming up {args.warmup:.0f} s')
    while time.time() - started < args.warmup and proc.poll() is None:
        time.sleep(1)
    free_gb = shutil.disk_usage(run_dir).free / 1e9
    if proc.poll() is not None:
        record['problems'].append(f'the game exited early ({proc.returncode})')
    elif free_gb < args.min_free_gb:
        record['problems'].append(f'skipped the trace: {free_gb:.0f} GB free, below --min-free-gb')
    elif args.until_file:
        wait_for_file(args, proc, record, log, name)
    if proc.poll() is None and args.trace_template != 'none' and free_gb >= args.min_free_gb:
        record_trace(args, run_dir, record)
    return proc, replayer


def wait_for_file(args, proc, record, log, name):
    """Keep the game running until a file or folder matching --until-file stops growing."""
    import glob
    deadline = time.monotonic() + args.until_timeout
    last, stable = None, 0
    log(f'{name}: waiting for {args.until_file}')
    while time.monotonic() < deadline and proc.poll() is None:
        hits = glob.glob(os.path.expanduser(args.until_file))
        size = sum(f.stat().st_size for h in hits for f in ([Path(h)] if Path(h).is_file() else Path(h).rglob('*'))
                   if f.is_file()) if hits else None
        stable = stable + 1 if size is not None and size == last else 0
        last = size
        if hits and stable >= 10:  # unchanged for 10 s
            record['until_file'] = dict(paths=hits, bytes=size)
            log(f'{name}: {hits[0]} written ({size / 1e6:.0f} MB)')
            return
        time.sleep(1)
    record['problems'].append(f'no complete {args.until_file} before the timeout or the game exit')


def internal_size(run_dir):
    """The game's own render size per eye, from its auto-fidelity console line.

    Not the OpenXR swapchain size: xrizer creates that at the runtime's recommended
    size, and the game renders at its fidelity level's size and scales into it."""
    import re
    found = None
    for f in sorted((run_dir / 'game').rglob('*')):
        if f.is_file() and f.suffix in ('.log', '.txt'):
            for line in f.read_text(errors='replace').splitlines():
                m = re.search(r'Level 0: Scale = ([0-9.]+) Resolution = (\d+) x (\d+)', line)
                if m:
                    found = f'{m.group(2)}x{m.group(3)}'
    return found


def pause_events(run_dir):
    """Alyx's pause and unpause console lines, in order, as 'paused'/'unpaused'."""
    events = []
    for f in sorted((run_dir / 'game').rglob('*')):
        if f.is_file() and f.suffix in ('.log', '.txt'):
            for line in f.read_text(errors='replace').splitlines():
                if 'unpaused the game' in line:
                    events.append('unpaused')
                elif 'paused the game' in line:
                    events.append('paused')
    return events


def loaded_map(run_dir):
    """The map the server spawned last, from the engine console in the game's logs."""
    found = None
    for f in sorted((run_dir / 'game').rglob('*')):
        if f.is_file() and f.suffix in ('.log', '.txt'):
            for line in f.read_text(errors='replace').splitlines():
                if 'Spawn Server:' in line:
                    found = line.split('Spawn Server:', 1)[1].strip()
    return found


def one_run(args, out, order, variant, log):
    name = f'{order:02d}-{variant}'
    run_dir = out / name
    run_dir.mkdir()
    (run_dir / 'timing').mkdir()
    record = dict(name=name, variant=variant, order=order, problems=[])
    hlvr = (NATIVE_GAME / 'game/hlvr') if VARIANTS[variant]['kind'] == 'native' else (WINDOWS_ALYX / 'game/hlvr')
    # One copy of each game's own save folder for the whole comparison, restored after every run.
    keep = out / f"original-save-{VARIANTS[variant]['kind']}"
    keep.mkdir(exist_ok=True)
    kept = [hlvr / OVERRIDE_CSV]
    if VARIANTS[variant]['kind'] == 'wine':
        kept.append(hlvr / 'console.log')  # -condebug writes it; a copy goes in the run folder
        kept += sorted((args.prefix / 'drive_c/users').glob('*/AppData/Local/openvr/openvrpaths.vrpath'))
    state = GameState(hlvr, args.save_backup, keep, kept)
    scale = render_scale(args, variant)
    record['render_scale'] = scale
    record['msaa'] = args.msaa if scale is not None else None
    argv, env = build_command(args, variant, run_dir)
    record.update(command=argv, service_env=service_env(args, run_dir / 'timing'),
                  identity=dict(dxmt_d3d11=file_sha256(args.prefix / 'drive_c/windows/system32/d3d11.dll'),
                                winemetal=file_sha256(args.wine_tree / 'lib/wine/x86_64-unix/winemetal.so'),
                                x86_client=file_sha256(args.x86_client_json.parent /
                                                       'src/xrt/targets/openxr/libopenxr_monado.dylib')))
    if args.dry_run:
        print(json.dumps(dict(name=name, argv=argv,
                              env={k: env[k] for k in sorted(env) if k not in os.environ or os.environ[k] != env[k]}),
                         indent=2))
        shutil.rmtree(run_dir)
        return None
    record['thermal'] = dict(start=wait_for_cool(args.thermal, args.cooldown if order > 1 else 0,
                                                 args.max_cool_wait, log))
    proc = replayer = None
    try:
        state.prepare()
        if scale is not None:
            (hlvr / OVERRIDE_CSV).write_text(override_csv(scale, args.msaa))
        proc, replayer = run_game(args, run_dir, name, variant, argv, env, record, log)
    except Exception as e:  # restore the game folders whatever happened
        import traceback
        record['problems'].append(f'runner error: {e!r}')
        (run_dir / 'runner-error.txt').write_text(traceback.format_exc())
        log(f'{name}: runner error {e!r}; see runner-error.txt')
    finally:
        if proc is not None and proc.poll() is None:
            log(f'{name}: stopping')
            try:
                stop_game(args, variant, proc, log)
            except Exception as e:
                record['problems'].append(f'could not stop the game: {e!r}')
        if replayer is not None and replayer.poll() is None:
            replayer.terminate()
        record['exit'] = proc.returncode if proc is not None else None
        if (hlvr / 'console.log').exists() and VARIANTS[variant]['kind'] == 'wine':
            (run_dir / 'game').mkdir(exist_ok=True)
            shutil.copy2(hlvr / 'console.log', run_dir / 'game/console.log')
        record['map'] = loaded_map(run_dir)
        record['internal_size'] = internal_size(run_dir)
        record['pause_events'] = pause_events(run_dir)
        if record['pause_events'] and record['pause_events'][-1] == 'paused':
            record['problems'].append('the game was paused at the end (measured a paused game?)')
        record['state_restored'] = state.restore()
        if not record['state_restored']:
            record['problems'].append(f'game files not restored; the originals are in {keep}')
        (run_dir / 'run.json').write_text(json.dumps(record, indent=2) + '\n')
    record['thermal']['end'] = thermal_state(args.thermal)
    if (run_dir / 'metal.trace').exists():
        # Exported after the game has stopped, so the export does not compete with it.
        record['problems'] += analysis.condense_trace(run_dir, record.get('clock_pair'), args.keep_traces)
    before = record.pop('_ktraces_before', None)
    if before is not None and not args.keep_traces:
        remove_new_ktraces({Path(p) for p in before}, record)
    for f in (run_dir / 'timing').glob('*.csv'):  # Monado's timing CSVs are large and compress well
        subprocess.run(['gzip', '-9', str(f)], check=False)
    record['swapchain_sizes'] = collect_sizes(run_dir)
    (run_dir / 'run.json').write_text(json.dumps(record, indent=2) + '\n')
    return run_dir


def analyze(out, refresh):
    results = []
    for run_dir in sorted(p for p in out.iterdir() if (p / 'run.json').exists()):
        variant = json.loads((run_dir / 'run.json').read_text())['variant']
        results.append(analysis.analyze_run(run_dir, VARIANTS[variant]['process'], refresh))
    summary = analysis.summarize(results)
    (out / 'results.json').write_text(json.dumps(dict(summary=summary, runs=results), indent=2) + '\n')
    text = analysis.markdown(summary, results)
    (out / 'summary.md').write_text(text)
    return text


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, help='new directory (default ~/Code/alyx-native/runs/gpu-compare-<time>)')
    p.add_argument('--variants', default='wine-dxmt,native-mvk', help=f'comma list from {", ".join(VARIANTS)}')
    p.add_argument('--repetitions', type=int, default=2, help='passes over the variants, alternating order')
    p.add_argument('--fidelity', type=int, default=0, help="the game's own fidelity level, without --render-scale")
    p.add_argument('--viewport-percent', type=int, default=50,
                   help='OXR_VIEWPORT_SCALE_PERCENTAGE for the game clients: the recommended render size, '
                        '50 gives 1400x1428 per eye from the 4000x2040 remote display')
    p.add_argument('--viewport-percent-native', type=int, default=53,
                   help='the same for the native build, which renders a square from the recommendation '
                        '(50% gives 1344x1344, 53% about 1410x1410)')
    p.add_argument('--render-scale', type=float, default=1.0,
                   help=f'vr_render_target_scale for a one-level {OVERRIDE_CSV} in each game, so both render '
                        'at a chosen size (the two builds ship different fidelity tables); negative: use '
                        "the games' own tables and --fidelity")
    p.add_argument('--render-scale-native', type=float, help='per-game scale, when the builds map scale differently')
    p.add_argument('--render-scale-wine', type=float)
    p.add_argument('--msaa', type=int, default=2, help='vr_msaa with --render-scale')
    p.add_argument('--no-msaa-args', action='store_true',
                   help="don't pass -msaa/+vr_msaa (the Windows build then renders at the full recommended size "
                        "with 8x MSAA); on 2026-10-26 both builds stopped submitting frames soon after loading "
                        'when they were passed together with the one-level override')
    p.add_argument('--save-name', default='autosave', help='save in the backup to load with +load')
    p.add_argument('--display', type=int, nargs=2, default=[4000, 2040], metavar=('W', 'H'),
                   help='remote HMD display size; sets the recommended render size for both games')
    p.add_argument('--refresh', type=int, default=120, help='offscreen compositor rate (Hz)')
    p.add_argument('--warmup', type=float, default=120, help='seconds from launch to the capture (load time)')
    p.add_argument('--capture', type=float, default=15,
                   help='Metal System Trace length (s); all processes, about 40 MB a second')
    p.add_argument('--trace-template', default='gpu',
                   help="'gpu' (GPU instrument only, enough for the analysis), 'none', or an xctrace template name, "
                        "e.g. 'Metal System Trace' (much larger)")
    p.add_argument('--game-env', action='append', default=[], metavar='KEY=VALUE',
                   help='extra environment for the game, e.g. MWXR_DXMT_FRAME_BOUNDARY=1')
    p.add_argument('--in-process-build', type=Path, default=BRIDGE / 'build-in-process/gate',
                   help='wineopenxr build for the Wine variants')
    p.add_argument('--until-file', help='after the warm-up, wait until this glob exists and stops growing '
                                        '(for example a DXMT frame capture), then stop the game')
    p.add_argument('--until-timeout', type=float, default=900)
    p.add_argument('--keep-traces', action='store_true',
                   help='keep each metal.trace; by default only its GPU intervals (a few MB) are kept')
    p.add_argument('--min-free-gb', type=float, default=20, help='skip a capture below this much free disk')
    p.add_argument('--timeout', type=int, default=600, help='native launcher timeout (s)')
    p.add_argument('--cooldown', type=float, default=120, help='minimum seconds between runs')
    p.add_argument('--max-cool-wait', type=float, default=900, help='longest wait for a nominal thermal state (s)')
    p.add_argument('--replay', type=Path, help='session.jsonl to replay; without it the head stays still')
    p.add_argument('--replay-from', type=float, default=0, help='recording time to start from (s)')
    p.add_argument('--replay-duration', type=float)
    p.add_argument('--replay-start-after', type=float, default=40,
                   help='hold the replay start pose this long after it connects (s); the game must have '
                        'loaded the save, and an unpause must happen well before the capture')
    p.add_argument('--game-arg', action='append', default=[], help='extra game argument (repeatable)')
    p.add_argument('--analyze-only', type=Path, help='re-analyze an existing output directory')
    p.add_argument('--dry-run', action='store_true', help='print each run command and exit')
    for key, value in DEFAULTS.items():
        p.add_argument('--' + key.replace('_', '-'), type=Path, default=value)
    args = p.parse_args()

    if args.analyze_only:
        for run_dir in sorted(p for p in args.analyze_only.iterdir() if (p / 'metal.trace').exists()):
            record = json.loads((run_dir / 'run.json').read_text())
            analysis.condense_trace(run_dir, record.get('clock_pair'), args.keep_traces)
        print(analyze(args.analyze_only, args.refresh))
        return 0
    variants = [v.strip() for v in args.variants.split(',') if v.strip()]
    unknown = [v for v in variants if v not in VARIANTS]
    if unknown:
        p.error('unknown variant(s): ' + ', '.join(unknown))
    problems = prerequisites(args, variants)
    if problems and not args.dry_run:
        for problem in problems:
            print('not ready:', problem, file=sys.stderr)
        return 2
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out = args.output or NATIVE_ROOT / f'runs/gpu-compare-{stamp}'
    if out.exists() and not args.dry_run:
        p.error(f'{out} exists')
    out.mkdir(parents=True, exist_ok=args.dry_run)
    log_file = None if args.dry_run else (out / 'compare.log').open('a')

    def log(message):
        line = f'{datetime.datetime.now():%H:%M:%S} {message}'
        print(line, flush=True)
        if log_file:
            log_file.write(line + '\n')
            log_file.flush()

    order = run_order(variants, args.repetitions)
    if not args.dry_run:
        (out / 'plan.json').write_text(json.dumps(dict(order=order, args={k: str(v) for k, v in vars(args).items()}),
                                                  indent=2) + '\n')
        log('order: ' + ' '.join(order))
    for i, variant in enumerate(order, 1):
        one_run(args, out, i, variant, log)
    if args.dry_run:
        if problems:
            print('\nwould not start:', *problems, sep='\n  ')
        if not any(out.iterdir()):
            out.rmdir()
        return 0
    text = analyze(out, args.refresh)
    log('results: ' + str(out / 'summary.md'))
    print(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
